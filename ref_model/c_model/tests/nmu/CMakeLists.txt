add_cmodel_test(test_axi_slave_port)
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
target_link_libraries(test_addr_trans PRIVATE yaml-cpp::yaml-cpp)
# The collective issuer cases load the shipped peripheral topology: a peripheral
# and its host router share a coordinate there, which is the case the issuer gate
# has to tell apart and no hand-built fixture states.
target_compile_definitions(test_addr_trans PRIVATE
  TOPOLOGY_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../../../sim/topologies")

add_cmodel_test(test_sam_table)

add_cmodel_test(test_sam_yaml)
target_link_libraries(test_sam_yaml PRIVATE yaml-cpp::yaml-cpp)
# The test globs sim/topologies/ directly instead of a copy next to the binary:
# `cmake -E copy_directory` mirrors additively, so a deleted topology YAML
# lingers in the build tree forever (mesh_1x1_vc1 and mesh_2x2_nonuniform_vc1
# both did) and a new one appears only when the test binary relinks. Reading the
# source directory is also what the Python twin does.
target_compile_definitions(test_sam_yaml PRIVATE
  TOPOLOGY_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../../../sim/topologies")

add_cmodel_test(test_sam_config)
target_link_libraries(test_sam_config PRIVATE yaml-cpp::yaml-cpp)
# The C++ half of the two-reader parity gate: it expands sim/configs/*.yml and
# compares against sim/configs/sam_rules.golden, which the Python half
# (sim/tools/test_sam_config_parity.py) is held to as well. Source directory,
# not a copy next to the binary -- same reason as test_sam_yaml above.
target_compile_definitions(test_sam_config PRIVATE
  CONFIG_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../../../sim/configs")

add_cmodel_test(test_rob)
# Pick up the shared loopback fixtures from tests/common/ via
# "common/loopback_*.hpp".
target_include_directories(test_rob PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_vc_allocator)
target_include_directories(test_vc_allocator PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(test_vc_allocator PRIVATE yaml-cpp::yaml-cpp)

add_cmodel_test(test_nmu)
target_include_directories(test_nmu PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(test_nmu PRIVATE yaml-cpp::yaml-cpp)

add_cmodel_test(test_nmu_credit)
target_include_directories(test_nmu_credit PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(test_nmu_credit PRIVATE yaml-cpp::yaml-cpp)

add_cmodel_test(test_nmu_collective)
# Pick up the shared fixtures from tests/common/ via "common/*.hpp".
target_include_directories(test_nmu_collective PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_nmu_dat_face)
target_include_directories(test_nmu_dat_face PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(test_nmu_dat_face PRIVATE yaml-cpp::yaml-cpp)


