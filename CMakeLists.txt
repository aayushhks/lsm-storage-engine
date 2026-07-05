cmake_minimum_required(VERSION 3.20)
project(lsm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# default to a debug build for a from-source engine; release is opt-in via
# -DCMAKE_BUILD_TYPE=Release. multi-config generators set the type per build.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "build type" FORCE)
endif()

option(LSM_BUILD_TESTS "build the test suite" ON)
option(LSM_BUILD_BENCH "build the benchmark cli" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Sanitizers)
lsm_apply_sanitizers()

# a single interface target carrying warnings-as-errors, linked by every
# first-party target so tests and benchmarks are held to the same bar as the
# engine itself.
add_library(lsm_warnings INTERFACE)
target_compile_options(lsm_warnings INTERFACE -Wall -Wextra -Werror)

add_subdirectory(src)

if(LSM_BUILD_TESTS)
  enable_testing()
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  add_subdirectory(tests)
endif()

if(LSM_BUILD_BENCH)
  add_subdirectory(bench)
endif()
