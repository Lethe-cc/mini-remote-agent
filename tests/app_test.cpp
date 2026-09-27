#include "app.hpp"
#include <imgui_internal.h>
#include <GLFW/glfw3.h>
#if __has_include(<backends/imgui_impl_opengl3.h>)
#include <backends/imgui_impl_opengl3.h>
#else
#include <imgui_impl_opengl3.h>
#endif
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>

void test_local_directory() {
    struct Fixture {
        std::filesystem::path path = std::filesystem::temp_directory_path() / ("mrc-directory-" + std::to_string(std::random_device{}()));
        ~Fixture() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    } fixture;
    std::filesystem::create_directories(fixture.path / "z-directory");
    std::filesystem::create_directory(fixture.path / "a-directory");
    { std::ofstream file(fixture.path / "z-file"); file << "123"; }
    { std::ofstream file(fixture.path / "a-file"); }
    mrc::LocalDirectory directory;
    auto wait = [&](uint64_t id) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (auto result = directory.poll()) {
                if (result->id != id) throw std::runtime_error("Stale directory result delivered");
                return std::move(*result);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        throw std::runtime_error("Local directory scan timed out");
    };
    const auto result = wait(directory.request(fixture.path));
    if (!result.error.empty() || result.path != std::filesystem::canonical(fixture.path) || result.entries.size() != 4)
        throw std::runtime_error("Local directory scan failed");
    const auto& entries = result.entries;
    if (entries[0].name != "a-directory" || !entries[0].directory || entries[0].regular ||
        entries[1].name != "z-directory" || entries[2].name != "a-file" || !entries[2].regular ||
        entries[3].name != "z-file" || entries[3].size != 3)
        throw std::runtime_error("Local directory ordering or file types incorrect");
    if (wait(directory.request(fixture.path / "missing")).error.empty())
        throw std::runtime_error("Invalid directory accepted");
    if (wait(directory.request(fixture.path / "a-file")).error.empty())
        throw std::runtime_error("Regular file accepted as directory");
    for (int index = 0; index < 20; ++index) directory.request(fixture.path / "missing");
    const auto latest = wait(directory.request(fixture.path / "a-directory"));
    if (!latest.error.empty() || !latest.entries.empty() || latest.path.filename() != "a-directory")
        throw std::runtime_error("Latest directory request was not used");
    directory.request(fixture.path);
}

void test_identity_cleared_on_disconnect(ImFont* font) {
    struct Fixture {
        std::filesystem::path dir = std::filesystem::temp_directory_path() / ("mrc-profile-" + std::to_string(std::random_device{}()));
        std::string previous{[] { const char* value = std::getenv("XDG_CONFIG_HOME"); return value ? std::string(value) : std::string(); }()};
        Fixture() {
            std::filesystem::create_directories(dir / "mini-remote-client");
            std::ofstream file(dir / "mini-remote-client" / "connection.txt");
            file << std::quoted(std::string("127.0.0.1")) << '\n' << 1 << '\n'
                 << std::quoted(std::string("tester")) << '\n' << 0 << '\n'
                 << std::quoted(std::string()) << '\n';
            setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
        }
        ~Fixture() {
            if (previous.empty()) unsetenv("XDG_CONFIG_HOME");
            else setenv("XDG_CONFIG_HOME", previous.c_str(), 1);
            std::error_code ignored; std::filesystem::remove_all(dir, ignored);
        }
    } fixture;
    mrc::App app(font);
    auto frame = [&] { ImGui::NewFrame(); app.draw(); ImGui::Render(); };
    frame();
    if (app.host() != "127.0.0.1" || app.username() != "tester")
        throw std::runtime_error("Saved profile did not restore the connection fields");
    if (!app.connect()) throw std::runtime_error("Connection request was rejected");
    // Nothing listens on port 1, so both workers fail and close the session.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline && (!app.host().empty() || !app.username().empty())) {
        frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!app.host().empty() || !app.username().empty())
        throw std::runtime_error("Disconnect kept the previous host or username");
    if (app.connected()) throw std::runtime_error("Failed connection reported as connected");
    app.disconnect();
    frame();
    // The screen of the session that just ended is gone, not merely scrolled away.
    // An empty terminal renders as one blank line per row, so check for printable
    // content rather than an empty string.
    const auto text = app.terminal_text();
    if (text.find_first_not_of(" \n\t") != std::string::npos)
        throw std::runtime_error("Disconnect left remote terminal output on screen");
    frame();
    frame();
    // The settings dialog dismisses itself once the session is closed, so the user
    // never has to clear the fields or close the window by hand.
    if (auto& context = *ImGui::GetCurrentContext(); context.OpenPopupStack.Size != 0)
        throw std::runtime_error("Connection dialog stayed open after disconnect");
    frame();
    if (ImGui::GetDrawData()->TotalVtxCount == 0) throw std::runtime_error("Disconnected view is blank");
    // The cleared identity must not come back on a later start either.
    {
        const mrc::App restarted(font);
        if (!restarted.host().empty() || !restarted.username().empty())
            throw std::runtime_error("Reloading the profile restored the previous host or username");
    }
    std::cout << "Disconnect clears the entered host and username\n";
}

void test_terminal_font_covers_cjk() {
    // Runs against its own context so the shared font atlas of the UI tests stays put.
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImFont* font = mrc::load_terminal_font();
    io.Fonts->Build();
    bool han = false, boxes = false;
    if (font) {
        if (ImFontBaked* baked = font->GetFontBaked(17.f)) {
            auto covers = [&](unsigned cp) { return baked->FindGlyphNoFallback((ImWchar)cp) != nullptr; };
            han = covers(0x4e2d) && covers(0x56fd);            // 中国
            boxes = covers(0x251c) && covers(0x2500) && covers(0x03bb); // ├ ─ λ
        }
    }
    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
    if (!font) throw std::runtime_error("Terminal font failed to load");
    if (!han) throw std::runtime_error("Terminal font has no Han glyphs; Chinese output renders as fallback boxes");
    if (!boxes) throw std::runtime_error("Terminal font cannot draw box-drawing or Greek cells");
    std::cout << "Terminal font covers Han and box-drawing glyphs\n";
}

void test_terminal_font_ranges_cover_prompt_icons() {
    // starship and powerlevel10k draw their icons from the private use area, so the
    // ranges must include it or the whole prompt turns into question marks.
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    bool pua = false, technical = false, dingbats = false;
    for (const ImWchar* range = mrc::terminal_glyph_ranges(io.Fonts); range && range[0]; range += 2) {
        if (range[0] <= 0xE0A0 && 0xE0A0 <= range[1]) pua = true;          // powerline branch icon
        if (range[0] <= 0x23F1 && 0x23F1 <= range[1]) technical = true;    // ⏱
        if (range[0] <= 0x2714 && 0x2714 <= range[1]) dingbats = true;     // ✔
    }
    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
    if (!pua) throw std::runtime_error("Private use area missing from the terminal glyph ranges");
    if (!technical) throw std::runtime_error("Misc technical block missing from the terminal glyph ranges");
    if (!dingbats) throw std::runtime_error("Dingbats block missing from the terminal glyph ranges");
    std::cout << "Terminal glyph ranges cover private-use-area and symbol blocks\n";
}

void test_icon_font_merge() {
    // Only meaningful where a Nerd Font or Powerline font is actually installed.
    std::filesystem::path icon;
    if (const char* home = std::getenv("HOME")) {
        const auto directory = std::filesystem::path(home) / "Library" / "Fonts";
        std::error_code ignored;
        for (const auto& entry : std::filesystem::directory_iterator(directory, ignored)) {
            const auto name = entry.path().filename().string();
            if (name.find("Nerd") == std::string::npos && name.find("Powerline") == std::string::npos) continue;
            icon = entry.path();
            break;
        }
    }
    if (icon.empty()) {
        std::cout << "No icon font installed; skipping private-use-area coverage\n";
        return;
    }
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    setenv("MRC_ICON_FONT", icon.string().c_str(), 1);
    ImFont* font = mrc::load_terminal_font();
    io.Fonts->Build();
    bool branch = false, latin = false;
    if (font) {
        if (ImFontBaked* baked = font->GetFontBaked(17.f)) {
            branch = baked->FindGlyphNoFallback((ImWchar)0xE0A0) != nullptr;
            latin = baked->FindGlyphNoFallback((ImWchar)'M') != nullptr;
        }
    }
    unsetenv("MRC_ICON_FONT");
    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
    if (!branch)
        throw std::runtime_error("Merged icon font did not provide the powerline branch glyph");
    if (!latin)
        throw std::runtime_error("Merging the icon font lost the primary monospace face");
    std::cout << "Icon font merge covers private-use-area glyphs (" << icon.filename().string() << ")\n";
}

int main(int argc, char** argv) {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1180, 780);
    io.DeltaTime = 1.0f / 60;
    auto* font = io.Fonts->AddFontDefault();
    io.Fonts->Build();
    int result = 0;
    try {
        test_local_directory();
        test_terminal_font_covers_cjk();
        test_terminal_font_ranges_cover_prompt_icons();
        test_icon_font_merge();
        mrc::App app(font);
        auto frame = [&] { ImGui::NewFrame(); app.draw(); ImGui::Render(); };
        frame();
        ImGui::ClosePopupToLevel(0, true);
        frame();
        auto& context = *ImGui::GetCurrentContext();
        if (context.TabBars.GetAliveCount() == 0) throw std::runtime_error("Missing workspace tabs");
        auto& bar = *context.TabBars.GetByIndex(0);
        if (bar.Tabs.Size != 2) throw std::runtime_error("Expected terminal and file tabs");
        const auto expected = bar.Tabs[1].ID;
        const ImVec2 target{bar.BarRect.Min.x + bar.Tabs[1].Offset + bar.Tabs[1].Width / 2,
                            (bar.BarRect.Min.y + bar.BarRect.Max.y) / 2};
        io.AddMousePosEvent(target.x, target.y);
        frame();
        io.AddMouseButtonEvent(0, true);
        frame();
        io.AddMouseButtonEvent(0, false);
        frame();
        frame();
        if (bar.SelectedTabId != expected) throw std::runtime_error("File tab did not activate");
        io.DisplaySize = ImVec2(640, 480);
        frame();
        if (ImGui::GetDrawData()->TotalVtxCount == 0) throw std::runtime_error("File page is blank");
        test_identity_cleared_on_disconnect(font);
        if (argc > 1) {
            if (!glfwInit()) throw std::runtime_error("GLFW initialization failed");
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
            glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
            glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
            auto* window = glfwCreateWindow(640, 480, "File page test", nullptr, nullptr);
            if (!window) throw std::runtime_error("Test window creation failed");
            glfwMakeContextCurrent(window);
            ImGui_ImplOpenGL3_Init("#version 150");
            ImGui_ImplOpenGL3_NewFrame();
            frame();
            int width, height;
            glfwGetFramebufferSize(window, &width, &height);
            glViewport(0, 0, width, height);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 3);
            glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
            std::ofstream image(argv[1], std::ios::binary);
            image << "P6\n" << width << ' ' << height << "\n255\n";
            for (int row = height - 1; row >= 0; --row)
                image.write(reinterpret_cast<const char*>(pixels.data() + static_cast<size_t>(row) * width * 3), width * 3);
            if (!image) throw std::runtime_error("Screenshot write failed");
            ImGui_ImplOpenGL3_Shutdown();
            glfwDestroyWindow(window);
            glfwTerminate();
        }
        std::cout << "File tab interaction and small-window rendering passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    ImGui::DestroyContext();
    return result;
}