add_cmodel_test(test_memory)
add_cmodel_test(test_axi_slave)
add_cmodel_test(test_axi_master)
target_link_libraries(test_axi_master PRIVATE yaml-cpp::yaml-cpp)
add_cmodel_test(test_axi_master_inject)
target_link_libraries(test_axi_master_inject PRIVATE yaml-cpp::yaml-cpp)
add_cmodel_test(test_scoreboard)
target_link_libraries(test_scoreboard PRIVATE yaml-cpp::yaml-cpp)
add_cmodel_test(test_axi_master_raw_order)
target_link_libraries(test_axi_master_raw_order PRIVATE yaml-cpp::yaml-cpp)

add_cmodel_test(test_wire_slave_port)
target_link_libraries(test_wire_slave_port PRIVATE yaml-cpp::yaml-cpp)
add_cmodel_test(test_protocol_rules)

# test_scenario_metadata — unit tests for metadata block parsing + strict mode.
add_cmodel_test(test_scenario_metadata)
target_link_libraries(test_scenario_metadata PRIVATE yaml-cpp::yaml-cpp noc_axi4_scenarios)
target_include_directories(test_scenario_metadata PRIVATE
    "${CMAKE_SOURCE_DIR}/../../sim/test_patterns")
