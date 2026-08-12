add_cmodel_test(test_pipeline_stage)
target_include_directories(test_pipeline_stage PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_cmodel_test(test_wormhole_arbiter)
target_include_directories(test_wormhole_arbiter PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_router)
target_include_directories(test_router PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_simple_router)
target_include_directories(test_simple_router PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_router_adapters)
target_include_directories(test_router_adapters PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_route_mask)
target_include_directories(test_route_mask PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_router_front_route)
target_include_directories(test_router_front_route PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_router_fork)
target_include_directories(test_router_fork PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_simple_router_fork)
target_include_directories(test_simple_router_fork PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_simple_router_join)
target_include_directories(test_simple_router_join PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

