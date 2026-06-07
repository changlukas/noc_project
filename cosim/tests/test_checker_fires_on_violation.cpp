// Stage 5b checker liveness regression.
// The injection_aw_unstable scenario uses a non-existent data_file to force
// a DPI error at the first master tick. This propagates through the centralized
// DPI error poll in tb_top.sv (cmodel_check_error → $display "[tb_top] DPI fatal"
// → $fatal) → non-zero exit.
// Expected: binary exits non-zero AND emits the "[tb_top] DPI fatal" marker.
// rc != 0 alone is insufficient — a Verilator build failure or a missing
// scenario YAML would also exit non-zero without the fatal-path marker.
#include "common/scenario.hpp"
#include <cstdio>
#include <cstdlib>
#include <gtest/gtest.h>
#include <string>

namespace {

// tb_top.sv:379 — emitted by the centralized DPI error poll right before $fatal.
constexpr const char* kDpiFatalMarker = "[tb_top] DPI fatal";

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

}  // namespace

TEST(CheckerLiveness, injection_forces_nonzero_exit) {
    SCENARIO("wb2axip / DPI error liveness: injected fault causes non-zero exit");
    const char* bin = std::getenv("COSIM_BIN");
    ASSERT_NE(bin, nullptr) << "COSIM_BIN env var not set";
    // SCENARIO_TREE_ROOT is the absolute path to tests/scenarios/ (CMake-injected).
    const std::string cmd =
        std::string(bin) +
        " +scenario=" SCENARIO_TREE_ROOT "sv-cosim-only/injection_aw_unstable/scenario.yaml";
    const ProcResult result = run_and_capture(cmd);
    EXPECT_NE(result.rc, 0)
        << "injection scenario should have caused non-zero exit (DPI error / checker fire)\n"
        << "output:\n"
        << result.output;
    // rc != 0 alone would also fire on a Verilator build failure or missing
    // scenario file. Require the centralized DPI-fatal marker to confirm the
    // exit went through tb_top's error path, not an unrelated failure mode.
    EXPECT_NE(result.output.find(kDpiFatalMarker), std::string::npos)
        << "injection scenario exited non-zero but did not emit DPI-fatal marker (\""
        << kDpiFatalMarker << "\")\noutput:\n"
        << result.output;
}
