# Resolve libvterm without a package manager.
#
# A system libvterm (brew install libvterm) is preferred; otherwise the pinned
# neovim/libvterm commit is built from source. MRC_BUNDLED_VTERM=ON skips the
# search. The tarball digest is checked, and a matching copy in
# MRC_LOCAL_DEPS_DIR is used instead of the network. Provides mrc_vterm.
include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/PinnedSource.cmake)

set(MRC_VTERM_VERSION "0.3.3" CACHE STRING "libvterm version used when bundling")
set(MRC_VTERM_SHA256 "582ac3f0e1337ea6f3edace67f8916e7627e7793b563bf94474f4ccdf6dfb0ee"
  CACHE STRING "SHA-256 of the libvterm tarball (pinned neovim/libvterm commit; empty disables verification)")

if(NOT MRC_BUNDLED_VTERM)
  find_path(VTERM_INCLUDE_DIR vterm.h)
  find_library(VTERM_LIBRARY NAMES vterm)
endif()

if(VTERM_INCLUDE_DIR AND VTERM_LIBRARY AND NOT MRC_BUNDLED_VTERM)
  message(STATUS "libvterm: system library ${VTERM_LIBRARY}")
  add_library(mrc_vterm INTERFACE)
  target_include_directories(mrc_vterm INTERFACE ${VTERM_INCLUDE_DIR})
  target_link_libraries(mrc_vterm INTERFACE ${VTERM_LIBRARY})
else()
  message(STATUS "libvterm: building pinned ${MRC_VTERM_VERSION} source")
  find_package(Perl REQUIRED)
  mrc_dependency_url(MRC_VTERM_URL NAME libvterm VERSION ${MRC_VTERM_VERSION}
    URL https://api.github.com/repos/neovim/libvterm/tarball/9d6d2112335080312ef8c36667fa717ded4f7daf
    SHA256 ${MRC_VTERM_SHA256})
  mrc_dependency_hash(MRC_VTERM_HASH_ARGS NAME libvterm SHA256 ${MRC_VTERM_SHA256})
  FetchContent_Declare(vterm URL ${MRC_VTERM_URL} ${MRC_VTERM_HASH_ARGS}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(vterm)
  file(GLOB encoding_tables "${vterm_SOURCE_DIR}/src/encoding/*.tbl")
  foreach(table IN LISTS encoding_tables)
    get_filename_component(name "${table}" NAME_WE)
    execute_process(COMMAND "${PERL_EXECUTABLE}" -CSD "${vterm_SOURCE_DIR}/tbl2inc_c.pl" "${table}"
      OUTPUT_FILE "${vterm_SOURCE_DIR}/src/encoding/${name}.inc"
      COMMAND_ERROR_IS_FATAL ANY)
  endforeach()
  file(GLOB vterm_sources CONFIGURE_DEPENDS "${vterm_SOURCE_DIR}/src/*.c")
  add_library(mrc_vterm STATIC ${vterm_sources})
  target_include_directories(mrc_vterm PUBLIC "${vterm_SOURCE_DIR}/include")
  set_target_properties(mrc_vterm PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
endif()
