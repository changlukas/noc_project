#include "nsu/vc_arbiter.hpp"
#include "nsu/nsu.hpp"
#include "ni/vc_pools.hpp"
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
                   uint8_t dst_id = 0x12, uint8_t rob_req = 0) {
    Flit f;
    f.set_header_field("axi_ch", axi_ch);
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("vc_id", initial_vc);
    f.set_header_field("src_id", 0x34);
    f.set_header_field("last", 1);
    f.set_header_field("rob_req", rob_req);
    if (axi_ch == ni::AXI_CH_R) {
        f.set_payload_field("R", "rid", id);
        f.set_payload_field("R", "rlast", rlast);
    } else if (axi_ch == ni::AXI_CH_B) {
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

    SCENARIO("NSU VcArbiter Mode A: B -> write_rsp_vc=0, R -> read_rsp_vc=1");
    ChannelModel noc(/*req*/ 32, /*rsp*/ 32);
    auto arb = VcArbiter::read_write_split(noc.rsp_out(), num_vc, 0, 1);

    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_B)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R)));
    EXPECT_EQ(arb.pending_size(0), 1u);
    EXPECT_EQ(arb.pending_size(1), 1u);

    arb.tick();
    arb.tick();
    auto f0 = noc.rsp_in().pop_flit();
    ASSERT_TRUE(f0.has_value());
    auto f1 = noc.rsp_in().pop_flit();
    ASSERT_TRUE(f1.has_value());
    // Round-robin starts at 0 -> VC=0 (B) drains first, then VC=1 (R).
    EXPECT_EQ(f0->get_header_field("axi_ch"), ni::AXI_CH_B);
    EXPECT_EQ(f0->get_header_field("vc_id"), 0u);
    EXPECT_EQ(f1->get_header_field("axi_ch"), ni::AXI_CH_R);
    EXPECT_EQ(f1->get_header_field("vc_id"), 1u);
}

// microarch §5a (RZ1): rsp VC = pool[(dst_id ^ id) % pool.size()], zero state.
// Replaces r_burst_vc_'s burst-coherence role -- a burst's beats share
// (dst_id, rid) so they hash to the same pool slot automatically.
//
// A multi-beat R burst (one rid) keeps every beat on its single mapped VC.
TEST(NsuVcArbiterPools, RBurstStaysOnOneVc) {
    SCENARIO(
        "NSU pools: all beats of one rid's R burst map to one VC (static map, not follow-state)");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split_pools(noc.rsp_out(), /*num_vc=*/4,
                                                 /*write_rsp_vcs=*/{0, 1}, /*read_rsp_vcs=*/{2, 3});
    // 4-beat burst, dst_id=0x12, rid=0x05 -> (0x12^0x05)%2=1 -> read pool[1]=VC3.
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 0)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 0)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 0)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 1)));
    // All four beats hash to VC=3; none on VC=2.
    EXPECT_EQ(arb.pending_size(3), 4u);
    EXPECT_EQ(arb.pending_size(2), 0u);
}

// Robbed (rob_req=1) R still goes through the static map -- the map applies to
// ALL R regardless of rob_req (design: burst coherence must hold even though
// the NMU RoB will reorder by rob_idx; the map only needs to keep one burst's
// beats together, which a pure function of (dst_id,rid) does unconditionally).
TEST(NsuVcArbiterPools, RobbedRBurstStaysOnOneVcToo) {
    SCENARIO("NSU pools: rob_req=1 R burst still maps every beat to one VC");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split_pools(noc.rsp_out(), /*num_vc=*/4,
                                                 /*write_rsp_vcs=*/{0, 1}, /*read_rsp_vcs=*/{2, 3});
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 0, 0x12, /*rob_req=*/1)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 1, 0x12, /*rob_req=*/1)));
    EXPECT_EQ(arb.pending_size(3), 2u);
    EXPECT_EQ(arb.pending_size(2), 0u);
}

// Distinct rids (each a single-beat read) hash to different pool slots -- the
// static map replaces round-robin spread with deterministic (dst,id) spread.
TEST(NsuVcArbiterPools, DistinctRidsSpreadAcrossPool) {
    SCENARIO("NSU pools: distinct rids hash across read pool {2,3} via static map");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split_pools(noc.rsp_out(), /*num_vc=*/4,
                                                 /*write_rsp_vcs=*/{0, 1}, /*read_rsp_vcs=*/{2, 3});
    // dst_id=0x12 for all: rid5 -> (0x12^0x05)%2=1 -> VC3; rid6 -> (0x12^0x06)%2=0 -> VC2;
    // rid7 -> (0x12^0x07)%2=1 -> VC3.
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 1)));  // rid5 -> VC3
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x06, 1)));  // rid6 -> VC2
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x07, 1)));  // rid7 -> VC3
    EXPECT_EQ(arb.pending_size(2), 1u);                                   // rid6
    EXPECT_EQ(arb.pending_size(3), 2u);                                   // rid5 + rid7
}

// B uses the write pool, R uses the read pool (response-class separation).
TEST(NsuVcArbiterPools, BUsesWritePoolRUsesReadPool) {
    SCENARIO("NSU pools: B -> write pool {0,1}, R -> read pool {2,3}");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split_pools(noc.rsp_out(), /*num_vc=*/4,
                                                 /*write_rsp_vcs=*/{0, 1}, /*read_rsp_vcs=*/{2, 3});
    // id=0x05, dst_id=0x12: B -> (0x12^0x05)%2=1 -> write pool[1]=VC1;
    // R -> same hash -> read pool[1]=VC3.
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_B, 0, 0x05, 1)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 1)));
    EXPECT_EQ(arb.pending_size(1), 1u);  // B on write pool
    EXPECT_EQ(arb.pending_size(3), 1u);  // R on read pool
}

// Two in-flight multi-beat R bursts (rid5, rid6) whose beats interleave must
// each stay on their own mapped VC -- the invariant per-(dst,id) hashing
// exists for (a single VC choice would misroute the interleaved beats).
TEST(NsuVcArbiterPools, InterleavedMultiBeatBurstsStayOnTheirOwnVc) {
    SCENARIO("NSU pools: interleaved rid5/rid6 multi-beat R bursts each map to one VC");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split_pools(noc.rsp_out(), /*num_vc=*/4,
                                                 /*write_rsp_vcs=*/{0, 1}, /*read_rsp_vcs=*/{2, 3});
    // rid5 -> VC3, rid6 -> VC2 (same hash as DistinctRidsSpreadAcrossPool above).
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 0)));  // rid5 -> VC3
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x06, 0)));  // rid6 -> VC2
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 0)));  // rid5 -> VC3
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x06, 0)));  // rid6 -> VC2
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 1)));  // rid5 last -> VC3
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x06, 1)));  // rid6 last -> VC2
    EXPECT_EQ(arb.pending_size(3), 3u);                                   // all rid5 beats
    EXPECT_EQ(arb.pending_size(2), 3u);                                   // all rid6 beats
}

// INVERTED from the pre-clause-2 test "SameBidRoundRobinsWritePool": that test
// asserted B has no pin and round-robins same-bid responses. The clause-2
// return-path static map now pins rob_req=0 B the same way it pins R -- a
// same-(dst,bid) bypass stream must share one VC to stay in order at the NMU.
// rob_req=1 (robbed) B keeps the old round-robin behavior; see the next test.
TEST(NsuVcArbiterPools, SameBidSameDstBypassPinsOneVc) {
    SCENARIO(
        "NSU pools: rob_req=0 same-(dst,bid) B responses pin to one VC (clause-2 return-path map)");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split_pools(noc.rsp_out(), /*num_vc=*/4,
                                                 /*write_rsp_vcs=*/{0, 1}, /*read_rsp_vcs=*/{2, 3});
    uint8_t a = push_and_vc(arb, noc, make_rsp_flit(ni::AXI_CH_B, 0, /*id=*/0x40, 1, 0x12, 0));
    uint8_t b = push_and_vc(arb, noc, make_rsp_flit(ni::AXI_CH_B, 0, /*id=*/0x40, 1, 0x12, 0));
    EXPECT_EQ(a, b) << "same (dst,bid) bypass responses must share one VC";
}

// rob_req=1 (robbed) B is order-free at the NMU slot path, so it keeps
// round-robining the write pool exactly as before clause 2.
TEST(NsuVcArbiterPools, SameBidRobbedRoundRobinsWritePool) {
    SCENARIO("NSU pools: rob_req=1 same-bid B responses still round-robin the write pool");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split_pools(noc.rsp_out(), /*num_vc=*/4,
                                                 /*write_rsp_vcs=*/{0, 1}, /*read_rsp_vcs=*/{2, 3});
    uint8_t a =
        push_and_vc(arb, noc, make_rsp_flit(ni::AXI_CH_B, 0, /*id=*/0x40, 1, 0x12, /*rob_req=*/1));
    uint8_t b =
        push_and_vc(arb, noc, make_rsp_flit(ni::AXI_CH_B, 0, /*id=*/0x40, 1, 0x12, /*rob_req=*/1));
    EXPECT_EQ(a, 0u);
    EXPECT_EQ(b, 1u) << "rob_req=1 B is order-free; round-robins the write pool";
}

// W6 (microarch §5a / meta_buffer.hpp:22-28): keying by (dst_id,id) dissolves
// the same-id multi-source contention the old r_burst_vc_[id] array had --
// two sources (different dst_id) with the same id now get distinct VCs
// instead of contending one array slot. Covers both brief bullets: same-id
// different-dst, and same-dst different-id.
TEST(NsuVcArbiterPools, DifferentDstOrIdYieldsDistinctVcs) {
    SCENARIO("NSU pools: rob_req=0 responses differing in dst_id or id can land on different VCs");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split_pools(noc.rsp_out(), /*num_vc=*/4,
                                                 /*write_rsp_vcs=*/{0, 1}, /*read_rsp_vcs=*/{2, 3});
    // Same id (0x05), different dst_id: (0x10^0x05)%2=1 vs (0x11^0x05)%2=0.
    uint8_t same_id_dst_a = push_and_vc(arb, noc, make_rsp_flit(ni::AXI_CH_B, 0, 0x05, 1, 0x10, 0));
    uint8_t same_id_dst_b = push_and_vc(arb, noc, make_rsp_flit(ni::AXI_CH_B, 0, 0x05, 1, 0x11, 0));
    EXPECT_NE(same_id_dst_a, same_id_dst_b)
        << "W6: same id, different dst_id must not contend one VC";

    // Same dst_id (0x12), different id: (0x12^0x05)%2=1 vs (0x12^0x06)%2=0.
    uint8_t same_dst_id_a = push_and_vc(arb, noc, make_rsp_flit(ni::AXI_CH_B, 0, 0x05, 1, 0x12, 0));
    uint8_t same_dst_id_b = push_and_vc(arb, noc, make_rsp_flit(ni::AXI_CH_B, 0, 0x06, 1, 0x12, 0));
    EXPECT_NE(same_dst_id_a, same_dst_id_b) << "different id, same dst_id must be able to spread";
}

// Pinned-VC-full refuses rather than spills to another VC (design: spilling a
// same-(dst,id) stream to a second VC would reorder it -- the exact hazard
// the pin exists to prevent). Fill the mapped VC to pending_depth_, then a
// same-(dst,id) push must fail, and no flit lands on the other pool VC.
TEST(NsuVcArbiterPools, PinnedVcFullRefusesInsteadOfSpilling) {
    SCENARIO("NSU pools: pinned VC full -> push_flit refuses, never spills to the pool's other VC");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split_pools(noc.rsp_out(), /*num_vc=*/4,
                                                 /*write_rsp_vcs=*/{0, 1}, /*read_rsp_vcs=*/{2, 3});
    // dst_id=0x12, id=0x05 -> (0x12^0x05)%2=1 -> read pool[1]=VC3. Fill VC3 to
    // the default pending_depth_ (4) with distinct rids that hash the same way
    // (rid must differ per beat's rlast semantics is irrelevant here; reuse rid5
    // across 4 separate single-beat "bursts").
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 1)));
    }
    EXPECT_EQ(arb.pending_size(3), 4u);
    EXPECT_FALSE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 1)))
        << "must refuse, not spill";
    EXPECT_EQ(arb.pending_size(2), 0u) << "refused push must not land on the pool's other VC";
}

INSTANTIATE_TEST_SUITE_P(NumVcMatrix, NsuVcArbParam,
                         ::testing::Values(std::size_t(1), std::size_t(2)),
                         [](const ::testing::TestParamInfo<std::size_t>& info) {
                             return "NumVc" + std::to_string(info.param);
                         });

// ---------------------------------------------------------------------------
// NsuConfig pools wiring — Task 5
// ---------------------------------------------------------------------------

TEST(NsuConfigPools, ConfigPoolsBuildSpreadingArbiter) {
    using ni::cmodel::nsu::NsuConfig;
    using ni::cmodel::nsu::detail::make_vc_arbiter;  // factory lives in nsu::detail
    SCENARIO("NsuConfig.write_rsp_vcs/read_rsp_vcs -> make_vc_arbiter -> pools arbiter");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    NsuConfig cfg{};
    cfg.num_vc = 4;
    cfg.write_rsp_vcs = {0, 1};
    cfg.read_rsp_vcs = {2, 3};
    cfg.port_params.meta_buffer_max_outstanding = 32;
    cfg.port_params.meta_buffer_max_unique_ids = 256;
    auto arb = make_vc_arbiter(cfg, noc.rsp_out());
    // dst_id=0x12 (default): rid5 -> (0x12^0x05)%2=1 -> VC3; rid6 -> (0x12^0x06)%2=0 -> VC2.
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 1)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x06, 1)));
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
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_B)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R)));
    arb.tick();
    arb.tick();
    for (int i = 0; i < 2; ++i) {
        auto f = noc.rsp_in().pop_flit();
        ASSERT_TRUE(f.has_value());
        EXPECT_EQ(f->get_header_field("vc_id"), 0u);
    }
}
