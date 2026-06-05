// Stage 5b checker liveness regression.
// The injection_aw_unstable scenario uses a non-existent data_file to force
// a DPI error at the first master tick. This propagates through the error-
// check path in axi_master_wrap → $fatal → non-zero exit.
// Expected: binary exits non-zero (wb2axip / DPI error path is live).
#include "common/scenario.hpp"
#include <cstdlib>
#include <gtest/gtest.h>
#include <string>

TEST(CheckerLiveness, injection_forces_nonzero_exit) {
    SCENARIO("wb2axip / DPI error liveness: injected fault causes non-zero exit");
    const char* bin = std::getenv("COSIM_BIN");
    ASSERT_NE(bin, nullptr) << "COSIM_BIN env var not set";
    // Scenario path relative to cosim2/verilator/ (the Vtb_top CWD).
    const std::string cmd =
        std::string(bin) + " +scenario=../tests/fixtures/injection_aw_unstable.yaml";
    const int rc = std::system(cmd.c_str());
    EXPECT_NE(rc, 0)
        << "injection scenario should have caused non-zero exit (DPI error / checker fire)";
}
