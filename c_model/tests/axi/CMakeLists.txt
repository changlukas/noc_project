add_cmodel_test(test_scaffold)
add_cmodel_test(test_memory)
add_cmodel_test(test_axi_slave)
add_cmodel_test(test_axi_master)
target_link_libraries(test_axi_master PRIVATE yaml-cpp::yaml-cpp)
add_cmodel_test(test_scoreboard)
target_link_libraries(test_scoreboard PRIVATE yaml-cpp::yaml-cpp)
add_cmodel_test(test_integration)
target_link_libraries(test_integration PRIVATE yaml-cpp::yaml-cpp)
add_cmodel_test(test_protocol_rules)
add_custom_command(TARGET test_integration POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures
    $<TARGET_FILE_DIR:test_integration>/fixtures)
