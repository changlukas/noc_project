add_cmodel_test(test_channel_model_per_vc_credit)
target_include_directories(test_channel_model_per_vc_credit PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_axi_master_callbacks)
target_include_directories(test_axi_master_callbacks PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(test_axi_master_callbacks PRIVATE yaml-cpp::yaml-cpp)

add_cmodel_test(test_perf_collector)
target_include_directories(test_perf_collector PRIVATE ${CMAKE_SOURCE_DIR}/include)
