#include "nsu/vc_arb.hpp"
#include "common/loopback_noc.hpp"
#include "common/scenario.hpp"
#include "ni/flit.hpp"
#include "ni_flit_constants.h"
#include <gtest/gtest.h>
#include <array>
#include <vector>

using ni::cmodel::Flit;
using ni::cmodel::nsu::VcArb;
using ni::cmodel::nsu::VcMode;
using ni::cmodel::testing::LoopbackNoc;

namespace {

Flit make_rsp_flit(uint8_t axi_ch, uint8_t initial_vc = 0) {
    Flit f;
    f.set_header_field("axi_ch", axi_ch);
    f.set_header_field("dst_id", 0x12);
    f.set_header_field("vc_id",  initial_vc);
    f.set_header_field("src_id", 0x34);
    f.set_header_field("last",   1);
    return f;
}

}  // namespace

TEST(NsuVcArb, Nsu_Degenerate_NumVc1_Passthrough) {
    SCENARIO("NSU VcArb: NUM_VC=1, both Mode A (read_write_split) and Mode B "
             "(multi_candidate) route B + R -> VC=0");

    // Mode A
    {
        LoopbackNoc noc(/*req*/32, /*rsp*/32);
        auto arb = VcArb::read_write_split(noc.rsp_out(), /*num_vc=*/1, 0, 0);
        ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_B)));
        ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R)));
        arb.tick(); arb.tick();
        for (int i = 0; i < 2; ++i) {
            auto f = noc.rsp_in().pop_flit(); ASSERT_TRUE(f.has_value());
            EXPECT_EQ(f->get_header_field("vc_id"), 0u);
        }
    }
    // Mode B -- even with multi_candidate, num_vc=1 forces VC=0.
    {
        LoopbackNoc noc(/*req*/32, /*rsp*/32);
        std::array<std::vector<uint8_t>, VcArb::AXI_CH_COUNT> candidates{};
        candidates[ni::AXI_CH_B] = {0};
        candidates[ni::AXI_CH_R] = {0};
        auto arb = VcArb::multi_candidate(noc.rsp_out(), /*num_vc=*/1, candidates);
        ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_B)));
        ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R)));
        arb.tick(); arb.tick();
        for (int i = 0; i < 2; ++i) {
            auto f = noc.rsp_in().pop_flit(); ASSERT_TRUE(f.has_value());
            EXPECT_EQ(f->get_header_field("vc_id"), 0u);
        }
    }
}

TEST(NsuVcArb, Nsu_ReadWriteSplit_B_R_GoSeparateVcs) {
    SCENARIO("NSU VcArb Mode A NUM_VC=2: B -> write_rsp_vc=0, R -> read_rsp_vc=1");
    LoopbackNoc noc(/*req*/32, /*rsp*/32);
    auto arb = VcArb::read_write_split(noc.rsp_out(), /*num_vc=*/2, 0, 1);

    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_B)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R)));
    EXPECT_EQ(arb.pending_size(0), 1u);
    EXPECT_EQ(arb.pending_size(1), 1u);

    arb.tick(); arb.tick();
    auto f0 = noc.rsp_in().pop_flit(); ASSERT_TRUE(f0.has_value());
    auto f1 = noc.rsp_in().pop_flit(); ASSERT_TRUE(f1.has_value());
    // Round-robin starts at 0 -> VC=0 (B) drains first, then VC=1 (R).
    EXPECT_EQ(f0->get_header_field("axi_ch"), ni::AXI_CH_B);
    EXPECT_EQ(f0->get_header_field("vc_id"),  0u);
    EXPECT_EQ(f1->get_header_field("axi_ch"), ni::AXI_CH_R);
    EXPECT_EQ(f1->get_header_field("vc_id"),  1u);
}

TEST(NsuVcArb, Nsu_MultiCandidate_HoLAvoidance) {
    SCENARIO("NSU VcArb Mode B NUM_VC=4: B candidates {0,1}; saturate VC=0 pending, "
             "next B picks VC=1, avoiding head-of-line block");
    LoopbackNoc noc(/*req*/64, /*rsp*/64);
    std::array<std::vector<uint8_t>,
               VcArb::AXI_CH_COUNT> candidates{};
    candidates[ni::AXI_CH_B] = {0, 1};
    candidates[ni::AXI_CH_R] = {2, 3};
    auto arb = VcArb::multi_candidate(noc.rsp_out(), /*num_vc=*/4,
                                      candidates, /*pending_depth=*/2);

    // Fill VC=0 pending to capacity (2 Bs land on VC=0).
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_B)));
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_B)));
    EXPECT_EQ(arb.pending_size(0), 2u);
    EXPECT_EQ(arb.pending_size(1), 0u);

    // Next B: VC=0 full -> candidate fallback picks VC=1.
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_B)));
    EXPECT_EQ(arb.pending_size(1), 1u);
}

TEST(NsuVcArb, Nsu_RoundRobinFairness) {
    SCENARIO("NSU VcArb NUM_VC=4: 4 R flits pre-routed to VCs 0-3 via Mode B "
             "candidates {0,1,2,3}; tick 4 times -> flits emerge in RR order");
    LoopbackNoc noc(/*req*/64, /*rsp*/64);
    std::array<std::vector<uint8_t>,
               VcArb::AXI_CH_COUNT> candidates{};
    candidates[ni::AXI_CH_B] = {0};
    candidates[ni::AXI_CH_R] = {0, 1, 2, 3};
    auto arb = VcArb::multi_candidate(noc.rsp_out(), /*num_vc=*/4,
                                      candidates, /*pending_depth=*/1);

    // Push 4 R flits: pending_depth=1 so VC=0 fills first, then VC=1, etc.
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R)));  // VC=0
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R)));  // VC=1
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R)));  // VC=2
    ASSERT_TRUE(arb.push_flit(make_rsp_flit(ni::AXI_CH_R)));  // VC=3

    arb.tick(); arb.tick(); arb.tick(); arb.tick();

    for (uint8_t expected_vc = 0; expected_vc < 4; ++expected_vc) {
        auto f = noc.rsp_in().pop_flit(); ASSERT_TRUE(f.has_value());
        EXPECT_EQ(f->get_header_field("vc_id"), expected_vc);
    }
}

TEST(NsuVcArb, Nsu_CreditGating) {
    SCENARIO("NSU VcArb: downstream per_vc_depth=1; 2 R flits land on VC=0 and VC=1 "
             "(pending_depth=1 forces second R to overflow to VC=1); "
             "tick drains VC=0 first; second tick advances RR to VC=1 after "
             "downstream credit for VC=0 is exhausted");
    LoopbackNoc noc(/*req*/64, /*rsp*/64);
    noc.set_per_vc_depth(1);
    std::array<std::vector<uint8_t>,
               VcArb::AXI_CH_COUNT> candidates{};
    candidates[ni::AXI_CH_B] = {0};
    candidates[ni::AXI_CH_R] = {0, 1};
    // pending_depth=1: first R fills VC=0, second R spills to VC=1.
    auto arb = VcArb::multi_candidate(noc.rsp_out(), /*num_vc=*/2,
                                      candidates, /*pending_depth=*/1);

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
