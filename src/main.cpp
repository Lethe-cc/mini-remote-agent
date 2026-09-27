#include "app.hpp"
#include <GLFW/glfw3.h>
#if __has_include(<backends/imgui_impl_glfw.h>)
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#else
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#endif
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
void style() {
    ImGui::StyleColorsLight();
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 0;
    style.ChildRounding = 0;
    style.FrameRounding = 3;
    style.PopupRounding = 4;
    style.GrabRounding = 2;
    style.WindowPadding = ImVec2(12, 10);
    style.FramePadding = ImVec2(8, 5);
    style.ItemSpacing = ImVec2(8, 7);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.94f, 0.96f, 0.95f, 1);
    style.Colors[ImGuiCol_Button] = ImVec4(0.77f, 0.85f, 0.82f, 1);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.60f, 0.77f, 0.70f, 1);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.42f, 0.65f, 0.56f, 1);
    style.Colors[ImGuiCol_Header] = ImVec4(0.65f, 0.82f, 0.75f, 1);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.71f, 0.85f, 0.79f, 1);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.51f, 0.72f, 0.64f, 1);
    style.Colors[ImGuiCol_Tab] = ImVec4(0.80f, 0.86f, 0.83f, 1);
    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.62f, 0.79f, 0.72f, 1);
    style.Colors[ImGuiCol_TabActive] = ImVec4(0.68f, 0.82f, 0.75f, 1);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.08f, 0.42f, 0.30f, 1);
    style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.76f, 0.39f, 0.24f, 1);
}
}
int main(int argc, char** argv) {
    const bool smoke = argc > 1 && std::string_view(argv[1]) == "--smoke-test";
    glfwSetErrorCallback([](int code, const char* message) { std::cerr << "GLFW " << code << ": " << message << '\n'; });
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    GLFWwindow* window = glfwCreateWindow(1180, 780, "Mini Remote Client", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwSetWindowSizeLimits(window, 640, 480, GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    auto* font = mrc::load_terminal_font();
    style();
    if (!ImGui_ImplGlfw_InitForOpenGL(window, true) || !ImGui_ImplOpenGL3_Init("#version 150")) return 1;
    int result = 0;
    try {
        mrc::App app(font);
        int frames = 0;
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
            app.draw();
            ImGui::Render();
            int width, height;
            glfwGetFramebufferSize(window, &width, &height);
            glViewport(0, 0, width, height);
            glClearColor(0.94f, 0.96f, 0.95f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
            if (smoke && ++frames >= 5) break;
            if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) glfwWaitEventsTimeout(0.1);
        }
    } catch (const std::exception& exception) { std::cerr << exception.what() << '\n'; result = 1; }
    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplGlfw_Shutdown(); ImGui::DestroyContext();
    glfwDestroyWindow(window); glfwTerminate();
    return result;
}