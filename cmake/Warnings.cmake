# Warning flags for this project's own targets.
#
# They are applied per target on purpose. A directory-wide add_compile_options()
# also reaches the FetchContent subprojects added later (GLFW alone produced 88
# warnings under -Wall -Wextra -Wpedantic), which hides real findings in our own
# code behind third-party noise.
include_guard(GLOBAL)

# mrc_target_warnings(<target> ...)
function(mrc_target_warnings)
  foreach(target IN LISTS ARGN)
    if(NOT TARGET ${target})
      message(FATAL_ERROR "mrc_target_warnings: unknown target '${target}'")
    endif()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endforeach()
endfunction()
