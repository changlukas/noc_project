include(GoogleTest)

# Capture tests/ root at function-definition time so add_cmodel_test, when
# invoked from subdirectories, still resolves the right include base. Using
# CMAKE_CURRENT_SOURCE_DIR inside the function would otherwise resolve to the
# caller's subdir (e.g. tests/axi/), which would not expose tests/common/.
set(NI_TESTS_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Helper macro: add a c_model test executable that depends on codegen_check.
# All test targets get tests/ on the include path so any test can use
# "common/test_logger.hpp" (SCENARIO macro + AxiMasterObserver) without each
# subdirectory CMakeLists having to wire target_include_directories itself.
function(add_cmodel_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE gtest_main)
  target_include_directories(${name} PRIVATE
    ${NI_TESTS_ROOT_DIR})
  add_dependencies(${name} codegen_check)
  gtest_discover_tests(${name})
endfunction()

add_cmodel_test(test_pins_smoke)
add_cmodel_test(test_flit)
add_cmodel_test(test_register_file)
target_sources(test_register_file PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/register_file.cpp
)

add_subdirectory(axi)
add_subdirectory(common)
add_subdirectory(cosim)
add_subdirectory(nmu)
add_subdirectory(nsu)
add_subdirectory(integration)
add_subdirectory(noc)

# Stage 5b cosim ctest entries — only built when Vtb_top binary exists.
# CI environments without Verilator skip silently; local dev with a built
# Vtb_top gets 2 additional test targets (CosimIntegration, CheckerLiveness).
if(EXISTS "${CMAKE_SOURCE_DIR}/../cosim/verilator/obj_dir/Vtb_top.exe"
   OR EXISTS "${CMAKE_SOURCE_DIR}/../cosim/verilator/obj_dir/Vtb_top")
    add_subdirectory(${CMAKE_SOURCE_DIR}/../cosim/tests
                     ${CMAKE_BINARY_DIR}/cosim_smoke_tests)
else()
    message(STATUS
        "Vtb_top binary not found — skipping cosim ctest entries "
        "(build cosim/verilator first for local co-sim smoke tests)")
endif()
