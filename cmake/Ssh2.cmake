# Resolve libssh2 for the macOS build without a package manager.
#
# Search order: the libssh2 CMake config package, a Homebrew/system header and
# library pair, then the pinned upstream release tarball built with FetchContent
# against Homebrew OpenSSL. MRC_BUNDLED_SSH2=ON forces the bundled build.
# Sets MRC_SSH2_TARGET, plus MRC_SSH2_INCLUDE_DIR when a bare library was found.
include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/PinnedSource.cmake)

option(MRC_BUNDLED_SSH2 "Build the pinned libssh2 source instead of a system library" OFF)
set(MRC_SSH2_VERSION "1.11.1" CACHE STRING "libssh2 version used when bundling")
set(MRC_SSH2_SHA256 "6f10cea6f9c274cf1a31c68316c3904f037f37fb3233411fe34d1bbb89dc4456"
  CACHE STRING "SHA-256 of the libssh2 tarball (update together with MRC_SSH2_VERSION; empty disables verification)")
set(MRC_SSH2_MINIMUM "1.11.1" CACHE STRING "Minimum libssh2 version this project relies on")

if(NOT MRC_BUNDLED_SSH2)
  find_package(libssh2 CONFIG QUIET)
  if(TARGET Libssh2::libssh2)
    set(MRC_SSH2_TARGET Libssh2::libssh2)
    message(STATUS "libssh2: system CMake package ${libssh2_VERSION}")
  elseif(TARGET libssh2::libssh2)
    set(MRC_SSH2_TARGET libssh2::libssh2)
    message(STATUS "libssh2: system CMake package ${libssh2_VERSION}")
  else()
    find_path(SSH2_INCLUDE_DIR libssh2.h)
    find_library(SSH2_LIBRARY NAMES ssh2 libssh2)
    if(SSH2_INCLUDE_DIR AND SSH2_LIBRARY)
      set(MRC_SSH2_TARGET ${SSH2_LIBRARY})
      set(MRC_SSH2_INCLUDE_DIR ${SSH2_INCLUDE_DIR})
      message(STATUS "libssh2: system library ${SSH2_LIBRARY}")
      # Remote overwrite needs the posix-rename@openssh.com helper added in 1.11.1.
      set(_version "")
      set(_complete ON)
      foreach(_part MAJOR MINOR PATCH)
        file(STRINGS "${SSH2_INCLUDE_DIR}/libssh2.h" _line LIMIT_COUNT 1
          REGEX "^#[ \t]*define[ \t]+LIBSSH2_VERSION_${_part}[ \t]")
        string(REGEX REPLACE "^#[ \t]*define[ \t]+LIBSSH2_VERSION_${_part}[ \t]+([0-9]+).*" "\\1"
          _num "${_line}")
        if(_num STREQUAL _line)
          set(_complete OFF)
        elseif(_version)
          string(APPEND _version ".${_num}")
        else()
          set(_version "${_num}")
        endif()
      endforeach()
      if(_complete)
        message(STATUS "libssh2 header version ${_version}")
        if(_version VERSION_LESS MRC_SSH2_MINIMUM)
          message(WARNING "libssh2 ${_version} is older than ${MRC_SSH2_MINIMUM}: remote overwrite is refused instead of renamed")
        endif()
      endif()
    endif()
  endif()
endif()

if(NOT MRC_SSH2_TARGET)
  message(STATUS "libssh2: building pinned ${MRC_SSH2_VERSION} source")
  find_package(OpenSSL REQUIRED)
  mrc_dependency_url(MRC_SSH2_URL NAME libssh2 VERSION ${MRC_SSH2_VERSION}
    URL https://api.github.com/repos/libssh2/libssh2/tarball/libssh2-${MRC_SSH2_VERSION}
    SHA256 ${MRC_SSH2_SHA256})
  mrc_dependency_hash(MRC_SSH2_HASH_ARGS NAME libssh2 SHA256 ${MRC_SSH2_SHA256})
  FetchContent_Declare(libssh2 URL ${MRC_SSH2_URL} ${MRC_SSH2_HASH_ARGS}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  mrc_fetch_populate(libssh2
    BUILD_EXAMPLES=OFF BUILD_TESTING=OFF BUILD_SHARED_LIBS=OFF BUILD_STATIC_LIBS=ON
    ENABLE_DEBUG_LOGGING=OFF CRYPTO_BACKEND=OpenSSL)
  set(MRC_SSH2_TARGET libssh2::libssh2_static)
endif()
