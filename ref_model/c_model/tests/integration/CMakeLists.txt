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
# loading the real config file the same way co-sim does.
add_cmodel_test(test_narrow_class_smoke)
target_link_libraries(test_narrow_class_smoke PRIVATE yaml-cpp::yaml-cpp)
target_include_directories(test_narrow_class_smoke PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
# The test reads sim/configs/ directly, same as test_sam_yaml: a copy next
# to the binary is mirrored additively by copy_directory, so a
# deleted or renamed config lingers in the build tree and the test keeps
# passing against a file the source tree no longer has.
target_compile_definitions(test_narrow_class_smoke PRIVATE
  CONFIG_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../../../sim/configs")
