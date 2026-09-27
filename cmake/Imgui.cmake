# Resolve Dear ImGui for the macOS build without a package manager.
#
# Homebrew has no imgui formula, so the default path builds the pinned upstream
# tarball with FetchContent. An already installed CMake config package is used
# when present; MRC_BUNDLED_IMGUI=ON skips the search entirely.
# Provides the imgui::imgui target (core library + GLFW and OpenGL3 backends).
include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/PinnedSource.cmake)

option(MRC_BUNDLED_IMGUI "Build the pinned Dear ImGui source instead of a system package" OFF)
set(MRC_IMGUI_VERSION "1.92.9" CACHE STRING "Dear ImGui version used when bundling")
set(MRC_IMGUI_SHA256 "79a518e964ec838c24200cd883b8b418fd18a26f6c1dcac6e6d287f56a800bea"
  CACHE STRING "SHA-256 of the Dear ImGui tarball (update together with MRC_IMGUI_VERSION; empty disables verification)")

if(NOT MRC_BUNDLED_IMGUI)
  find_package(imgui CONFIG QUIET)
  if(TARGET imgui::imgui)
    message(STATUS "Dear ImGui: system CMake package")
  endif()
endif()

if(NOT TARGET imgui::imgui)
  message(STATUS "Dear ImGui: building pinned ${MRC_IMGUI_VERSION} source")
  mrc_dependency_url(MRC_IMGUI_URL NAME imgui VERSION ${MRC_IMGUI_VERSION}
    URL https://api.github.com/repos/ocornut/imgui/tarball/v${MRC_IMGUI_VERSION}
    SHA256 ${MRC_IMGUI_SHA256})
  mrc_dependency_hash(MRC_IMGUI_HASH_ARGS NAME imgui SHA256 ${MRC_IMGUI_SHA256})
  FetchContent_Declare(imgui URL ${MRC_IMGUI_URL} ${MRC_IMGUI_HASH_ARGS}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(imgui)

  # Core library only; the GLFW and OpenGL3 backends are compiled alongside it,
  # which is what the imgui[glfw-binding,opengl3-binding] features provide.
  add_library(mrc_imgui_core STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp)
  target_include_directories(mrc_imgui_core PUBLIC ${imgui_SOURCE_DIR})
  target_compile_definitions(mrc_imgui_core PUBLIC IMGUI_USE_WCHAR32)

  add_library(mrc_imgui_backends STATIC
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
  target_include_directories(mrc_imgui_backends PUBLIC ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(mrc_imgui_backends PUBLIC mrc_imgui_core glfw OpenGL::GL)

  add_library(imgui::imgui ALIAS mrc_imgui_backends)
endif()
