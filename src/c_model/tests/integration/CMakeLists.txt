add_cmodel_test(test_port_pair_loopback)
target_link_libraries(test_port_pair_loopback PRIVATE yaml-cpp::yaml-cpp)

# Stage 3 packetize/depacketize end-to-end: extends the port-pair loopback
# by inserting the four real packetize/depacketize modules + ChannelModel
# between the NMU AxiSlavePort and the NSU AxiMasterPort, and asserts the
# Stage 2 Scoreboard sees zero mismatch on each fixture.
add_cmodel_test(test_request_response_loopback)
target_link_libraries(test_request_response_loopback PRIVATE yaml-cpp::yaml-cpp)
# Pick up tests/common/channel_model.hpp via "common/..." includes.
target_include_directories(test_request_response_loopback PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
add_custom_command(TARGET test_request_response_loopback POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/../../config
    $<TARGET_FILE_DIR:test_request_response_loopback>/config)
