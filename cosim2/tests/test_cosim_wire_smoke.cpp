// Stage 5b wire-level co-sim smoke tests.
// Parameterized over 4 conformity/multibeat/multioutstanding scenarios.
// Each test invokes Vtb_top binary via system() and expects exit 0.
// COSIM_BIN env var must point to the Vtb_top binary.
#include "common/scenario.hpp"
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>

namespace {

constexpr const char* kCosimBinaryEnv = "COSIM_BIN";

class CosimWireSmoke : public ::testing::TestWithParam<std::string> {};

TEST_P(CosimWireSmoke, scenario_passes_wb2axip) {
    SCENARIO(("Stage 5b wire-level: " + GetParam()).c_str());
    const char* bin = std::getenv(kCosimBinaryEnv);
    ASSERT_NE(bin, nullptr) << "COSIM_BIN env var not set";
    // Scenario path relative to cosim2/verilator/ (the Vtb_top CWD).
    const std::string scenario = "../tests/fixtures/" + GetParam();
    const std::string cmd = std::string(bin) + " +scenario=" + scenario;
    const int rc = std::system(cmd.c_str());
    EXPECT_EQ(rc, 0) << "scenario " << GetParam() << " failed (exit " << rc << ")";
}

INSTANTIATE_TEST_SUITE_P(
    WireSmoke, CosimWireSmoke,
    ::testing::Values(
        "conformity_write_read.yaml",
        "conformity_backpressure.yaml",
        "multibeat_incr_8beat.yaml",
        "multioutstanding_aw_stress.yaml"));

}  // namespace
