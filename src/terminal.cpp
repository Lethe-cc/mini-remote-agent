#include "terminal.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace mrc {
Terminal::Terminal(int rows, int columns) : rows_(rows), columns_(columns) {
    terminal_ = vterm_new(rows, columns);
    if (!terminal_) throw std::runtime_error("Cannot allocate terminal");
    vterm_set_utf8(terminal_, 1);
    vterm_output_set_callback(terminal_, output, this);
    screen_ = vterm_obtain_screen(terminal_);
    callbacks_.sb_pushline = push_line;
    callbacks_.movecursor = move_cursor;
    callbacks_.settermprop = property;
    vterm_screen_set_callbacks(screen_, &callbacks_, this);
    vterm_screen_enable_altscreen(screen_, 1);
    VTermColor foreground{}, background{};
    vterm_color_rgb(&foreground, 222, 229, 224);
    vterm_color_rgb(&background, 24, 28, 27);
    vterm_state_set_default_colors(vterm_obtain_state(terminal_), &foreground, &background);
    reset();
}
Terminal::~Terminal() { vterm_free(terminal_); }
void Terminal::reset() {
    history_.clear();
    output_.clear();
    alternate_ = false;
    cursor_visible_ = true;
    vterm_screen_reset(screen_, 1);
}
void Terminal::feed(std::string_view bytes) {
    vterm_input_write(terminal_, bytes.data(), bytes.size());
    vterm_screen_flush_damage(screen_);
}
void Terminal::resize(int rows, int columns) {
    rows_ = std::clamp(rows, 2, 300);
    columns_ = std::clamp(columns, 2, 500);
    vterm_set_size(terminal_, rows_, columns_);
    vterm_screen_flush_damage(screen_);
}
void Terminal::key(VTermKey key, VTermModifier modifiers) { vterm_keyboard_key(terminal_, key, modifiers); }
void Terminal::character(uint32_t codepoint, VTermModifier modifiers) {
    vterm_keyboard_unichar(terminal_, codepoint, modifiers);
}
void Terminal::paste(std::string_view text) {
    vterm_keyboard_start_paste(terminal_);
    for (char byte : text) {
        if (byte != '\x1b' && byte != '\0') output_.push_back(byte);
    }
    vterm_keyboard_end_paste(terminal_);
}
std::string Terminal::take_output() { return std::exchange(output_, {}); }
void Terminal::output(const char* bytes, size_t length, void* user) {
    static_cast<Terminal*>(user)->output_.append(bytes, length);
}
int Terminal::push_line(int columns, const VTermScreenCell* cells, void* user) {
    auto& self = *static_cast<Terminal*>(user);
    if (!self.alternate_) {
        self.history_.emplace_back(cells, cells + columns);
        if (self.history_.size() > 5000) self.history_.pop_front();
    }
    return 1;
}
int Terminal::move_cursor(VTermPos, VTermPos, int visible, void* user) {
    static_cast<Terminal*>(user)->cursor_visible_ = visible != 0;
    return 1;
}
int Terminal::property(VTermProp property, VTermValue* value, void* user) {
    auto& self = *static_cast<Terminal*>(user);
    if (property == VTERM_PROP_CURSORVISIBLE) self.cursor_visible_ = value->boolean != 0;
    if (property == VTERM_PROP_ALTSCREEN) self.alternate_ = value->boolean != 0;
    return 1;
}
VTermScreenCell Terminal::cell(int row, int column, int scroll) const {
    VTermScreenCell result{};
    auto* state = vterm_obtain_state(terminal_);
    vterm_state_get_default_colors(state, &result.fg, &result.bg);
    result.width = 1;
    scroll = alternate_ ? 0 : std::clamp(scroll, 0, history_size());
    const int source = row - scroll;
    if (source < 0) {
        const auto& line = history_[static_cast<size_t>(history_size() + source)];
        if (column < static_cast<int>(line.size())) result = line[static_cast<size_t>(column)];
    } else if (source < rows_ && column < columns_) {
        vterm_screen_get_cell(screen_, {source, column}, &result);
    }
    vterm_state_convert_color_to_rgb(state, &result.fg);
    vterm_state_convert_color_to_rgb(state, &result.bg);
    if (result.attrs.reverse) std::swap(result.fg, result.bg);
    return result;
}
VTermPos Terminal::cursor() const {
    VTermPos position{};
    vterm_state_get_cursorpos(vterm_obtain_state(terminal_), &position);
    return position;
}
std::string Terminal::utf8(uint32_t codepoint) {
    std::string result;
    if (codepoint <= 0x7f) result += static_cast<char>(codepoint);
    else if (codepoint <= 0x7ff) {
        result += static_cast<char>(0xc0 | (codepoint >> 6));
        result += static_cast<char>(0x80 | (codepoint & 0x3f));
    } else if (codepoint <= 0xffff) {
        result += static_cast<char>(0xe0 | (codepoint >> 12));
        result += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        result += static_cast<char>(0x80 | (codepoint & 0x3f));
    } else if (codepoint <= 0x10ffff) {
        result += static_cast<char>(0xf0 | (codepoint >> 18));
        result += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
        result += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        result += static_cast<char>(0x80 | (codepoint & 0x3f));
    }
    return result;
}
std::string Terminal::visible_text(int scroll) const {
    std::string result;
    for (int row = 0; row < rows_; ++row) {
        std::string line;
        for (int column = 0; column < columns_; ++column) {
            const auto value = cell(row, column, scroll);
            if (value.chars[0] == UINT32_MAX) continue;
            if (!value.chars[0]) line += ' ';
            for (auto codepoint : value.chars) {
                if (!codepoint) break;
                line += utf8(codepoint);
            }
        }
        const auto end = line.find_last_not_of(' ');
        if (end != std::string::npos) result += line.substr(0, end + 1);
        result += '\n';
    }
    return result;
}
}