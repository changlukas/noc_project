add_cmodel_test(test_port_pair_loopback)
target_link_libraries(test_port_pair_loopback PRIVATE yaml-cpp::yaml-cpp noc_axi4_scenarios)
# Stage 2 axi scenarios live under tests/scenarios/ (Stage B unified tree).
# scenario_parser resolves data_file: relative to YAML dir; no copy needed.
# port_params.yaml is loaded from config/ next to the binary.
target_include_directories(test_port_pair_loopback PRIVATE
  "${CMAKE_SOURCE_DIR}/../tests/scenarios")
target_compile_definitions(test_port_pair_loopback PRIVATE
  SCENARIO_TREE_ROOT="${CMAKE_SOURCE_DIR}/../tests/scenarios/")
add_custom_command(TARGET test_port_pair_loopback POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/../../config
    $<TARGET_FILE_DIR:test_port_pair_loopback>/config)

# Stage 3 packetize/depacketize end-to-end: extends the port-pair loopback
# by inserting the four real packetize/depacketize modules + LoopbackNoc
# between the NMU AxiSlavePort and the NSU AxiMasterPort, and asserts the
# Stage 2 Scoreboard sees zero mismatch on each fixture.
add_cmodel_test(test_request_response_loopback)
target_link_libraries(test_request_response_loopback PRIVATE yaml-cpp::yaml-cpp noc_axi4_scenarios)
# Pick up tests/common/loopback_noc.hpp via "common/..." includes.
target_include_directories(test_request_response_loopback PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  "${CMAKE_SOURCE_DIR}/../tests/scenarios")
target_compile_definitions(test_request_response_loopback PRIVATE
  SCENARIO_TREE_ROOT="${CMAKE_SOURCE_DIR}/../tests/scenarios/")
add_custom_command(TARGET test_request_response_loopback POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/../../config
    $<TARGET_FILE_DIR:test_request_response_loopback>/config)
