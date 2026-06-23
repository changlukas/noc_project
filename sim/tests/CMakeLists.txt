# sim/tests — Stage 5b wire-level smoke tests (GoogleTest + ctest).
# These tests invoke the Vtb_top binary as a subprocess via std::system().
# COSIM_BIN is injected via set_tests_properties ENVIRONMENT so ctest
# can locate the binary regardless of working directory.

cmake_minimum_required(VERSION 3.20)

# Derive the Vtb_top binary path: all build artifacts live under the
# top-level build/ tree (build/verilator/obj_dir).
set(VERILATOR_BUILD_DIR "${CMAKE_CURRENT_LIST_DIR}/../../build/verilator")
set(COSIM_BIN_PATH "${VERILATOR_BUILD_DIR}/obj_dir/Vtb_top.exe")
if(NOT EXISTS "${COSIM_BIN_PATH}")
    set(COSIM_BIN_PATH "${VERILATOR_BUILD_DIR}/obj_dir/Vtb_top")
endif()

# Stage B unified scenario tree. Scenarios live at repo-root tests/scenarios/.
# CMAKE_CURRENT_LIST_DIR == .../sim/tests, so ../../tests/scenarios/ is the
# repo-root tree.
set(SCENARIO_TREE_ABS "${CMAKE_CURRENT_LIST_DIR}/../../tests/scenarios/")

# -----------------------------------------------------------------------
# test_cosim_integration — run-all over kAllAxi4Scenarios
# -----------------------------------------------------------------------
add_executable(test_cosim_integration test_cosim_integration.cpp)
target_link_libraries(test_cosim_integration PRIVATE GTest::gtest_main yaml-cpp::yaml-cpp
    noc_axi4_scenarios)
target_include_directories(test_cosim_integration PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(test_cosim_integration PRIVATE
    SCENARIO_TREE_ROOT="${SCENARIO_TREE_ABS}")

# -----------------------------------------------------------------------
# Bidirectional coordinate variants for CosimIntegration.
# The per-node variants (node0 = identity, node1 = +0x1_0000_0000) are
# materialized + committed under each pattern dir in the scenario tree
# (tests/scenarios/<id>/node{0,1}/scenario.yaml), generated offline by
# sim/tools/gen_coordinate_scenarios.py. The test reads them directly from
# the source tree; only the bring-up subset (single-beat, non-exclusive,
# read-bearing patterns) has a node0/node1 tree, and ids without one SKIP.
# -----------------------------------------------------------------------
target_compile_definitions(test_cosim_integration PRIVATE
    COORD_VARIANT_ROOT="${SCENARIO_TREE_ABS}")

# -----------------------------------------------------------------------
# ctest registrations.
# WORKING_DIRECTORY is the verilator BUILD dir so Vtb_top writes its
# in-project read-dump into build artifacts, not the source tree. Scenarios
# are anchored to YAML location, so cwd does not matter for data_file:
# resolution.
# -----------------------------------------------------------------------
add_test(NAME CosimIntegration COMMAND test_cosim_integration)
set_tests_properties(CosimIntegration PROPERTIES
    ENVIRONMENT "COSIM_BIN=${COSIM_BIN_PATH}"
    WORKING_DIRECTORY ${VERILATOR_BUILD_DIR})
