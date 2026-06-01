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
