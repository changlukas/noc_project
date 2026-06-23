add_cmodel_test(test_port_pair_loopback)
target_link_libraries(test_port_pair_loopback PRIVATE yaml-cpp::yaml-cpp noc_axi4_scenarios)
# Stage 2 axi scenarios live under sim/test_patterns/ (Stage B unified tree).
# scenario_parser resolves data_file: relative to YAML dir; no copy needed.
# port_params.yaml is loaded from config/ next to the binary.
target_include_directories(test_port_pair_loopback PRIVATE
  "${CMAKE_SOURCE_DIR}/../sim/test_patterns")
target_compile_definitions(test_port_pair_loopback PRIVATE
  SCENARIO_TREE_ROOT="${CMAKE_SOURCE_DIR}/../sim/test_patterns/")
add_custom_command(TARGET test_port_pair_loopback POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/../../config
    $<TARGET_FILE_DIR:test_port_pair_loopback>/config)

# Stage 3 packetize/depacketize end-to-end: extends the port-pair loopback
# by inserting the four real packetize/depacketize modules + ChannelModel
# between the NMU AxiSlavePort and the NSU AxiMasterPort, and asserts the
# Stage 2 Scoreboard sees zero mismatch on each fixture.
add_cmodel_test(test_request_response_loopback)
target_link_libraries(test_request_response_loopback PRIVATE yaml-cpp::yaml-cpp noc_axi4_scenarios)
# Pick up tests/common/channel_model.hpp via "common/..." includes.
target_include_directories(test_request_response_loopback PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  "${CMAKE_SOURCE_DIR}/../sim/test_patterns")
target_compile_definitions(test_request_response_loopback PRIVATE
  SCENARIO_TREE_ROOT="${CMAKE_SOURCE_DIR}/../sim/test_patterns/")
add_custom_command(TARGET test_request_response_loopback POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/../../config
    $<TARGET_FILE_DIR:test_request_response_loopback>/config)

# Task 5: bidirectional loopback over a hand-assembled TwoNodeFabric. Two full-NI
# nodes (NMU+NSU) wired to two per-node Routers + a link; both flows scored by the
# Stage 2 Scoreboard across num_vc {1,2,4,8}.
add_cmodel_test(test_router_loopback)
target_link_libraries(test_router_loopback PRIVATE yaml-cpp::yaml-cpp noc_axi4_scenarios)
target_include_directories(test_router_loopback PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  "${CMAKE_SOURCE_DIR}/../sim/test_patterns")
target_compile_definitions(test_router_loopback PRIVATE
  SCENARIO_TREE_ROOT="${CMAKE_SOURCE_DIR}/../sim/test_patterns/")
add_custom_command(TARGET test_router_loopback POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/../../config
    $<TARGET_FILE_DIR:test_router_loopback>/config)
