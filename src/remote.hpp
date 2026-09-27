#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mrc {
struct ConnectionConfig {
    std::string host;
    int port{22};
    std::string username;
    std::string password;
    std::string private_key;
    std::string passphrase;
    std::filesystem::path known_hosts;
};
struct FileEntry {
    std::string name;
    uint64_t size{};
    bool directory{};
    bool symlink{};
    bool regular{};
};
enum class Operation { List, Upload, Download, MakeDirectory, Rename, Remove };
struct FileJob {
    uint64_t id{};
    Operation operation{Operation::List};
    std::string remote;
    std::string destination{};
    std::filesystem::path local{};
    bool directory{};
    bool overwrite{};
};
enum class EventType { Status, Error, HostKey, TerminalData, Directory, Progress, Done, Exists, Closed };
struct Event {
    EventType type{};
    bool files{};
    uint64_t id{};
    std::string text;
    uint64_t completed{};
    uint64_t total{};
    std::vector<FileEntry> entries{};
};

class Remote {
public:
    Remote();
    ~Remote();
    Remote(const Remote&) = delete;
    Remote& operator=(const Remote&) = delete;
    void connect(ConnectionConfig config);
    void disconnect();
    bool running() const;
    void trust(bool accepted);
    bool send(std::string bytes);
    void resize(int rows, int columns);
    bool enqueue(FileJob job);
    void cancel(uint64_t id);
    std::vector<Event> poll();

private:
    class Connection;
    void terminal_worker(std::stop_token stop, ConnectionConfig config);
    void file_worker(std::stop_token stop, ConnectionConfig config);
    void emit(Event event, std::stop_token stop = {});
    void execute(Connection& connection, const FileJob& job, std::stop_token stop);
    bool cancelled(uint64_t id, std::stop_token stop) const;
    mutable std::mutex mutex_;
    std::mutex host_mutex_;
    std::condition_variable_any wake_;
    std::deque<Event> events_;
    std::deque<FileJob> jobs_;
    std::string input_;
    int rows_{24};
    int columns_{80};
    int trust_decision_{};
    std::atomic<uint64_t> cancelled_id_{};
    std::atomic<bool> terminal_running_{};
    std::atomic<bool> files_running_{};
    std::jthread terminal_thread_;
    std::jthread files_thread_;
};

std::filesystem::path config_directory();
std::string remote_join(const std::string& directory, const std::string& name);
bool valid_filename(const std::string& name);
}