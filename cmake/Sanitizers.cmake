# sanitizers are selected at configure time and applied to the whole build, not
# per target: address and thread sanitizers cannot share a binary, and the flags
# must reach both the compile and link steps of every translation unit. so a
# sanitized run is a separate build directory configured with, e.g.,
#   cmake -S . -B build-asan -DLSM_SANITIZE=asan-ubsan
# rather than an extra target inside the normal build.

set(LSM_SANITIZE "off" CACHE STRING "sanitizer build: off, asan-ubsan, or tsan")
set_property(CACHE LSM_SANITIZE PROPERTY STRINGS off asan-ubsan tsan)

function(lsm_apply_sanitizers)
  if(LSM_SANITIZE STREQUAL "off")
    return()
  elseif(LSM_SANITIZE STREQUAL "asan-ubsan")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                        -fno-sanitize-recover=all -g -O1)
    add_link_options(-fsanitize=address,undefined)
  elseif(LSM_SANITIZE STREQUAL "tsan")
    # wired now; exercised from m6 when background compaction introduces threads.
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=thread)
  else()
    message(FATAL_ERROR "unknown LSM_SANITIZE='${LSM_SANITIZE}' (want off|asan-ubsan|tsan)")
  endif()
  message(STATUS "sanitizer build: ${LSM_SANITIZE}")
endfunction()
