add_cmodel_test(test_scaffold)
add_cmodel_test(test_memory)
add_cmodel_test(test_axi_slave)
add_cmodel_test(test_axi_master)
target_link_libraries(test_axi_master PRIVATE yaml-cpp::yaml-cpp)
add_cmodel_test(test_axi_master_inject)
target_link_libraries(test_axi_master_inject PRIVATE yaml-cpp::yaml-cpp)
add_cmodel_test(test_scoreboard)
target_link_libraries(test_scoreboard PRIVATE yaml-cpp::yaml-cpp)
add_cmodel_test(test_integration)
target_link_libraries(test_integration PRIVATE yaml-cpp::yaml-cpp noc_axi4_scenarios)
target_include_directories(test_integration PRIVATE
    "${CMAKE_SOURCE_DIR}/../sim/test_patterns")
add_cmodel_test(test_axi_master_raw_order)
target_link_libraries(test_axi_master_raw_order PRIVATE yaml-cpp::yaml-cpp)
add_cmodel_test(test_protocol_rules)

# Stage B unified scenario tree. test_integration consumes scenarios via
# absolute path built from SCENARIO_TREE_ROOT; no per-binary copy is needed
# because scenario_parser resolves data_file: relative to the YAML's dir.
target_compile_definitions(test_integration PRIVATE
  SCENARIO_TREE_ROOT="${CMAKE_SOURCE_DIR}/../sim/test_patterns/")

# test_scenario_metadata — unit tests for metadata block parsing + strict mode.
add_cmodel_test(test_scenario_metadata)
target_link_libraries(test_scenario_metadata PRIVATE yaml-cpp::yaml-cpp noc_axi4_scenarios)
target_include_directories(test_scenario_metadata PRIVATE
    "${CMAKE_SOURCE_DIR}/../sim/test_patterns")
