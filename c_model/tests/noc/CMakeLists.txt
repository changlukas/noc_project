add_cmodel_test(test_wormhole_arbiter)
target_include_directories(test_wormhole_arbiter PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_router)
target_include_directories(test_router PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_router_adapters)
target_include_directories(test_router_adapters PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_router_front_route)
target_include_directories(test_router_front_route PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_router_config_getters)
target_include_directories(test_router_config_getters PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_two_node_fabric_at)
target_include_directories(test_two_node_fabric_at PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
