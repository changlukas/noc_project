// Smoke test: Nmu class constructs cleanly + tick() doesn't crash.
// Verifies ctor sequence (member init order, factory return-by-value via
// detail::make_vc_arbiter, sub-module ref dependencies) before Task 3
// integration. Does NOT exercise full e2e flow; that's integration testbench.
#include "axi/types.hpp"
#include "common/channel_model.hpp"
#include "common/scenario.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include "nmu/nmu.hpp"
#include <cstdint>
#include <gtest/gtest.h>

using ni::cmodel::Flit;
using ni::cmodel::nmu::Nmu;
using ni::cmodel::nmu::NmuConfig;
using ni::cmodel::nmu::NmuStandalone;
using ni::cmodel::testing::ChannelModel;
namespace axi = ni::cmodel::axi;

TEST(NmuTopLevel, ConstructsAndTicksWithoutCrash) {
    SCENARIO(
        "Nmu top-level smoke: NUM_VC=1 default config, construct + tick 10x "
        "should not crash. Verifies ctor sequence + member init order.");
    ChannelModel channel(/*req*/ 64, /*rsp*/ 64);
    NmuConfig cfg{};
    cfg.src_id = 0x12;
    cfg.port_params.aw_queue_depth = 16;
    cfg.port_params.w_queue_depth = 16;
    cfg.port_params.ar_queue_depth = 16;
    cfg.port_params.b_queue_depth = 16;
    cfg.port_params.r_queue_depth = 16;
    cfg.port_params.depkt_b_q_depth = 16;
    cfg.port_params.depkt_r_q_depth = 16;
    Nmu nmu(cfg, channel.nmu_req_out(), channel.nmu_rsp_in());

    EXPECT_EQ(&nmu.axi_slave_port(), &nmu.axi_slave_port())
        << "axi_slave_port() returns stable reference";

    for (int i = 0; i < 10; ++i) {
        nmu.tick();
        channel.tick();
    }
    SUCCEED();  // reaching here means no abort during ctor or tick
}

// Write round-trip e2e: AW + W in via AxiSlavePort, observe AW + W flits
// on the NoC req-out face, inject a synthetic B flit on the NoC rsp-in
// face, observe the B beat at the AxiSlavePort response side.
//
// Pinpoints: member-declaration order, tick-order (depacketize before
// axi_slave_port, then wormhole, then vc_arbiter), Packetize -> Wormhole
// -> VcArbiter -> NocReqOut wiring, and the symmetric
// NocRspIn -> Depacketize -> AxiSlavePort return path inside the
// assembled pipeline. Uses NmuStandalone so the test does not depend on
// the ChannelModel / NSU side; any break in Nmu-internal wiring surfaces
// here even if the integration testbench's harness happens to mask it.
TEST(NmuTopLevel, WriteRoundTripProducesReqFlitsAndObservesBResp) {
    SCENARIO(
        "Nmu write round-trip: push AW+W into AxiSlavePort, drain AW+W "
        "flits from null NoC req-out, inject synthetic B flit into null "
        "NoC rsp-in, expect BBeat at AxiSlavePort.pop_b(). Regression "
        "gate for assembled-pipeline wiring + tick order.");

    constexpr uint8_t kSrcId = 0x12;
    constexpr uint8_t kAxiId = 0x05;
    constexpr uint64_t kAddr = 0x100;  // dst_id = (0x100 >> 16) & 0xff = 0

    NmuConfig cfg{};
    cfg.src_id = kSrcId;
    // PortParams has no defaults ("fail loud" — see nmu/port_params.hpp);
    // a hermetic test sets just the per-channel queue depths it exercises.
    cfg.port_params.aw_queue_depth = 16;
    cfg.port_params.w_queue_depth = 16;
    cfg.port_params.ar_queue_depth = 16;
    cfg.port_params.b_queue_depth = 16;
    cfg.port_params.r_queue_depth = 16;
    cfg.port_params.depkt_b_q_depth = 16;
    cfg.port_params.depkt_r_q_depth = 16;
    NmuStandalone nmu(cfg);

    // Push one 1-beat write transaction into the upstream AXI face.
    axi::AwBeat aw{};
    aw.id = kAxiId;
    aw.addr = kAddr;
    aw.len = 0;  // 1 beat
    aw.size = 2;
    aw.burst = axi::Burst::INCR;
    ASSERT_TRUE(nmu.axi_slave_port().push_aw(aw));
    axi::WBeat w{};
    w.strb = 0xF;
    w.last = true;
    ASSERT_TRUE(nmu.axi_slave_port().push_w(w));

    // Drain the req-out face. Bounded loop: pipeline is
    // AxiSlavePort -> Rob -> Packetize -> WormholeArbiter -> VcArbiter
    // -> NullNocReqOut, each tick boundary forwards one stage. 32 ticks
    // is generous; any breakage stalls indefinitely and trips the loop bound.
    bool saw_aw_flit = false;
    bool saw_w_flit = false;
    for (int i = 0; i < 32 && !(saw_aw_flit && saw_w_flit); ++i) {
        nmu.tick();
        while (auto f = nmu.pop_req_flit()) {
            uint64_t ch = f->get_header_field("axi_ch");
            uint64_t src = f->get_header_field("src_id");
            EXPECT_EQ(src, kSrcId) << "req flit src_id should match NmuConfig.src_id";
            if (ch == ni::AXI_CH_AW) {
                EXPECT_EQ(f->get_payload_field("AW", "awid"), kAxiId);
                EXPECT_EQ(f->get_payload_field("AW", "awaddr"), kAddr);
                saw_aw_flit = true;
            } else if (ch == ni::AXI_CH_W) {
                EXPECT_EQ(f->get_payload_field("W", "wlast"), 1u);
                saw_w_flit = true;
            } else {
                ADD_FAILURE() << "unexpected req flit axi_ch=" << ch << " (expected AW or W)";
            }
        }
    }
    ASSERT_TRUE(saw_aw_flit) << "Nmu never produced AW flit on NoC req-out face";
    ASSERT_TRUE(saw_w_flit) << "Nmu never produced W flit on NoC req-out face";

    // Inject a synthetic B response flit. The NMU Depacketize tick reads
    // axi_ch + bid + bresp + buser; src_id/dst_id/last are honored but
    // not consumed by the AxiSlavePort drain.
    Flit b_flit;
    b_flit.set_header_field("axi_ch", ni::AXI_CH_B);
    b_flit.set_header_field("src_id", 0x00);
    b_flit.set_header_field("dst_id", kSrcId);
    b_flit.set_header_field("vc_id", 0);
    b_flit.set_header_field("last", 1);
    b_flit.set_payload_field("B", "bid", kAxiId);
    b_flit.set_payload_field("B", "bresp", static_cast<uint64_t>(axi::Resp::OKAY));
    b_flit.set_payload_field("B", "buser", 0);
    nmu.inject_rsp_flit(b_flit);

    // Drain the response side. Depacketize.tick() ingests the flit; the
    // AxiSlavePort.tick() that follows in the same Nmu.tick() pulls it
    // into b_q_. So a single nmu.tick() is sufficient, but loop for slack.
    std::optional<axi::BBeat> b_out;
    for (int i = 0; i < 8 && !b_out; ++i) {
        nmu.tick();
        b_out = nmu.axi_slave_port().pop_b();
    }
    ASSERT_TRUE(b_out.has_value()) << "Nmu never surfaced B beat to AxiSlavePort";
    EXPECT_EQ(b_out->id, kAxiId);
    EXPECT_EQ(b_out->resp, axi::Resp::OKAY);
}
