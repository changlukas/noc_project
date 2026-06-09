#include "nmu/vc_arbiter.hpp"
#include "nmu/packetize.hpp"
#include "noc/wormhole_arbiter.hpp"
#include "axi/types.hpp"
#include "common/channel_model.hpp"
#include "common/scenario.hpp"
#include "ni/flit.hpp"
#include "ni_flit_constants.h"
#include <gtest/gtest.h>
#include <array>
#include <vector>

using ni::cmodel::Flit;
using ni::cmodel::nmu::VcArbiter;
using ni::cmodel::nmu::VcMode;
using ni::cmodel::testing::ChannelModel;

namespace {

Flit make_flit(uint8_t axi_ch, uint8_t dst_id = 0, uint8_t initial_vc = 0, uint64_t wlast = 0) {
    Flit f;
    f.set_header_field("axi_ch", axi_ch);
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("vc_id", initial_vc);
    f.set_header_field("src_id", 0x12);
    f.set_header_field("last", 1);  // legacy; VcArbiter does not consult header.last
    if (axi_ch == ni::AXI_CH_W) {
        f.set_payload_field("W", "wlast", wlast);
    }
    return f;
}

}  // namespace

// ---------------------------------------------------------------------------
// Parameterized fixture — NUM_VC ∈ {1, 2, 4, 8}
// ---------------------------------------------------------------------------

class NmuVcArbParam : public ::testing::TestWithParam<std::size_t> {};

// ReadWriteSplit: AW → write_vc=0, AR → read_vc=1.
// Requires num_vc ≥ 2 (distinct write/read VCs).
TEST_P(NmuVcArbParam, ReadWriteSplit_AW_AR_GoSeparateVcs) {
    const std::size_t num_vc = GetParam();
    if (num_vc < 2) GTEST_SKIP() << "needs NUM_VC >= 2";

    SCENARIO(
        "NMU VcArbiter Mode A: AW -> write_vc=0, AR -> read_vc=1; "
        "verify pending queues + downstream stamps");
    ChannelModel noc(/*req*/ 32, /*rsp*/ 32);
    auto arb = VcArbiter::read_write_split(noc.req_out(), num_vc,
                                           /*write_vc=*/0, /*read_vc=*/1);

    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    EXPECT_EQ(arb.pending_size(0), 1u);  // AW landed on write_vc
    EXPECT_EQ(arb.pending_size(1), 1u);  // AR landed on read_vc

    arb.tick();
    arb.tick();
    auto f0 = noc.req_in().pop_flit();
    ASSERT_TRUE(f0.has_value());
    auto f1 = noc.req_in().pop_flit();
    ASSERT_TRUE(f1.has_value());
    // Round-robin starts at 0 -> VC=0 (AW) drains first, then VC=1 (AR).
    EXPECT_EQ(f0->get_header_field("axi_ch"), ni::AXI_CH_AW);
    EXPECT_EQ(f0->get_header_field("vc_id"), 0u);
    EXPECT_EQ(f1->get_header_field("axi_ch"), ni::AXI_CH_AR);
    EXPECT_EQ(f1->get_header_field("vc_id"), 1u);
}

// MultiCandidate HoL-avoidance.
// Candidate lists are adapted to num_vc:
//   num_vc=1  : single-VC, no multi-candidate overflow possible; skip
//   num_vc=2  : AR={0,1}
//   num_vc=4  : AR={0,1,2,3}
//   num_vc=8  : AR={0,1,2,3,4,5,6,7}
// The scenario: saturate the first AR candidate VC (VC=0) with pending_depth=2,
// verify the next AR spills to VC=1 (second candidate). AR has no Constraint
// A1 restriction (no AW/W pairing needed), so multiple ARs can be pushed freely.
TEST_P(NmuVcArbParam, MultiCandidate_HoLAvoidance) {
    const std::size_t num_vc = GetParam();
    if (num_vc < 2) GTEST_SKIP() << "needs NUM_VC >= 2 for multi-candidate AR overflow";

    SCENARIO(
        "NMU VcArbiter Mode B: saturate first AR candidate VC (VC=0) -> "
        "next AR picks second candidate VC (VC=1), avoiding head-of-line block");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);

    // AR spans all VCs in order; AW/W get a single VC=0 candidate.
    std::array<std::vector<uint8_t>, VcArbiter::AXI_CH_COUNT> candidates{};
    candidates[ni::AXI_CH_AW] = {0};
    candidates[ni::AXI_CH_W] = {0};
    for (std::size_t i = 0; i < num_vc; ++i) {
        candidates[ni::AXI_CH_AR].push_back(static_cast<uint8_t>(i));
    }
    auto arb = VcArbiter::multi_candidate(noc.req_out(), num_vc, candidates, /*pending_depth=*/2);

    // Fill VC=0 pending to capacity (2 ARs land on VC=0).
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    EXPECT_EQ(arb.pending_size(0), 2u);
    EXPECT_EQ(arb.pending_size(1), 0u);

    // Next AR: VC=0 full -> candidate fallback picks VC=1.
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    EXPECT_EQ(arb.pending_size(1), 1u);
}

// W follows AW invariant: all W beats of a burst route to the same VC as
// their paired AW. With Constraint A1 (WormholeArbiter upstream serializes
// AW + all W beats before next AW), a single outstanding AW at a time is
// the supported pattern. Requires num_vc >= 2 so the AW vs W VC assignment
// is observable (write_vc=0 for both in Mode A; read_vc=1 for AR).
TEST_P(NmuVcArbParam, WFollowsAW_InvariantEnforced) {
    const std::size_t num_vc = GetParam();
    if (num_vc < 2) GTEST_SKIP() << "needs num_vc >= 2";

    SCENARIO(
        "NMU VcArbiter NUM_VC=2: single outstanding AW + 3 W beats (last "
        "with wlast=1) — all W beats route to AW's VC. After W with wlast, "
        "current_aw_vc_ resets, allowing next AW to be pushed.");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split(noc.req_out(), num_vc, 0, 1);
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));
    EXPECT_TRUE(arb.has_current_aw());

    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_W, 0, 0, /*wlast=*/0)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_W, 0, 0, /*wlast=*/0)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_W, 0, 0, /*wlast=*/1)));
    EXPECT_FALSE(arb.has_current_aw());  // reset after wlast

    // All 4 flits land on VC=0 (write_vc)
    EXPECT_EQ(arb.pending_size(0), 4u);
    EXPECT_EQ(arb.pending_size(1), 0u);

    // Drain all 4 to make room, then verify next AW can be pushed.
    for (int i = 0; i < 4; ++i) {
        arb.tick();
        noc.req_in().pop_flit();
    }
    EXPECT_EQ(arb.pending_size(0), 0u);

    // Next AW can now be pushed (current_aw_vc_ is clear, pending is empty)
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));
}

// current_aw_vc_ resets based on payload.W.wlast, NOT header.last.
// Requires num_vc >= 2 (uses read_write_split with write_vc=0, read_vc=1).
TEST_P(NmuVcArbParam, WlastFromPayloadNotHeader) {
    const std::size_t num_vc = GetParam();
    if (num_vc < 2) GTEST_SKIP() << "needs NUM_VC >= 2 (uses write_vc=0, read_vc=1)";

    SCENARIO(
        "NMU VcArbiter: current_aw_vc_ resets based on payload.W.wlast, "
        "NOT header.last. Push AW + 3 W beats; only the 3rd has "
        "payload.wlast=1; verify current_aw_vc_ only resets on the 3rd.");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split(noc.req_out(), num_vc, 0, 1);
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));
    EXPECT_TRUE(arb.has_current_aw());

    // Beat 1: payload.wlast=0 (intermediate W beat); even if header.last=1 in
    // the input flit (legacy bug shape), current_aw_vc_ MUST NOT reset.
    Flit w1;
    w1.set_header_field("axi_ch", ni::AXI_CH_W);
    w1.set_header_field("last", 1);  // bait: legacy bug-shape header.last
    w1.set_payload_field("W", "wlast", 0);
    ASSERT_TRUE(arb.push_flit(w1));
    EXPECT_TRUE(arb.has_current_aw()) << "wlast=0 -> must not reset";

    Flit w2;
    w2.set_header_field("axi_ch", ni::AXI_CH_W);
    w2.set_header_field("last", 1);
    w2.set_payload_field("W", "wlast", 0);
    ASSERT_TRUE(arb.push_flit(w2));
    EXPECT_TRUE(arb.has_current_aw());

    Flit w3;
    w3.set_header_field("axi_ch", ni::AXI_CH_W);
    w3.set_header_field("last", 1);
    w3.set_payload_field("W", "wlast", 1);  // genuine burst end
    ASSERT_TRUE(arb.push_flit(w3));
    EXPECT_FALSE(arb.has_current_aw());
}

// Round-robin fairness: num_vc ARs pre-routed to num_vc distinct VCs via
// Mode B candidates with pending_depth=1; tick num_vc times -> flits emerge
// in RR order VC=0, VC=1, ..., VC=num_vc-1.
// Requires num_vc ≥ 2 so there is more than one VC to arbitrate.
TEST_P(NmuVcArbParam, RoundRobinFairness_AllVcsServiced_NoStarvation) {
    const std::size_t num_vc = GetParam();
    if (num_vc < 2) GTEST_SKIP() << "needs NUM_VC >= 2 to exercise round-robin";

    SCENARIO(
        "NMU VcArbiter Mode B: num_vc ARs pre-routed to distinct VCs via "
        "candidate list; tick num_vc times -> flits emerge in RR order");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    std::array<std::vector<uint8_t>, VcArbiter::AXI_CH_COUNT> candidates{};
    candidates[ni::AXI_CH_AW] = {0};
    candidates[ni::AXI_CH_W] = {0};
    // AR candidate list spans all VCs; pending_depth=1 fills each VC in order.
    for (std::size_t i = 0; i < num_vc; ++i) {
        candidates[ni::AXI_CH_AR].push_back(static_cast<uint8_t>(i));
    }
    auto arb = VcArbiter::multi_candidate(noc.req_out(), num_vc, candidates, /*pending_depth=*/1);

    // Push num_vc ARs: first fills VC=0, second fills VC=1, ..., etc.
    for (std::size_t i = 0; i < num_vc; ++i) {
        ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
        EXPECT_EQ(arb.pending_size(i), 1u);
    }

    for (std::size_t i = 0; i < num_vc; ++i) arb.tick();

    for (std::size_t i = 0; i < num_vc; ++i) {
        EXPECT_EQ(arb.pending_size(i), 0u);
    }

    for (std::size_t expected_vc = 0; expected_vc < num_vc; ++expected_vc) {
        auto f = noc.req_in().pop_flit();
        ASSERT_TRUE(f.has_value());
        EXPECT_EQ(f->get_header_field("vc_id"), expected_vc);
    }
}

// Credit gating: ChannelModel per_vc_depth=1 caps downstream credit.
// Works at any NUM_VC (the per-VC independent credit logic is the same).
// Uses a single VC (VC=0) via read_write_split to keep the scenario simple.
TEST_P(NmuVcArbParam, CreditGating_TickIdleWhenAllVcsBlocked) {
    const std::size_t num_vc = GetParam();

    SCENARIO(
        "NMU VcArbiter: all flits -> VC=0; ChannelModel per_vc_depth=1 caps "
        "downstream credit. Push 4 AR flits, tick drains 1. Subsequent "
        "tick is idle (downstream credit exhausted). Pop downstream -> "
        "credit returns -> next tick drains 1 more.");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    noc.set_per_vc_depth(1);
    auto arb = VcArbiter::read_write_split(noc.req_out(), num_vc, 0, 0,
                                           /*pending_depth=*/8);
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    EXPECT_EQ(arb.pending_size(0), 4u);

    // First tick: VC=0 has pending + downstream credit -> 1 flit out.
    arb.tick();
    EXPECT_EQ(arb.pending_size(0), 3u);
    EXPECT_EQ(noc.nmu_req_per_vc_in_flight(0), 1u);

    // Downstream credit exhausted (per_vc_depth=1) -> next tick is idle.
    arb.tick();
    EXPECT_EQ(arb.pending_size(0), 3u) << "tick must be idle, no spurious push";
    EXPECT_EQ(noc.nmu_req_per_vc_in_flight(0), 1u);

    // Pop downstream -> credit returns -> next tick drains.
    auto f = noc.req_in().pop_flit();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(noc.nmu_req_per_vc_in_flight(0), 0u);
    arb.tick();
    EXPECT_EQ(arb.pending_size(0), 2u);
}

// Backpressure: VcArbiter pending_depth=2, single VC. After 2 pushes, pending
// is full -> push_flit returns false; credit_avail(0) also returns false.
// Works at any NUM_VC (tests VC=0 in isolation).
TEST_P(NmuVcArbParam, BackpressureChain_VcArbToUpstream) {
    const std::size_t num_vc = GetParam();

    SCENARIO(
        "NMU VcArbiter: VcArbiter pending_depth=2. After 2 pushes, "
        "VcArbiter's pending_[0] is full -> push_flit returns false; "
        "credit_avail(0) also returns false. Backpressure visible to "
        "upstream Packetize.");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split(noc.req_out(), num_vc, 0, 0,
                                           /*pending_depth=*/2);
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    EXPECT_EQ(arb.pending_size(0), 2u);
    EXPECT_FALSE(arb.credit_avail(0));
    EXPECT_FALSE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
}

INSTANTIATE_TEST_SUITE_P(NumVcMatrix, NmuVcArbParam,
                         ::testing::Values(std::size_t(1), std::size_t(2), std::size_t(4),
                                           std::size_t(8)),
                         [](const ::testing::TestParamInfo<std::size_t>& info) {
                             return "NumVc" + std::to_string(info.param);
                         });

// ---------------------------------------------------------------------------
// Plain TEST() — not parameterized:
//   Degenerate_NumVc1_AllModesPassthrough  : specifically tests NUM_VC=1 behavior
//   EnabledModeMixedWith_PriorRoundTests   : decorator transparency at NUM_VC=1
//   WHeaderLastMatchesWlast                : §12 fix; decorator at NUM_VC=1
//   2 death tests                          : EXPECT_DEATH doesn't compose with TEST_P
// ---------------------------------------------------------------------------

TEST(NmuVcArbiter, Degenerate_NumVc1_AllModesPassthrough) {
    SCENARIO(
        "NMU VcArbiter: NUM_VC=1, both Mode A (read_write_split) and Mode B "
        "(multi_candidate) route every axi_ch -> VC=0; behavior "
        "observationally identical to direct Packetize -> ChannelModel");

    // Mode A
    {
        ChannelModel noc(/*req*/ 32, /*rsp*/ 32);
        auto arb = VcArbiter::read_write_split(noc.req_out(), /*num_vc=*/1, 0, 0);
        ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));
        ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_W, 0, 0, /*wlast=*/1)));
        ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
        EXPECT_EQ(arb.pending_size(0), 3u);
        arb.tick();
        arb.tick();
        arb.tick();
        EXPECT_EQ(arb.pending_size(0), 0u);
        for (int i = 0; i < 3; ++i) {
            auto f = noc.req_in().pop_flit();
            ASSERT_TRUE(f.has_value());
            EXPECT_EQ(f->get_header_field("vc_id"), 0u);
        }
    }
    // Mode B -- even with multi_candidate, num_vc=1 forces VC=0.
    {
        ChannelModel noc(/*req*/ 32, /*rsp*/ 32);
        std::array<std::vector<uint8_t>, VcArbiter::AXI_CH_COUNT> candidates{};
        candidates[ni::AXI_CH_AW] = {0};
        candidates[ni::AXI_CH_W] = {0};
        candidates[ni::AXI_CH_AR] = {0};
        auto arb = VcArbiter::multi_candidate(noc.req_out(), /*num_vc=*/1, candidates);
        ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AW)));
        ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_W, 0, 0, /*wlast=*/1)));
        ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
        EXPECT_EQ(arb.pending_size(0), 3u);
        arb.tick();
        arb.tick();
        arb.tick();
        EXPECT_EQ(arb.pending_size(0), 0u);
        for (int i = 0; i < 3; ++i) {
            auto f = noc.req_in().pop_flit();
            ASSERT_TRUE(f.has_value());
            EXPECT_EQ(f->get_header_field("vc_id"), 0u);
        }
    }
}

TEST(NmuVcArbiter, EnabledModeMixedWith_PriorRoundTests) {
    SCENARIO(
        "NMU VcArbiter decorator is transparent to nmu::Packetize: wire "
        "Packetize -> WormholeArbiter -> VcArbiter -> ChannelModel with "
        "NUM_VC=1 and verify Packetize-emitted AW + W flits arrive intact "
        "at NSU side.");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto vc_arb = VcArbiter::read_write_split(noc.req_out(), /*num_vc=*/1, 0, 0);
    ni::cmodel::noc::WormholeArbiter<ni::cmodel::noc::NocReqOut> wh_arb(
        vc_arb, /*num_inputs=*/3, std::vector<ni::cmodel::noc::ChannelPairing>{{0, 1}});
    ni::cmodel::nmu::Packetize pkt(wh_arb.input(0), wh_arb.input(1), wh_arb.input(2),
                                   /*src_id=*/0x12);

    ni::cmodel::axi::AwBeat aw{};
    aw.id = 0x07;
    aw.addr = 0x340000;
    aw.len = 0;
    aw.size = 5;
    aw.burst = ni::cmodel::axi::Burst::INCR;
    ASSERT_TRUE(pkt.push_aw(aw));

    ni::cmodel::axi::WBeat w{};
    for (int i = 0; i < 32; ++i) w.data[i] = static_cast<uint8_t>(i);
    w.strb = 0xFFFFFFFF;
    w.last = true;
    ASSERT_TRUE(pkt.push_w(w));

    wh_arb.tick();  // AW drains from wh_arb input(0) to vc_arb; locks to input(1)
    wh_arb.tick();  // W drains from wh_arb input(1) to vc_arb; unlocks
    vc_arb.tick();
    vc_arb.tick();
    auto f_aw = noc.req_in().pop_flit();
    ASSERT_TRUE(f_aw.has_value());
    auto f_w = noc.req_in().pop_flit();
    ASSERT_TRUE(f_w.has_value());
    EXPECT_EQ(f_aw->get_header_field("axi_ch"), ni::AXI_CH_AW);
    EXPECT_EQ(f_w->get_header_field("axi_ch"), ni::AXI_CH_W);
    EXPECT_EQ(f_aw->get_header_field("dst_id"), 0x34u);
    EXPECT_EQ(f_w->get_header_field("dst_id"), 0x34u);
}

TEST(NmuVcArbiter, WHeaderLastMatchesWlast) {
    SCENARIO(
        "NMU VcArbiter: header.last on W flits emitted via Packetize -> "
        "WormholeArbiter -> VcArbiter -> downstream matches payload.wlast "
        "(verifies §12 packetize fix is preserved end-to-end through the "
        "decorator pipeline).");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto vc_arb = VcArbiter::read_write_split(noc.req_out(), /*num_vc=*/1, 0, 0);
    ni::cmodel::noc::WormholeArbiter<ni::cmodel::noc::NocReqOut> wh_arb(
        vc_arb, /*num_inputs=*/3, std::vector<ni::cmodel::noc::ChannelPairing>{{0, 1}});
    ni::cmodel::nmu::Packetize pkt(wh_arb.input(0), wh_arb.input(1), wh_arb.input(2),
                                   /*src_id=*/0x12);

    ni::cmodel::axi::AwBeat aw{};
    aw.id = 0x07;
    aw.addr = 0x340000;
    aw.len = 2;
    aw.size = 5;
    aw.burst = ni::cmodel::axi::Burst::INCR;
    ASSERT_TRUE(pkt.push_aw(aw));

    auto make_w = [](bool last) {
        ni::cmodel::axi::WBeat w{};
        for (int i = 0; i < 32; ++i) w.data[i] = 0;
        w.strb = 0xFFFFFFFF;
        w.last = last;
        return w;
    };
    ASSERT_TRUE(pkt.push_w(make_w(false)));
    ASSERT_TRUE(pkt.push_w(make_w(false)));
    ASSERT_TRUE(pkt.push_w(make_w(true)));

    // Drain AW + 3 W flits: wh_arb ticks first, then vc_arb ticks.
    for (int i = 0; i < 4; ++i) wh_arb.tick();
    for (int i = 0; i < 4; ++i) vc_arb.tick();

    noc.req_in().pop_flit();  // discard AW
    for (int i = 0; i < 3; ++i) {
        auto f = noc.req_in().pop_flit();
        ASSERT_TRUE(f.has_value());
        uint64_t expected = (i == 2) ? 1u : 0u;
        EXPECT_EQ(f->get_header_field("last"), expected);
        EXPECT_EQ(f->get_payload_field("W", "wlast"), expected);
    }
}

namespace {

class LyingDownstream : public ni::cmodel::noc::NocReqOut {
  public:
    bool push_flit(const Flit&) override { return false; }
    bool credit_avail(uint8_t) const override { return true; }
};

}  // namespace

TEST(NmuVcArbDeath, WFollowsAW_WBeforeAW_DeathTest) {
    SCENARIO(
        "NMU VcArbiter: push_flit(W) before any push_flit(AW) violates "
        "Constraint A1; current_aw_vc_ has no value so VcArbiter must "
        "assert+abort instead of UB.");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    auto arb = VcArbiter::read_write_split(noc.req_out(), /*num_vc=*/2, 0, 1);
    EXPECT_DEATH(
        {
            Flit w;
            w.set_header_field("axi_ch", ni::AXI_CH_W);
            w.set_payload_field("W", "wlast", 1);
            arb.push_flit(w);
        },
        ".*");
}

TEST(NmuVcArbDeath, ProtocolViolation_LyingDownstream_DeathTest) {
    SCENARIO(
        "NMU VcArbiter: downstream lies -- credit_avail returns true but "
        "push_flit returns false. VcArbiter::tick must assert+abort (the "
        "protocol guarantees credit_avail=true implies push_flit "
        "success on the next call).");
    LyingDownstream liar;
    auto arb = VcArbiter::read_write_split(liar, /*num_vc=*/1, 0, 0);
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_AR)));
    EXPECT_DEATH({ arb.tick(); }, ".*");
}
