#include "terminal.hpp"
#include <cstdlib>
#include <iostream>

void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
    mrc::Terminal terminal(4, 20);
    terminal.feed("hello\r\n\x1b[31mred\x1b[0m");
    require(terminal.cell(0, 0).chars[0] == 'h', "text rendering");
    require(terminal.cell(1, 0).fg.rgb.red > terminal.cell(1, 0).fg.rgb.green, "ANSI color");
    terminal.feed("\x1b[?1049h\x1b[2Jalternate");
    terminal.feed("\x1b[?1049l");
    require(terminal.cell(0, 0).chars[0] == 'h', "alternate screen restores primary");
    terminal.feed("\x1b[3;1H\xe4\xb8\xad");
    require(terminal.cell(2, 0).chars[0] == 0x4e2d, "UTF-8 decoding");
    require(terminal.cell(2, 0).width == 2, "wide character");
    terminal.key(VTERM_KEY_UP);
    require(terminal.take_output() == "\x1b[A", "arrow encoding");
    terminal.character('c', VTERM_MOD_CTRL);
    require(terminal.take_output() == "\x03", "control encoding");
    terminal.feed("\x1b[?2004h");
    terminal.paste("text");
    require(terminal.take_output() == "\x1b[200~text\x1b[201~", "bracketed paste");
    terminal.feed("\r\n1\r\n2\r\n3\r\n4\r\n5");
    require(terminal.history_size() > 0, "scrollback");
    require(!terminal.visible_text(terminal.history_size()).empty(), "history rendering");
    terminal.resize(10, 40);
    require(terminal.rows() == 10 && terminal.columns() == 40, "resize");
    terminal.reset();
    require(terminal.history_size() == 0, "reset clears history");
    std::cout << "Terminal tests passed\n";
}