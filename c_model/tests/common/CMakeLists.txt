add_cmodel_test(test_loopback_latency)
# Pick up the shared channel-model fixtures from tests/common/ via
# "common/channel_model.hpp".
target_include_directories(test_loopback_latency PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Self-tests for the test_logger framework (SCENARIO + AxiMasterObserver).
# Uses "common/test_logger.hpp" — same include-via-tests/ pattern as above.
# Links yaml-cpp because test_logger.hpp transitively pulls in the AxiMaster
# scenario_parser path, and one test stands up a real AxiMaster from inline
# scenario YAML for happy-path coverage.
add_cmodel_test(test_test_logger)
target_include_directories(test_test_logger PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(test_test_logger PRIVATE yaml-cpp::yaml-cpp)

add_cmodel_test(test_channel_model_per_vc_credit)
target_include_directories(test_channel_model_per_vc_credit PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_narrow_io)
target_include_directories(test_narrow_io PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_perf_stats)
target_include_directories(test_perf_stats PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_axi_master_callbacks)
target_include_directories(test_axi_master_callbacks PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(test_axi_master_callbacks PRIVATE yaml-cpp::yaml-cpp)

add_cmodel_test(test_ni_perf_observer)
target_include_directories(test_ni_perf_observer PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_perf_report)
target_include_directories(test_perf_report PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_cmodel_test(test_flit_link_probe)
target_include_directories(test_flit_link_probe PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
