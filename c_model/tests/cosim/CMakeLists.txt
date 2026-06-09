add_cmodel_test(test_channel_model_shell_adapter)
# Add c_model/tests/ to include path so the adapter (and its ChannelModel
# dependency in tests/common/) resolves via "common/channel_model.hpp".
target_include_directories(test_channel_model_shell_adapter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_master_shell_adapter test_master_shell_adapter.cpp)
target_link_libraries(test_master_shell_adapter PRIVATE yaml-cpp::yaml-cpp)

add_cmodel_test(test_slave_shell_adapter test_slave_shell_adapter.cpp)

add_cmodel_test(test_nmu_shell_adapter test_nmu_shell_adapter.cpp)
target_link_libraries(test_nmu_shell_adapter PRIVATE yaml-cpp::yaml-cpp)

add_cmodel_test(test_nsu_shell_adapter test_nsu_shell_adapter.cpp)
target_link_libraries(test_nsu_shell_adapter PRIVATE yaml-cpp::yaml-cpp)

# test_cmodel_dpi — DPI session state-machine / lifecycle test.
# Excluded from the default build until Task 5 defines cmodel_channel_model_create;
# Task 5 removes the EXCLUDE_FROM_ALL property.
#
# Expanded inline (not via add_cmodel_test) so gtest_discover_tests can receive
# PROPERTIES ENVIRONMENT — the shared helper has no way to forward per-target env.
add_executable(test_cmodel_dpi test_cmodel_dpi.cpp)
target_link_libraries(test_cmodel_dpi PRIVATE gtest_main)
target_include_directories(test_cmodel_dpi PRIVATE
    ${NI_TESTS_ROOT_DIR})
add_dependencies(test_cmodel_dpi codegen_check)
target_sources(test_cmodel_dpi PRIVATE
    ${CMAKE_SOURCE_DIR}/../cosim/c/cmodel_dpi.cpp)
target_link_libraries(test_cmodel_dpi PRIVATE yaml-cpp::yaml-cpp)
target_include_directories(test_cmodel_dpi PRIVATE
    ${CMAKE_SOURCE_DIR}/../cosim/c
    $ENV{VERILATOR_ROOT}/include)
set_target_properties(test_cmodel_dpi PROPERTIES EXCLUDE_FROM_ALL TRUE)
# Pass scenario YAML env var to every discovered test case (e.g.
# CmodelDpiLifecycleTest.walk_session_state_machine). Task 5 removes EXCLUDE_FROM_ALL;
# verify with `ctest --test-dir c_model/build -N` afterwards.
gtest_discover_tests(test_cmodel_dpi
    PROPERTIES ENVIRONMENT
        "CMODEL_TEST_SCENARIO_YAML=${CMAKE_SOURCE_DIR}/../tests/scenarios/AX4-BAS-001_single_write_no_read/scenario.yaml")
