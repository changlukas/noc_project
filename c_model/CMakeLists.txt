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
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
)
# Windows MSVC compatibility
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

FetchContent_Declare(
  yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG        yaml-cpp-0.9.0
)
set(YAML_CPP_BUILD_TESTS  OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS  OFF CACHE BOOL "" FORCE)
set(YAML_CPP_FORMAT_SOURCE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(yaml-cpp)

enable_testing()
add_subdirectory(tests)
