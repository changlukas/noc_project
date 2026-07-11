add_cmodel_test(test_axi_slave_port)
target_link_libraries(test_axi_slave_port PRIVATE yaml-cpp::yaml-cpp)
# Pick up the shared loopback fixtures from tests/common/ via
# "common/loopback_*.hpp" — single source of truth for both NMU + NSU tests.
target_include_directories(test_axi_slave_port PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_packetize)
# Pick up the shared loopback fixtures from tests/common/ via
# "common/loopback_*.hpp".
target_include_directories(test_packetize PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_nmu_req_bridge)
target_link_libraries(test_nmu_req_bridge PRIVATE yaml-cpp::yaml-cpp)

add_cmodel_test(test_depacketize)
# Pick up the shared loopback fixtures from tests/common/ via
# "common/loopback_*.hpp".
target_include_directories(test_depacketize PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_addr_trans)

add_cmodel_test(test_sam_table)

add_cmodel_test(test_sam_yaml)
target_link_libraries(test_sam_yaml PRIVATE yaml-cpp::yaml-cpp)
# sim/topologies/*.yaml is loaded at runtime from topologies/ (relative to
# the test binary's working dir). Mirror the test_axi_slave_port pattern of
# copying real config next to the binary at build time.
add_custom_command(TARGET test_sam_yaml POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../sim/topologies
    $<TARGET_FILE_DIR:test_sam_yaml>/topologies)

add_cmodel_test(test_rob)
# Pick up the shared loopback fixtures from tests/common/ via
# "common/loopback_*.hpp".
target_include_directories(test_rob PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_vc_arbiter)
target_include_directories(test_vc_arbiter PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(test_vc_arbiter PRIVATE yaml-cpp::yaml-cpp)

add_cmodel_test(test_nmu)
target_include_directories(test_nmu PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(test_nmu PRIVATE yaml-cpp::yaml-cpp)

add_cmodel_test(test_nmu_credit)
target_include_directories(test_nmu_credit PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(test_nmu_credit PRIVATE yaml-cpp::yaml-cpp)


