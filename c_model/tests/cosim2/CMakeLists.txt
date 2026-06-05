add_cmodel_test(test_loopback_noc_shell_adapter)
# Add c_model/tests/ to include path so the adapter (and its LoopbackNoc
# dependency in tests/common/) resolves via "common/loopback_noc.hpp".
target_include_directories(test_loopback_noc_shell_adapter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..)
