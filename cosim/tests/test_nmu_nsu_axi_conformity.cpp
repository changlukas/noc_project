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

// Scenarios scoped to max_outstanding_write=1 and single-beat writes.
// The DPI snapshot model presents one AW/W per tick; multi-outstanding or
// multi-beat bursts desynchronize wb2axip's internal beat counters.
// See cosim/KNOWN_LIMITATIONS.md §2 for the root-cause and future fix path.
// The original multi-beat / multi-outstanding scenarios
// (burst_incr_2beat, burst_incr_8beat, backpressure_retry with max_ow=4)
// are retained in c_model/tests/axi/fixtures/ for the c_model AXI unit
// tests but are NOT included here.
INSTANTIATE_TEST_SUITE_P(SmokeSet, NmuNsuAxiConformity,
    ::testing::Values(
        "conformity_write_read.yaml",
        "conformity_seq_writes.yaml",
        "conformity_backpressure.yaml"
    ));
}  // namespace
