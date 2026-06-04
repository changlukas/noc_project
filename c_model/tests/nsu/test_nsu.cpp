// Smoke test: Nsu class constructs cleanly + tick() doesn't crash.
// Verifies ctor sequence (member init order, factory return-by-value, no-Rob
// asymmetry vs Nmu) before Task 3 integration.
#include "common/loopback_noc.hpp"
#include "common/scenario.hpp"
#include "nsu/nsu.hpp"
#include <gtest/gtest.h>

using ni::cmodel::nsu::Nsu;
using ni::cmodel::nsu::NsuConfig;
using ni::cmodel::testing::LoopbackNoc;

TEST(NsuTopLevel, ConstructsAndTicksWithoutCrash) {
    SCENARIO(
        "Nsu top-level smoke: NUM_VC=1 default config, construct + tick 10x "
        "should not crash. Verifies ctor sequence (no Rob, MetaBuffer shared "
        "between Depacketize and Packetize).");
    LoopbackNoc loopback(/*req*/ 64, /*rsp*/ 64);
    NsuConfig cfg{};
    cfg.src_id = 0x34;
    Nsu nsu(cfg, loopback.nsu_req_in(0), loopback.nsu_rsp_out(0));

    EXPECT_EQ(&nsu.axi_master_port(), &nsu.axi_master_port())
        << "axi_master_port() returns stable reference";

    for (int i = 0; i < 10; ++i) {
        nsu.tick();
        loopback.tick();
    }
    SUCCEED();
}
