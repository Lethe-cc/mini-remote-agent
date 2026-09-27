#pragma once

#include <vterm.h>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace mrc {
class Terminal {
public:
    Terminal(int rows = 24, int columns = 80);
    ~Terminal();
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;
    void feed(std::string_view bytes);
    void resize(int rows, int columns);
    void reset();
    void key(VTermKey key, VTermModifier modifiers = VTERM_MOD_NONE);
    void character(uint32_t codepoint, VTermModifier modifiers = VTERM_MOD_NONE);
    void paste(std::string_view text);
    std::string take_output();
    VTermScreenCell cell(int row, int column, int scroll = 0) const;
    VTermPos cursor() const;
    bool cursor_visible() const { return cursor_visible_; }
    int rows() const { return rows_; }
    int columns() const { return columns_; }
    int history_size() const { return static_cast<int>(history_.size()); }
    std::string visible_text(int scroll = 0) const;
    static std::string utf8(uint32_t codepoint);

private:
    static void output(const char* bytes, size_t length, void* user);
    static int push_line(int columns, const VTermScreenCell* cells, void* user);
    static int move_cursor(VTermPos, VTermPos, int visible, void* user);
    static int property(VTermProp property, VTermValue* value, void* user);
    VTerm* terminal_{};
    VTermScreen* screen_{};
    VTermScreenCallbacks callbacks_{};
    int rows_;
    int columns_;
    bool cursor_visible_{true};
    bool alternate_{};
    std::string output_;
    std::deque<std::vector<VTermScreenCell>> history_;
};
}