include(GoogleTest)

# Helper macro: add a c_model test executable that depends on codegen_check.
function(add_cmodel_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE gtest_main)
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
add_subdirectory(nmu)
add_subdirectory(nsu)
add_subdirectory(integration)
