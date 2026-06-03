add_cmodel_test(test_loopback_latency)
# Pick up the shared loopback fixtures from tests/common/ via
# "common/loopback_*.hpp".
target_include_directories(test_loopback_latency PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
