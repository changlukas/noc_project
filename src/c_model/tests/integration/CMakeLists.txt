add_cmodel_test(test_port_pair_loopback)

# Packetize/depacketize end-to-end: extends the port-pair loopback
# by inserting the four real packetize/depacketize modules + ChannelModel
# between the NMU AxiSlavePort and the NSU AxiMasterPort, and asserts the
# Scoreboard sees zero mismatch on each fixture.
add_cmodel_test(test_request_response_loopback)
target_link_libraries(test_request_response_loopback PRIVATE yaml-cpp::yaml-cpp)
# Pick up tests/common/channel_model.hpp via "common/..." includes.
target_include_directories(test_request_response_loopback PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

# S2 T2d config-space narrow smoke: real NMU/NSU pipeline + Scoreboard,
# loading the real topology YAML the same way co-sim does.
add_cmodel_test(test_narrow_class_smoke)
target_link_libraries(test_narrow_class_smoke PRIVATE yaml-cpp::yaml-cpp)
target_include_directories(test_narrow_class_smoke PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
# sim/topologies/*.yaml is loaded at runtime from topologies/ (relative to
# the test binary's working dir); mirrors test_sam_yaml's copy step.
add_custom_command(TARGET test_narrow_class_smoke POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../sim/topologies
    $<TARGET_FILE_DIR:test_narrow_class_smoke>/topologies)
