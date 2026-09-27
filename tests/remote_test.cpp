#include "remote.hpp"
#include <libssh2.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifdef __linux__
#include <csignal>
#include <sys/resource.h>
#endif

using namespace std::chrono_literals;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::string contents(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.is_open(), "Cannot read test file");
    return {(std::istreambuf_iterator<char>(file)), {}};
}
#ifdef __linux__
class FileSizeLimit {
public:
    FileSizeLimit() {
        require(getrlimit(RLIMIT_FSIZE, &previous_) == 0, "Cannot read file size limit");
        handler_ = std::signal(SIGXFSZ, SIG_IGN);
        require(handler_ != SIG_ERR, "Cannot ignore file size signal");
        auto limited = previous_;
        limited.rlim_cur = 4096;
        if (setrlimit(RLIMIT_FSIZE, &limited) != 0) {
            std::signal(SIGXFSZ, handler_);
            throw std::runtime_error("Cannot set test file size limit");
        }
    }
    ~FileSizeLimit() {
        setrlimit(RLIMIT_FSIZE, &previous_);
        std::signal(SIGXFSZ, handler_);
    }
private:
    rlimit previous_{};
    void (*handler_)(int){};
};
#endif
class Harness {
public:
    mrc::Remote remote;
    std::vector<mrc::Event> pending;
    int trust_requests{};
    mrc::Event wait(mrc::EventType type, uint64_t id = 0, bool files = true) {
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        while (std::chrono::steady_clock::now() < deadline) {
            auto events = remote.poll();
            for (auto& event : events) {
                if (event.type == mrc::EventType::HostKey) { ++trust_requests; remote.trust(true); }
                else pending.push_back(std::move(event));
            }
            for (auto iterator = pending.begin(); iterator != pending.end(); ++iterator) {
                if (iterator->type == type && iterator->id == id && iterator->files == files &&
                    (type != mrc::EventType::Status || iterator->text == "Connected")) {
                    auto result = std::move(*iterator); pending.erase(iterator); return result;
                }
                if (iterator->type == mrc::EventType::Error && type != mrc::EventType::Error)
                    throw std::runtime_error(iterator->text);
            }
            std::this_thread::sleep_for(5ms);
        }
        throw std::runtime_error("Timed out waiting for event " + std::to_string(static_cast<int>(type)));
    }
    void submit(mrc::FileJob job) { require(remote.enqueue(std::move(job)), "Queue rejected job"); }
    void disconnect() {
        remote.disconnect();
        const auto deadline = std::chrono::steady_clock::now() + 12s;
        while (remote.running() && std::chrono::steady_clock::now() < deadline) {
            remote.poll(); std::this_thread::sleep_for(5ms);
        }
        require(!remote.running(), "Disconnect timeout");
        pending.clear();
    }
};
int main(int argc, char** argv) {
    try {
        require(argc == 6, "Expected port, username, private-key, fixture-directory, wrong-known-hosts");
        const auto root = std::filesystem::path(argv[4]);
        mrc::ConnectionConfig config;
        config.host = "127.0.0.1";
        config.port = std::stoi(argv[1]);
        config.username = argv[2];
        config.private_key = argv[3];
        config.known_hosts = root / "known_hosts";
        Harness harness;
        harness.remote.connect(config);
        harness.wait(mrc::EventType::Status, 0, false);
        harness.wait(mrc::EventType::Status);
        require(harness.trust_requests == 1, "Host should be confirmed once across both sessions");
        require(harness.remote.send("printf '\\115\\122\\103\\137\\117\\113\\012'\r"), "Terminal input rejected");
        std::string terminal;
        while (terminal.find("MRC_OK") == std::string::npos)
            terminal += harness.wait(mrc::EventType::TerminalData, 0, false).text;
        harness.remote.resize(35, 100);
        std::string payload(200000, 'R');
        payload[777] = '\0';
        { std::ofstream file(root / "source.bin", std::ios::binary); file.write(payload.data(), payload.size()); }
        const auto remote_dir = (root / "remote").string();
        harness.submit({1, mrc::Operation::MakeDirectory, remote_dir});
        harness.wait(mrc::EventType::Done, 1);
        mrc::FileJob upload{2, mrc::Operation::Upload, remote_dir + "/data.bin", {}, root / "source.bin"};
        harness.submit(upload);
        harness.wait(mrc::EventType::Done, 2);
        harness.submit({3, mrc::Operation::List, remote_dir});
        auto listing = harness.wait(mrc::EventType::Directory, 3);
        require(listing.entries.size() == 1 && listing.entries[0].size == payload.size(), "Remote listing mismatch");
        harness.submit({4, mrc::Operation::Download, upload.remote, {}, root / "download.bin"});
        harness.wait(mrc::EventType::Done, 4);
        std::ifstream downloaded(root / "download.bin", std::ios::binary);
        std::string actual((std::istreambuf_iterator<char>(downloaded)), {});
        require(actual == payload, "Binary transfer mismatch");
        upload.id = 5;
        harness.submit(upload);
        harness.wait(mrc::EventType::Exists, 5);
        upload.overwrite = true;
        harness.submit(upload);
    #if LIBSSH2_VERSION_NUM >= 0x010b01
        harness.wait(mrc::EventType::Done, 5);
    #else
        require(harness.wait(mrc::EventType::Error, 5).text.find("1.11.1") != std::string::npos, "Unsafe overwrite fallback");
    #endif
        mrc::FileJob download{6, mrc::Operation::Download, upload.remote, {}, root / "download.bin"};
        harness.submit(download);
        harness.wait(mrc::EventType::Exists, 6);
        download.overwrite = true;
        harness.submit(download);
        harness.wait(mrc::EventType::Done, 6);
        harness.submit({7, mrc::Operation::Rename, upload.remote, remote_dir + "/renamed.bin"});
        harness.wait(mrc::EventType::Done, 7);
        harness.submit({8, mrc::Operation::Remove, remote_dir + "/renamed.bin"});
        harness.wait(mrc::EventType::Done, 8);
        mrc::FileJob remove_dir{9, mrc::Operation::Remove, remote_dir};
        remove_dir.directory = true;
        harness.submit(remove_dir);
        harness.wait(mrc::EventType::Done, 9);
        require(!std::filesystem::exists(remote_dir), "Remote directory removal failed");
        { std::ofstream empty(root / "empty.bin", std::ios::binary); }
        harness.submit({10, mrc::Operation::Upload, (root / "remote-empty.bin").string(), {}, root / "empty.bin"});
        harness.wait(mrc::EventType::Done, 10);
        harness.submit({11, mrc::Operation::Download, (root / "remote-empty.bin").string(), {}, root / "empty-download.bin"});
        harness.wait(mrc::EventType::Done, 11);
        require(std::filesystem::file_size(root / "empty-download.bin") == 0, "Empty transfer failed");
        {
            std::ofstream large(root / "large.bin", std::ios::binary);
            large.seekp(32 * 1024 * 1024 - 1); large.put('X');
        }
        harness.submit({12, mrc::Operation::Upload, (root / "cancelled.bin").string(), {}, root / "large.bin"});
        harness.wait(mrc::EventType::Progress, 12);
        harness.remote.cancel(12);
        require(harness.wait(mrc::EventType::Error, 12).text == "Cancelled", "Active transfer not cancelled");
        require(!std::filesystem::exists(root / "cancelled.bin"), "Cancelled transfer published a partial file");
        std::filesystem::create_symlink(root / "source.bin", root / "source.link");
        harness.submit({13, mrc::Operation::List, root.string()});
        const auto types = harness.wait(mrc::EventType::Directory, 13);
        bool regular_seen = false, special_seen = false, link_seen = false;
        for (const auto& entry : types.entries) {
            if (entry.name == "source.bin") { require(entry.regular, "Regular file not identified"); regular_seen = true; }
            if (entry.name == "source.fifo") { require(!entry.regular, "FIFO identified as regular"); special_seen = true; }
            if (entry.name == "source.link") { require(entry.symlink && !entry.regular, "Symlink identified as regular"); link_seen = true; }
        }
        require(regular_seen && special_seen && link_seen, "Missing file type fixtures");
        uint64_t next_id = 14;
        for (const auto& source : {root / "source.fifo", root / "source.link", root}) {
            harness.submit({next_id, mrc::Operation::Upload, (root / "invalid-upload.bin").string(), {}, source});
            require(harness.wait(mrc::EventType::Error, next_id++).text.find("regular") != std::string::npos, "Special upload accepted");
            harness.submit({next_id, mrc::Operation::Download, source.string(), {}, root / "invalid-download.bin"});
            require(harness.wait(mrc::EventType::Error, next_id++).text.find("regular") != std::string::npos, "Special download accepted");
        }
        require(!std::filesystem::exists(root / "invalid-upload.bin") && !std::filesystem::exists(root / "invalid-download.bin"),
            "Rejected transfer published a destination");
        std::filesystem::copy_file(root / "source.bin", root / "changing.bin");
        mrc::FileJob changing{next_id++, mrc::Operation::Upload, (root / "remote-empty.bin").string(), {}, root / "changing.bin"};
        harness.submit(changing);
        harness.wait(mrc::EventType::Exists, changing.id);
        std::filesystem::remove(changing.local);
        std::filesystem::create_symlink(root / "source.bin", changing.local);
        changing.overwrite = true;
        harness.submit(changing);
        require(harness.wait(mrc::EventType::Error, changing.id).text.find("regular") != std::string::npos, "Changed source not revalidated");
        require(std::filesystem::file_size(root / "remote-empty.bin") == 0, "Rejected overwrite changed destination");
        mrc::FileJob cancel_download{next_id++, mrc::Operation::Download, (root / "large.bin").string(), {}, root / "download.bin"};
        cancel_download.overwrite = true;
        harness.submit(cancel_download);
        harness.wait(mrc::EventType::Progress, cancel_download.id);
        harness.remote.cancel(cancel_download.id);
        require(harness.wait(mrc::EventType::Error, cancel_download.id).text == "Cancelled", "Active download not cancelled");
        require(contents(cancel_download.local) == payload, "Cancelled download changed original destination");
        mrc::FileJob cancel_upload{next_id++, mrc::Operation::Upload, (root / "remote-empty.bin").string(), {}, root / "large.bin"};
        cancel_upload.overwrite = true;
        harness.submit(cancel_upload);
        harness.wait(mrc::EventType::Progress, cancel_upload.id);
        harness.remote.cancel(cancel_upload.id);
        require(harness.wait(mrc::EventType::Error, cancel_upload.id).text == "Cancelled", "Overwrite upload not cancelled");
        require(std::filesystem::file_size(root / "remote-empty.bin") == 0, "Cancelled upload changed original destination");
        mrc::FileJob missing_parent{next_id++, mrc::Operation::Download, (root / "source.bin").string(), {}, root / "missing-parent" / "file.bin"};
        harness.submit(missing_parent);
        require(harness.wait(mrc::EventType::Error, missing_parent.id).text.find("create local") != std::string::npos,
            "Local destination creation failure not reported");
    #ifdef __linux__
        {
            FileSizeLimit limit;
            mrc::FileJob write_failure{next_id++, mrc::Operation::Download, (root / "source.bin").string(), {}, root / "download.bin"};
            write_failure.overwrite = true;
            harness.submit(write_failure);
            require(harness.wait(mrc::EventType::Error, write_failure.id).text.find("Write local file failed") != std::string::npos,
            "Local write failure not reported");
        }
        require(contents(root / "download.bin") == payload, "Failed write changed original destination");
    #endif
        const auto protected_directory = root / "protected-directory";
        std::filesystem::create_directory(protected_directory);
        { std::ofstream marker(protected_directory / "keep"); marker << "original"; }
        mrc::FileJob publish_failure{next_id++, mrc::Operation::Download, (root / "source.bin").string(), {}, protected_directory};
        publish_failure.overwrite = true;
        harness.submit(publish_failure);
        harness.wait(mrc::EventType::Error, publish_failure.id);
        require(contents(protected_directory / "keep") == "original", "Failed publish damaged existing directory");
        mrc::FileJob interrupted{next_id++, mrc::Operation::Download, (root / "large.bin").string(), {}, root / "interrupted.bin"};
        harness.submit(interrupted);
        harness.wait(mrc::EventType::Progress, interrupted.id);
        harness.remote.disconnect();
        require(harness.wait(mrc::EventType::Error, interrupted.id).text == "Cancelled", "Disconnect did not stop active transfer");
        harness.disconnect();
        require(!std::filesystem::exists(interrupted.local), "Disconnected transfer published a partial file");
        for (const auto& entry : std::filesystem::directory_iterator(root))
            require(entry.path().extension() != ".part", "Transfer temporary file leaked");
        harness.disconnect();
        harness.remote.connect(config);
        harness.wait(mrc::EventType::Status);
        harness.wait(mrc::EventType::Status, 0, false);
        require(harness.trust_requests == 1, "Saved host key was not reused");
        harness.disconnect();
        config.known_hosts = argv[5];
        harness.remote.connect(config);
        require(harness.wait(mrc::EventType::Error).text.find("MISMATCH") != std::string::npos, "Changed host key accepted");
        harness.disconnect();
        require(mrc::valid_filename("hello.txt") && !mrc::valid_filename("../escape"), "Filename validation");
        std::cout << "SSH/SFTP integration tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}