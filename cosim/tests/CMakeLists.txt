# cosim/tests — Stage 5b wire-level smoke tests (GoogleTest + ctest).
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
# CMAKE_CURRENT_LIST_DIR == .../cosim/tests, so ../../tests/scenarios/ is the
# repo-root tree.
set(SCENARIO_TREE_ABS "${CMAKE_CURRENT_LIST_DIR}/../../tests/scenarios/")

# -----------------------------------------------------------------------
# test_cosim_integration — run-all over kAllAxi4Scenarios, wb2axip SKIPs
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
# Bidirectional coordinate-variant generation for CosimIntegration.
# COSIM_BIDIR_SUBSET = scenarios that are BOTH wb2axip-runnable (single-beat,
# non-exclusive, no injection, max_outstanding_write<=1) AND read-bearing
# (reads_checked>0 satisfies tb_top's non-vacuous PASS guard). Write-only and
# wb2axip-blocked scenarios stay in the full kAllAxi4Scenarios sweep but SKIP.
# -----------------------------------------------------------------------
set(COSIM_BIDIR_SUBSET
    AX4-BAS-002_single_read_default_fill
    AX4-BAS-003_single_write_read_aligned
    AX4-BAS-004_conformity_write_read
    AX4-BAS-005_multi_id_single_beat_sequential
    AX4-ORD-001_multi_txn_same_id
    AX4-HSH-002_conformity_backpressure)

# The generator must run under an interpreter that has PyYAML (py -3 on Windows
# may not). Fail configure early if it is missing.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND ${Python3_EXECUTABLE} -c "import yaml"
                RESULT_VARIABLE _yaml_rc)
if(NOT _yaml_rc EQUAL 0)
    message(FATAL_ERROR
        "scenario generator needs PyYAML: pip install pyyaml for ${Python3_EXECUTABLE}")
endif()

set(COORD_VARIANT_ROOT "${CMAKE_CURRENT_BINARY_DIR}/coord_scenarios")
set(_coord_outputs "")
foreach(id IN LISTS COSIM_BIDIR_SUBSET)
    set(_n0 "${COORD_VARIANT_ROOT}/${id}/node0/scenario.yaml")
    set(_n1 "${COORD_VARIANT_ROOT}/${id}/node1/scenario.yaml")
    add_custom_command(
        OUTPUT "${_n0}" "${_n1}"
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_LIST_DIR}/../tools/gen_coordinate_scenarios.py
                "${SCENARIO_TREE_ABS}${id}/scenario.yaml"
                "${COORD_VARIANT_ROOT}/${id}"
        DEPENDS ${CMAKE_CURRENT_LIST_DIR}/../tools/gen_coordinate_scenarios.py
                "${SCENARIO_TREE_ABS}${id}/scenario.yaml"
        COMMENT "Generating coordinate variants for ${id}")
    list(APPEND _coord_outputs "${_n0}" "${_n1}")
endforeach()
add_custom_target(gen_coord_scenarios DEPENDS ${_coord_outputs})
add_dependencies(test_cosim_integration gen_coord_scenarios)
target_compile_definitions(test_cosim_integration PRIVATE
    COORD_VARIANT_ROOT="${COORD_VARIANT_ROOT}")

# -----------------------------------------------------------------------
# test_checker_fires_on_violation — injection liveness regression
# -----------------------------------------------------------------------
add_executable(test_checker_fires_on_violation test_checker_fires_on_violation.cpp)
target_link_libraries(test_checker_fires_on_violation PRIVATE GTest::gtest_main
    yaml-cpp::yaml-cpp noc_axi4_scenarios)
target_include_directories(test_checker_fires_on_violation PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
    "${CMAKE_CURRENT_LIST_DIR}/../../tests/scenarios")
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
# WORKING_DIRECTORY is the verilator BUILD dir so Vtb_top writes its
# in-project read-dump into build artifacts, not the source tree. Scenarios
# are anchored to YAML location, so cwd does not matter for data_file:
# resolution.
# -----------------------------------------------------------------------
add_test(NAME CosimIntegration COMMAND test_cosim_integration)
set_tests_properties(CosimIntegration PROPERTIES
    ENVIRONMENT "COSIM_BIN=${COSIM_BIN_PATH}"
    WORKING_DIRECTORY ${VERILATOR_BUILD_DIR})

add_test(NAME CheckerLiveness COMMAND test_checker_fires_on_violation)
set_tests_properties(CheckerLiveness PROPERTIES
    ENVIRONMENT "COSIM_BIN=${COSIM_BIN_PATH}"
    WORKING_DIRECTORY ${VERILATOR_BUILD_DIR})

add_test(NAME Wb2axipBlock COMMAND test_wb2axip_block)
