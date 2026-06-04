// Smoke test: Nmu class constructs cleanly + tick() doesn't crash.
// Verifies ctor sequence (member init order, factory return-by-value via
// detail::make_vc_arbiter, sub-module ref dependencies) before Task 3
// integration. Does NOT exercise full e2e flow; that's integration testbench.
#include "common/loopback_noc.hpp"
#include "common/scenario.hpp"
#include "nmu/nmu.hpp"
#include <gtest/gtest.h>

using ni::cmodel::nmu::Nmu;
using ni::cmodel::nmu::NmuConfig;
using ni::cmodel::testing::LoopbackNoc;

TEST(NmuTopLevel, ConstructsAndTicksWithoutCrash) {
    SCENARIO(
        "Nmu top-level smoke: NUM_VC=1 default config, construct + tick 10x "
        "should not crash. Verifies ctor sequence + member init order.");
    LoopbackNoc loopback(/*req*/ 64, /*rsp*/ 64);
    NmuConfig cfg{};
    cfg.src_id = 0x12;
    Nmu nmu(cfg, loopback.nmu_req_out(), loopback.nmu_rsp_in());

    EXPECT_EQ(&nmu.axi_slave_port(), &nmu.axi_slave_port())
        << "axi_slave_port() returns stable reference";

    for (int i = 0; i < 10; ++i) {
        nmu.tick();
        loopback.tick();
    }
    SUCCEED();  // reaching here means no abort during ctor or tick
}
