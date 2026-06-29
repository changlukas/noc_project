cmake_minimum_required(VERSION 3.20)
project(c_model CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Locate elaborated headers from specgen
set(SPECGEN_CPP_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../specgen/generated/cpp")
if(NOT EXISTS "${SPECGEN_CPP_INCLUDE}/ni_flit_constants.h")
  message(FATAL_ERROR "specgen generated/cpp dir not found: ${SPECGEN_CPP_INCLUDE}")
endif()

# Drift gate: refuse to build if codegen output is stale.
# Use find_package(Python3) so CMake passes an absolute interpreter path to the
# custom command. Bypasses Windows py.exe launcher ambiguity where the same
# `py -3` invocation from cmd.exe vs bash vs PowerShell can resolve to
# different Python installs (and only one of them has the required PyYAML).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_target(codegen_check
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../specgen/tools/codegen.py --check
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../specgen
  COMMENT "Verifying codegen artifacts are not stale"
)

include_directories(${SPECGEN_CPP_INCLUDE})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

include(FetchContent)

# Tests build by default. -DBUILD_TESTING=OFF gives a sim-only / offline
# configure. Declared before the guard below so the if() sees it. (option +
# enable_testing rather than include(CTest): the latter also adds unused CDash
# targets that would appear even at BUILD_TESTING=OFF.)
option(BUILD_TESTING "Build the c_model gtest tree" ON)
if(BUILD_TESTING)
  enable_testing()
endif()

# yaml-cpp: shared by sim and tests, fetched in both modes.
FetchContent_Declare(
  yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG        yaml-cpp-0.9.0
)
set(YAML_CPP_BUILD_TESTS  OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS  OFF CACHE BOOL "" FORCE)
set(YAML_CPP_FORMAT_SOURCE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(yaml-cpp)

# googletest: tests only. Skipped entirely when BUILD_TESTING=OFF, so a
# sim-only / offline configure neither fetches nor builds it.
if(BUILD_TESTING)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
  )
  # Windows MSVC compatibility
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

# GCC < 9 keeps std::filesystem in a separate library (libstdc++fs);
# scenario_parser.hpp and several tests use std::filesystem, so targets must
# link stdc++fs. GCC >= 9 folded it into libstdc++ (no flag needed) and other
# toolchains (Clang/libc++, MSVC, mingw GCC 9+) do not use this name. Restrict
# to old GCC. link_libraries() applies to every target defined below (tests +
# scenarios), but not to the FetchContent deps declared above.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
  link_libraries(stdc++fs)
  message(STATUS "GCC ${CMAKE_CXX_COMPILER_VERSION} < 9: linking stdc++fs for std::filesystem")
endif()

# Tests and their scenario-list INTERFACE library. Both serve only the gtest
# tree (noc_axi4_scenarios is consumed by tests/axi and tests/wrap, not by sim,
# which reads scenario YAML at runtime), so they sit under the guard. The
# scenarios subdir must come before tests/ so tests/axi and tests/wrap can link
# noc_axi4_scenarios.
if(BUILD_TESTING)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../sim/test_patterns
                   ${CMAKE_CURRENT_BINARY_DIR}/sim/test_patterns)
  add_subdirectory(tests)
endif()
