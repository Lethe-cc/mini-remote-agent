# Resolve GLFW for the macOS build without a package manager.
#
# `brew install glfw` provides the glfw3 CMake config package; without it the
# pinned upstream tarball is built with FetchContent. MRC_BUNDLED_GLFW=ON forces
# the bundled build. Provides the `glfw` target.
include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/PinnedSource.cmake)

option(MRC_BUNDLED_GLFW "Build the pinned GLFW source instead of a system package" OFF)
set(MRC_GLFW_VERSION "3.5.1" CACHE STRING "GLFW version used when bundling")
set(MRC_GLFW_SHA256 "d3f0153c48ad0c6fc0e20807c369765a186a1e8490d481e8f34c53b3e945bb9a"
  CACHE STRING "SHA-256 of the GLFW tarball (update together with MRC_GLFW_VERSION; empty disables verification)")

if(NOT MRC_BUNDLED_GLFW)
  find_package(glfw3 CONFIG QUIET)
  if(TARGET glfw)
    message(STATUS "GLFW: system CMake package glfw3")
  endif()
endif()

if(NOT TARGET glfw)
  message(STATUS "GLFW: building pinned ${MRC_GLFW_VERSION} source")
  mrc_dependency_url(MRC_GLFW_URL NAME glfw VERSION ${MRC_GLFW_VERSION}
    URL https://api.github.com/repos/glfw/glfw/tarball/${MRC_GLFW_VERSION}
    SHA256 ${MRC_GLFW_SHA256})
  mrc_dependency_hash(MRC_GLFW_HASH_ARGS NAME glfw SHA256 ${MRC_GLFW_SHA256})
  FetchContent_Declare(glfw URL ${MRC_GLFW_URL} ${MRC_GLFW_HASH_ARGS}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  # GLFW declares an old cmake_minimum_required, so these go through the cache.
  mrc_fetch_populate(glfw
    GLFW_BUILD_EXAMPLES=OFF GLFW_BUILD_TESTS=OFF GLFW_BUILD_DOCS=OFF
    GLFW_INSTALL=OFF BUILD_TESTING=OFF)
endif()
