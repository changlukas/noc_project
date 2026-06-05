add_cmodel_test(test_loopback_noc_shell_adapter)
# Add c_model/tests/ to include path so the adapter (and its LoopbackNoc
# dependency in tests/common/) resolves via "common/loopback_noc.hpp".
target_include_directories(test_loopback_noc_shell_adapter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_master_shell_adapter test_master_shell_adapter.cpp)
target_link_libraries(test_master_shell_adapter PRIVATE yaml-cpp::yaml-cpp)
