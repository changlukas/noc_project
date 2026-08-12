#include "nsu/vc_allocator.hpp"
#include "common/channel_model.hpp"
#include "common/scenario.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include <gtest/gtest.h>
#include <array>
#include <vector>

using ni::cmodel::Flit;
using ni::cmodel::nsu::VcAllocator;
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
uint8_t push_and_vc(VcAllocator& arb, ChannelModel& /*noc*/, const Flit& flit) {
    std::array<std::size_t, VcAllocator::NUM_VC_MAX> before{};
    for (uint8_t v = 0; v < VcAllocator::NUM_VC_MAX; ++v) before[v] = arb.pending_size(v);
    if (!arb.push_flit(flit)) return 0xFF;
    for (uint8_t v = 0; v < VcAllocator::NUM_VC_MAX; ++v) {
        if (arb.pending_size(v) > before[v]) return v;
    }
    return 0xFF;
}

}  // namespace

// Fixed VC id (same-destination bypass): rsp VC = (dst_id ^ rid) % num_vc, zero state.
// Replaces the retired r_burst_vc_ array's burst-coherence role -- a burst's
// beats share (dst_id, rid) so they hash to the same VC automatically.
//
// A multi-beat R burst (one rid) keeps every beat on its single mapped VC.
TEST(NsuVcAllocator, RBurstStaysOnOneVc) {
    SCENARIO(
        "NSU VcAllocator: all beats of one rid's R burst map to one VC (static map, not "
        "follow-state)");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    VcAllocator arb(noc.rsp_out(), /*num_vc=*/4);
    // 4-beat burst, dst_id=0x12, rid=0x05 -> (0x12^0x05)%4=3 -> VC3.
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 0)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 0)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 0)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 1)));
    // All four beats hash to VC=3; no other VC sees a beat.
    EXPECT_EQ(arb.pending_size(3), 4u);
    EXPECT_EQ(arb.pending_size(0), 0u);
}

// Robbed (ordering_req=1) R still goes through the static map -- the map applies to
// ALL R regardless of ordering_req (design: burst coherence must hold even though
// the NMU RoB will reorder by ordering_tag; the map only needs to keep one burst's
// beats together, which a pure function of (dst_id,rid) does unconditionally).
TEST(NsuVcAllocator, RobbedRBurstStaysOnOneVcToo) {
    SCENARIO("NSU VcAllocator: ordering_req=1 R burst still maps every beat to one VC");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    VcAllocator arb(noc.rsp_out(), /*num_vc=*/4);
    ASSERT_TRUE(
        arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 0, 0x12, /*ordering_req=*/1)));
    ASSERT_TRUE(
        arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 1, 0x12, /*ordering_req=*/1)));
    EXPECT_EQ(arb.pending_size(3), 2u);
    EXPECT_EQ(arb.pending_size(0), 0u);
}

// Distinct rids (each a single-beat read) hash to different VCs -- the static
// map replaces round-robin spread with deterministic (dst,id) spread.
TEST(NsuVcAllocator, DistinctRidsSpreadAcrossVcs) {
    SCENARIO("NSU VcAllocator: distinct rids hash across the VC set via the static map");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    VcAllocator arb(noc.rsp_out(), /*num_vc=*/4);
    // dst_id=0x12 for all: rid5 -> (0x12^0x05)%4=3; rid6 -> (0x12^0x06)%4=0;
    // rid7 -> (0x12^0x07)%4=1.
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 1)));  // rid5 -> VC3
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x06, 1)));  // rid6 -> VC0
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x07, 1)));  // rid7 -> VC1
    EXPECT_EQ(arb.pending_size(3), 1u);
    EXPECT_EQ(arb.pending_size(0), 1u);
    EXPECT_EQ(arb.pending_size(1), 1u);
}

// Two in-flight multi-beat R bursts (rid5, rid6) whose beats interleave must
// each stay on their own mapped VC -- the invariant per-(dst,id) hashing
// exists for (a single VC choice would misroute the interleaved beats).
TEST(NsuVcAllocator, InterleavedMultiBeatBurstsStayOnTheirOwnVc) {
    SCENARIO("NSU VcAllocator: interleaved rid5/rid6 multi-beat R bursts each map to one VC");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    VcAllocator arb(noc.rsp_out(), /*num_vc=*/4);
    // rid5 -> VC3, rid6 -> VC0 (same hash as DistinctRidsSpreadAcrossVcs above).
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 0)));  // rid5 -> VC3
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x06, 0)));  // rid6 -> VC0
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 0)));  // rid5 -> VC3
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x06, 0)));  // rid6 -> VC0
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 1)));  // rid5 last -> VC3
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x06, 1)));  // rid6 last -> VC0
    EXPECT_EQ(arb.pending_size(3), 3u);                                         // all rid5 beats
    EXPECT_EQ(arb.pending_size(0), 3u);                                         // all rid6 beats
}

// B carries no fixed VC: it is order-free at the NMU slot path (and single-VC
// on the RSP face), so same-bid responses round-robin the VC set.
TEST(NsuVcAllocator, SameBidRoundRobins) {
    SCENARIO("NSU VcAllocator: same-bid B responses round-robin the VC set");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    VcAllocator arb(noc.rsp_out(), /*num_vc=*/4);
    uint8_t a = push_and_vc(arb, noc, make_rsp_flit(ni::AXI_CH_NarrowB, 0, /*id=*/0x04, 1, 0x12));
    uint8_t b = push_and_vc(arb, noc, make_rsp_flit(ni::AXI_CH_NarrowB, 0, /*id=*/0x04, 1, 0x12));
    EXPECT_EQ(a, 0u);
    EXPECT_EQ(b, 1u) << "B is order-free; round-robins";
}

// W6: keying by (dst_id,id) dissolves the same-id multi-source contention the
// old r_burst_vc_[id] array had -- two sources (different dst_id) with the
// same rid now get distinct VCs instead of contending one array slot.
TEST(NsuVcAllocator, SameRidDifferentDstYieldsDistinctVcs) {
    SCENARIO("NSU VcAllocator: same rid from different dst_id can land on different VCs");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    VcAllocator arb(noc.rsp_out(), /*num_vc=*/4);
    // Same rid (0x05), different dst_id: (0x10^0x05)%4=1 vs (0x11^0x05)%4=0.
    uint8_t a = push_and_vc(arb, noc, make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 1, 0x10));
    uint8_t b = push_and_vc(arb, noc, make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 1, 0x11));
    EXPECT_NE(a, b) << "W6: same rid, different dst_id must not contend one VC";
}

// Fixed-VC-full refuses rather than spills to another VC (design: spilling a
// same-(dst,id) stream to a second VC would reorder it -- the exact hazard
// the fixed VC exists to prevent). Fill the mapped VC to pending_depth_, then
// a same-(dst,id) push must fail, and no flit lands on any other VC.
TEST(NsuVcAllocator, FixedVcFullRefusesInsteadOfSpilling) {
    SCENARIO("NSU VcAllocator: fixed VC full -> push_flit refuses, never spills to another VC");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    VcAllocator arb(noc.rsp_out(), /*num_vc=*/4);
    // dst_id=0x12, rid=0x05 -> (0x12^0x05)%4=3 -> VC3. Fill VC3 to the default
    // pending_depth_ (4) by reusing rid5 across 4 separate single-beat "bursts".
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 1)));
    }
    EXPECT_EQ(arb.pending_size(3), 4u);
    EXPECT_FALSE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, 0x05, 1)))
        << "must refuse, not spill";
    EXPECT_EQ(arb.pending_size(0), 0u) << "refused push must not land on another VC";
}

// ---------------------------------------------------------------------------
// Parameterized fixture — NUM_VC ∈ {1, 2, 4} (see INSTANTIATE below)
// ---------------------------------------------------------------------------

class NsuVcAllocatorParam : public ::testing::TestWithParam<std::size_t> {};

// R is an ordered same-destination stream held on its mapped VC end to end, so
// it leaves with fixed_vc=1 and routers must not reallocate it. B is order-free
// and leaves the bit clear. NUM_VC=1 stamps the same way -- the bit must be
// honest even on a face that has no second VC to move to.
TEST_P(NsuVcAllocatorParam, FixedVcStampedOnRNotB) {
    const std::size_t num_vc = GetParam();

    SCENARIO("NSU VcAllocator: R leaves with fixed_vc=1, B with fixed_vc=0");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    VcAllocator arb(noc.rsp_out(), num_vc);
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, /*id=*/0x05, /*rlast=*/0)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_NarrowR, 0, /*id=*/0x05, /*rlast=*/1)));
    Flit b_in = make_rsp_flit(ni::AXI_CH_NarrowB, 0, /*id=*/0x04);
    b_in.set_header_field("fixed_vc", 1);  // a pass-through (no stamp) would show up here
    ASSERT_TRUE(arb.push_flit(b_in));

    for (int i = 0; i < 3; ++i) arb.tick();
    for (int i = 0; i < 3; ++i) {
        auto f = noc.rsp_in().pop_flit();
        ASSERT_TRUE(f.has_value());
        uint64_t expected = (f->get_header_field("axi_ch") == ni::AXI_CH_NarrowR) ? 1u : 0u;
        EXPECT_EQ(f->get_header_field("fixed_vc"), expected)
            << "axi_ch=" << f->get_header_field("axi_ch");
    }
}

INSTANTIATE_TEST_SUITE_P(NumVcMatrix, NsuVcAllocatorParam,
                         ::testing::Values(std::size_t(1), std::size_t(2), std::size_t(4)),
                         [](const ::testing::TestParamInfo<std::size_t>& info) {
                             return "NumVc" + std::to_string(info.param);
                         });

// ---------------------------------------------------------------------------
// Plain TEST() — not parameterized:
//   Nsu_Degenerate_NumVc1_Passthrough : specifically tests NUM_VC=1 behavior
// ---------------------------------------------------------------------------

TEST(NsuVcAllocator, Nsu_Degenerate_NumVc1_Passthrough) {
    SCENARIO("NSU VcAllocator: NUM_VC=1 routes B + R -> VC=0");

    ChannelModel noc(/*req*/ 32, /*rsp*/ 32);
    VcAllocator arb(noc.rsp_out(), /*num_vc=*/1);
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
