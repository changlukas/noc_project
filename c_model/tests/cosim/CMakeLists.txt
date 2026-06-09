add_cmodel_test(test_channel_model_shell_adapter)
# Add c_model/tests/ to include path so the adapter (and its ChannelModel
# dependency in tests/common/) resolves via "common/channel_model.hpp".
target_include_directories(test_channel_model_shell_adapter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_master_shell_adapter test_master_shell_adapter.cpp)
target_link_libraries(test_master_shell_adapter PRIVATE yaml-cpp::yaml-cpp)

add_cmodel_test(test_slave_shell_adapter test_slave_shell_adapter.cpp)

add_cmodel_test(test_nmu_shell_adapter test_nmu_shell_adapter.cpp)
target_link_libraries(test_nmu_shell_adapter PRIVATE yaml-cpp::yaml-cpp)

add_cmodel_test(test_nsu_shell_adapter test_nsu_shell_adapter.cpp)
target_link_libraries(test_nsu_shell_adapter PRIVATE yaml-cpp::yaml-cpp)

# test_cmodel_dpi — DPI session state-machine / lifecycle test.
# Excluded from the default build until Task 5 defines cmodel_channel_model_create;
# Task 5 removes the EXCLUDE_FROM_ALL property.
add_cmodel_test(test_cmodel_dpi)
target_sources(test_cmodel_dpi PRIVATE
    ${CMAKE_SOURCE_DIR}/../cosim/c/cmodel_dpi.cpp)
target_link_libraries(test_cmodel_dpi PRIVATE yaml-cpp::yaml-cpp)
target_include_directories(test_cmodel_dpi PRIVATE
    ${CMAKE_SOURCE_DIR}/../cosim/c
    $ENV{VERILATOR_ROOT}/include)
set_target_properties(test_cmodel_dpi PROPERTIES EXCLUDE_FROM_ALL TRUE)
