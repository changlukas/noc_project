// S3a T4 NI restructure — NSU DAT face tests.
//
// S3a gave Nsu a second (DAT) NoC face alongside REQ/RSP (stage design §5):
// a second Depacketize ingress for DAT (AW/W) demuxing into the SAME shared
// s1_aw_/s1_w_/s1_ar_ registers as REQ, and a standalone VcArbiter for DAT
// egress (R) with no wormhole arbiter in front (single input -- a 1-input
// wormhole arbiter would be dead code). Steering (T6) has not moved yet —
// Packetize still emits everything on RSP — so this file drives the DAT
// face directly via NsuStandalone's queue-backed mocks (the same pattern
// nsu_wrap.hpp will use for the real DPI boundary, T5).
//
// Covers: DAT face push/pop via mocks, per-network face independence
// (backpressure one face, the other still flows), and that the REQ and DAT
// ingresses drain independently within a single Nsu::tick() when they don't
// contend for the same shared S1 register.
#include "axi/types.hpp"
#include "common/scenario.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include "nsu/nsu_standalone.hpp"
#include <cstdint>
#include <gtest/gtest.h>

using ni::cmodel::Flit;
using ni::cmodel::nsu::NsuConfig;
using ni::cmodel::nsu::NsuStandalone;
namespace axi = ni::cmodel::axi;

namespace {

constexpr uint8_t kNsuSrcId = 0x34;
constexpr uint8_t kRequesterSrcId = 0x12;

NsuConfig make_cfg(uint8_t src_id) {
    NsuConfig cfg{};
    cfg.src_id = src_id;
    cfg.port_params.aw_queue_depth = 16;
    cfg.port_params.w_queue_depth = 16;
    cfg.port_params.ar_queue_depth = 16;
    cfg.port_params.b_queue_depth = 16;
    cfg.port_params.r_queue_depth = 16;
    cfg.port_params.meta_buffer_max_outstanding = 32;
    cfg.port_params.meta_buffer_max_unique_ids = 256;
    return cfg;
}

Flit make_data_aw(uint8_t awid, uint64_t addr) {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_DataAw);
    f.set_header_field("src_id", kRequesterSrcId);
    f.set_header_field("dst_id", kNsuSrcId);
    f.set_header_field("vc_id", 0);
    f.set_header_field("flit_tail", 0);  // AW opens wormhole packet
    f.set_header_field("ordering_req", 0);
    f.set_header_field("ordering_tag", 0);
    f.set_payload_field("AW", "awid", awid);
    f.set_payload_field("AW", "awaddr", addr);
    f.set_payload_field("AW", "awlen", 0);
    f.set_payload_field("AW", "awsize", 6);
    f.set_payload_field("AW", "awburst", static_cast<uint64_t>(axi::Burst::INCR));
    return f;
}

Flit make_data_w() {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_DataW);
    f.set_header_field("dst_id", kNsuSrcId);
    f.set_header_field("vc_id", 0);
    f.set_header_field("flit_tail", 1);  // wlast closes wormhole packet
    f.set_payload_field("DATA_W", "wlast", 1);
    f.set_payload_field("DATA_W", "wstrb", 0xFFu);
    return f;
}

Flit make_narrow_ar(uint8_t arid, uint64_t addr) {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_NarrowAr);
    f.set_header_field("src_id", kRequesterSrcId);
    f.set_header_field("dst_id", kNsuSrcId);
    f.set_header_field("vc_id", 0);
    f.set_header_field("flit_tail", 1);
    f.set_payload_field("AR", "arid", arid);
    f.set_payload_field("AR", "araddr", addr);
    f.set_payload_field("AR", "arsize", 2);
    f.set_payload_field("AR", "arburst", static_cast<uint64_t>(axi::Burst::INCR));
    return f;
}

Flit make_data_r_flit(uint8_t rid) {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_DataR);
    f.set_header_field("src_id", kNsuSrcId);
    f.set_header_field("dst_id", kRequesterSrcId);
    f.set_header_field("vc_id", 0);
    f.set_header_field("flit_tail", 1);
    f.set_payload_field("DATA_R", "rid", rid);
    f.set_payload_field("DATA_R", "rresp", static_cast<uint64_t>(axi::Resp::OKAY));
    f.set_payload_field("DATA_R", "rlast", 1);
    return f;
}

}  // namespace

// DAT face push/pop via mocks: push a synthetic DataR flit directly into
// dat_vc_arbiter() (no Packetize involved -- steering is T6; no wormhole
// arbiter in front either, per §5.2's "single input" ruling), tick, drain
// via pop_dat_rsp_flit().
TEST(NsuDatFace, EgressPushPopViaMock) {
    SCENARIO(
        "NSU DAT egress: push a DataR flit directly into dat_vc_arbiter() (mocking what "
        "Packetize will do post-T6). pop_dat_rsp_flit() must return it with axi_ch/rid intact.");

    NsuStandalone nsu(make_cfg(kNsuSrcId));
    ASSERT_TRUE(nsu.nsu().dat_vc_arbiter().push_flit(make_data_r_flit(0x09)));

    std::optional<Flit> out;
    for (int t = 0; t < 8 && !out; ++t) {
        nsu.tick();
        out = nsu.pop_dat_rsp_flit();
    }
    ASSERT_TRUE(out.has_value()) << "DAT egress never produced the R flit";
    EXPECT_EQ(out->get_header_field("axi_ch"), static_cast<uint64_t>(ni::AXI_CH_DataR));
    EXPECT_EQ(out->get_payload_field("DATA_R", "rid"), 0x09u);
}

// Per-network face independence: block RSP's credit (seed=0) and confirm a
// DAT flit pushed directly into dat_vc_arbiter() still drains -- the two are
// independent VcArbiter instances with independent downstream sinks.
TEST(NsuDatFace, RspBackpressureDoesNotStallDat) {
    SCENARIO(
        "NSU: RSP face credit-blocked (seed=0) while a DataR flit is pushed directly into the "
        "DAT face. The DAT face must still drain to pop_dat_rsp_flit().");

    NsuStandalone nsu(make_cfg(kNsuSrcId));
    nsu.enable_rsp_ready_track();  // ready defaults false: RSP face blocked from tick 0
    ASSERT_FALSE(nsu.rsp_credit_avail());

    ASSERT_TRUE(nsu.nsu().dat_vc_arbiter().push_flit(make_data_r_flit(0x01)));
    int dat_drained = 0;
    for (int t = 0; t < 8; ++t) {
        nsu.tick();
        while (nsu.pop_dat_rsp_flit()) ++dat_drained;
    }
    EXPECT_EQ(dat_drained, 1) << "DAT face must drain despite RSP being blocked";
}

// Symmetric case: block DAT's credit and confirm a normal write round-trip
// (AW+W in via REQ ingress, B out via RSP egress -- default credit-OFF, the
// ready/valid predicate) still completes.
TEST(NsuDatFace, DatBackpressureDoesNotStallRsp) {
    SCENARIO(
        "NSU: DAT face credit-blocked (seed=0) while a normal AW+W write round-trips through the "
        "REQ ingress and RSP egress. RSP (default credit-OFF) must still produce the B flit.");

    NsuStandalone nsu(make_cfg(kNsuSrcId));
    nsu.enable_dat_noc_credit(/*seed=*/0);
    ASSERT_FALSE(nsu.dat_rsp_credit_avail());

    constexpr uint8_t kAxiId = 0x03;
    nsu.inject_req_flit(make_data_aw(kAxiId, 0x100));
    nsu.inject_req_flit(make_data_w());

    std::optional<axi::AwBeat> aw_out;
    std::optional<axi::WBeat> w_out;
    for (int t = 0; t < 16 && !(aw_out && w_out); ++t) {
        nsu.tick();
        if (!aw_out) aw_out = nsu.axi_master_port().pop_aw();
        if (!w_out) w_out = nsu.axi_master_port().pop_w();
    }
    ASSERT_TRUE(aw_out.has_value());
    ASSERT_TRUE(w_out.has_value());

    axi::BBeat b{};
    b.id = kAxiId;
    b.resp = axi::Resp::OKAY;
    ASSERT_TRUE(nsu.axi_master_port().push_b(b));

    bool saw_b = false;
    for (int t = 0; t < 32 && !saw_b; ++t) {
        nsu.tick();
        if (nsu.pop_rsp_flit()) saw_b = true;
    }
    EXPECT_TRUE(saw_b) << "RSP face must produce the B flit despite DAT being blocked";
}

// DAT ingress: inject DataAw+DataW via inject_dat_req_flit() (the second
// physical ingress) instead of inject_req_flit(). They must demux into the
// SAME s1_aw_/s1_w_ registers and surface at AxiMasterPort exactly as a
// REQ-ingress AW/W would.
TEST(NsuDatFace, DatIngressDeliversDataAwDataW) {
    SCENARIO(
        "NSU: inject a DataAw+DataW pair via inject_dat_req_flit() (the DAT ingress). Both must "
        "surface at axi_master_port().pop_aw()/pop_w(), proving the DAT ingress shares the same "
        "S1 registers as REQ.");

    NsuStandalone nsu(make_cfg(kNsuSrcId));
    nsu.inject_dat_req_flit(make_data_aw(0x06, 0x200));
    nsu.inject_dat_req_flit(make_data_w());

    std::optional<axi::AwBeat> aw_out;
    std::optional<axi::WBeat> w_out;
    for (int t = 0; t < 16 && !(aw_out && w_out); ++t) {
        nsu.tick();
        if (!aw_out) aw_out = nsu.axi_master_port().pop_aw();
        if (!w_out) w_out = nsu.axi_master_port().pop_w();
    }
    ASSERT_TRUE(aw_out.has_value()) << "DAT ingress never surfaced the AW beat";
    ASSERT_TRUE(w_out.has_value()) << "DAT ingress never surfaced the W beat";
    EXPECT_EQ(aw_out->id, 0x06u);
    EXPECT_TRUE(w_out->last);
}

// Tick-order preservation: an AR on the REQ ingress and an AW+W pair on the
// DAT ingress, injected the same cycle, land in DIFFERENT S1 registers
// (s1_ar_ vs s1_aw_/s1_w_) so neither ingress's per-network `pending` stash
// blocks the other -- both must be observable within a few ticks.
TEST(NsuDatFace, ReqArAndDatAwWDrainIndependently) {
    SCENARIO(
        "NSU: inject a NarrowAr via the REQ ingress and a DataAw+DataW pair via the DAT ingress "
        "in the same cycle. All three land in independent S1 registers (AR vs AW vs W), so a few "
        "ticks must surface all of them -- proving the two physical ingresses do not contend.");

    NsuStandalone nsu(make_cfg(kNsuSrcId));
    nsu.inject_req_flit(make_narrow_ar(0x07, 0x300));
    nsu.inject_dat_req_flit(make_data_aw(0x08, 0x400));
    nsu.inject_dat_req_flit(make_data_w());

    std::optional<axi::ArBeat> ar_out;
    std::optional<axi::AwBeat> aw_out;
    std::optional<axi::WBeat> w_out;
    for (int t = 0; t < 16 && !(ar_out && aw_out && w_out); ++t) {
        nsu.tick();
        if (!ar_out) ar_out = nsu.axi_master_port().pop_ar();
        if (!aw_out) aw_out = nsu.axi_master_port().pop_aw();
        if (!w_out) w_out = nsu.axi_master_port().pop_w();
    }
    EXPECT_TRUE(ar_out.has_value()) << "REQ-ingress AR never surfaced";
    EXPECT_TRUE(aw_out.has_value()) << "DAT-ingress AW never surfaced";
    EXPECT_TRUE(w_out.has_value()) << "DAT-ingress W never surfaced";
}
