# Generates scenarios_list.hpp by globbing AX4-* directories.
# CONFIGURE_DEPENDS triggers re-glob when files change.
file(GLOB scenario_yamls CONFIGURE_DEPENDS LIST_DIRECTORIES false
     "${CMAKE_CURRENT_SOURCE_DIR}/AX4-*/scenario.yaml")
list(SORT scenario_yamls)

set(scenario_ids "")
foreach(yaml ${scenario_yamls})
    get_filename_component(dir "${yaml}" DIRECTORY)
    get_filename_component(id  "${dir}" NAME)
    string(APPEND scenario_ids "    \"${id}\",\n")
endforeach()
list(LENGTH scenario_yamls scenario_count)

configure_file(scenarios_list.hpp.in
               "${CMAKE_CURRENT_BINARY_DIR}/generated/scenarios_list.hpp"
               @ONLY)

add_library(noc_axi4_scenarios INTERFACE)
target_include_directories(noc_axi4_scenarios INTERFACE
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_sources(noc_axi4_scenarios INTERFACE
    "${CMAKE_CURRENT_BINARY_DIR}/generated/scenarios_list.hpp")
