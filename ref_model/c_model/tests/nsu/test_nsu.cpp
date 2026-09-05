// Smoke test: Nsu class constructs cleanly + tick() doesn't crash.
// Verifies ctor sequence (member init order, factory return-by-value, no-Rob
// asymmetry vs Nmu) in isolation.
#include "axi/types.hpp"
#include "common/channel_model.hpp"
#include "flit.hpp"
#include "ni/channel_mode.hpp"
#include "ni_flit_constants.h"
#include "nsu/nsu_standalone.hpp"
#include "router/null_adapters.hpp"
#include <cstdint>
#include <gtest/gtest.h>

using ni::cmodel::Flit;
using ni::cmodel::ni::ChannelMode;
using ni::cmodel::nsu::Nsu;
using ni::cmodel::nsu::NsuConfig;
using ni::cmodel::nsu::NsuStandalone;
using ni::cmodel::testing::ChannelModel;
namespace axi = ni::cmodel::axi;

namespace {

void expect_normalized_data_request_ingress(ChannelMode mode, bool use_dat) {
    NsuConfig cfg{};
    cfg.src_id = 0x34;
    cfg.port_params.aw_queue_depth = 16;
    cfg.port_params.w_queue_depth = 16;
    cfg.port_params.ar_queue_depth = 16;
    cfg.port_params.b_queue_depth = 16;
    cfg.port_params.r_queue_depth = 16;
    cfg.port_params.meta_buffer_max_outstanding = 32;
    cfg.port_params.meta_buffer_max_unique_ids = axi::NOC_ID_SPACE;
    NsuStandalone nsu(cfg);
    nsu.set_channel_mode(mode);

    Flit aw;
    aw.set_header_field("axi_ch", ::ni::AXI_CH_DataAw);
    aw.set_header_field("src_id", 0x12);
    aw.set_header_field("dst_id", 0x34);
    aw.set_header_field("vc_id", 0);
    aw.set_header_field("flit_tail", 0);
    aw.set_payload_field("AW", "awid", 0x05);
    aw.set_payload_field("AW", "awaddr", 0x100);
    aw.set_payload_field("AW", "awlen", 0);
    aw.set_payload_field("AW", "awsize", 3);
    aw.set_payload_field("AW", "awburst", static_cast<uint64_t>(axi::Burst::INCR));

    Flit w;
    w.set_header_field("axi_ch", ::ni::AXI_CH_DataW);
    w.set_header_field("src_id", 0x12);
    w.set_header_field("dst_id", 0x34);
    w.set_header_field("vc_id", 0);
    w.set_header_field("flit_tail", 1);
    w.set_payload_field("NARROW_W", "wlast", 1);
    w.set_payload_field("NARROW_W", "wstrb", 0xA5);
    std::array<uint8_t, axi::NARROW_DATA_BYTES> expected{};
    for (std::size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<uint8_t>(0x70 + i);
    w.set_payload_bytes("NARROW_W", "wdata", expected.data(), ::ni::width::NOC_NARROW_DATA_WIDTH);

    Flit ar;
    ar.set_header_field("axi_ch", ::ni::AXI_CH_DataAr);
    ar.set_header_field("src_id", 0x12);
    ar.set_header_field("dst_id", 0x34);
    ar.set_header_field("vc_id", 0);
    ar.set_header_field("flit_tail", 1);
    ar.set_payload_field("AR", "arid", 0x06);
    ar.set_payload_field("AR", "araddr", 0x200);
    ar.set_payload_field("AR", "arlen", 0);
    ar.set_payload_field("AR", "arsize", 3);
    ar.set_payload_field("AR", "arburst", static_cast<uint64_t>(axi::Burst::INCR));

    if (use_dat) {
        nsu.inject_dat_req_flit(aw);
        nsu.inject_dat_req_flit(w);
        nsu.inject_dat_req_flit(ar);
    } else {
        nsu.inject_req_flit(aw);
        nsu.inject_req_flit(w);
        nsu.inject_req_flit(ar);
    }

    std::optional<axi::AwBeat> aw_out;
    std::optional<axi::WBeat> w_out;
    std::optional<axi::ArBeat> ar_out;
    for (int t = 0; t < 32 && !(aw_out && w_out && ar_out); ++t) {
        nsu.tick();
        if (!aw_out) aw_out = nsu.axi_master_port().pop_aw();
        if (!w_out) w_out = nsu.axi_master_port().pop_w();
        if (!ar_out) ar_out = nsu.axi_master_port().pop_ar();
    }
    ASSERT_TRUE(aw_out.has_value());
    ASSERT_TRUE(w_out.has_value());
    ASSERT_TRUE(ar_out.has_value());
    EXPECT_EQ(w_out->strb, 0xA5u);
    for (std::size_t i = 0; i < expected.size(); ++i) EXPECT_EQ(w_out->data[i], expected[i]);
    for (std::size_t i = expected.size(); i < w_out->data.size(); ++i)
        EXPECT_EQ(w_out->data[i], 0u);
}

}  // namespace

TEST(NsuTopLevel, TwoChannel64AcceptsDataRequestsOnReqWithNarrowWPayload) {
    expect_normalized_data_request_ingress(ChannelMode::TwoChannel64, /*use_dat=*/false);
}

TEST(NsuTopLevel, TwoChannel64ReqDataDoesNotDependOnDatVcOrReturnDatCredit) {
    NsuConfig cfg{};
    cfg.src_id = 0x34;
    cfg.port_params.aw_queue_depth = 16;
    cfg.port_params.w_queue_depth = 16;
    cfg.port_params.ar_queue_depth = 16;
    cfg.port_params.b_queue_depth = 16;
    cfg.port_params.r_queue_depth = 16;
    cfg.port_params.meta_buffer_max_outstanding = 32;
    cfg.port_params.meta_buffer_max_unique_ids = axi::NOC_ID_SPACE;
    cfg.dat_num_vc = 1;
    NsuStandalone nsu(cfg);
    nsu.set_channel_mode(ChannelMode::TwoChannel64);

    Flit aw;
    aw.set_header_field("axi_ch", ::ni::AXI_CH_DataAw);
    aw.set_header_field("src_id", 0x12);
    aw.set_header_field("dst_id", 0x34);
    aw.set_header_field("vc_id", 1);
    aw.set_header_field("flit_tail", 0);
    aw.set_payload_field("AW", "awid", 0x05);
    aw.set_payload_field("AW", "awaddr", 0x100);
    aw.set_payload_field("AW", "awlen", 0);
    aw.set_payload_field("AW", "awsize", 3);
    aw.set_payload_field("AW", "awburst", static_cast<uint64_t>(axi::Burst::INCR));

    Flit w;
    w.set_header_field("axi_ch", ::ni::AXI_CH_DataW);
    w.set_header_field("src_id", 0x12);
    w.set_header_field("dst_id", 0x34);
    w.set_header_field("vc_id", 1);
    w.set_header_field("flit_tail", 1);
    w.set_payload_field("NARROW_W", "wlast", 1);
    w.set_payload_field("NARROW_W", "wstrb", 0xFF);

    nsu.inject_req_flit(aw);
    nsu.inject_req_flit(w);

    std::optional<axi::AwBeat> aw_out;
    std::optional<axi::WBeat> w_out;
    for (int t = 0; t < 16 && !(aw_out && w_out); ++t) {
        nsu.tick();
        if (!aw_out) aw_out = nsu.axi_master_port().pop_aw();
        if (!w_out) w_out = nsu.axi_master_port().pop_w();
    }
    ASSERT_TRUE(aw_out.has_value());
    ASSERT_TRUE(w_out.has_value());
    for (uint8_t vc = 0; vc < cfg.dat_num_vc; ++vc) EXPECT_FALSE(nsu.dat_req_take_credit(vc));
}

TEST(NsuTopLevel, ThreeChannel64AcceptsDataRequestsOnDatWithNarrowWPayload) {
    expect_normalized_data_request_ingress(ChannelMode::ThreeChannel64, /*use_dat=*/true);
}

TEST(NsuTopLevel, ConstructsAndTicksWithoutCrash) {
    ChannelModel channel(/*req*/ 64, /*rsp*/ 64);
    NsuConfig cfg{};
    cfg.src_id = 0x34;
    cfg.port_params.aw_queue_depth = 16;
    cfg.port_params.w_queue_depth = 16;
    cfg.port_params.ar_queue_depth = 16;
    cfg.port_params.b_queue_depth = 16;
    cfg.port_params.r_queue_depth = 16;
    cfg.port_params.meta_buffer_max_outstanding = 32;
    cfg.port_params.meta_buffer_max_unique_ids = axi::NOC_ID_SPACE;
    // DAT face (S3a T4): unused by this smoke test, wired to the shared
    // null sentinel (router/null_adapters.hpp).
    Nsu nsu(cfg, channel.nsu_req_in(0), channel.nsu_rsp_out(0), ni::cmodel::router::null_req_in(),
            ni::cmodel::router::null_rsp_out());

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
// axi_master_port, then wormhole, then vc_allocator), shared MetaBuffer
// allocate-on-AW + lookup-on-B path, and Packetize -> WormholeArbiter
// -> VcAllocator -> NocRspOut wiring. Uses NsuStandalone so the test
// does not depend on ChannelModel / NMU side.
TEST(NsuTopLevel, WriteRoundTripDecodesReqFlitsAndProducesBRspFlit) {
    constexpr uint8_t kNsuSrcId = 0x34;
    constexpr uint8_t kRequesterSrcId = 0x12;
    constexpr uint8_t kAxiId = 0x07;
    constexpr uint64_t kAddr = 0x200;

    NsuConfig cfg{};
    cfg.src_id = kNsuSrcId;
    // PortParams self-defaults from ni_params.h generated constants (see
    // nsu/port_params.hpp); set explicitly here for a hermetic, self-documenting test.
    cfg.port_params.aw_queue_depth = 16;
    cfg.port_params.w_queue_depth = 16;
    cfg.port_params.ar_queue_depth = 16;
    cfg.port_params.b_queue_depth = 16;
    cfg.port_params.r_queue_depth = 16;
    cfg.port_params.meta_buffer_max_outstanding = 32;
    cfg.port_params.meta_buffer_max_unique_ids = axi::NOC_ID_SPACE;
    NsuStandalone nsu(cfg);

    // Build an AW flit. NSU Depacketize allocates {src_id, ordering_req,
    // ordering_tag} into MetaBuffer keyed by awid; Packetize.push_b later
    // reads m.src_id back as the response flit's dst_id.
    Flit aw_flit;
    aw_flit.set_header_field("axi_ch", ::ni::AXI_CH_NarrowAw);
    aw_flit.set_header_field("src_id", kRequesterSrcId);
    aw_flit.set_header_field("dst_id", kNsuSrcId);
    aw_flit.set_header_field("vc_id", 0);
    aw_flit.set_header_field("flit_tail", 0);  // AW opens wormhole packet
    aw_flit.set_header_field("ordering_req", 0);
    aw_flit.set_header_field("ordering_tag", 0);
    aw_flit.set_payload_field("AW", "awid", kAxiId);
    aw_flit.set_payload_field("AW", "awaddr", kAddr);
    aw_flit.set_payload_field("AW", "awlen", 0);
    aw_flit.set_payload_field("AW", "awsize", 2);
    aw_flit.set_payload_field("AW", "awburst", static_cast<uint64_t>(axi::Burst::INCR));
    nsu.inject_req_flit(aw_flit);

    Flit w_flit;
    w_flit.set_header_field("axi_ch", ::ni::AXI_CH_NarrowW);
    w_flit.set_header_field("src_id", kRequesterSrcId);
    w_flit.set_header_field("dst_id", kNsuSrcId);
    w_flit.set_header_field("vc_id", 0);
    w_flit.set_header_field("flit_tail", 1);  // wlast closes wormhole packet
    w_flit.set_payload_field("NARROW_W", "wlast", 1);
    w_flit.set_payload_field("NARROW_W", "wstrb", 0xF);
    nsu.inject_req_flit(w_flit);

    // Drain the downstream AXI face. NSU req is a 2-stage pipeline:
    // Depacketize (S1) → AxiMasterPort drain (S2). Flit needs ≥2 ticks;
    // loop up to 16 for slack and multi-beat bursts.
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
    // response path runs Packetize.push_b -> wormhole_arbiter -> vc_allocator
    // -> QueueNocRspOut; Packetize reads dst_id from the MetaBuffer
    // entry saved at AW ingress.
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
    EXPECT_EQ(b_flit->get_header_field("axi_ch"), static_cast<uint64_t>(::ni::AXI_CH_NarrowB));
    EXPECT_EQ(b_flit->get_header_field("src_id"), kNsuSrcId)
        << "rsp flit src_id should be the NSU's own src_id";
    EXPECT_EQ(b_flit->get_header_field("dst_id"), kRequesterSrcId)
        << "rsp flit dst_id should route back to the original requester "
           "(MetaBuffer.peek_write read of the AW's src_id entry)";
    EXPECT_EQ(b_flit->get_payload_field("B", "bid"), kAxiId);
    EXPECT_EQ(b_flit->get_payload_field("B", "bresp"), static_cast<uint64_t>(axi::Resp::OKAY));
}

// Same drive as the round-trip above, except the requester sits on port 1 at
// its own coordinate and this NSU sits on port 2 at its own. The B must name
// the requester's port, so it reaches the requester and not the tile beside
// it, and must name this NSU's own port as its source.
TEST(NsuTopLevel, EchoesTheRequestersPortBackOntoTheBResponse) {
    constexpr uint8_t kNsuSrcId = 0x34;
    constexpr uint8_t kNsuPortId = 2;
    constexpr uint8_t kRequesterSrcId = 0x12;
    constexpr uint8_t kAxiId = 0x07;
    constexpr uint64_t kAddr = 0x200;

    NsuConfig cfg{};
    cfg.src_id = kNsuSrcId;
    cfg.port_id = kNsuPortId;
    cfg.port_params.aw_queue_depth = 16;
    cfg.port_params.w_queue_depth = 16;
    cfg.port_params.ar_queue_depth = 16;
    cfg.port_params.b_queue_depth = 16;
    cfg.port_params.r_queue_depth = 16;
    cfg.port_params.meta_buffer_max_outstanding = 32;
    cfg.port_params.meta_buffer_max_unique_ids = axi::NOC_ID_SPACE;
    NsuStandalone nsu(cfg);

    Flit aw_flit;
    aw_flit.set_header_field("axi_ch", ::ni::AXI_CH_NarrowAw);
    aw_flit.set_header_field("src_id", kRequesterSrcId);
    aw_flit.set_header_field("dst_id", kNsuSrcId);
    // Addressed to this NSU's own port; a wrong value here aborts at ingress.
    aw_flit.set_header_field("dst_port_id", kNsuPortId);
    aw_flit.set_header_field("src_port_id", 1);
    aw_flit.set_header_field("vc_id", 0);
    aw_flit.set_header_field("flit_tail", 0);
    aw_flit.set_header_field("ordering_req", 0);
    aw_flit.set_header_field("ordering_tag", 0);
    aw_flit.set_payload_field("AW", "awid", kAxiId);
    aw_flit.set_payload_field("AW", "awaddr", kAddr);
    aw_flit.set_payload_field("AW", "awlen", 0);
    aw_flit.set_payload_field("AW", "awsize", 2);
    aw_flit.set_payload_field("AW", "awburst", static_cast<uint64_t>(axi::Burst::INCR));
    nsu.inject_req_flit(aw_flit);

    Flit w_flit;
    w_flit.set_header_field("axi_ch", ::ni::AXI_CH_NarrowW);
    w_flit.set_header_field("src_id", kRequesterSrcId);
    w_flit.set_header_field("dst_id", kNsuSrcId);
    w_flit.set_header_field("dst_port_id", kNsuPortId);
    w_flit.set_header_field("src_port_id", 1);
    w_flit.set_header_field("vc_id", 0);
    w_flit.set_header_field("flit_tail", 1);
    w_flit.set_payload_field("NARROW_W", "wlast", 1);
    w_flit.set_payload_field("NARROW_W", "wstrb", 0xF);
    nsu.inject_req_flit(w_flit);

    std::optional<axi::AwBeat> aw_out;
    std::optional<axi::WBeat> w_out;
    for (int i = 0; i < 16 && !(aw_out && w_out); ++i) {
        nsu.tick();
        if (!aw_out) aw_out = nsu.axi_master_port().pop_aw();
        if (!w_out) w_out = nsu.axi_master_port().pop_w();
    }
    ASSERT_TRUE(aw_out.has_value()) << "Nsu never surfaced AW beat to AxiMasterPort";
    ASSERT_TRUE(w_out.has_value()) << "Nsu never surfaced W beat to AxiMasterPort";

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
    EXPECT_EQ(b_flit->get_header_field("dst_port_id"), 1u)
        << "B should be addressed back to the port that issued the AW";
    EXPECT_EQ(b_flit->get_header_field("src_port_id"), kNsuPortId)
        << "the B should name this NSU's own port, from cfg.port_id";
}
