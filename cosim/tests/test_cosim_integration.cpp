// Stage 5b wire-level co-sim smoke tests.
// Parameterized over 4 conformity/multibeat/multioutstanding scenarios.
// Each test invokes Vtb_top binary as a subprocess, captures stdout+stderr,
// and asserts BOTH exit 0 AND the tb_top PASS marker — relying on rc alone
// would treat a Verilator build failure (non-zero rc, no marker) as a
// scoreboard pass.
// COSIM_BIN env var must point to the Vtb_top binary.
#include "common/scenario.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>

namespace {

constexpr const char* kCosimBinaryEnv = "COSIM_BIN";

// tb_top.sv:351 — emitted only on clean scenario completion + scoreboard clean.
constexpr const char* kPassMarker = "PASS: scenario complete, scoreboard clean";

struct ProcResult {
    int rc;
    std::string output;
};

// Run a command and capture combined stdout+stderr. Returns rc + output.
// Falls back to rc = -1 if popen fails.
ProcResult run_and_capture(const std::string& cmd) {
    ProcResult r{};
    // Redirect stderr into stdout so $fatal / $display from Verilator both
    // land in the captured buffer regardless of which stream tb_top writes to.
    const std::string full_cmd = cmd + " 2>&1";
#ifdef _WIN32
    FILE* p = _popen(full_cmd.c_str(), "r");
#else
    FILE* p = popen(full_cmd.c_str(), "r");
#endif
    if (!p) {
        r.rc = -1;
        return r;
    }
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), p)) {
        r.output += buf;
    }
#ifdef _WIN32
    r.rc = _pclose(p);
#else
    r.rc = pclose(p);
#endif
    return r;
}

class CosimWireSmoke : public ::testing::TestWithParam<std::string> {};

TEST_P(CosimWireSmoke, scenario_passes_wb2axip) {
    SCENARIO(("Stage 5b wire-level: " + GetParam()).c_str());
    const char* bin = std::getenv(kCosimBinaryEnv);
    ASSERT_NE(bin, nullptr) << "COSIM_BIN env var not set";
    // SCENARIO_TREE_ROOT is the absolute path to tests/scenarios/ (injected
    // at compile time by CMake). All scenarios consumed by this smoke test
    // live under common/. scenario_parser resolves data_file: paths relative
    // to the scenario.yaml's own directory, so Vtb_top's cwd doesn't matter.
    const std::string scenario =
        std::string(SCENARIO_TREE_ROOT) + GetParam() + "/scenario.yaml";
    const std::string cmd = std::string(bin) + " +scenario=" + scenario;
    const ProcResult result = run_and_capture(cmd);
    EXPECT_EQ(result.rc, 0) << "scenario " << GetParam() << " failed (exit "
                            << result.rc << ")\noutput:\n"
                            << result.output;
    // rc == 0 alone is not sufficient: a Verilator stub that does nothing
    // would also return 0. Require the tb_top PASS marker so a missing scenario
    // file, an early $finish(0), or a silently degraded testbench is caught.
    EXPECT_NE(result.output.find(kPassMarker), std::string::npos)
        << "scenario " << GetParam() << " did not emit PASS marker (\""
        << kPassMarker << "\")\noutput:\n"
        << result.output;
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
// Scenarios are post-Stage-B common scenarios scope-limited to
// max_outstanding_{write,read} == 1 (default) and single-beat (AWLEN=0).
// wb2axip's faxi_slave additionally checks AW/W ordering (faxi_slave.v:561
// + :805-807) which a multi-outstanding master can violate by interleaving
// W beats across AW IDs — the older L1 fixtures conformity_backpressure /
// multi_id_single_beat_sequential were explicitly tuned for these limits
// (max_outstanding_write=1) and were dedup'd against L3 scenarios that
// raise max_outstanding to 4-8. Only L3 scenarios that defaulted to
// max_outstanding=1 are usable here.
INSTANTIATE_TEST_SUITE_P(
    WireSmoke, CosimWireSmoke,
    ::testing::Values(
        // common/ layer — max_outstanding=1 by default, wb2axip-compatible.
        "common/single_write_read_aligned",
        "common/multi_txn_same_id",
        // sv-cosim-only/ layer — L1-tuned versions with max_outstanding=1
        // explicit, restored so wb2axip serialization constraint stays met
        // (see header comment above).
        "sv-cosim-only/conformity_write_read",
        "sv-cosim-only/conformity_backpressure",
        "sv-cosim-only/multi_id_single_beat_sequential"));

}  // namespace
