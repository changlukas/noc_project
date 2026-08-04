#include "nsu/vc_arbiter.hpp"
#include "nsu/nsu.hpp"
#include "ni/virtual_network.hpp"
#include "common/channel_model.hpp"
#include "common/scenario.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include <gtest/gtest.h>
#include <array>
#include <vector>

using ni::cmodel::Flit;
using ni::cmodel::nsu::VcArbiter;
using ni::cmodel::testing::ChannelModel;

namespace {

Flit make_rsp_flit(uint8_t axi_ch, uint8_t initial_vc = 0, uint8_t id = 0, uint64_t rlast = 1,
                   uint8_t dst_id = 0x12, uint8_t ordering_req = 0) {
    Flit f;
    f.set_header_field("axi_ch", axi_ch);
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("vc_id", initial_vc);
    f.set_header_field("src_id", 0x34);
    f.set_header_field("flit_tail", 1);
    f.set_header_field("ordering_req", ordering_req);
    if (axi_ch == ni::AXI_CH_NarrowR) {
        f.set_payload_field("NARROW_R", "rid", id);
        f.set_payload_field("NARROW_R", "rlast", rlast);
    } else if (axi_ch == ni::AXI_CH_NarrowB) {
        f.set_payload_field("B", "bid", id);
    }
    return f;
}

// Returns the VC a pushed flit landed on (0xFF on push failure).
uint8_t push_and_vc(VcArbiter& arb, ChannelModel& /*noc*/, const Flit& flit) {
    std::array<std::size_t, VcArbiter::NUM_VC_MAX> before{};
    for (uint8_t v = 0; v < VcArbiter::NUM_VC_MAX; ++v) before[v] = arb.pending_size(v);
    if (!arb.push_flit(flit)) return 0xFF;
    for (uint8_t v = 0; v < VcArbiter::NUM_VC_MAX; ++v) {
        if (arb.pending_size(v) > before[v]) return v;
    }
    return 0xFF;
}

}  // namespace

// ---------------------------------------------------------------------------
// Parameterized fixture — NUM_VC ∈ {1, 2, 4, 8}
// ---------------------------------------------------------------------------

class NsuVcArbParam : public ::testing::TestWithParam<std::size_t> {};

// ReadWriteSplit: B -> write_rsp_vc=0, R -> read_rsp_vc=1.
// Requires num_vc ≥ 2 (distinct write/read VCs).
TEST_P(NsuVcArbParam, Nsu_ReadWriteSplit_B_R_GoSeparateVcs) {
    const std::size_t num_vc = GetParam();
    if (num_vc < 2) GTEST_SKIP() << "needs NUM_VC >= 2";

    SCENARIO("NSU VcArbiter read/write VC split: B -> write_rsp_vc=0, R -> read_rsp_vc=1");
    ChannelModel noc(/*req*/ 32, /*rsp*/ 32);
    auto arb = VcArbiter::read_write_split(noc.rsp_out(), num_vc, 0, 1);

    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowB)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR)));
    EXPECT_EQ(arb.pending_size(0), 1u);
    EXPECT_EQ(arb.pending_size(1), 1u);

    arb.tick();
    arb.tick();
    auto f0 = noc.rsp_in().pop_flit();
    ASSERT_TRUE(f0.has_value());
    auto f1 = noc.rsp_in().pop_flit();
    ASSERT_TRUE(f1.has_value());
    // Round-robin starts at 0 -> VC=0 (B) drains first, then VC=1 (R).
    EXPECT_EQ(f0->get_header_field("axi_ch"), ni::AXI_CH_NarrowB);
    EXPECT_EQ(f0->get_header_field("vc_id"), 0u);
    EXPECT_EQ(f1->get_header_field("axi_ch"), ni::AXI_CH_NarrowR);
    EXPECT_EQ(f1->get_header_field("vc_id"), 1u);
}

// Fixed VC id (same-destination bypass): rsp VC = vnet[(dst_id ^ id) % vnet.size()], zero state.
// Replaces the retired r_burst_vc_ array's burst-coherence role -- a burst's
// beats share (dst_id, rid) so they hash to the same vnet slot automatically.
//
// A multi-beat R burst (one rid) keeps every beat on its single mapped VC.
TEST(NsuVcArbiterVnets, RBurstStaysOnOneVc) {
    SCENARIO(
        "NSU vnets: all beats of one rid's R burst map to one VC (static map, not follow-state)");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split(noc.rsp_out(), /*num_vc=*/4,
                                           /*write_rsp_vcs=*/std::vector<uint8_t>{0, 1},
                                           /*read_rsp_vcs=*/std::vector<uint8_t>{2, 3});
    // 4-beat burst, dst_id=0x12, rid=0x05 -> (0x12^0x05)%2=1 -> read vnet[1]=VC3.
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 0)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 0)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 0)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 1)));
    // All four beats hash to VC=3; none on VC=2.
    EXPECT_EQ(arb.pending_size(3), 4u);
    EXPECT_EQ(arb.pending_size(2), 0u);
}

// Robbed (ordering_req=1) R still goes through the static map -- the map applies to
// ALL R regardless of ordering_req (design: burst coherence must hold even though
// the NMU RoB will reorder by ordering_tag; the map only needs to keep one burst's
// beats together, which a pure function of (dst_id,rid) does unconditionally).
TEST(NsuVcArbiterVnets, RobbedRBurstStaysOnOneVcToo) {
    SCENARIO("NSU vnets: ordering_req=1 R burst still maps every beat to one VC");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split(noc.rsp_out(), /*num_vc=*/4,
                                           /*write_rsp_vcs=*/std::vector<uint8_t>{0, 1},
                                           /*read_rsp_vcs=*/std::vector<uint8_t>{2, 3});
    ASSERT_TRUE(
        arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 0, 0x12, /*ordering_req=*/1)));
    ASSERT_TRUE(
        arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 1, 0x12, /*ordering_req=*/1)));
    EXPECT_EQ(arb.pending_size(3), 2u);
    EXPECT_EQ(arb.pending_size(2), 0u);
}

// Distinct rids (each a single-beat read) hash to different vnet slots -- the
// static map replaces round-robin spread with deterministic (dst,id) spread.
TEST(NsuVcArbiterVnets, DistinctRidsSpreadAcrossVnet) {
    SCENARIO("NSU vnets: distinct rids hash across read vnet {2,3} via static map");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split(noc.rsp_out(), /*num_vc=*/4,
                                           /*write_rsp_vcs=*/std::vector<uint8_t>{0, 1},
                                           /*read_rsp_vcs=*/std::vector<uint8_t>{2, 3});
    // dst_id=0x12 for all: rid5 -> (0x12^0x05)%2=1 -> VC3; rid6 -> (0x12^0x06)%2=0 -> VC2;
    // rid7 -> (0x12^0x07)%2=1 -> VC3.
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 1)));  // rid5 -> VC3
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x06, 1)));  // rid6 -> VC2
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x07, 1)));  // rid7 -> VC3
    EXPECT_EQ(arb.pending_size(2), 1u);                                         // rid6
    EXPECT_EQ(arb.pending_size(3), 2u);                                         // rid5 + rid7
}

// B uses the write vnet, R uses the read vnet (response-class separation).
TEST(NsuVcArbiterVnets, BUsesWriteVnetRUsesReadVnet) {
    SCENARIO("NSU vnets: B -> write vnet {0,1}, R -> read vnet {2,3}");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split(noc.rsp_out(), /*num_vc=*/4,
                                           /*write_rsp_vcs=*/std::vector<uint8_t>{0, 1},
                                           /*read_rsp_vcs=*/std::vector<uint8_t>{2, 3});
    // id=0x05, dst_id=0x12: B -> (0x12^0x05)%2=1 -> write vnet[1]=VC1;
    // R -> same hash -> read vnet[1]=VC3.
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowB, 0, 0x05, 1)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 1)));
    EXPECT_EQ(arb.pending_size(1), 1u);  // B on write vnet
    EXPECT_EQ(arb.pending_size(3), 1u);  // R on read vnet
}

// Two in-flight multi-beat R bursts (rid5, rid6) whose beats interleave must
// each stay on their own mapped VC -- the invariant per-(dst,id) hashing
// exists for (a single VC choice would misroute the interleaved beats).
TEST(NsuVcArbiterVnets, InterleavedMultiBeatBurstsStayOnTheirOwnVc) {
    SCENARIO("NSU vnets: interleaved rid5/rid6 multi-beat R bursts each map to one VC");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split(noc.rsp_out(), /*num_vc=*/4,
                                           /*write_rsp_vcs=*/std::vector<uint8_t>{0, 1},
                                           /*read_rsp_vcs=*/std::vector<uint8_t>{2, 3});
    // rid5 -> VC3, rid6 -> VC2 (same hash as DistinctRidsSpreadAcrossVnet above).
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 0)));  // rid5 -> VC3
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x06, 0)));  // rid6 -> VC2
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 0)));  // rid5 -> VC3
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x06, 0)));  // rid6 -> VC2
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 1)));  // rid5 last -> VC3
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x06, 1)));  // rid6 last -> VC2
    EXPECT_EQ(arb.pending_size(3), 3u);                                         // all rid5 beats
    EXPECT_EQ(arb.pending_size(2), 3u);                                         // all rid6 beats
}

// INVERTED from the retired pre-same-destination-bypass same-bid round-robin test: that
// test asserted B has no fixed VC and round-robins same-bid responses. The
// same-destination-bypass return-path map now fixes ordering_req=0 B the same way it fixes R --
// a same-(dst,bid) bypass stream must share one VC to stay in order at the NMU.
// ordering_req=1 (robbed) B keeps the old round-robin behavior; see the next test.
TEST(NsuVcArbiterVnets, SameBidSameDstBypassFixedVcId) {
    SCENARIO(
        "NSU vnets: ordering_req=0 same-(dst,bid) B responses fix to one VC "
        "(same-destination-bypass "
        "return-path map)");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split(noc.rsp_out(), /*num_vc=*/4,
                                           /*write_rsp_vcs=*/std::vector<uint8_t>{0, 1},
                                           /*read_rsp_vcs=*/std::vector<uint8_t>{2, 3});
    uint8_t a =
        push_and_vc(arb, noc, make_rsp_flit(ni::AXI_CH_NarrowB, 0, /*id=*/0x40, 1, 0x12, 0));
    uint8_t b =
        push_and_vc(arb, noc, make_rsp_flit(ni::AXI_CH_NarrowB, 0, /*id=*/0x40, 1, 0x12, 0));
    EXPECT_EQ(a, b) << "same (dst,bid) bypass responses must share one VC";
}

// ordering_req=1 (robbed) B is order-free at the NMU slot path, so it keeps
// round-robining the write vnet exactly as before the same-destination bypass.
TEST(NsuVcArbiterVnets, SameBidRobbedRoundRobinsWriteVnet) {
    SCENARIO("NSU vnets: ordering_req=1 same-bid B responses still round-robin the write vnet");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split(noc.rsp_out(), /*num_vc=*/4,
                                           /*write_rsp_vcs=*/std::vector<uint8_t>{0, 1},
                                           /*read_rsp_vcs=*/std::vector<uint8_t>{2, 3});
    uint8_t a = push_and_vc(
        arb, noc, make_rsp_flit(ni::AXI_CH_NarrowB, 0, /*id=*/0x40, 1, 0x12, /*ordering_req=*/1));
    uint8_t b = push_and_vc(
        arb, noc, make_rsp_flit(ni::AXI_CH_NarrowB, 0, /*id=*/0x40, 1, 0x12, /*ordering_req=*/1));
    EXPECT_EQ(a, 0u);
    EXPECT_EQ(b, 1u) << "ordering_req=1 B is order-free; round-robins the write vnet";
}

// W6: keying by (dst_id,id) dissolves the same-id multi-source contention
// the old r_burst_vc_[id] array had -- two sources (different dst_id) with
// the same id now get distinct VCs instead of contending one array slot.
// Covers both cases: same-id different-dst, and same-dst different-id.
TEST(NsuVcArbiterVnets, DifferentDstOrIdYieldsDistinctVcs) {
    SCENARIO(
        "NSU vnets: ordering_req=0 responses differing in dst_id or id can land on different VCs");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split(noc.rsp_out(), /*num_vc=*/4,
                                           /*write_rsp_vcs=*/std::vector<uint8_t>{0, 1},
                                           /*read_rsp_vcs=*/std::vector<uint8_t>{2, 3});
    // Same id (0x05), different dst_id: (0x10^0x05)%2=1 vs (0x11^0x05)%2=0.
    uint8_t same_id_dst_a =
        push_and_vc(arb, noc, make_rsp_flit(ni::AXI_CH_NarrowB, 0, 0x05, 1, 0x10, 0));
    uint8_t same_id_dst_b =
        push_and_vc(arb, noc, make_rsp_flit(ni::AXI_CH_NarrowB, 0, 0x05, 1, 0x11, 0));
    EXPECT_NE(same_id_dst_a, same_id_dst_b)
        << "W6: same id, different dst_id must not contend one VC";

    // Same dst_id (0x12), different id: (0x12^0x05)%2=1 vs (0x12^0x06)%2=0.
    uint8_t same_dst_id_a =
        push_and_vc(arb, noc, make_rsp_flit(ni::AXI_CH_NarrowB, 0, 0x05, 1, 0x12, 0));
    uint8_t same_dst_id_b =
        push_and_vc(arb, noc, make_rsp_flit(ni::AXI_CH_NarrowB, 0, 0x06, 1, 0x12, 0));
    EXPECT_NE(same_dst_id_a, same_dst_id_b) << "different id, same dst_id must be able to spread";
}

// Fixed-VC-full refuses rather than spills to another VC (design: spilling a
// same-(dst,id) stream to a second VC would reorder it -- the exact hazard
// the fixed VC exists to prevent). Fill the mapped VC to pending_depth_, then
// a same-(dst,id) push must fail, and no flit lands on the other vnet VC.
TEST(NsuVcArbiterVnets, FixedVcFullRefusesInsteadOfSpilling) {
    SCENARIO("NSU vnets: fixed VC full -> push_flit refuses, never spills to the vnet's other VC");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split(noc.rsp_out(), /*num_vc=*/4,
                                           /*write_rsp_vcs=*/std::vector<uint8_t>{0, 1},
                                           /*read_rsp_vcs=*/std::vector<uint8_t>{2, 3});
    // dst_id=0x12, id=0x05 -> (0x12^0x05)%2=1 -> read vnet[1]=VC3. Fill VC3 to
    // the default pending_depth_ (4) with distinct rids that hash the same way
    // (rid must differ per beat's rlast semantics is irrelevant here; reuse rid5
    // across 4 separate single-beat "bursts").
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 1)));
    }
    EXPECT_EQ(arb.pending_size(3), 4u);
    EXPECT_FALSE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 1)))
        << "must refuse, not spill";
    EXPECT_EQ(arb.pending_size(2), 0u) << "refused push must not land on the vnet's other VC";
}

INSTANTIATE_TEST_SUITE_P(NumVcMatrix, NsuVcArbParam,
                         ::testing::Values(std::size_t(1), std::size_t(2)),
                         [](const ::testing::TestParamInfo<std::size_t>& info) {
                             return "NumVc" + std::to_string(info.param);
                         });

// ---------------------------------------------------------------------------
// NsuConfig vnets wiring
// ---------------------------------------------------------------------------

TEST(NsuConfigVnets, ConfigVnetsBuildSpreadingArbiter) {
    using ni::cmodel::nsu::NsuConfig;
    using ni::cmodel::nsu::detail::make_vc_arbiter;  // factory lives in nsu::detail
    SCENARIO("NsuConfig.write_rsp_vcs/read_rsp_vcs -> make_vc_arbiter -> vnet arbiter");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    NsuConfig cfg{};
    cfg.num_vc = 4;
    cfg.write_rsp_vcs = {0, 1};
    cfg.read_rsp_vcs = {2, 3};
    cfg.port_params.meta_buffer_max_outstanding = 32;
    cfg.port_params.meta_buffer_max_unique_ids = 256;
    auto arb = make_vc_arbiter(cfg, noc.rsp_out());
    // dst_id=0x12 (default): rid5 -> (0x12^0x05)%2=1 -> VC3; rid6 -> (0x12^0x06)%2=0 -> VC2.
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 1)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x06, 1)));
    EXPECT_EQ(arb.pending_size(2), 1u);  // rid6
    EXPECT_EQ(arb.pending_size(3), 1u);  // rid5
}

// ---------------------------------------------------------------------------
// Plain TEST() — not parameterized:
//   Nsu_Degenerate_NumVc1_Passthrough : specifically tests NUM_VC=1 behavior
// ---------------------------------------------------------------------------

TEST(NsuVcArbiter, Nsu_Degenerate_NumVc1_Passthrough) {
    SCENARIO("NSU VcArbiter: NUM_VC=1, read_write_split routes B + R -> VC=0");

    ChannelModel noc(/*req*/ 32, /*rsp*/ 32);
    auto arb = VcArbiter::read_write_split(noc.rsp_out(), /*num_vc=*/1, 0, 0);
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowB)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR)));
    arb.tick();
    arb.tick();
    for (int i = 0; i < 2; ++i) {
        auto f = noc.rsp_in().pop_flit();
        ASSERT_TRUE(f.has_value());
        EXPECT_EQ(f->get_header_field("vc_id"), 0u);
    }
}
