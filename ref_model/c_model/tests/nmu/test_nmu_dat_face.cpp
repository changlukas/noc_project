// S3a T4 NI restructure — NMU DAT face tests.
//
// S3a gave Nmu a second (DAT) NoC face alongside REQ/RSP (stage design §5):
// a per-network WormholeArbiter+VcAllocator pair for DAT egress (AW/W) and a
// second Depacketize ingress for DAT (R). Packetize now steers Data-class
// AW/W/R here for real (T6) — this file instead drives the DAT face directly
// via NmuStandalone's queue-backed mocks, exercising the arbiter/ingress
// mechanics in isolation from Packetize (the same pattern nmu_wrap.hpp uses
// at the real DPI boundary, T5).
//
// Covers: DAT face push/pop via mocks, per-network face independence
// (backpressure one face, the other still flows), and that the two Nmu::tick()
// ingresses (RSP + DAT) drain independently within a single tick.
#include "axi/types.hpp"
#include "common/scenario.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include "nmu/nmu_standalone.hpp"
#include <cstdint>
#include <gtest/gtest.h>

using ni::cmodel::Flit;
using ni::cmodel::nmu::NmuConfig;
using ni::cmodel::nmu::NmuStandalone;
using ni::cmodel::nmu::addr_trans::SamTable;
namespace axi = ni::cmodel::axi;

namespace {

NmuConfig make_cfg(uint8_t src_id) {
    NmuConfig cfg{};
    cfg.src_id = src_id;
    cfg.sam = SamTable::uniform(16, 16, 0x100000000ull);
    cfg.port_params.aw_queue_depth = 16;
    cfg.port_params.w_queue_depth = 16;
    cfg.port_params.ar_queue_depth = 16;
    cfg.port_params.b_queue_depth = 16;
    cfg.port_params.r_queue_depth = 16;
    cfg.port_params.depkt_b_q_depth = 16;
    cfg.port_params.depkt_r_q_depth = 16;
    return cfg;
}

// Data-class AW opening a 1-beat wormhole packet (flit_tail=0, per the AW/W
// pairing lock -- WormholeArbiter's own comment: "AW=0, W=wlast").
Flit make_data_aw(uint8_t awid, uint8_t dst_id) {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_DataAw);
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("flit_tail", 0);
    f.set_payload_field("AW", "awid", awid);
    f.set_payload_field("AW", "awaddr", 0x100);
    f.set_payload_field("AW", "awlen", 0);
    f.set_payload_field("AW", "awsize", 6);
    f.set_payload_field("AW", "awburst", static_cast<uint64_t>(axi::Burst::INCR));
    return f;
}

Flit make_data_w(uint8_t dst_id) {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_DataW);
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("flit_tail", 1);  // wlast closes the wormhole packet
    f.set_payload_field("DATA_W", "wlast", 1);
    f.set_payload_field("DATA_W", "wstrb", 0xFFu);
    return f;
}

Flit make_data_r(uint8_t rid, uint8_t src_id, uint8_t dst_id) {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_DataR);
    f.set_header_field("src_id", src_id);
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("vc_id", 0);
    f.set_header_field("flit_tail", 1);
    f.set_header_field("ordering_tag", 0);
    f.set_header_field("ordering_req", 0);
    f.set_payload_field("DATA_R", "rid", rid);
    f.set_payload_field("DATA_R", "rresp", static_cast<uint64_t>(axi::Resp::OKAY));
    f.set_payload_field("DATA_R", "ruser", 0);
    f.set_payload_field("DATA_R", "rlast", 1);
    return f;
}

}  // namespace

// DAT face push/pop via mocks: push AW then W directly into
// dat_wormhole_arbiter().input(0/1) (bypassing Packetize, to isolate the
// arbiter's own mechanics from steering), tick, and drain via
// pop_dat_req_flit(). The {AW,W} lock must hold on the DAT pair exactly as it
// does on REQ's (same WormholeArbiter class, per-network instance).
TEST(NmuDatFace, EgressPushPopViaMocksPreservesAwWOrder) {
    SCENARIO(
        "NMU DAT egress: push DataAw into dat_wormhole_arbiter().input(0), DataW into input(1) "
        "(bypassing Packetize's own real steering, to test the arbiter in isolation). AW drains "
        "first (locks {AW,W}), then W (unlocks). pop_dat_req_flit() must return them in that "
        "order with axi_ch preserved.");

    NmuStandalone nmu(make_cfg(0x12));
    ASSERT_TRUE(nmu.nmu().dat_wormhole_arbiter().input(0).push_flit(make_data_aw(0x05, 0x01)));
    ASSERT_TRUE(nmu.nmu().dat_wormhole_arbiter().input(1).push_flit(make_data_w(0x01)));

    std::optional<Flit> aw_out, w_out;
    for (int t = 0; t < 8 && !(aw_out && w_out); ++t) {
        nmu.tick();
        if (!aw_out) aw_out = nmu.pop_dat_req_flit();
        if (!w_out && aw_out) w_out = nmu.pop_dat_req_flit();
    }
    ASSERT_TRUE(aw_out.has_value()) << "DAT egress never produced the AW flit";
    ASSERT_TRUE(w_out.has_value()) << "DAT egress never produced the W flit";
    EXPECT_EQ(aw_out->get_header_field("axi_ch"), static_cast<uint64_t>(ni::AXI_CH_DataAw));
    EXPECT_EQ(aw_out->get_payload_field("AW", "awid"), 0x05u);
    EXPECT_EQ(w_out->get_header_field("axi_ch"), static_cast<uint64_t>(ni::AXI_CH_DataW));
}

// Per-network face independence: block the REQ face's credit (seed=0, no
// receive_credit) and confirm the DAT face -- a fully independent
// WormholeArbiter+VcAllocator pair with its own downstream sink -- still
// drains normally. REQ backpressure must not reach DAT.
TEST(NmuDatFace, ReqBackpressureDoesNotStallDat) {
    SCENARIO(
        "NMU: REQ face credit-blocked (seed=0) while a DAT AW+W pair is pushed directly. The DAT "
        "face must still drain to pop_dat_req_flit() -- proving the two per-network arbiter pairs "
        "do not share backpressure state.");

    NmuStandalone nmu(make_cfg(0x12));
    nmu.enable_req_ready_track();  // ready defaults false: REQ face blocked from tick 0
    ASSERT_FALSE(nmu.req_credit_avail());

    ASSERT_TRUE(nmu.nmu().dat_wormhole_arbiter().input(0).push_flit(make_data_aw(0x01, 0x01)));
    ASSERT_TRUE(nmu.nmu().dat_wormhole_arbiter().input(1).push_flit(make_data_w(0x01)));

    int dat_drained = 0;
    for (int t = 0; t < 8; ++t) {
        nmu.tick();
        while (nmu.pop_dat_req_flit()) ++dat_drained;
    }
    EXPECT_EQ(dat_drained, 2) << "DAT face must drain both flits despite REQ being blocked";
}

// Symmetric case: block the DAT face's credit and confirm REQ still flows
// (an AR pushed through the real AxiSlavePort -> Rob -> Packetize -> REQ
// wormhole/VC path, exactly as today).
TEST(NmuDatFace, DatBackpressureDoesNotStallReq) {
    SCENARIO(
        "NMU: DAT face credit-blocked (seed=0) while a normal AR is issued through the AXI slave "
        "port. The REQ face (default credit-OFF, i.e. the ready/valid predicate -- no counter "
        "consulted) must still produce the AR flit.");

    NmuStandalone nmu(make_cfg(0x12));
    nmu.enable_dat_noc_credit(/*seed=*/0);
    ASSERT_FALSE(nmu.dat_req_credit_avail());

    axi::ArBeat ar{};
    ar.id = 0x03;
    ar.addr = 0x100;
    ar.len = 0;
    ar.size = 2;
    ar.burst = axi::Burst::INCR;
    ASSERT_TRUE(nmu.axi_slave_port().push_ar(ar));

    bool saw_ar = false;
    for (int t = 0; t < 32 && !saw_ar; ++t) {
        nmu.tick();
        if (auto f = nmu.pop_req_flit()) saw_ar = true;
    }
    EXPECT_TRUE(saw_ar) << "REQ face must produce the AR flit despite DAT being blocked";
}

// DAT ingress (RSP-side second ingress, S3a T4): inject a DataR flit via
// inject_dat_rsp_flit() instead of the normal RSP mock, and confirm it
// surfaces at axi_slave_port().pop_r() -- proving the DAT ingress shares
// nmu::Depacketize's b_q_/r_q_ output queues with the RSP ingress and the
// RoBless drain path (default RobMode) picks it up identically.
TEST(NmuDatFace, DatIngressDeliversDataRToAxiSlavePort) {
    SCENARIO(
        "NMU: issue an AR (drains on REQ, unaffected by S3a T4), then inject the matching R "
        "response via inject_dat_rsp_flit() (the DAT ingress) instead of inject_rsp_flit() (RSP). "
        "The beat must still surface at axi_slave_port().pop_r().");

    constexpr uint8_t kSrcId = 0x12;
    constexpr uint8_t kArId = 0x07;
    NmuStandalone nmu(make_cfg(kSrcId));

    axi::ArBeat ar{};
    ar.id = kArId;
    ar.addr = 0x100;
    ar.len = 0;
    ar.size = 2;
    ar.burst = axi::Burst::INCR;
    ASSERT_TRUE(nmu.axi_slave_port().push_ar(ar));
    for (int t = 0; t < 32; ++t) {
        nmu.tick();
        while (nmu.pop_req_flit()) {
        }
    }

    nmu.inject_dat_rsp_flit(make_data_r(kArId, /*src_id=*/0x00, /*dst_id=*/kSrcId));

    std::optional<axi::RBeat> r_out;
    for (int t = 0; t < 32 && !r_out; ++t) {
        nmu.tick();
        r_out = nmu.axi_slave_port().pop_r();
    }
    ASSERT_TRUE(r_out.has_value()) << "DAT ingress R flit never surfaced at AxiSlavePort";
    EXPECT_EQ(r_out->id, kArId);
    EXPECT_TRUE(r_out->last);
}

// Tick-order preservation: injecting one flit on the RSP ingress and one on
// the DAT ingress in the SAME cycle, both must be consumed within that one
// Nmu::tick() call (Depacketize::tick() drains both ingresses per S3a §5.4 —
// neither one's pending_ stash blocks the other).
TEST(NmuDatFace, RspAndDatIngressesDrainIndependentlyInOneTick) {
    SCENARIO(
        "NMU: outstanding AR for id=1 and id=2, then inject the two R responses in the same "
        "cycle -- id=1 via inject_rsp_flit (RSP ingress), id=2 via inject_dat_rsp_flit (DAT "
        "ingress). A single Depacketize.tick() must drain both ingresses (each up to their own "
        "queue depth), matching the class comment's independent-pending_ claim.");

    constexpr uint8_t kSrcId = 0x12;
    NmuStandalone nmu(make_cfg(kSrcId));

    axi::ArBeat ar1{}, ar2{};
    ar1.id = 0x01;
    ar1.addr = 0x100;
    ar1.len = 0;
    ar1.size = 2;
    ar1.burst = axi::Burst::INCR;
    ar2 = ar1;
    ar2.id = 0x02;
    ASSERT_TRUE(nmu.axi_slave_port().push_ar(ar1));
    ASSERT_TRUE(nmu.axi_slave_port().push_ar(ar2));
    for (int t = 0; t < 32; ++t) {
        nmu.tick();
        while (nmu.pop_req_flit()) {
        }
    }

    nmu.inject_rsp_flit(make_data_r(0x01, /*src_id=*/0x00, /*dst_id=*/kSrcId));
    nmu.inject_dat_rsp_flit(make_data_r(0x02, /*src_id=*/0x00, /*dst_id=*/kSrcId));

    std::optional<axi::RBeat> r1, r2;
    for (int t = 0; t < 32 && !(r1 && r2); ++t) {
        nmu.tick();
        if (auto r = nmu.axi_slave_port().pop_r()) {
            if (r->id == 0x01)
                r1 = r;
            else
                r2 = r;
        }
    }
    EXPECT_TRUE(r1.has_value()) << "RSP-ingress R never surfaced";
    EXPECT_TRUE(r2.has_value()) << "DAT-ingress R never surfaced";
}
