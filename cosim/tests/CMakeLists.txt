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

# Stage B unified scenario tree. Scenarios live at repo-root tests/scenarios/.
# CMAKE_CURRENT_LIST_DIR == .../cosim/tests, so ../../tests/scenarios/ is the
# repo-root tree.
set(SCENARIO_TREE_ABS "${CMAKE_CURRENT_LIST_DIR}/../../tests/scenarios/")

# -----------------------------------------------------------------------
# test_cosim_wire_smoke — parameterized over 4 conformity scenarios
# -----------------------------------------------------------------------
add_executable(test_cosim_wire_smoke test_cosim_wire_smoke.cpp)
target_link_libraries(test_cosim_wire_smoke PRIVATE GTest::gtest_main)
target_include_directories(test_cosim_wire_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests)
target_compile_definitions(test_cosim_wire_smoke PRIVATE
    SCENARIO_TREE_ROOT="${SCENARIO_TREE_ABS}")

# -----------------------------------------------------------------------
# test_checker_fires_on_violation — injection liveness regression
# -----------------------------------------------------------------------
add_executable(test_checker_fires_on_violation test_checker_fires_on_violation.cpp)
target_link_libraries(test_checker_fires_on_violation PRIVATE GTest::gtest_main)
target_include_directories(test_checker_fires_on_violation PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests)
target_compile_definitions(test_checker_fires_on_violation PRIVATE
    SCENARIO_TREE_ROOT="${SCENARIO_TREE_ABS}")

# -----------------------------------------------------------------------
# test_wb2axip_block — unit tests for wb2axip_block_reason predicate
# Pure C++ logic unit test; no SCENARIO_TREE_ROOT or COSIM_BIN needed.
# -----------------------------------------------------------------------
add_executable(test_wb2axip_block test_wb2axip_block.cpp)
target_link_libraries(test_wb2axip_block PRIVATE GTest::gtest_main yaml-cpp::yaml-cpp)
target_include_directories(test_wb2axip_block PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR})

# -----------------------------------------------------------------------
# ctest registrations.
# WORKING_DIRECTORY stays COSIM_VERILATOR_DIR so Vtb_top can still write its
# in-project read-dump there. Scenarios are anchored to YAML location now,
# so cwd no longer matters for data_file: resolution.
# -----------------------------------------------------------------------
add_test(NAME CosimWireSmoke COMMAND test_cosim_wire_smoke)
set_tests_properties(CosimWireSmoke PROPERTIES
    ENVIRONMENT "COSIM_BIN=${COSIM_BIN_PATH}"
    WORKING_DIRECTORY ${COSIM_VERILATOR_DIR})

add_test(NAME CheckerLiveness COMMAND test_checker_fires_on_violation)
set_tests_properties(CheckerLiveness PROPERTIES
    ENVIRONMENT "COSIM_BIN=${COSIM_BIN_PATH}"
    WORKING_DIRECTORY ${COSIM_VERILATOR_DIR})

add_test(NAME Wb2axipBlock COMMAND test_wb2axip_block)
