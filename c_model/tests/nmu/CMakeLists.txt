add_cmodel_test(test_axi_slave_port)
target_link_libraries(test_axi_slave_port PRIVATE yaml-cpp::yaml-cpp)
# Pick up the shared loopback fixtures from tests/common/ via
# "common/loopback_*.hpp" — single source of truth for both NMU + NSU tests.
target_include_directories(test_axi_slave_port PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
# port_params.yaml is loaded at runtime from config/port_params.yaml (relative
# to the test binary's working dir). Mirror the axi/test_integration pattern
# of copying scenario inputs next to the binary at build time.
add_custom_command(TARGET test_axi_slave_port POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/../../config
    $<TARGET_FILE_DIR:test_axi_slave_port>/config)
