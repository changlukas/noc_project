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

# Stage 3 packetize/depacketize end-to-end: extends the port-pair loopback
# by inserting the four real packetize/depacketize modules + LoopbackNoc
# between the NMU AxiSlavePort and the NSU AxiMasterPort, and asserts the
# Stage 2 Scoreboard sees zero mismatch on each fixture.
add_cmodel_test(test_request_response_loopback)
target_link_libraries(test_request_response_loopback PRIVATE yaml-cpp::yaml-cpp)
# Pick up tests/common/{loopback_noc,test_packetize_adapter}.hpp via
# "common/..." includes.
target_include_directories(test_request_response_loopback PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
# Same fixture / config staging as the port-pair test — the binary loads
# fixtures from ./fixtures and config/port_params.yaml from ./config
# relative to its working directory.
add_custom_command(TARGET test_request_response_loopback POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/../axi/fixtures
    $<TARGET_FILE_DIR:test_request_response_loopback>/fixtures
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/../../config
    $<TARGET_FILE_DIR:test_request_response_loopback>/config)
