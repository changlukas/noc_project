# cosim/tests — Stage 5b wire-level smoke tests (GoogleTest + ctest).
# These tests invoke the Vtb_top binary as a subprocess via std::system().
# COSIM_BIN is injected via set_tests_properties ENVIRONMENT so ctest
# can locate the binary regardless of working directory.

cmake_minimum_required(VERSION 3.20)

# Derive the Vtb_top binary path relative to cosim/ (parent of this directory).
set(COSIM_VERILATOR_DIR "${CMAKE_CURRENT_LIST_DIR}/../verilator")
set(COSIM_BIN_PATH "${COSIM_VERILATOR_DIR}/obj_dir/Vtb_top.exe")
if(NOT EXISTS "${COSIM_BIN_PATH}")
    set(COSIM_BIN_PATH "${COSIM_VERILATOR_DIR}/obj_dir/Vtb_top")
endif()

# -----------------------------------------------------------------------
# test_cosim_wire_smoke — parameterized over 4 conformity scenarios
# -----------------------------------------------------------------------
add_executable(test_cosim_wire_smoke test_cosim_wire_smoke.cpp)
target_link_libraries(test_cosim_wire_smoke PRIVATE GTest::gtest_main)
target_include_directories(test_cosim_wire_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests)

# -----------------------------------------------------------------------
# test_checker_fires_on_violation — injection liveness regression
# -----------------------------------------------------------------------
add_executable(test_checker_fires_on_violation test_checker_fires_on_violation.cpp)
target_link_libraries(test_checker_fires_on_violation PRIVATE GTest::gtest_main)
target_include_directories(test_checker_fires_on_violation PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests)

# -----------------------------------------------------------------------
# Copy fixtures next to both binaries so the test can find them.
# The binaries run from COSIM_VERILATOR_DIR (the normal Vtb_top CWD),
# and scenarios use paths relative to that directory.
# Copying fixtures to both the build dir AND the verilator dir ensures
# +scenario=fixtures/... resolves correctly from the verilator working dir.
# -----------------------------------------------------------------------
add_custom_command(TARGET test_cosim_wire_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_LIST_DIR}/fixtures
        $<TARGET_FILE_DIR:test_cosim_wire_smoke>/fixtures)

add_custom_command(TARGET test_checker_fires_on_violation POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_LIST_DIR}/fixtures
        $<TARGET_FILE_DIR:test_checker_fires_on_violation>/fixtures)

# -----------------------------------------------------------------------
# ctest registrations.
# WORKING_DIRECTORY is COSIM_VERILATOR_DIR so that +scenario=../tests/...
# paths in YAML data_file fields resolve correctly relative to the
# verilator binary location (same as manual smoke runs).
# -----------------------------------------------------------------------
add_test(NAME CosimWireSmoke COMMAND test_cosim_wire_smoke)
set_tests_properties(CosimWireSmoke PROPERTIES
    ENVIRONMENT "COSIM_BIN=${COSIM_BIN_PATH}"
    WORKING_DIRECTORY ${COSIM_VERILATOR_DIR})

add_test(NAME CheckerLiveness COMMAND test_checker_fires_on_violation)
set_tests_properties(CheckerLiveness PROPERTIES
    ENVIRONMENT "COSIM_BIN=${COSIM_BIN_PATH}"
    WORKING_DIRECTORY ${COSIM_VERILATOR_DIR})
