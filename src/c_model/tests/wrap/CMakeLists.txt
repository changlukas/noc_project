add_cmodel_test(test_router_wrap)
target_include_directories(test_router_wrap PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_nmu_wrap test_nmu_wrap.cpp)
target_link_libraries(test_nmu_wrap PRIVATE yaml-cpp::yaml-cpp)

add_cmodel_test(test_dat_merge_wrap)


# test_cmodel_dpi — DPI session state-machine / lifecycle test.
# Expanded inline (not via add_cmodel_test): it links cmodel_dpi.cpp in and needs
# the Verilator svdpi.h include dir, which the shared helper does not wire.
add_executable(test_cmodel_dpi test_cmodel_dpi.cpp)
target_link_libraries(test_cmodel_dpi PRIVATE gtest_main)
target_include_directories(test_cmodel_dpi PRIVATE
    ${NI_TESTS_ROOT_DIR})
add_dependencies(test_cmodel_dpi codegen_check)
target_sources(test_cmodel_dpi PRIVATE
    ${CMAKE_SOURCE_DIR}/../dpi/cmodel_dpi.cpp)
target_link_libraries(test_cmodel_dpi PRIVATE yaml-cpp::yaml-cpp)
# svdpi.h lives under the Verilator install. Prefer VERILATOR_ROOT when the
# environment provides it; otherwise fall back to common install locations
# (MSYS2 mingw64 on Windows, distro share dirs on Linux). A bare
# $ENV{VERILATOR_ROOT} expansion silently degrades to "/include" when the
# variable is unset at configure time, breaking the build later.
find_path(VERILATOR_INCLUDE_DIR vltstd/svdpi.h
    HINTS $ENV{VERILATOR_ROOT}/include
    PATHS C:/msys64/mingw64/share/verilator/include
          /usr/share/verilator/include
          /usr/local/share/verilator/include)
if(NOT VERILATOR_INCLUDE_DIR)
    message(FATAL_ERROR
        "Verilator include dir (vltstd/svdpi.h) not found - set VERILATOR_ROOT")
endif()
target_include_directories(test_cmodel_dpi PRIVATE
    ${CMAKE_SOURCE_DIR}/../dpi
    ${VERILATOR_INCLUDE_DIR}
    ${VERILATOR_INCLUDE_DIR}/vltstd)
gtest_discover_tests(test_cmodel_dpi)
