// ctest-level entry that drives the Verilator co-sim binary across the
// smoke YAML scenario set. Each scenario becomes one parameterized ctest case.
#include "common/scenario.hpp"
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>

namespace {
constexpr const char* kCosimBinaryEnv = "COSIM_BIN";

class NmuNsuAxiConformity : public ::testing::TestWithParam<std::string> {};

TEST_P(NmuNsuAxiConformity, scenario_passes_wb2axip_checker) {
    SCENARIO(("co-sim conformity check: " + GetParam()).c_str());

    const char* bin = std::getenv(kCosimBinaryEnv);
    ASSERT_NE(bin, nullptr) << "Set " << kCosimBinaryEnv << " env var.";

    // Resolve scenario path relative to CWD (set by CMake WORKING_DIRECTORY
    // to wherever fixtures + config are staged).
    std::string scenario = "fixtures/" + GetParam();
    ASSERT_TRUE(std::filesystem::exists(scenario)) << scenario;

    std::string cmd = std::string(bin) + " +scenario=" + scenario;
    int rc = std::system(cmd.c_str());
    EXPECT_EQ(rc, 0) << "co-sim returned non-zero for " << GetParam();
}

INSTANTIATE_TEST_SUITE_P(SmokeSet, NmuNsuAxiConformity,
    ::testing::Values(
        "burst_incr_8beat.yaml",
        "burst_incr_2beat.yaml",
        "backpressure_retry.yaml"
    ));
}  // namespace
