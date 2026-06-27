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
using ni::cmodel::nsu::VcMode;
using ni::cmodel::testing::ChannelModel;

namespace {

Flit make_rsp_flit(uint8_t axi_ch, uint8_t initial_vc = 0, uint8_t id = 0, uint64_t rlast = 1) {
    Flit f;
    f.set_header_field("axi_ch", axi_ch);
    f.set_header_field("dst_id", 0x12);
    f.set_header_field("vc_id", initial_vc);
    f.set_header_field("src_id", 0x34);
    f.set_header_field("last", 1);
    if (axi_ch == ni::AXI_CH_R) {
        f.set_payload_field("R", "rid", id);
        f.set_payload_field("R", "rlast", rlast);
    } else if (axi_ch == ni::AXI_CH_B) {
        f.set_payload_field("B", "bid", id);
    }
    return f;
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

// MultiCandidate HoL-avoidance.
// Candidate lists are adapted to num_vc:
//   num_vc=1  : single-VC, no multi-candidate overflow possible; skip
//   num_vc=2  : B={0,1}, R={0}  — B has 2 candidates; R shares VC=0
//   num_vc=4  : B={0,1}, R={2,3}
//   num_vc=8  : B={0,1,2,3}, R={4,5,6,7}
// The scenario: saturate the first B candidate VC, verify the next B
// spills to the second candidate VC.
TEST_P(NsuVcArbParam, Nsu_MultiCandidate_HoLAvoidance) {
    const std::size_t num_vc = GetParam();
    if (num_vc < 2) GTEST_SKIP() << "needs NUM_VC >= 2 for multi-candidate B overflow";

    SCENARIO(
        "NSU VcArbiter Mode B: saturate first B candidate VC -> next B "
        "picks second candidate VC, avoiding head-of-line block");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);

    // B always gets 2 candidates (VC=0 and VC=1); R fills the remainder
    // (or shares VC=0 when num_vc=2 and there is no separate upper half).
    std::array<std::vector<uint8_t>, VcArbiter::AXI_CH_COUNT> candidates{};
    candidates[ni::AXI_CH_B] = {0, 1};
    if (num_vc > 2) {
        for (std::size_t i = num_vc / 2; i < num_vc; ++i) {
            candidates[ni::AXI_CH_R].push_back(static_cast<uint8_t>(i));
        }
    } else {
        candidates[ni::AXI_CH_R] = {0};  // num_vc=2: R shares VC=0
    }
    auto arb = VcArbiter::multi_candidate(noc.rsp_out(), num_vc, candidates, /*pending_depth=*/2);

    // Fill VC=0 pending to capacity (2 Bs land on VC=0).
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_B)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_B)));
    EXPECT_EQ(arb.pending_size(0), 2u);
    EXPECT_EQ(arb.pending_size(1), 0u);

    // Next B: VC=0 full -> candidate fallback picks VC=1.
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_B)));
    EXPECT_EQ(arb.pending_size(1), 1u);
}

// Round-robin fairness: num_vc R flits pre-routed to num_vc distinct VCs via
// Mode B candidates with pending_depth=1; tick num_vc times -> flits emerge
// in RR order VC=0, VC=1, ..., VC=num_vc-1.
// Requires num_vc ≥ 2 so there is more than one VC to arbitrate.
TEST_P(NsuVcArbParam, Nsu_RoundRobinFairness) {
    const std::size_t num_vc = GetParam();
    if (num_vc < 2) GTEST_SKIP() << "needs NUM_VC >= 2 to exercise round-robin";

    SCENARIO(
        "NSU VcArbiter Mode B: num_vc R flits pre-routed to distinct VCs via "
        "candidate list; tick num_vc times -> flits emerge in RR order");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    std::array<std::vector<uint8_t>, VcArbiter::AXI_CH_COUNT> candidates{};
    candidates[ni::AXI_CH_B] = {0};
    // R candidate list spans all VCs; pending_depth=1 fills each VC in order.
    for (std::size_t i = 0; i < num_vc; ++i) {
        candidates[ni::AXI_CH_R].push_back(static_cast<uint8_t>(i));
    }
    auto arb = VcArbiter::multi_candidate(noc.rsp_out(), num_vc, candidates, /*pending_depth=*/1);

    // Push num_vc R flits: first fills VC=0, second VC=1, ..., etc.
    for (std::size_t i = 0; i < num_vc; ++i) {
        ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R)));
        EXPECT_EQ(arb.pending_size(i), 1u);
    }

    for (std::size_t i = 0; i < num_vc; ++i) arb.tick();

    for (std::size_t expected_vc = 0; expected_vc < num_vc; ++expected_vc) {
        auto f = noc.rsp_in().pop_flit();
        ASSERT_TRUE(f.has_value());
        EXPECT_EQ(f->get_header_field("vc_id"), expected_vc);
    }
}

// Credit gating: downstream per_vc_depth=1; two R flits land on VC=0 and
// VC=1 (pending_depth=1 forces second R to overflow to VC=1); tick drains
// VC=0 first; second tick advances RR to VC=1 after VC=0 credit exhausted.
// Requires num_vc ≥ 2 (uses two distinct VCs).
TEST_P(NsuVcArbParam, Nsu_CreditGating) {
    const std::size_t num_vc = GetParam();
    if (num_vc < 2) GTEST_SKIP() << "needs NUM_VC >= 2 (uses VC=0 and VC=1)";

    SCENARIO(
        "NSU VcArbiter: downstream per_vc_depth=1; pending_depth=1 forces "
        "second R to VC=1; tick drains VC=0 first; second tick advances "
        "RR to VC=1 after VC=0 credit exhausted");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    noc.set_per_vc_depth(1);
    std::array<std::vector<uint8_t>, VcArbiter::AXI_CH_COUNT> candidates{};
    candidates[ni::AXI_CH_B] = {0};
    candidates[ni::AXI_CH_R] = {0, 1};
    auto arb = VcArbiter::multi_candidate(noc.rsp_out(), num_vc, candidates, /*pending_depth=*/1);

    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R)));  // VC=0
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R)));  // VC=1 (VC=0 full)
    EXPECT_EQ(arb.pending_size(0), 1u);
    EXPECT_EQ(arb.pending_size(1), 1u);

    // First tick: VC=0 has pending + downstream credit -> 1 flit out.
    arb.tick();
    EXPECT_EQ(noc.nsu_rsp_per_vc_in_flight(0), 1u);
    EXPECT_EQ(arb.pending_size(0), 0u);

    // Second tick: VC=0 downstream credit exhausted (per_vc_depth=1),
    // round-robin advances to VC=1 which still has credit.
    arb.tick();
    EXPECT_EQ(noc.nsu_rsp_per_vc_in_flight(1), 1u);
    EXPECT_EQ(arb.pending_size(1), 0u);
}

// A multi-beat R burst (one rid) keeps every beat on its single bound VC.
TEST(NsuVcArbiterPools, RBurstStaysOnOneVc) {
    SCENARIO("NSU pools: all beats of one rid's R burst map to one VC");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split_pools(noc.rsp_out(), /*num_vc=*/4,
                                                 /*write_rsp_vcs=*/{0, 1}, /*read_rsp_vcs=*/{2, 3});
    // 4-beat burst, rid=0x05; beats 1-3 rlast=0, beat 4 rlast=1.
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 0)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 0)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 0)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 1)));
    // All four beats on the first read-pool VC (2); none on VC=3.
    EXPECT_EQ(arb.pending_size(2), 4u);
    EXPECT_EQ(arb.pending_size(3), 0u);
}

// Distinct rids (each a single-beat read) round-robin across the read pool.
TEST(NsuVcArbiterPools, DistinctRidsSpreadAcrossPool) {
    SCENARIO("NSU pools: distinct rids round-robin over read pool {2,3}");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split_pools(noc.rsp_out(), /*num_vc=*/4,
                                                 /*write_rsp_vcs=*/{0, 1}, /*read_rsp_vcs=*/{2, 3});
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 1)));  // rid5 -> VC2, releases
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x06, 1)));  // rid6 -> VC3, releases
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x07, 1)));  // rid7 -> VC2
    EXPECT_EQ(arb.pending_size(2), 2u);                                   // rid5 + rid7
    EXPECT_EQ(arb.pending_size(3), 1u);                                   // rid6
}

// B uses the write pool, R uses the read pool (response-class separation).
TEST(NsuVcArbiterPools, BUsesWritePoolRUsesReadPool) {
    SCENARIO("NSU pools: B -> write pool {0,1}, R -> read pool {2,3}");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split_pools(noc.rsp_out(), /*num_vc=*/4,
                                                 /*write_rsp_vcs=*/{0, 1}, /*read_rsp_vcs=*/{2, 3});
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_B, 0, 0x05, 1)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 1)));
    EXPECT_EQ(arb.pending_size(0), 1u);  // B on write pool
    EXPECT_EQ(arb.pending_size(2), 1u);  // R on read pool
}

// Two in-flight multi-beat R bursts (rid5, rid6) whose beats interleave must
// each stay on their own bound VC -- this is the invariant per-id binding
// exists for (a single current_r_vc_ would misroute the interleaved beats).
TEST(NsuVcArbiterPools, InterleavedMultiBeatBurstsStayOnTheirOwnVc) {
    SCENARIO("NSU pools: interleaved rid5/rid6 multi-beat R bursts each pin to one VC");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split_pools(noc.rsp_out(), /*num_vc=*/4,
                                                 /*write_rsp_vcs=*/{0, 1}, /*read_rsp_vcs=*/{2, 3});
    // rid5 beat1 (binds VC2), rid6 beat1 (binds VC3), then interleave remaining beats.
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 0)));  // rid5 -> VC2
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x06, 0)));  // rid6 -> VC3
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 0)));  // rid5 -> VC2 (bound)
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x06, 0)));  // rid6 -> VC3 (bound)
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 1)));  // rid5 last -> VC2
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x06, 1)));  // rid6 last -> VC3
    EXPECT_EQ(arb.pending_size(2), 3u);                                   // all rid5 beats
    EXPECT_EQ(arb.pending_size(3), 3u);                                   // all rid6 beats
}

INSTANTIATE_TEST_SUITE_P(NumVcMatrix, NsuVcArbParam,
                         ::testing::Values(std::size_t(1), std::size_t(2), std::size_t(4),
                                           std::size_t(8)),
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
    auto arb = make_vc_arbiter(cfg, noc.rsp_out());
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x05, 1)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R, 0, 0x06, 1)));
    EXPECT_EQ(arb.pending_size(2), 1u);  // rid5
    EXPECT_EQ(arb.pending_size(3), 1u);  // rid6
}

// ---------------------------------------------------------------------------
// Plain TEST() — not parameterized:
//   Nsu_Degenerate_NumVc1_Passthrough : specifically tests NUM_VC=1 behavior
// ---------------------------------------------------------------------------

TEST(NsuVcArbiter, Nsu_Degenerate_NumVc1_Passthrough) {
    SCENARIO(
        "NSU VcArbiter: NUM_VC=1, both Mode A (read_write_split) and Mode B "
        "(multi_candidate) route B + R -> VC=0");

    // Mode A
    {
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
    // Mode B -- even with multi_candidate, num_vc=1 forces VC=0.
    {
        ChannelModel noc(/*req*/ 32, /*rsp*/ 32);
        std::array<std::vector<uint8_t>, VcArbiter::AXI_CH_COUNT> candidates{};
        candidates[ni::AXI_CH_B] = {0};
        candidates[ni::AXI_CH_R] = {0};
        auto arb = VcArbiter::multi_candidate(noc.rsp_out(), /*num_vc=*/1, candidates);
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
}
