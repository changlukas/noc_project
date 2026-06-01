add_cmodel_test(test_port_pair_loopback)
target_link_libraries(test_port_pair_loopback PRIVATE yaml-cpp::yaml-cpp)
# Reuse the Stage 2 axi fixtures directly so this test exercises the same
# burst / outstanding / WRAP / narrow YAML scenarios end-to-end through the
# port pair. port_params.yaml is loaded from config/.
add_custom_command(TARGET test_port_pair_loopback POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/../axi/fixtures
    $<TARGET_FILE_DIR:test_port_pair_loopback>/fixtures
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/../../config
    $<TARGET_FILE_DIR:test_port_pair_loopback>/config)
