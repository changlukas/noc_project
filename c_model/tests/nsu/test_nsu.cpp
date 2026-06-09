// Smoke test: Nsu class constructs cleanly + tick() doesn't crash.
// Verifies ctor sequence (member init order, factory return-by-value, no-Rob
// asymmetry vs Nmu) before Task 3 integration.
#include "axi/types.hpp"
#include "common/channel_model.hpp"
#include "common/scenario.hpp"
#include "ni/flit.hpp"
#include "ni_flit_constants.h"
#include "nsu/nsu.hpp"
#include <cstdint>
#include <gtest/gtest.h>

using ni::cmodel::Flit;
using ni::cmodel::nsu::Nsu;
using ni::cmodel::nsu::NsuConfig;
using ni::cmodel::nsu::NsuStandalone;
using ni::cmodel::testing::ChannelModel;
namespace axi = ni::cmodel::axi;

TEST(NsuTopLevel, ConstructsAndTicksWithoutCrash) {
    SCENARIO(
        "Nsu top-level smoke: NUM_VC=1 default config, construct + tick 10x "
        "should not crash. Verifies ctor sequence (no Rob, MetaBuffer shared "
        "between Depacketize and Packetize).");
    ChannelModel channel(/*req*/ 64, /*rsp*/ 64);
    NsuConfig cfg{};
    cfg.src_id = 0x34;
    Nsu nsu(cfg, channel.nsu_req_in(0), channel.nsu_rsp_out(0));

    EXPECT_EQ(&nsu.axi_master_port(), &nsu.axi_master_port())
        << "axi_master_port() returns stable reference";

    for (int i = 0; i < 10; ++i) {
        nsu.tick();
        channel.tick();
    }
    SUCCEED();
}

// Write round-trip e2e: AW + W flits injected on the NoC req-in face,
// observe AW + W beats at AxiMasterPort.pop_*, push a B beat back via
// AxiMasterPort.push_b, observe the B flit on the NoC rsp-out face with
// dst_id routed back to the original requester's src_id.
//
// Pinpoints: member-declaration order (MetaBuffer must be live before
// Depacketize and Packetize), tick-order (depacketize before
// axi_master_port, then wormhole, then vc_arbiter), shared MetaBuffer
// snapshot-on-AW + lookup-on-B path, and Packetize -> WormholeArbiter
// -> VcArbiter -> NocRspOut wiring. Uses NsuStandalone so the test
// does not depend on ChannelModel / NMU side.
TEST(NsuTopLevel, WriteRoundTripDecodesReqFlitsAndProducesBRspFlit) {
    SCENARIO(
        "Nsu write round-trip: inject AW+W flits into null NoC req-in, "
        "drain AW+W beats from AxiMasterPort, push synthetic B beat into "
        "AxiMasterPort, expect B flit on null NoC rsp-out with dst_id == "
        "original requester. Regression gate for assembled-pipeline wiring "
        "+ MetaBuffer share + tick order.");

    constexpr uint8_t kNsuSrcId = 0x34;
    constexpr uint8_t kRequesterSrcId = 0x12;
    constexpr uint8_t kAxiId = 0x07;
    constexpr uint64_t kAddr = 0x200;

    NsuConfig cfg{};
    cfg.src_id = kNsuSrcId;
    // PortParams + depacketize depths have no defaults ("fail loud" — see
    // ni/port_params.hpp); a hermetic test sets just the depths it exercises.
    cfg.port_params.aw_queue_depth = 16;
    cfg.port_params.w_queue_depth = 16;
    cfg.port_params.ar_queue_depth = 16;
    cfg.port_params.b_queue_depth = 16;
    cfg.port_params.r_queue_depth = 16;
    NsuStandalone nsu(cfg);

    // Build an AW flit. NSU Depacketize snapshots {src_id, rob_req,
    // rob_idx} into MetaBuffer keyed by awid; Packetize.push_b later
    // reads m.src_id back as the response flit's dst_id.
    Flit aw_flit;
    aw_flit.set_header_field("axi_ch", ni::AXI_CH_AW);
    aw_flit.set_header_field("src_id", kRequesterSrcId);
    aw_flit.set_header_field("dst_id", kNsuSrcId);
    aw_flit.set_header_field("vc_id", 0);
    aw_flit.set_header_field("last", 0);  // AW opens wormhole packet
    aw_flit.set_header_field("rob_req", 0);
    aw_flit.set_header_field("rob_idx", 0);
    aw_flit.set_payload_field("AW", "awid", kAxiId);
    aw_flit.set_payload_field("AW", "awaddr", kAddr);
    aw_flit.set_payload_field("AW", "awlen", 0);
    aw_flit.set_payload_field("AW", "awsize", 2);
    aw_flit.set_payload_field("AW", "awburst", static_cast<uint64_t>(axi::Burst::INCR));
    nsu.inject_req_flit(aw_flit);

    Flit w_flit;
    w_flit.set_header_field("axi_ch", ni::AXI_CH_W);
    w_flit.set_header_field("src_id", kRequesterSrcId);
    w_flit.set_header_field("dst_id", kNsuSrcId);
    w_flit.set_header_field("vc_id", 0);
    w_flit.set_header_field("last", 1);  // wlast closes wormhole packet
    w_flit.set_payload_field("W", "wlast", 1);
    w_flit.set_payload_field("W", "wstrb", 0xF);
    nsu.inject_req_flit(w_flit);

    // Drain the downstream AXI face. Pipeline depth:
    // NullNocReqIn -> Depacketize -> AxiMasterPort each one tick boundary.
    std::optional<axi::AwBeat> aw_out;
    std::optional<axi::WBeat> w_out;
    for (int i = 0; i < 16 && !(aw_out && w_out); ++i) {
        nsu.tick();
        if (!aw_out) aw_out = nsu.axi_master_port().pop_aw();
        if (!w_out) w_out = nsu.axi_master_port().pop_w();
    }
    ASSERT_TRUE(aw_out.has_value()) << "Nsu never surfaced AW beat to AxiMasterPort";
    ASSERT_TRUE(w_out.has_value()) << "Nsu never surfaced W beat to AxiMasterPort";
    EXPECT_EQ(aw_out->id, kAxiId);
    EXPECT_EQ(aw_out->addr, kAddr);
    EXPECT_TRUE(w_out->last);

    // Push the B response into the downstream-facing AXI port. The
    // response path runs Packetize.push_b -> wormhole_arbiter -> vc_arbiter
    // -> NullNocRspOut; Packetize reads dst_id from the MetaBuffer
    // snapshot saved at AW ingress.
    axi::BBeat b{};
    b.id = kAxiId;
    b.resp = axi::Resp::OKAY;
    ASSERT_TRUE(nsu.axi_master_port().push_b(b));

    std::optional<Flit> b_flit;
    for (int i = 0; i < 32 && !b_flit; ++i) {
        nsu.tick();
        b_flit = nsu.pop_rsp_flit();
    }
    ASSERT_TRUE(b_flit.has_value()) << "Nsu never produced B flit on NoC rsp-out face";
    EXPECT_EQ(b_flit->get_header_field("axi_ch"), static_cast<uint64_t>(ni::AXI_CH_B));
    EXPECT_EQ(b_flit->get_header_field("src_id"), kNsuSrcId)
        << "rsp flit src_id should be the NSU's own src_id";
    EXPECT_EQ(b_flit->get_header_field("dst_id"), kRequesterSrcId)
        << "rsp flit dst_id should route back to the original requester "
           "(MetaBuffer.peek_write read of the AW's src_id snapshot)";
    EXPECT_EQ(b_flit->get_payload_field("B", "bid"), kAxiId);
    EXPECT_EQ(b_flit->get_payload_field("B", "bresp"), static_cast<uint64_t>(axi::Resp::OKAY));
}
