add_cmodel_test(test_axi_dpi_adapter)
target_link_libraries(test_axi_dpi_adapter PRIVATE yaml-cpp::yaml-cpp)
# Pick up tests/common/loopback_noc.hpp via "common/..." includes.
target_include_directories(test_axi_dpi_adapter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..)
# Copy AXI fixtures + config so the binary can load them via relative paths.
add_custom_command(TARGET test_axi_dpi_adapter POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/../axi/fixtures
        $<TARGET_FILE_DIR:test_axi_dpi_adapter>/fixtures
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/../../config
        $<TARGET_FILE_DIR:test_axi_dpi_adapter>/config)
