#include "app.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

namespace mrc {
namespace {
template<size_t Size> void set(std::array<char, Size>& target, const std::string& value) {
    const size_t count = std::min(value.size(), Size - 1);
    std::memcpy(target.data(), value.data(), count);
    target[count] = 0;
}
template<size_t Size> void wipe(std::array<char, Size>& target) {
    volatile char* memory = target.data();
    for (size_t index = 0; index < Size; ++index) memory[index] = 0;
}
std::string path_text(const std::filesystem::path& path) {
    auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
std::filesystem::path utf8_path(std::string_view text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
std::string size_text(uint64_t bytes) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024 && unit < 4) { value /= 1024; ++unit; }
    std::ostringstream text;
    text << std::fixed << std::setprecision(unit ? 1 : 0) << value << ' ' << units[unit];
    return text.str();
}
void sort_entries(std::vector<FileEntry>& entries) {
    std::sort(entries.begin(), entries.end(), [](const FileEntry& left, const FileEntry& right) {
        if (left.directory != right.directory) return left.directory;
        return left.name < right.name;
    });
}
void tooltip(const char* text) { if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", text); }
ImU32 color(VTermColor value) { return IM_COL32(value.rgb.red, value.rgb.green, value.rgb.blue, 255); }
void modal_layout(float width) {
    const auto size = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(width, size.x - 32), 0));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(size.x - 32, size.y - 32));
}
bool contains_ci(std::string_view text, std::string_view needle) {
    auto lower = [](char value) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    };
    std::string haystack;
    for (const char value : text) haystack += lower(value);
    std::string target;
    for (const char value : needle) target += lower(value);
    return haystack.find(target) != std::string::npos;
}
// Locates a font that provides the private-use-area icons used by starship and
// powerlevel10k prompts. Only user font directories are scanned, so the lookup
// stays cheap and deterministic; MRC_ICON_FONT overrides it and "none" opts out.
std::filesystem::path icon_font_candidate() {
    if (const char* override = std::getenv("MRC_ICON_FONT")) {
        if (std::string_view(override) == "none") return {};
        return utf8_path(override);
    }
    std::vector<std::filesystem::path> directories;
    if (const char* home = std::getenv("HOME")) {
        directories.push_back(std::filesystem::path(home) / "Library" / "Fonts");
        directories.push_back(std::filesystem::path(home) / ".local" / "share" / "fonts");
    }
    directories.emplace_back("/Library/Fonts");
    for (const char* needle : {"nerd", "powerline"}) {
        for (const auto& directory : directories) {
            std::error_code ignored;
            if (!std::filesystem::is_directory(directory, ignored)) continue;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(directory, ignored)) {
                if (ignored) break;
                const auto name = path_text(entry.path().filename());
                if (!contains_ci(name, needle)) continue;
                const auto extension = entry.path().extension();
                if (extension != ".ttf" && extension != ".ttc" && extension != ".otf") continue;
                return entry.path();
            }
        }
    }
    return {};
}
}

ImFont* load_terminal_font() {
    auto& io = ImGui::GetIO();
    const ImWchar* const ranges = terminal_glyph_ranges(io.Fonts);
    const char* const monospace[] = {
        std::getenv("MRC_FONT"),
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationMono-Regular.ttf",
        "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/Supplemental/Menlo.ttc",
        "C:/Windows/Fonts/consola.ttf"
    };
    ImFont* font = nullptr;
    for (const char* candidate : monospace) {
        if (!candidate || !std::filesystem::exists(candidate)) continue;
        font = io.Fonts->AddFontFromFileTTF(candidate, 17.f, nullptr, ranges);
        if (font) break;
    }
    if (!font) return io.Fonts->AddFontDefault();
    ImFontConfig merge;
    merge.MergeMode = true;   // merged faces only fill glyphs the primary lacks
    // None of the monospace faces above ship Han ideographs, so merge a CJK family
    // in. Every candidate is checked for existence because ImGui asserts on a
    // missing font file, and the usable files differ per platform.
    const char* const cjk[] = {
        std::getenv("MRC_CJK_FONT"),
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/System/Library/Fonts/STHeiti Medium.ttc",
        "/System/Library/Fonts/Supplemental/Songti.ttc",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
        "C:/Windows/Fonts/msyh.ttc"
    };
    for (const char* candidate : cjk) {
        if (!candidate || !std::filesystem::exists(candidate)) continue;
        if (io.Fonts->AddFontFromFileTTF(candidate, 17.f, &merge, ranges)) break;
    }
    // starship and powerlevel10k draw their icons from the private use area, which
    // no system monospace face provides: an installed Nerd Font or Powerline font
    // supplies them.
    if (const auto icon = icon_font_candidate(); !icon.empty() && std::filesystem::exists(icon))
        io.Fonts->AddFontFromFileTTF(path_text(icon).c_str(), 17.f, &merge, ranges);
    // Symbol faces cover the remaining blocks, for example ⬆ and ⚡.
    const char* const symbols[] = {
        "/System/Library/Fonts/Apple Symbols.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "C:/Windows/Fonts/seguiemj.ttf"
    };
    for (const char* candidate : symbols)
        if (std::filesystem::exists(candidate))
            io.Fonts->AddFontFromFileTTF(candidate, 17.f, &merge, ranges);
    return font;
}

const ImWchar* terminal_glyph_ranges(ImFontAtlas* atlas) {
    // The atlas keeps a pointer to these, so the storage outlives the call.
    static ImVector<ImWchar> ranges;
    if (!ranges.empty()) return ranges.Data;
    static const ImWchar blocks[] = {
        0x2190, 0x21FF,   // arrows
        0x2300, 0x23FF,   // misc technical (timers)
        0x2500, 0x257F,   // box drawing (vim, tmux)
        0x2580, 0x259F,   // block elements (progress bars)
        0x25A0, 0x25FF,   // geometric shapes
        0x2600, 0x26FF,   // misc symbols
        0x2700, 0x27BF,   // dingbats (✔ ✘ ❯)
        0x2B00, 0x2BFF,   // misc symbols and arrows (⬆ ⬇)
        0xE000, 0xF8FF,   // private use area: Powerline, Nerd Font, FontAwesome
        0
    };
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(atlas->GetGlyphRangesChineseFull());
    builder.AddRanges(blocks);
    builder.BuildRanges(&ranges);
    return ranges.Data;
}

LocalDirectory::LocalDirectory() : thread_([this](std::stop_token stop) { worker(stop); }) {}
LocalDirectory::~LocalDirectory() {
    thread_.request_stop();
    wake_.notify_all();
}
uint64_t LocalDirectory::request(std::filesystem::path path) {
    std::lock_guard lock(mutex_);
    const auto id = ++generation_;
    pending_ = Result{id, std::move(path), {}, {}};
    result_.reset();
    wake_.notify_all();
    return id;
}
std::optional<LocalDirectory::Result> LocalDirectory::poll() {
    std::lock_guard lock(mutex_);
    return std::exchange(result_, std::nullopt);
}
void LocalDirectory::worker(std::stop_token stop) {
    while (!stop.stop_requested()) {
        Result result;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, stop, [&] { return pending_.has_value(); });
            if (stop.stop_requested()) return;
            result = std::move(*pending_);
            pending_.reset();
        }
        try {
            result.path = std::filesystem::canonical(result.path);
            for (const auto& entry : std::filesystem::directory_iterator(result.path)) {
                if (stop.stop_requested() || result.id != generation_) break;
                std::error_code ignored;
                const auto status = entry.symlink_status(ignored);
                const bool symlink = std::filesystem::is_symlink(status);
                const bool directory = std::filesystem::is_directory(status) || (symlink && entry.is_directory(ignored));
                const bool regular = std::filesystem::is_regular_file(status);
                uint64_t size = 0;
                if (regular) { size = entry.file_size(ignored); if (ignored) size = 0; }
                result.entries.push_back({path_text(entry.path().filename()), size, directory, symlink, regular});
            }
            if (stop.stop_requested()) return;
            if (result.id != generation_) continue;
            sort_entries(result.entries);
        } catch (const std::exception& exception) {
            result.entries.clear();
            result.error = exception.what();
        }
        std::lock_guard lock(mutex_);
        if (!stop.stop_requested() && result.id == generation_) result_ = std::move(result);
    }
}

App::App(ImFont* terminal_font) : terminal_font_(terminal_font), config_dir_(config_directory()) {
    local_path_ = std::filesystem::current_path();
    set(local_input_, path_text(local_path_));
    set(remote_input_, ".");
    load_profile();
    refresh_local();
}
void App::load_profile() {
    std::ifstream stream(config_dir_ / "connection.txt");
    std::string host, username, key;
    int port = 22, mode = 0;
    if (stream >> std::quoted(host) >> port >> std::quoted(username) >> mode >> std::quoted(key)) {
        set(host_, host); set(username_, username); set(key_, key);
        port_ = std::clamp(port, 1, 65535); authentication_ = mode == 1 ? 1 : 0;
    }
}
void App::save_profile() {
    std::filesystem::create_directories(config_dir_);
    std::ofstream stream(config_dir_ / "connection.txt");
    stream << std::quoted(host_.data()) << '\n' << port_ << '\n' << std::quoted(username_.data()) << '\n'
           << authentication_ << '\n' << std::quoted(key_.data()) << '\n';
    if (!stream) throw std::runtime_error("Cannot save connection profile");
}
bool App::connect() {
    try {
        save_profile();
        clear_terminal_view();
        remote_entries_.clear(); remote_selected_.clear();
        host_prompts_.clear(); overwrite_prompts_.clear(); error_.clear();
        ConnectionConfig config;
        config.host = host_.data(); config.port = port_; config.username = username_.data();
        if (authentication_ == 0) config.password = password_.data();
        else { config.private_key = key_.data(); config.passphrase = passphrase_.data(); }
        config.known_hosts = config_dir_ / "known_hosts";
        remote_.connect(std::move(config));
        wipe(password_); wipe(passphrase_);
        return true;
    } catch (const std::exception& exception) {
        error_ = exception.what();
        return false;
    }
}
void App::disconnect() {
    remote_.disconnect();
    terminal_connected_ = false; files_connected_ = false;
    host_prompts_.clear(); overwrite_prompts_.clear();
    forget_identity();
    // The previous session's screen belongs to the connection that just ended.
    clear_terminal_view();
    // An empty dialog needs no further input, so it dismisses itself.
    close_settings_dialog_ = true;
}
void App::clear_terminal_view() {
    terminal_.reset();
    scroll_ = 0;
    selected_ = false;
    selection_start_ = selection_end_ = 0;
}
void App::forget_identity() {
    // A closed session must not leave the previous address and user behind, not even
    // for the next start, so the saved profile is rewritten without them. Port,
    // authentication mode and key path are kept as convenience.
    set(host_, ""); set(username_, "");
    std::error_code ignored;
    try {
        save_profile();
    } catch (const std::exception&) {
        // Best effort: if the cleared profile cannot be written, drop the stale one
        // instead of letting the old identity come back on the next launch.
        std::filesystem::remove(config_dir_ / "connection.txt", ignored);
    }
}
void App::events() {
    if (auto result = local_directory_.poll(); result && result->id == local_listing_id_) {
        local_listing_ = false;
        if (result->error.empty()) {
            local_path_ = std::move(result->path);
            local_entries_ = std::move(result->entries);
            local_selected_.clear();
        } else error_ = "Local: " + result->error;
        set(local_input_, path_text(local_path_));
    }
    for (auto& event : remote_.poll()) {
        switch (event.type) {
        case EventType::HostKey: host_prompts_.push_back(std::move(event.text)); break;
        case EventType::TerminalData: terminal_.feed(event.text); break;
        case EventType::Status:
            (event.files ? files_status_ : terminal_status_) = event.text;
            if (event.text == "Connected") {
                if (event.files) { files_connected_ = true; refresh_remote("."); }
                else terminal_connected_ = true;
            }
            break;
        case EventType::Closed:
            (event.files ? files_connected_ : terminal_connected_) = false;
            (event.files ? files_status_ : terminal_status_) = "Disconnected";
            if (event.files) {
                listing_ = false;
                for (auto& [id, transfer] : transfers_) {
                    if (!transfer.finished) { transfer.status = "Disconnected"; transfer.finished = true; }
                }
            }
            // Also forget the identity when the peer or the network dropped us.
            if (!remote_.running()) disconnect();
            break;
        case EventType::Directory:
            if (event.id == listing_id_) {
                remote_path_ = event.text; set(remote_input_, remote_path_);
                remote_entries_ = std::move(event.entries); sort_entries(remote_entries_);
                remote_selected_.clear(); listing_ = false;
            }
            break;
        case EventType::Progress:
            if (auto found = transfers_.find(event.id); found != transfers_.end()) {
                auto& transfer = found->second;
                if (transfer.status != "Transferring") transfer.started = std::chrono::steady_clock::now();
                transfer.status = "Transferring"; transfer.completed = event.completed; transfer.total = event.total;
            }
            break;
        case EventType::Exists:
            if (auto found = transfers_.find(event.id); found != transfers_.end()) {
                found->second.status = "Awaiting confirmation";
                overwrite_prompts_.push_back(event.id);
            }
            break;
        case EventType::Done:
            if (auto found = transfers_.find(event.id); found != transfers_.end()) {
                found->second.status = "Complete"; found->second.finished = true;
            }
            refresh_local();
            if (files_connected_ && !listing_) refresh_remote(remote_path_);
            break;
        case EventType::Error:
            if (auto found = transfers_.find(event.id); found != transfers_.end()) {
                found->second.status = event.text; found->second.finished = true;
            }
            if (event.id == listing_id_) { listing_ = false; set(remote_input_, remote_path_); }
            error_ = (event.files ? "SFTP: " : "SSH: ") + event.text;
            break;
        }
    }
    auto output = terminal_.take_output();
    if (!output.empty() && terminal_connected_ && !remote_.send(std::move(output))) error_ = "Terminal input queue is full";
}
void App::draw() {
    events();
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Mini Remote Client", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("Mini Remote Client");
    ImGui::SameLine();
    ImGui::TextDisabled(" / %s", host_[0] ? host_.data() : "No connection");
    if (ImGui::Button("Connection")) show_connection_ = true;
    ImGui::SameLine();
    ImGui::BeginDisabled(!remote_.running());
    if (ImGui::Button("Disconnect")) disconnect();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextColored(terminal_connected_ ? ImVec4(0.08f, 0.45f, 0.34f, 1) : ImVec4(0.45f, 0.46f, 0.46f, 1),
        "SSH: %s   SFTP: %s", terminal_status_.c_str(), files_status_.c_str());
    ImGui::Separator();
    const float queue_height = transfers_.empty() ? 0 : std::min(170.0f, ImGui::GetContentRegionAvail().y * 0.3f);
    ImGui::BeginChild("Workspace", ImVec2(0, -queue_height - (error_.empty() ? 0 : 44)), false);
    if (ImGui::BeginTabBar("Views")) {
        if (ImGui::BeginTabItem("Terminal")) { terminal_view(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Files")) { files_view(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    if (!error_.empty()) {
        if (ImGui::SmallButton("Dismiss")) error_.clear();
        ImGui::SameLine();
        ImGui::TextWrapped("%s", error_.c_str());
    }
    if (!transfers_.empty()) transfer_view();
    connection_dialog();
    dialogs();
    ImGui::End();
}
void App::connection_dialog() {
    if (show_connection_) { ImGui::OpenPopup("Connection settings"); show_connection_ = false; }
    modal_layout(540);
    if (!ImGui::BeginPopupModal("Connection settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (close_settings_dialog_) {
        close_settings_dialog_ = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::BeginDisabled(remote_.running());
    ImGui::PushItemWidth(-120);
    ImGui::InputText("Host", host_.data(), host_.size());
    ImGui::InputInt("Port", &port_, 0);
    ImGui::InputText("Username", username_.data(), username_.size());
    ImGui::RadioButton("Password", &authentication_, 0); ImGui::SameLine();
    ImGui::RadioButton("Private key", &authentication_, 1);
    if (authentication_ == 0) ImGui::InputText("Password##secret", password_.data(), password_.size(), ImGuiInputTextFlags_Password);
    else {
        ImGui::InputText("Key path", key_.data(), key_.size());
        ImGui::InputText("Passphrase", passphrase_.data(), passphrase_.size(), ImGuiInputTextFlags_Password);
    }
    ImGui::PopItemWidth();
    const bool valid = host_[0] && username_[0] && port_ > 0 && port_ <= 65535 && (authentication_ == 0 || key_[0]);
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button("Connect", ImVec2(100, 0))) {
        if (connect()) ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Close")) { wipe(password_); wipe(passphrase_); ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

void App::terminal_view() {
    if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(selection_text().c_str());
    tooltip("Copy selection or visible terminal (Ctrl+Shift+C)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!terminal_connected_);
    if (ImGui::SmallButton("Paste")) { if (const char* text = ImGui::GetClipboardText()) terminal_.paste(text); }
    ImGui::EndDisabled();
    tooltip("Paste clipboard (Ctrl+Shift+V)");
    ImGui::SameLine();
    if (ImGui::SmallButton("Latest")) { scroll_ = 0; selected_ = false; }
    ImGui::SameLine();
    ImGui::TextDisabled("%d x %d  |  %d history", terminal_.columns(), terminal_.rows(), terminal_.history_size());
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(24 / 255.f, 28 / 255.f, 27 / 255.f, 1));
    ImGui::BeginChild("Terminal screen", ImVec2(0, 0), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushFont(terminal_font_);
    const ImVec2 cell_size{ImGui::CalcTextSize("M").x, ImGui::GetTextLineHeight()};
    const auto available = ImGui::GetContentRegionAvail();
    const int rows = std::clamp(static_cast<int>(available.y / cell_size.y), 2, 300);
    const int columns = std::clamp(static_cast<int>(available.x / cell_size.x), 2, 500);
    if (rows != terminal_.rows() || columns != terminal_.columns()) {
        terminal_.resize(rows, columns); remote_.resize(rows, columns); selected_ = false;
    }
    const auto origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("Terminal input", ImVec2(std::max(1.f, available.x), std::max(1.f, available.y)));
    const bool hovered = ImGui::IsItemHovered();
    auto& io = ImGui::GetIO();
    if (hovered && io.MouseWheel != 0) {
        scroll_ = std::clamp(scroll_ + static_cast<int>(io.MouseWheel * 3), 0, terminal_.history_size());
        selected_ = false;
    }
    auto mouse_cell = [&] {
        const int column = std::clamp(static_cast<int>((io.MousePos.x - origin.x) / cell_size.x), 0, columns - 1);
        const int row = std::clamp(static_cast<int>((io.MousePos.y - origin.y) / cell_size.y), 0, rows - 1);
        return row * columns + column;
    };
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImGui::SetWindowFocus(); selected_ = false; selection_start_ = selection_end_ = mouse_cell();
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        selected_ = true; selection_end_ = mouse_cell();
    }
    auto* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(origin, ImVec2(origin.x + available.x, origin.y + available.y), true);
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const auto cell = terminal_.cell(row, column, scroll_);
            const ImVec2 start{origin.x + column * cell_size.x, origin.y + row * cell_size.y};
            const ImVec2 end{start.x + cell_size.x, start.y + cell_size.y};
            draw->AddRectFilled(start, end, color(cell.bg));
            const int position = row * columns + column;
            if (selected_ && position >= std::min(selection_start_, selection_end_) && position <= std::max(selection_start_, selection_end_))
                draw->AddRectFilled(start, end, IM_COL32(63, 105, 108, 180));
        }
        for (int column = 0; column < columns; ++column) {
            const auto cell = terminal_.cell(row, column, scroll_);
            const ImVec2 start{origin.x + column * cell_size.x, origin.y + row * cell_size.y};
            const ImVec2 end{start.x + cell_size.x * std::max(1, static_cast<int>(cell.width)), start.y + cell_size.y};
            if (cell.chars[0] != UINT32_MAX && !cell.attrs.conceal) {
                std::string text;
                for (auto codepoint : cell.chars) { if (!codepoint) break; text += Terminal::utf8(codepoint); }
                draw->AddText(start, color(cell.fg), text.c_str());
                if (cell.attrs.bold) draw->AddText(ImVec2(start.x + 0.5f, start.y), color(cell.fg), text.c_str());
                if (cell.attrs.underline) draw->AddLine(ImVec2(start.x, end.y - 1), ImVec2(end.x, end.y - 1), color(cell.fg));
            }
        }
    }
    const bool focused = ImGui::IsWindowFocused();
    if (focused && terminal_connected_ && scroll_ == 0 && terminal_.cursor_visible() && std::fmod(ImGui::GetTime(), 1.2) < 0.8) {
        const auto cursor = terminal_.cursor();
        const ImVec2 start{origin.x + cursor.col * cell_size.x, origin.y + cursor.row * cell_size.y};
        draw->AddRect(start, ImVec2(start.x + cell_size.x, start.y + cell_size.y), IM_COL32(150, 225, 180, 255));
    }
    draw->PopClipRect();
    if (focused) terminal_input();
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}
std::string App::selection_text() const {
    if (!selected_) return terminal_.visible_text(scroll_);
    std::string result;
    const int first = std::min(selection_start_, selection_end_);
    const int last = std::max(selection_start_, selection_end_);
    for (int position = first; position <= last; ++position) {
        const int column = position % terminal_.columns();
        if (column == 0 && position > first) result += '\n';
        const auto cell = terminal_.cell(position / terminal_.columns(), column, scroll_);
        if (cell.chars[0] == UINT32_MAX) continue;
        if (!cell.chars[0]) result += ' ';
        for (auto codepoint : cell.chars) { if (!codepoint) break; result += Terminal::utf8(codepoint); }
    }
    return result;
}
void App::terminal_input() {
    auto& io = ImGui::GetIO();
    const bool shortcut = (io.KeyCtrl && io.KeyShift) || io.KeySuper;
    if (shortcut && ImGui::IsKeyPressed(ImGuiKey_C, false)) { ImGui::SetClipboardText(selection_text().c_str()); return; }
    if (!terminal_connected_) return;
    if (shortcut && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
        if (const char* text = ImGui::GetClipboardText()) terminal_.paste(text);
        scroll_ = 0; return;
    }
    if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_PageUp)) { scroll_ = std::min(terminal_.history_size(), scroll_ + terminal_.rows()); return; }
    if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_PageDown)) { scroll_ = std::max(0, scroll_ - terminal_.rows()); return; }
    const auto modifiers = static_cast<VTermModifier>((io.KeyShift ? VTERM_MOD_SHIFT : 0) |
        (io.KeyCtrl ? VTERM_MOD_CTRL : 0) | (io.KeyAlt ? VTERM_MOD_ALT : 0));
    const std::pair<ImGuiKey, VTermKey> keys[] = {
        {ImGuiKey_Enter, VTERM_KEY_ENTER}, {ImGuiKey_KeypadEnter, VTERM_KEY_ENTER},
        {ImGuiKey_Backspace, VTERM_KEY_BACKSPACE}, {ImGuiKey_Tab, VTERM_KEY_TAB}, {ImGuiKey_Escape, VTERM_KEY_ESCAPE},
        {ImGuiKey_UpArrow, VTERM_KEY_UP}, {ImGuiKey_DownArrow, VTERM_KEY_DOWN},
        {ImGuiKey_LeftArrow, VTERM_KEY_LEFT}, {ImGuiKey_RightArrow, VTERM_KEY_RIGHT},
        {ImGuiKey_Home, VTERM_KEY_HOME}, {ImGuiKey_End, VTERM_KEY_END},
        {ImGuiKey_Insert, VTERM_KEY_INS}, {ImGuiKey_Delete, VTERM_KEY_DEL},
        {ImGuiKey_PageUp, VTERM_KEY_PAGEUP}, {ImGuiKey_PageDown, VTERM_KEY_PAGEDOWN}
    };
    for (const auto& [imgui_key, terminal_key] : keys)
        if (ImGui::IsKeyPressed(imgui_key)) { terminal_.key(terminal_key, modifiers); scroll_ = 0; }
    for (int index = 0; index < 12; ++index)
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_F1 + index))) terminal_.key(static_cast<VTermKey>(VTERM_KEY_FUNCTION(index + 1)), modifiers);
    if (io.KeyCtrl && !io.KeyAlt && !io.KeySuper) {
        for (int index = 0; index < 26; ++index)
            if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_A + index))) terminal_.character('a' + index, modifiers);
        if (ImGui::IsKeyPressed(ImGuiKey_Space)) terminal_.character(' ', modifiers);
        const std::pair<ImGuiKey, char> control_keys[] = {{ImGuiKey_LeftBracket, '['}, {ImGuiKey_Backslash, '\\'}, {ImGuiKey_RightBracket, ']'}, {ImGuiKey_Minus, '_'}};
        for (const auto& [key, character] : control_keys) if (ImGui::IsKeyPressed(key)) terminal_.character(character, modifiers);
    } else if (!io.KeySuper) {
        for (auto codepoint : io.InputQueueCharacters) {
            if (codepoint >= 32 && codepoint != 127) {
                terminal_.character(codepoint, io.KeyAlt ? VTERM_MOD_ALT : VTERM_MOD_NONE); scroll_ = 0;
            }
        }
    }
}

void App::refresh_local(const std::filesystem::path& path) {
    if (!path.empty()) local_requested_path_ = path;
    else if (!local_listing_) local_requested_path_ = local_path_;
    local_listing_id_ = local_directory_.request(local_requested_path_);
    local_listing_ = true;
    set(local_input_, path_text(local_requested_path_));
}
void App::refresh_remote(const std::string& path) {
    FileJob job{next_id_++, Operation::List, path};
    if (remote_.enqueue(job)) { listing_id_ = job.id; listing_ = true; }
    else error_ = "SFTP queue is full or disconnected";
}
void App::submit(FileJob job) {
    job.id = next_id_++;
    if (remote_.enqueue(job)) {
        if (job.operation == Operation::Upload || job.operation == Operation::Download) transfers_.emplace(job.id, Transfer{job});
    } else error_ = "SFTP queue is full or disconnected";
}
void App::files_view() {
    if (ImGui::BeginTable("File panes", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextRow(); ImGui::TableNextColumn(); file_pane(false);
        ImGui::TableNextColumn(); file_pane(true);
        ImGui::EndTable();
    }
}
void App::file_pane(bool remote) {
    ImGui::PushID(remote ? "remote" : "local");
    auto& entries = remote ? remote_entries_ : local_entries_;
    auto& selection = remote ? remote_selected_ : local_selected_;
    auto& path_input = remote ? remote_input_ : local_input_;
    ImGui::TextUnformatted(remote ? "REMOTE" : "LOCAL");
    ImGui::BeginDisabled(remote && (!files_connected_ || listing_));
    if (ImGui::ArrowButton("Parent", ImGuiDir_Up)) {
        if (remote) refresh_remote(remote_join(remote_path_, ".."));
        else { const auto parent = local_path_.parent_path(); if (!parent.empty()) refresh_local(parent); }
    }
    tooltip("Parent directory");
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) { if (remote) refresh_remote(remote_path_); else refresh_local(); }
    ImGui::SameLine();
    ImGui::BeginDisabled(!remote && local_listing_);
    if (ImGui::Button("New folder")) file_action(Operation::MakeDirectory, remote);
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##Path", path_input.data(), path_input.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (remote) refresh_remote(path_input.data());
        else refresh_local(utf8_path(path_input.data()));
    }
    ImGui::BeginDisabled(selection.empty() || (!remote && local_listing_));
    if (ImGui::Button("Rename")) file_action(Operation::Rename, remote);
    ImGui::SameLine();
    if (ImGui::Button("Delete")) file_action(Operation::Remove, remote);
    ImGui::SameLine();
    ImGui::BeginDisabled(!files_connected_ || listing_ || local_listing_);
    if (ImGui::ArrowButton("Transfer", remote ? ImGuiDir_Left : ImGuiDir_Right)) transfer(!remote);
    tooltip(remote ? "Download selected file" : "Upload selected file");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    const float height = std::max(50.0f, ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing());
    std::string navigate;
    if (ImGui::BeginTable("Entries", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable, ImVec2(0, height))) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 85);
        ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(entries.size()));
        while (clipper.Step()) for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
            const auto& entry = entries[static_cast<size_t>(index)];
            ImGui::PushID(index); ImGui::TableNextRow(); ImGui::TableNextColumn();
            const std::string label = (entry.directory ? "+ " : entry.symlink ? "@ " : "  ") + entry.name;
            if (ImGui::Selectable("##entry", selection == entry.name, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                selection = entry.name;
                if (ImGui::IsMouseDoubleClicked(0) && (entry.directory || (remote && entry.symlink))) navigate = entry.name;
            }
            ImGui::SameLine(); ImGui::TextUnformatted(label.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", entry.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(entry.directory ? "<DIR>" : size_text(entry.size).c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!navigate.empty() && (!remote || (files_connected_ && !listing_))) {
        if (remote) refresh_remote(remote_join(remote_path_, navigate));
        else refresh_local(local_path_ / utf8_path(navigate));
    }
    ImGui::TextDisabled((remote ? listing_ : local_listing_) ? "Loading..." : "%zu items", entries.size());
    ImGui::PopID();
}
void App::transfer(bool upload) {
    const auto& selected = upload ? local_selected_ : remote_selected_;
    const auto& entries = upload ? local_entries_ : remote_entries_;
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const FileEntry& entry) { return entry.name == selected; });
    if (found == entries.end()) return;
    if (!found->regular) { error_ = "Select a regular file; directories, symbolic links and special files cannot be transferred"; return; }
    if (!valid_filename(selected)) { error_ = "Unsafe destination filename"; return; }
#ifdef _WIN32
    if (selected.find_first_of(":<>\"|?*") != std::string::npos || selected.back() == '.' || selected.back() == ' ') {
        error_ = "Filename is not valid on Windows"; return;
    }
#endif
    submit({0, upload ? Operation::Upload : Operation::Download, remote_join(remote_path_, selected), {}, local_path_ / utf8_path(selected)});
}
void App::file_action(Operation operation, bool remote) {
    const auto& selected = remote ? remote_selected_ : local_selected_;
    const auto& entries = remote ? remote_entries_ : local_entries_;
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const FileEntry& entry) { return entry.name == selected; });
    action_ = {};
    action_.operation = operation;
    action_.remote = remote_join(remote_path_, selected);
    action_.local = local_path_ / utf8_path(selected);
    action_.directory = found != entries.end() && found->directory && !found->symlink;
    action_remote_ = remote; action_open_ = true;
    set(action_name_, operation == Operation::Rename ? selected : "");
}
void App::transfer_view() {
    ImGui::Separator();
    ImGui::TextUnformatted("TRANSFERS"); ImGui::SameLine();
    if (ImGui::SmallButton("Clear finished")) std::erase_if(transfers_, [](const auto& item) { return item.second.finished; });
    if (!ImGui::BeginTable("Transfers", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable, ImVec2(0, 0))) return;
    ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60);
    ImGui::TableHeadersRow();
    for (auto& [id, transfer] : transfers_) {
        ImGui::PushID(static_cast<int>(id)); ImGui::TableNextRow(); ImGui::TableNextColumn();
        ImGui::TextUnformatted(path_text(transfer.job.local.filename()).c_str());
        tooltip(transfer.job.operation == Operation::Upload ? "Upload" : "Download");
        ImGui::TableNextColumn();
        const float fraction = transfer.status == "Complete" ? 1 : transfer.total ? static_cast<float>(static_cast<double>(transfer.completed) / transfer.total) : 0;
        const auto progress = size_text(transfer.completed) + " / " + size_text(transfer.total);
        ImGui::ProgressBar(fraction, ImVec2(-1, 0), progress.c_str());
        ImGui::TableNextColumn();
        if (transfer.status == "Transferring") {
            const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - transfer.started).count();
            ImGui::Text("%s/s", size_text(static_cast<uint64_t>(transfer.completed / std::max(0.1, seconds))).c_str());
        } else { ImGui::TextUnformatted(transfer.status.c_str()); tooltip(transfer.status.c_str()); }
        ImGui::TableNextColumn();
        if (!transfer.finished && ImGui::SmallButton("Cancel")) {
            if (transfer.status == "Awaiting confirmation") { transfer.finished = true; transfer.status = "Cancelled"; }
            else { remote_.cancel(id); transfer.status = "Cancelling"; }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}
void App::dialogs() {
    if (!host_prompts_.empty() && !ImGui::IsPopupOpen("Trust host key")) ImGui::OpenPopup("Trust host key");
    modal_layout(640);
    if (ImGui::BeginPopupModal("Trust host key", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", host_prompts_.empty() ? "Connection cancelled" : host_prompts_.front().c_str());
        ImGui::TextWrapped("Verify this fingerprint with the server administrator before trusting it.");
        ImGui::BeginDisabled(host_prompts_.empty());
        if (ImGui::Button("Trust and save")) { remote_.trust(true); host_prompts_.pop_front(); ImGui::CloseCurrentPopup(); }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Reject")) { remote_.trust(false); host_prompts_.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    while (!overwrite_prompts_.empty()) {
        const auto found = transfers_.find(overwrite_prompts_.front());
        if (found == transfers_.end() || found->second.finished) overwrite_prompts_.pop_front();
        else break;
    }
    if (!overwrite_prompts_.empty() && !ImGui::IsPopupOpen("Replace file?")) ImGui::OpenPopup("Replace file?");
    modal_layout(540);
    if (ImGui::BeginPopupModal("Replace file?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (overwrite_prompts_.empty()) ImGui::CloseCurrentPopup();
        else {
            auto& transfer = transfers_.at(overwrite_prompts_.front());
            ImGui::TextWrapped("Replace %s?", transfer.job.operation == Operation::Upload ? transfer.job.remote.c_str() : path_text(transfer.job.local).c_str());
            if (ImGui::Button("Replace")) {
                transfer.job.overwrite = true;
                if (remote_.enqueue(transfer.job)) transfer.status = "Queued";
                else { transfer.status = "Disconnected or queue full"; transfer.finished = true; }
                overwrite_prompts_.pop_front(); ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Skip")) {
                transfer.status = "Skipped"; transfer.finished = true;
                overwrite_prompts_.pop_front(); ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    if (action_open_) { ImGui::OpenPopup("File operation"); action_open_ = false; }
    modal_layout(540);
    if (ImGui::BeginPopupModal("File operation", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const bool deleting = action_.operation == Operation::Remove;
        ImGui::TextWrapped("%s", action_remote_ ? action_.remote.c_str() : path_text(action_.local).c_str());
        if (deleting) ImGui::TextUnformatted("Permanently delete this item?");
        else ImGui::InputText("Name", action_name_.data(), action_name_.size());
        ImGui::BeginDisabled(!deleting && !valid_filename(action_name_.data()));
        if (ImGui::Button(deleting ? "Delete" : "Apply")) {
            try {
                if (action_remote_) {
                    if (action_.operation == Operation::MakeDirectory) action_.remote = remote_join(remote_path_, action_name_.data());
                    if (action_.operation == Operation::Rename) action_.destination = remote_join(remote_path_, action_name_.data());
                    submit(action_);
                } else {
                    const auto destination = local_path_ / utf8_path(action_name_.data());
                    if (deleting) std::filesystem::remove(action_.local);
                    else if (std::filesystem::exists(destination)) throw std::runtime_error("Destination already exists");
                    else if (action_.operation == Operation::MakeDirectory) std::filesystem::create_directory(destination);
                    else std::filesystem::rename(action_.local, destination);
                    refresh_local();
                }
                ImGui::CloseCurrentPopup();
            } catch (const std::exception& exception) { error_ = exception.what(); }
        }
        ImGui::EndDisabled(); ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
}