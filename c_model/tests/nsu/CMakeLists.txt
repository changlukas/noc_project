add_cmodel_test(test_nsu_pipeline)
target_link_libraries(test_nsu_pipeline PRIVATE yaml-cpp::yaml-cpp)

add_cmodel_test(test_axi_master_port)
target_link_libraries(test_axi_master_port PRIVATE yaml-cpp::yaml-cpp)
# Pick up the shared loopback fixtures from tests/common/ via
# "common/loopback_*.hpp" — single source of truth for both NMU + NSU tests.
target_include_directories(test_axi_master_port PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
add_custom_command(TARGET test_axi_master_port POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/../../config
    $<TARGET_FILE_DIR:test_axi_master_port>/config)

add_cmodel_test(test_meta_buffer)

# Note: distinct target name from nmu/test_packetize to avoid CMake target
# collision (add_cmodel_test creates a bare-named target). Source file mirrors
# the target name; gtest suite name remains NsuPacketize.
add_cmodel_test(test_nsu_packetize)
# Pick up the shared loopback fixtures from tests/common/ via
# "common/loopback_*.hpp".
target_include_directories(test_nsu_packetize PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Note: distinct target name from nmu/test_depacketize to avoid CMake target
# collision (add_cmodel_test creates a bare-named target). Source file mirrors
# the target name; gtest suite name remains NsuDepacketize.
add_cmodel_test(test_nsu_depacketize)
# Pick up the shared loopback fixtures from tests/common/ via
# "common/loopback_*.hpp".
target_include_directories(test_nsu_depacketize PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Note: distinct target name from nmu/test_vc_arbiter to avoid CMake target
# collision (add_cmodel_test creates a bare-named target). Source file
# mirrors the target name; gtest suite name remains NsuVcArbiter.
add_cmodel_test(test_nsu_vc_arbiter)
target_include_directories(test_nsu_vc_arbiter PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(test_nsu_vc_arbiter PRIVATE yaml-cpp::yaml-cpp)

add_cmodel_test(test_nsu)
target_include_directories(test_nsu PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(test_nsu PRIVATE yaml-cpp::yaml-cpp)
