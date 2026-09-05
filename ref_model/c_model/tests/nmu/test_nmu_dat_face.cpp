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
#include "common/dat_flit_builders.hpp"
#include "flit.hpp"
#include "ni/channel_mode.hpp"
#include "ni_flit_constants.h"
#include "nmu/nmu_standalone.hpp"
#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <vector>

using ni::cmodel::Flit;
using ni::cmodel::ni::ChannelMode;
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

using ni::cmodel::testing::make_data_aw;
using ni::cmodel::testing::make_data_r;
using ni::cmodel::testing::make_data_w;

void expect_normalized_data_request_path(ChannelMode mode, bool use_dat) {
    NmuStandalone nmu(make_cfg(0x12));
    nmu.set_channel_mode(mode);

    axi::AwBeat aw{};
    aw.id = 0x05;
    aw.addr = 0x1000;
    aw.len = 1;
    aw.size = 3;
    aw.burst = axi::Burst::INCR;
    axi::WBeat w0{};
    w0.last = false;
    w0.strb = 0xFF;
    axi::WBeat w1{};
    w1.last = true;
    w1.strb = 0xFF00;
    for (std::size_t i = 0; i < axi::NARROW_DATA_BYTES; ++i) {
        w0.data[i] = static_cast<uint8_t>(0xD0 + i);
        w1.data[axi::NARROW_DATA_BYTES + i] = static_cast<uint8_t>(0xE0 + i);
    }
    axi::ArBeat ar{};
    ar.id = 0x06;
    ar.addr = 0x200;
    ar.len = 0;
    ar.size = 3;
    ar.burst = axi::Burst::INCR;

    ASSERT_TRUE(nmu.axi_slave_port().push_aw(aw));
    ASSERT_TRUE(nmu.axi_slave_port().push_w(w0));
    ASSERT_TRUE(nmu.axi_slave_port().push_w(w1));
    ASSERT_TRUE(nmu.axi_slave_port().push_ar(ar));

    std::vector<Flit> req_flits;
    std::vector<Flit> dat_flits;
    for (int t = 0; t < 64 && req_flits.size() + dat_flits.size() < 4; ++t) {
        nmu.tick();
        while (auto f = nmu.pop_req_flit()) req_flits.push_back(*f);
        while (auto f = nmu.pop_dat_req_flit()) dat_flits.push_back(*f);
    }

    const auto& active = use_dat ? dat_flits : req_flits;
    const auto& idle = use_dat ? req_flits : dat_flits;
    ASSERT_EQ(active.size(), 4u);
    EXPECT_TRUE(idle.empty());

    bool saw_aw = false;
    std::size_t w_index = 0;
    bool saw_ar = false;
    for (const auto& f : active) {
        const auto axi_ch = f.get_header_field("axi_ch");
        saw_aw |= axi_ch == ::ni::AXI_CH_DataAw;
        saw_ar |= axi_ch == ::ni::AXI_CH_DataAr;
        if (axi_ch != ::ni::AXI_CH_DataW) continue;
        EXPECT_EQ(f.get_payload_field("NARROW_W", "wstrb"), 0xFFu);
        std::array<uint8_t, axi::NARROW_DATA_BYTES> data{};
        f.get_payload_bytes("NARROW_W", "wdata", data.data(),
                            ::ni::width::NOC_NARROW_DATA_WIDTH);
        const uint8_t base = w_index == 0 ? 0xD0 : 0xE0;
        for (std::size_t i = 0; i < data.size(); ++i)
            EXPECT_EQ(data[i], static_cast<uint8_t>(base + i));
        ++w_index;
    }
    EXPECT_TRUE(saw_aw);
    EXPECT_EQ(w_index, 2u);
    EXPECT_TRUE(saw_ar);
}

}  // namespace

TEST(NmuDatFace, TwoChannel64SteersDataRequestsToReqWithNarrowWPayload) {
    expect_normalized_data_request_path(ChannelMode::TwoChannel64, /*use_dat=*/false);
}

TEST(NmuDatFace, ThreeChannel64SteersDataRequestsToDatWithNarrowWPayload) {
    expect_normalized_data_request_path(ChannelMode::ThreeChannel64, /*use_dat=*/true);
}

namespace {

void expect_normalized_data_r_reanchor(::ni::cmodel::ni::ChannelMode mode,
                                       bool use_dat_ingress) {
    constexpr uint8_t kSrcId = 0x12;
    constexpr uint8_t kArId = 0x07;
    NmuStandalone nmu(make_cfg(kSrcId));
    nmu.set_channel_mode(mode);

    axi::ArBeat ar{};
    ar.id = kArId;
    ar.addr = 0x1000;
    ar.len = 1;
    ar.size = 3;
    ar.burst = axi::Burst::INCR;
    ASSERT_TRUE(nmu.axi_slave_port().push_ar(ar));
    for (int t = 0; t < 32; ++t) {
        nmu.tick();
        while (nmu.pop_req_flit()) {
        }
        while (nmu.pop_dat_req_flit()) {
        }
    }

    for (uint8_t beat = 0; beat < 2; ++beat) {
        Flit f;
        f.set_header_field("axi_ch", ::ni::AXI_CH_DataR);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("flit_tail", 1);
        f.set_payload_field("NARROW_R", "rid", kArId);
        f.set_payload_field("NARROW_R", "rlast", beat == 1 ? 1u : 0u);
        std::array<uint8_t, axi::NARROW_DATA_BYTES> lane{};
        for (std::size_t i = 0; i < lane.size(); ++i)
            lane[i] = static_cast<uint8_t>((beat == 0 ? 0xA0 : 0xB0) + i);
        f.set_payload_bytes("NARROW_R", "rdata", lane.data(), ::ni::width::NOC_NARROW_DATA_WIDTH);
        if (use_dat_ingress)
            nmu.inject_dat_rsp_flit(f);
        else
            nmu.inject_rsp_flit(f);
    }

    for (uint8_t beat = 0; beat < 2; ++beat) {
        std::optional<axi::RBeat> out;
        for (int t = 0; t < 32 && !out; ++t) {
            nmu.tick();
            out = nmu.axi_slave_port().pop_r();
        }
        ASSERT_TRUE(out.has_value());
        EXPECT_EQ(out->id, kArId);
        EXPECT_EQ(out->last, beat == 1);
        const std::size_t offset = beat * axi::NARROW_DATA_BYTES;
        for (std::size_t i = 0; i < out->data.size(); ++i) {
            const uint8_t expected = i >= offset && i < offset + axi::NARROW_DATA_BYTES
                                         ? static_cast<uint8_t>((beat == 0 ? 0xA0 : 0xB0) + i - offset)
                                         : 0;
            EXPECT_EQ(out->data[i], expected) << "beat=" << unsigned(beat) << " byte=" << i;
        }
    }
}

}  // namespace

TEST(NmuDatFace, TwoChannel64ReanchorsConsecutiveDataRBeatsToAddressedAxiLanes) {
    expect_normalized_data_r_reanchor(::ni::cmodel::ni::ChannelMode::TwoChannel64,
                                      /*use_dat_ingress=*/false);
}

TEST(NmuDatFace, ThreeChannel64ReanchorsConsecutiveDataRBeatsToAddressedAxiLanes) {
    expect_normalized_data_r_reanchor(::ni::cmodel::ni::ChannelMode::ThreeChannel64,
                                      /*use_dat_ingress=*/true);
}

// DAT face push/pop via mocks: push AW then W directly into
// dat_wormhole_arbiter().input(0/1) (bypassing Packetize, to isolate the
// arbiter's own mechanics from steering), tick, and drain via
// pop_dat_req_flit(). The {AW,W} lock must hold on the DAT pair exactly as it
// does on REQ's (same WormholeArbiter class, per-network instance).
TEST(NmuDatFace, EgressPushPopViaMocksPreservesAwWOrder) {
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
    EXPECT_EQ(aw_out->get_header_field("axi_ch"), static_cast<uint64_t>(::ni::AXI_CH_DataAw));
    EXPECT_EQ(aw_out->get_payload_field("AW", "awid"), 0x05u);
    EXPECT_EQ(w_out->get_header_field("axi_ch"), static_cast<uint64_t>(::ni::AXI_CH_DataW));
}

// Per-network face independence: block the REQ face's credit (seed=0, no
// receive_credit) and confirm the DAT face -- a fully independent
// WormholeArbiter+VcAllocator pair with its own downstream sink -- still
// drains normally. REQ backpressure must not reach DAT.
TEST(NmuDatFace, ReqBackpressureDoesNotStallDat) {
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
