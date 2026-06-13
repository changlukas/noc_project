add_cmodel_test(test_wormhole_arbiter)
target_include_directories(test_wormhole_arbiter PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_router)
target_include_directories(test_router PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
