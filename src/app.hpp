#pragma once

#include "remote.hpp"
#include "terminal.hpp"
#include <imgui.h>
#include <array>
#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <string>

namespace mrc {
// Builds the terminal font and the glyph ranges it bakes. Monospace faces such as
// Menlo carry no Han glyphs, so a CJK font is merged in; without that merge Chinese
// remote output renders as fallback boxes. Prompts from starship and powerlevel10k
// also use private-use-area icons and symbol blocks, so those ranges are requested
// and an installed icon font is merged when one is found. Both are declared here so
// the ranges and the resulting coverage can be checked by tests.
ImFont* load_terminal_font();
const ImWchar* terminal_glyph_ranges(ImFontAtlas* atlas);
class LocalDirectory {
public:
    struct Result {
        uint64_t id{};
        std::filesystem::path path;
        std::vector<FileEntry> entries;
        std::string error;
    };
    LocalDirectory();
    ~LocalDirectory();
    uint64_t request(std::filesystem::path path);
    std::optional<Result> poll();
private:
    void worker(std::stop_token stop);
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::optional<Result> pending_;
    std::optional<Result> result_;
    std::atomic<uint64_t> generation_{};
    std::jthread thread_;
};

class App {
public:
    explicit App(ImFont* terminal_font);
    void draw();
    // Connection lifecycle, shared by the dialog buttons and the remote event
    // handling. A closed connection forgets the entered host and username.
    bool connect();
    void disconnect();
    bool connected() const { return terminal_connected_ || files_connected_; }
    std::string host() const { return host_.data(); }
    std::string username() const { return username_.data(); }
    std::string terminal_text() const { return terminal_.visible_text(scroll_); }
private:
    void forget_identity();
    void clear_terminal_view();
    struct Transfer {
        FileJob job;
        std::string status{"Queued"};
        uint64_t completed{};
        uint64_t total{};
        bool finished{};
        std::chrono::steady_clock::time_point started{};
    };
    void events();
    void connection_dialog();
    void terminal_view();
    void terminal_input();
    void files_view();
    void file_pane(bool remote);
    void transfer_view();
    void dialogs();
    void refresh_local(const std::filesystem::path& path = {});
    void refresh_remote(const std::string& path);
    void submit(FileJob job);
    void transfer(bool upload);
    void file_action(Operation operation, bool remote);
    void save_profile();
    void load_profile();
    std::string selection_text() const;
    LocalDirectory local_directory_;
    Remote remote_;
    Terminal terminal_;
    ImFont* terminal_font_;
    std::filesystem::path config_dir_;
    std::array<char, 256> host_{};
    std::array<char, 128> username_{};
    std::array<char, 256> password_{};
    std::array<char, 256> passphrase_{};
    std::array<char, 4096> key_{};
    int port_{22};
    int authentication_{};
    bool show_connection_{true};
    bool close_settings_dialog_{};
    bool terminal_connected_{};
    bool files_connected_{};
    std::string terminal_status_{"Disconnected"};
    std::string files_status_{"Disconnected"};
    std::string error_;
    std::deque<std::string> host_prompts_;
    std::deque<uint64_t> overwrite_prompts_;
    std::array<char, 4096> local_input_{};
    std::array<char, 4096> remote_input_{};
    std::filesystem::path local_path_;
    std::filesystem::path local_requested_path_;
    uint64_t local_listing_id_{};
    bool local_listing_{};
    std::string remote_path_{"."};
    std::vector<FileEntry> local_entries_;
    std::vector<FileEntry> remote_entries_;
    std::string local_selected_;
    std::string remote_selected_;
    uint64_t next_id_{1};
    uint64_t listing_id_{};
    bool listing_{};
    std::map<uint64_t, Transfer> transfers_;
    bool action_open_{};
    bool action_remote_{};
    FileJob action_;
    std::array<char, 256> action_name_{};
    int scroll_{};
    bool selected_{};
    int selection_start_{};
    int selection_end_{};
};
}