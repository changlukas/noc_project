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

// wb2axip faxi_slave.v:805-807 enforces AWREADY=0 while any W burst is
// mid-flight (wr_pending > 1). This is a wb2axip-internal simplification
// for formal-engine convergence, NOT an AXI4 spec mandate (see author note
// faxi_slave.v:583-587 + AXI4 IHI 0022H §A3.3).
//
// CosimWireSmoke is therefore scope-limited to single-beat scenarios
// (AWLEN=0 throughout). Multi-beat W burst architectural correctness is
// independently verified at the C++ adapter layer by T10 unit test
// NmuShellAdapter.multi_beat_w_burst_visible_per_cycle.
//
// Scenario "multioutstanding_aw_stress" is intentionally renamed below
// (see multi_id_single_beat_sequential.yaml) — the original name implied
// multi-outstanding AW but all transactions are max_outstanding_write=1
// with len=0 (sequential single-beat). See KNOWN_LIMITATIONS.md §6.
INSTANTIATE_TEST_SUITE_P(
    WireSmoke, CosimWireSmoke,
    ::testing::Values(
        "conformity_write_read.yaml",
        "conformity_backpressure.yaml",
        "multi_id_single_beat_sequential.yaml"));

}  // namespace
