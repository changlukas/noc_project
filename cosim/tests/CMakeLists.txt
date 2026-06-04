# Stage 5 co-sim smoke tests: GoogleTest wrapper that invokes the Verilator
# binary for each YAML scenario and checks for a zero exit code.
# This file is included from c_model/tests/CMakeLists.txt as an out-of-tree
# subdir, so CMAKE_SOURCE_DIR == c_model/ and NI_TESTS_ROOT_DIR is set.

add_executable(test_nmu_nsu_axi_conformity
    ${CMAKE_CURRENT_LIST_DIR}/test_nmu_nsu_axi_conformity.cpp)
target_link_libraries(test_nmu_nsu_axi_conformity PRIVATE gtest_main)
target_include_directories(test_nmu_nsu_axi_conformity PRIVATE
    ${NI_TESTS_ROOT_DIR})
add_dependencies(test_nmu_nsu_axi_conformity codegen_check)

# Stage the AXI fixture YAML files and config into the binary output directory
# so the test can reference them via relative paths (fixtures/<name>.yaml and
# config/port_params.yaml), matching how the standalone harness is invoked.
add_custom_command(TARGET test_nmu_nsu_axi_conformity POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/tests/axi/fixtures
        $<TARGET_FILE_DIR:test_nmu_nsu_axi_conformity>/fixtures
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/config
        $<TARGET_FILE_DIR:test_nmu_nsu_axi_conformity>/config)

# Absolute path to the Verilator-built binary.
# NOTE: this path is repo-local; if the repo is relocated, rebuild via
#   cd cosim/verilator && make
set(COSIM_BIN
    "${CMAKE_SOURCE_DIR}/../cosim/verilator/obj_dir/Vtb_axi_conformity.exe")

gtest_discover_tests(test_nmu_nsu_axi_conformity
    PROPERTIES
        ENVIRONMENT "COSIM_BIN=${COSIM_BIN}"
        WORKING_DIRECTORY $<TARGET_FILE_DIR:test_nmu_nsu_axi_conformity>)
