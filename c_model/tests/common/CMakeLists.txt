add_cmodel_test(test_loopback_latency)
# Pick up the shared loopback fixtures from tests/common/ via
# "common/loopback_*.hpp".
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
