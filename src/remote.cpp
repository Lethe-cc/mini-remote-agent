#include "remote.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace mrc {
namespace {
using Clock = std::chrono::steady_clock;
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
void close_socket(Socket socket) { closesocket(socket); }
void nonblocking(Socket socket, bool enabled) {
    u_long mode = enabled ? 1 : 0;
    if (ioctlsocket(socket, FIONBIO, &mode)) throw std::runtime_error("Cannot set socket mode");
}
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
void close_socket(Socket socket) { close(socket); }
void nonblocking(Socket socket, bool enabled) {
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0 || fcntl(socket, F_SETFL, enabled ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) < 0)
        throw std::runtime_error("Cannot set socket mode");
}
#endif
void erase_secret(std::string& value) {
    volatile char* memory = value.empty() ? nullptr : value.data();
    for (size_t index = 0; index < value.size(); ++index) memory[index] = 0;
    value.clear();
}
struct SecretsGuard {
    ConnectionConfig& config;
    ~SecretsGuard() { erase_secret(config.password); erase_secret(config.passphrase); }
};
std::string suffix() {
    std::random_device random;
    std::ostringstream stream;
    stream << ".mrc-" << std::hex << random() << random() << ".part";
    return stream.str();
}
std::string fingerprint(LIBSSH2_SESSION* session) {
    const auto* hash = reinterpret_cast<const unsigned char*>(libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA256));
    if (!hash) throw std::runtime_error("Server did not provide a SHA-256 host fingerprint");
    std::ostringstream stream;
    stream << "SHA256 (hex): ";
    for (int index = 0; index < 32; ++index) stream << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[index]);
    return stream.str();
}
int key_mask(int type) {
    switch (type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA: return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
    case LIBSSH2_HOSTKEY_TYPE_ED25519: return LIBSSH2_KNOWNHOST_KEY_ED25519;
    default: throw std::runtime_error("Unsupported server host key type");
    }
}
}

class Remote::Connection {
public:
    Remote& owner;
    bool files;
    std::stop_token stop;
    Socket socket{invalid_socket};
    LIBSSH2_SESSION* session{};
    LIBSSH2_CHANNEL* channel{};
    LIBSSH2_SFTP* sftp{};
    Connection(Remote& owner, bool files, std::stop_token stop) : owner(owner), files(files), stop(stop) {}
    ~Connection() {
        if (session) {
            libssh2_session_set_blocking(session, 1);
            libssh2_session_set_timeout(session, 1000);
            if (sftp) libssh2_sftp_shutdown(sftp);
            if (channel) libssh2_channel_free(channel);
            libssh2_session_disconnect(session, "Client closed");
            libssh2_session_free(session);
        }
        if (socket != invalid_socket) close_socket(socket);
    }
    std::string error(const std::string& operation) const {
        char* detail = nullptr;
        libssh2_session_last_error(session, &detail, nullptr, 0);
        return operation + ": " + (detail ? detail : "SSH error");
    }
    void check(int result, const std::string& operation) const {
        if (result < 0) throw std::runtime_error(error(operation));
    }
    void open(ConnectionConfig& config) {
        if (config.host.empty() || config.username.empty() || config.port < 1 || config.port > 65535)
            throw std::runtime_error("Host, username and a valid port are required");
        owner.emit({EventType::Status, files, 0, "Connecting"}, stop);
        addrinfo hints{};
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family = AF_UNSPEC;
        addrinfo* addresses = nullptr;
        const auto service = std::to_string(config.port);
        if (getaddrinfo(config.host.c_str(), service.c_str(), &hints, &addresses))
            throw std::runtime_error("Cannot resolve host");
        const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> cleanup(addresses, freeaddrinfo);
        const auto deadline = Clock::now() + std::chrono::seconds(8);
        for (auto* address = addresses; address && !stop.stop_requested(); address = address->ai_next) {
            socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (socket == invalid_socket) continue;
            nonblocking(socket, true);
            const int result = ::connect(socket, address->ai_addr, static_cast<int>(address->ai_addrlen));
            bool connected = result == 0;
            while (!connected && !stop.stop_requested() && Clock::now() < deadline) {
                fd_set writers;
                FD_ZERO(&writers);
                FD_SET(socket, &writers);
                timeval timeout{0, 100000};
                const int ready = select(static_cast<int>(socket + 1), nullptr, &writers, nullptr, &timeout);
                if (ready < 0) break;
                if (ready == 0) continue;
                int status = 0;
#ifdef _WIN32
                int length = sizeof(status);
#else
                socklen_t length = sizeof(status);
#endif
                connected = getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&status), &length) == 0 && status == 0;
                break;
            }
            if (connected) break;
            close_socket(socket);
            socket = invalid_socket;
        }
        if (stop.stop_requested()) throw std::runtime_error("Cancelled");
        if (socket == invalid_socket) throw std::runtime_error("TCP connection failed or timed out");
        session = libssh2_session_init();
        if (!session) throw std::runtime_error("Cannot allocate SSH session");
        libssh2_session_set_blocking(session, 1);
        libssh2_session_set_timeout(session, 8000);
        check(libssh2_session_handshake(session, socket), "SSH handshake");
        verify(config);
        if (stop.stop_requested()) throw std::runtime_error("Cancelled");
        owner.emit({EventType::Status, files, 0, "Authenticating"}, stop);
        if (!config.private_key.empty()) {
            check(libssh2_userauth_publickey_fromfile(session, config.username.c_str(), nullptr,
                  config.private_key.c_str(), config.passphrase.c_str()), "Private key authentication");
        } else {
            check(libssh2_userauth_password(session, config.username.c_str(), config.password.c_str()), "Password authentication");
        }
        erase_secret(config.password);
        erase_secret(config.passphrase);
    }
    void verify(const ConnectionConfig& config) {
        std::unique_lock host_lock(owner.host_mutex_);
        const std::unique_ptr<LIBSSH2_KNOWNHOSTS, decltype(&libssh2_knownhost_free)> hosts(
            libssh2_knownhost_init(session), libssh2_knownhost_free);
        if (!hosts) throw std::runtime_error("Cannot allocate known-host database");
        if (std::filesystem::exists(config.known_hosts))
            check(libssh2_knownhost_readfile(hosts.get(), config.known_hosts.string().c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH), "Read known hosts");
        size_t length = 0;
        int type = 0;
        const char* key = libssh2_session_hostkey(session, &length, &type);
        if (!key) throw std::runtime_error("Missing server host key");
        const int mask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | key_mask(type);
        libssh2_knownhost* found = nullptr;
        const int result = libssh2_knownhost_checkp(hosts.get(), config.host.c_str(), config.port, key, length, mask, &found);
        if (result == LIBSSH2_KNOWNHOST_CHECK_MATCH) return;
        if (result != LIBSSH2_KNOWNHOST_CHECK_NOTFOUND)
            throw std::runtime_error("HOST KEY MISMATCH: connection refused. Verify the server before updating known_hosts.");
        {
            std::lock_guard lock(owner.mutex_);
            owner.trust_decision_ = 0;
        }
        owner.emit({EventType::HostKey, files, 0, config.host + ":" + std::to_string(config.port) + "\n" + fingerprint(session)}, stop);
        {
            std::unique_lock lock(owner.mutex_);
            owner.wake_.wait(lock, stop, [&] { return owner.trust_decision_ != 0; });
            if (stop.stop_requested() || owner.trust_decision_ != 1) throw std::runtime_error("Host key not trusted");
        }
        const auto hostname = config.port == 22 ? config.host : "[" + config.host + "]:" + std::to_string(config.port);
        check(libssh2_knownhost_addc(hosts.get(), hostname.c_str(), nullptr, key, length, nullptr, 0, mask, nullptr), "Store host key");
        std::filesystem::create_directories(config.known_hosts.parent_path());
        auto temporary = config.known_hosts;
        temporary += suffix();
        check(libssh2_knownhost_writefile(hosts.get(), temporary.string().c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH), "Write known hosts");
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), config.known_hosts.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            std::filesystem::remove(temporary);
            throw std::runtime_error("Cannot replace known_hosts");
        }
#else
        std::filesystem::permissions(temporary, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
        std::filesystem::rename(temporary, config.known_hosts);
#endif
    }
};

Remote::Remote() {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("Winsock initialization failed");
#endif
    if (libssh2_init(0)) throw std::runtime_error("libssh2 initialization failed");
}
Remote::~Remote() {
    disconnect();
    if (terminal_thread_.joinable()) terminal_thread_.join();
    if (files_thread_.joinable()) files_thread_.join();
    libssh2_exit();
#ifdef _WIN32
    WSACleanup();
#endif
}
bool Remote::running() const { return terminal_running_ || files_running_; }
void Remote::connect(ConnectionConfig config) {
    SecretsGuard guard{config};
    if (running()) throw std::runtime_error("Disconnect before reconnecting");
    if (terminal_thread_.joinable()) terminal_thread_.join();
    if (files_thread_.joinable()) files_thread_.join();
    {
        std::lock_guard lock(mutex_);
        jobs_.clear(); events_.clear(); input_.clear();
        trust_decision_ = 0;
    }
    cancelled_id_ = 0;
    terminal_running_ = true;
    files_running_ = true;
    terminal_thread_ = std::jthread([this, config](std::stop_token stop) mutable { terminal_worker(stop, std::move(config)); });
    files_thread_ = std::jthread([this, config](std::stop_token stop) mutable { file_worker(stop, std::move(config)); });
}
void Remote::disconnect() {
    terminal_thread_.request_stop();
    files_thread_.request_stop();
    wake_.notify_all();
}
void Remote::trust(bool accepted) {
    { std::lock_guard lock(mutex_); trust_decision_ = accepted ? 1 : -1; }
    if (!accepted) disconnect();
    wake_.notify_all();
}
bool Remote::send(std::string bytes) {
    std::lock_guard lock(mutex_);
    if (!terminal_running_ || input_.size() + bytes.size() > 1024 * 1024) return false;
    input_ += bytes;
    wake_.notify_all();
    return true;
}
void Remote::resize(int rows, int columns) {
    std::lock_guard lock(mutex_);
    rows_ = rows; columns_ = columns;
    wake_.notify_all();
}
bool Remote::enqueue(FileJob job) {
    std::lock_guard lock(mutex_);
    if (!files_running_ || jobs_.size() >= 64) return false;
    jobs_.push_back(std::move(job));
    wake_.notify_all();
    return true;
}
void Remote::cancel(uint64_t id) {
    std::lock_guard lock(mutex_);
    const auto queued = std::find_if(jobs_.begin(), jobs_.end(), [id](const FileJob& job) { return job.id == id; });
    if (queued != jobs_.end()) {
        jobs_.erase(queued);
        events_.push_back({EventType::Error, true, id, "Cancelled"});
    } else cancelled_id_ = id;
    wake_.notify_all();
}
bool Remote::cancelled(uint64_t id, std::stop_token stop) const { return stop.stop_requested() || cancelled_id_ == id; }
void Remote::emit(Event event, std::stop_token stop) {
    std::unique_lock lock(mutex_);
    if (event.type == EventType::Progress) {
        const auto previous = std::find_if(events_.begin(), events_.end(), [&](const Event& candidate) {
            return candidate.type == EventType::Progress && candidate.id == event.id;
        });
        if (previous != events_.end()) { *previous = std::move(event); return; }
    }
    if (event.type == EventType::TerminalData) {
        wake_.wait(lock, stop, [&] { return events_.size() < 256; });
        if (stop.stop_requested()) return;
    }
    events_.push_back(std::move(event));
}
std::vector<Event> Remote::poll() {
    std::lock_guard lock(mutex_);
    std::vector<Event> result;
    while (!events_.empty()) { result.push_back(std::move(events_.front())); events_.pop_front(); }
    wake_.notify_all();
    return result;
}
void Remote::terminal_worker(std::stop_token stop, ConnectionConfig config) {
    SecretsGuard guard{config};
    try {
        Connection connection(*this, false, stop);
        connection.open(config);
        connection.channel = libssh2_channel_open_session(connection.session);
        if (!connection.channel) throw std::runtime_error(connection.error("Open terminal channel"));
        int rows, columns;
        { std::lock_guard lock(mutex_); rows = rows_; columns = columns_; }
        connection.check(libssh2_channel_request_pty_ex(connection.channel, "xterm-256color", 14, nullptr, 0, columns, rows, 0, 0), "Request PTY");
        connection.check(libssh2_channel_shell(connection.channel), "Start shell");
        libssh2_session_set_blocking(connection.session, 0);
        emit({EventType::Status, false, 0, "Connected"}, stop);
        std::array<char, 16384> buffer{};
        std::string pending;
        int requested_rows = rows, requested_columns = columns;
        bool resizing = false;
        while (!stop.stop_requested()) {
            {
                std::lock_guard lock(mutex_);
                if (pending.empty()) pending = std::exchange(input_, {});
                if (!resizing && (rows != rows_ || columns != columns_)) {
                    requested_rows = rows_; requested_columns = columns_; resizing = true;
                }
            }
            if (resizing) {
                const int result = libssh2_channel_request_pty_size(connection.channel, requested_columns, requested_rows);
                if (result != LIBSSH2_ERROR_EAGAIN) {
                    connection.check(result, "Resize PTY");
                    rows = requested_rows; columns = requested_columns; resizing = false;
                }
            }
            if (!pending.empty()) {
                const auto written = libssh2_channel_write(connection.channel, pending.data(), pending.size());
                if (written > 0) pending.erase(0, static_cast<size_t>(written));
                else if (written != LIBSSH2_ERROR_EAGAIN && written < 0) throw std::runtime_error(connection.error("Write terminal"));
            }
            for (int stream = 0; stream <= 1; ++stream) {
                const auto count = libssh2_channel_read_ex(connection.channel, stream, buffer.data(), buffer.size());
                if (count > 0) emit({EventType::TerminalData, false, 0, std::string(buffer.data(), static_cast<size_t>(count))}, stop);
                else if (count != LIBSSH2_ERROR_EAGAIN && count < 0) throw std::runtime_error(connection.error("Read terminal"));
            }
            if (libssh2_channel_eof(connection.channel)) break;
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, stop, std::chrono::milliseconds(8), [&] { return !input_.empty() && pending.empty(); });
        }
    } catch (const std::exception& error) {
        if (!stop.stop_requested()) emit({EventType::Error, false, 0, error.what()});
    }
    emit({EventType::Closed, false, 0, "Disconnected"});
    terminal_running_ = false;
}
void Remote::file_worker(std::stop_token stop, ConnectionConfig config) {
    SecretsGuard guard{config};
    try {
        Connection connection(*this, true, stop);
        connection.open(config);
        connection.sftp = libssh2_sftp_init(connection.session);
        if (!connection.sftp) throw std::runtime_error(connection.error("Start SFTP"));
        emit({EventType::Status, true, 0, "Connected"}, stop);
        while (!stop.stop_requested()) {
            FileJob job;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, stop, [&] { return !jobs_.empty(); });
                if (stop.stop_requested()) break;
                job = std::move(jobs_.front()); jobs_.pop_front();
            }
            try { execute(connection, job, stop); }
            catch (const std::exception& error) { emit({EventType::Error, true, job.id, error.what()}); }
        }
    } catch (const std::exception& error) {
        if (!stop.stop_requested()) emit({EventType::Error, true, 0, error.what()});
    }
    {
        std::lock_guard lock(mutex_);
        for (const auto& job : jobs_) events_.push_back({EventType::Error, true, job.id, "Disconnected"});
        jobs_.clear();
    }
    emit({EventType::Closed, true, 0, "Disconnected"});
    files_running_ = false;
}

void Remote::execute(Connection& connection, const FileJob& job, std::stop_token stop) {
    auto* sftp = connection.sftp;
    auto check = [&](int result, const std::string& operation) {
        if (result < 0) throw std::runtime_error(operation + " failed (SFTP " + std::to_string(libssh2_sftp_last_error(sftp)) + ")");
    };
    auto cancellation = [&] {
        if (cancelled(job.id, stop)) throw std::runtime_error("Cancelled");
    };
    cancellation();
    if (job.operation == Operation::List) {
        std::array<char, 8192> canonical{};
        const int length = libssh2_sftp_realpath(sftp, job.remote.c_str(), canonical.data(), static_cast<unsigned int>(canonical.size()));
        check(length, "Resolve remote directory");
        const std::string path(canonical.data(), static_cast<size_t>(length));
        auto* handle = libssh2_sftp_opendir(sftp, path.c_str());
        if (!handle) throw std::runtime_error(connection.error("Open remote directory"));
        const std::unique_ptr<LIBSSH2_SFTP_HANDLE, decltype(&libssh2_sftp_close_handle)> cleanup(handle, libssh2_sftp_close_handle);
        Event event{EventType::Directory, true, job.id, path};
        while (true) {
            cancellation();
            LIBSSH2_SFTP_ATTRIBUTES attributes{};
            const int count = libssh2_sftp_readdir(handle, canonical.data(), canonical.size(), &attributes);
            check(count, "Read directory");
            if (!count) break;
            std::string name(canonical.data(), static_cast<size_t>(count));
            if (name == "." || name == "..") continue;
            event.entries.push_back({name, attributes.filesize,
                LIBSSH2_SFTP_S_ISDIR(attributes.permissions) != 0,
                LIBSSH2_SFTP_S_ISLNK(attributes.permissions) != 0,
                (attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISREG(attributes.permissions)});
        }
        emit(std::move(event), stop);
        return;
    }
    if (job.operation == Operation::MakeDirectory) check(libssh2_sftp_mkdir(sftp, job.remote.c_str(), 0755), "Create directory");
    else if (job.operation == Operation::Rename) check(libssh2_sftp_rename_ex(sftp, job.remote.c_str(), static_cast<unsigned int>(job.remote.size()),
        job.destination.c_str(), static_cast<unsigned int>(job.destination.size()), 0), "Rename");
    else if (job.operation == Operation::Remove) {
        check(job.directory ? libssh2_sftp_rmdir(sftp, job.remote.c_str()) : libssh2_sftp_unlink(sftp, job.remote.c_str()), "Delete");
    } else {
        const bool upload = job.operation == Operation::Upload;
        if (upload && !std::filesystem::is_regular_file(std::filesystem::symlink_status(job.local)))
            throw std::runtime_error("Expected a regular local source file (no symbolic links)");
        LIBSSH2_SFTP_ATTRIBUTES attributes{};
        const int stat_result = libssh2_sftp_lstat(sftp, job.remote.c_str(), &attributes);
        if (!upload) check(stat_result, "Stat remote file");
        if (upload && stat_result < 0 && libssh2_sftp_last_error(sftp) != LIBSSH2_FX_NO_SUCH_FILE) check(stat_result, "Stat destination");
        if (stat_result == 0 && (!(attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) || !LIBSSH2_SFTP_S_ISREG(attributes.permissions)))
            throw std::runtime_error("Expected a regular remote file (no symbolic links)");
        if (!job.overwrite && ((upload && stat_result == 0) || (!upload && std::filesystem::exists(job.local)))) {
            emit({EventType::Exists, true, job.id, "Destination exists"}, stop);
            return;
        }
        const uint64_t total = upload ? std::filesystem::file_size(job.local) : attributes.filesize;
        const std::string temporary_remote = upload ? job.remote + suffix() : job.remote;
        auto temporary_local = job.local;
        if (!upload) temporary_local += suffix();
        std::ifstream input;
        std::ofstream output;
        if (upload) {
            input.open(job.local, std::ios::binary);
            if (!input) throw std::runtime_error("Cannot open local source");
        }
        const unsigned long flags = upload ? LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_EXCL : LIBSSH2_FXF_READ;
        auto* handle = libssh2_sftp_open(sftp, temporary_remote.c_str(), flags, 0600);
        if (!handle) throw std::runtime_error(connection.error("Open transfer file"));
        try {
            if (!upload) {
                LIBSSH2_SFTP_ATTRIBUTES opened{};
                check(libssh2_sftp_fstat(handle, &opened), "Stat opened remote file");
                if (!(opened.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) || !LIBSSH2_SFTP_S_ISREG(opened.permissions))
                    throw std::runtime_error("Expected a regular remote source file");
                output.open(temporary_local, std::ios::binary | std::ios::trunc);
                if (!output) throw std::runtime_error("Cannot create local download");
            }
            std::array<char, 65536> buffer{};
            uint64_t completed = 0;
            auto last_progress = Clock::now();
            emit({EventType::Progress, true, job.id, "Transferring", 0, total}, stop);
            while (true) {
                cancellation();
                if (upload) {
                    input.read(buffer.data(), buffer.size());
                    const auto count = input.gcount();
                    if (!count) {
                        if (input.bad()) throw std::runtime_error("Read local file failed");
                        break;
                    }
                    size_t offset = 0;
                    while (offset < static_cast<size_t>(count)) {
                        cancellation();
                        const auto written = libssh2_sftp_write(handle, buffer.data() + offset, static_cast<size_t>(count) - offset);
                        if (written <= 0) throw std::runtime_error(connection.error("Upload"));
                        offset += static_cast<size_t>(written);
                    }
                    completed += static_cast<uint64_t>(count);
                } else {
                    const auto count = libssh2_sftp_read(handle, buffer.data(), buffer.size());
                    if (count < 0) throw std::runtime_error(connection.error("Download"));
                    if (!count) break;
                    output.write(buffer.data(), count);
                    if (!output) throw std::runtime_error("Write local file failed");
                    completed += static_cast<uint64_t>(count);
                }
                if (Clock::now() - last_progress > std::chrono::milliseconds(100)) {
                    emit({EventType::Progress, true, job.id, "Transferring", completed, total}, stop);
                    last_progress = Clock::now();
                }
            }
            const int close_result = libssh2_sftp_close_handle(handle);
            handle = nullptr;
            check(close_result, "Close transfer");
            cancellation();
            if (upload) {
                if (job.overwrite) {
#if LIBSSH2_VERSION_NUM >= 0x010b01
                    check(libssh2_sftp_posix_rename_ex(sftp, temporary_remote.c_str(), temporary_remote.size(),
                        job.remote.c_str(), job.remote.size()), "Atomic overwrite (server must support posix-rename)");
#else
                    throw std::runtime_error("Remote overwrite requires libssh2 >= 1.11.1; original file preserved");
#endif
                } else {
                    check(libssh2_sftp_rename_ex(sftp, temporary_remote.c_str(), static_cast<unsigned int>(temporary_remote.size()),
                        job.remote.c_str(), static_cast<unsigned int>(job.remote.size()), 0), "Publish upload");
                }
            } else {
                output.close();
                if (!output) throw std::runtime_error("Flush local file failed");
                if (!job.overwrite) {
                    std::filesystem::create_hard_link(temporary_local, job.local);
                    std::filesystem::remove(temporary_local);
                } else {
#ifdef _WIN32
                    if (!MoveFileExW(temporary_local.c_str(), job.local.c_str(), MOVEFILE_REPLACE_EXISTING))
                        throw std::runtime_error("Cannot replace destination");
#else
                    std::filesystem::rename(temporary_local, job.local);
#endif
                }
            }
            emit({EventType::Progress, true, job.id, "Transferring", completed, completed}, stop);
        } catch (...) {
            if (handle) libssh2_sftp_close_handle(handle);
            if (upload) libssh2_sftp_unlink(sftp, temporary_remote.c_str());
            else {
                output.close();
                std::error_code ignored;
                std::filesystem::remove(temporary_local, ignored);
            }
            throw;
        }
    }
    emit({EventType::Done, true, job.id, "Complete"}, stop);
}
std::filesystem::path config_directory() {
#ifdef _WIN32
    if (const char* base = std::getenv("APPDATA")) return std::filesystem::path(base) / "mini-remote-client";
#else
    if (const char* base = std::getenv("XDG_CONFIG_HOME")) return std::filesystem::path(base) / "mini-remote-client";
    if (const char* home = std::getenv("HOME")) return std::filesystem::path(home) / ".config" / "mini-remote-client";
#endif
    throw std::runtime_error("Cannot determine user configuration directory");
}
std::string remote_join(const std::string& directory, const std::string& name) {
    if (directory.empty() || directory == "/") return "/" + name;
    return directory.back() == '/' ? directory + name : directory + "/" + name;
}
bool valid_filename(const std::string& name) {
    return !name.empty() && name != "." && name != ".." && name.find_first_of("/\\") == std::string::npos && name.find('\0') == std::string::npos;
}
}