#include "nmu/vc_arbiter.hpp"
#include "nmu/packetize.hpp"
#include "ni/wormhole_arbiter.hpp"
#include "axi/types.hpp"
#include "common/channel_model.hpp"
#include "common/scenario.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include <gtest/gtest.h>
#include <array>
#include <vector>

using ni::cmodel::Flit;
using ni::cmodel::nmu::VcArbiter;
using ni::cmodel::testing::ChannelModel;

namespace {

Flit make_flit(uint8_t axi_ch, uint8_t dst_id = 0, uint8_t initial_vc = 0, uint64_t wlast = 0,
               uint8_t id = 0) {
    Flit f;
    f.set_header_field("axi_ch", axi_ch);
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("vc_id", initial_vc);
    f.set_header_field("src_id", 0x12);
    f.set_header_field("flit_tail", 1);  // legacy; VcArbiter does not consult header.flit_tail
    if (axi_ch == ni::AXI_CH_NarrowW) {
        f.set_payload_field("NARROW_W", "wlast", wlast);
    } else if (axi_ch == ni::AXI_CH_NarrowAw) {
        f.set_payload_field("AW", "awid", id);
    } else if (axi_ch == ni::AXI_CH_NarrowAr) {
        f.set_payload_field("AR", "arid", id);
    }
    return f;
}

// Push a flit, drain one to the channel, return the assigned vc_id.
uint8_t push_and_vc(VcArbiter& arb, ChannelModel& noc, const Flit& f) {
    EXPECT_TRUE(arb.push_flit(f));
    arb.tick();
    auto out = noc.req_in().pop_flit();
    EXPECT_TRUE(out.has_value());
    return static_cast<uint8_t>(out->get_header_field("vc_id"));
}

}  // namespace

// ---------------------------------------------------------------------------
// Parameterized fixture — NUM_VC ∈ {1, 2} (see INSTANTIATE below)
// ---------------------------------------------------------------------------

class NmuVcArbParam : public ::testing::TestWithParam<std::size_t> {};

// W follows AW invariant: all W beats of a burst route to the same VC as
// their paired AW. With the WormholeArbiter upstream serializing AW + all W
// beats before the next AW, a single outstanding AW at a time is
// the supported pattern.
TEST_P(NmuVcArbParam, WFollowsAW_InvariantEnforced) {
    const std::size_t num_vc = GetParam();

    SCENARIO(
        "NMU VcArbiter: single outstanding AW + 3 W beats (last "
        "with wlast=1) — all W beats route to AW's VC. After W with wlast, "
        "current_aw_vc_ resets, allowing next AW to be pushed.");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    VcArbiter arb(noc.req_out(), num_vc);
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_NarrowAw)));
    EXPECT_TRUE(arb.has_current_aw());

    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_NarrowW, 0, 0, /*wlast=*/0)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_NarrowW, 0, 0, /*wlast=*/0)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_NarrowW, 0, 0, /*wlast=*/1)));
    EXPECT_FALSE(arb.has_current_aw());  // reset after wlast

    // All 4 flits land on the AW's VC (round-robin starts at 0).
    EXPECT_EQ(arb.pending_size(0), 4u);
    EXPECT_EQ(arb.pending_size(1), 0u);

    // Drain all 4 to make room, then verify next AW can be pushed.
    for (int i = 0; i < 4; ++i) {
        arb.tick();
        noc.req_in().pop_flit();
    }
    EXPECT_EQ(arb.pending_size(0), 0u);

    // Next AW can now be pushed (current_aw_vc_ is clear, pending is empty)
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_NarrowAw)));
}

// current_aw_vc_ resets based on payload.W.wlast, NOT header.flit_tail.
TEST_P(NmuVcArbParam, WlastFromPayloadNotHeader) {
    const std::size_t num_vc = GetParam();

    SCENARIO(
        "NMU VcArbiter: current_aw_vc_ resets based on payload.W.wlast, "
        "NOT header.flit_tail. Push AW + 3 W beats; only the 3rd has "
        "payload.wlast=1; verify current_aw_vc_ only resets on the 3rd.");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    VcArbiter arb(noc.req_out(), num_vc);
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_NarrowAw)));
    EXPECT_TRUE(arb.has_current_aw());

    // Beat 1: payload.wlast=0 (intermediate W beat); even if header.flit_tail=1 in
    // the input flit (legacy bug shape), current_aw_vc_ MUST NOT reset.
    Flit w1;
    w1.set_header_field("axi_ch", ni::AXI_CH_NarrowW);
    w1.set_header_field("flit_tail", 1);  // bait: legacy bug-shape header.flit_tail
    w1.set_payload_field("NARROW_W", "wlast", 0);
    ASSERT_TRUE(arb.push_flit(w1));
    EXPECT_TRUE(arb.has_current_aw()) << "wlast=0 -> must not reset";

    Flit w2;
    w2.set_header_field("axi_ch", ni::AXI_CH_NarrowW);
    w2.set_header_field("flit_tail", 1);
    w2.set_payload_field("NARROW_W", "wlast", 0);
    ASSERT_TRUE(arb.push_flit(w2));
    EXPECT_TRUE(arb.has_current_aw());

    Flit w3;
    w3.set_header_field("axi_ch", ni::AXI_CH_NarrowW);
    w3.set_header_field("flit_tail", 1);
    w3.set_payload_field("NARROW_W", "wlast", 1);  // genuine burst end
    ASSERT_TRUE(arb.push_flit(w3));
    EXPECT_FALSE(arb.has_current_aw());
}

// Credit gating: ChannelModel per_vc_depth=1 caps downstream credit.
// Works at any NUM_VC: one AW + its W beats all pin to the AW's VC (VC=0,
// round-robin start), so every other VC is empty and cannot mask the stall.
TEST_P(NmuVcArbParam, CreditGating_TickIdleWhenAllVcsBlocked) {
    const std::size_t num_vc = GetParam();

    SCENARIO(
        "NMU VcArbiter: AW + 3 W beats all -> VC=0; ChannelModel per_vc_depth=1 "
        "caps downstream credit. Tick drains 1. Subsequent "
        "tick is idle (downstream credit exhausted). Pop downstream -> "
        "credit returns -> next tick drains 1 more.");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    noc.set_per_vc_depth(1);
    VcArbiter arb(noc.req_out(), num_vc, /*pending_depth=*/8);
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_NarrowAw)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_NarrowW, 0, 0, /*wlast=*/0)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_NarrowW, 0, 0, /*wlast=*/0)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_NarrowW, 0, 0, /*wlast=*/1)));
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

// Backpressure: VcArbiter pending_depth=2. After 2 pushes the AW's VC is
// full -> push_flit returns false; credit_avail also returns false.
// Works at any NUM_VC (AW + W pin to one VC).
TEST_P(NmuVcArbParam, BackpressureChain_VcArbToUpstream) {
    const std::size_t num_vc = GetParam();

    SCENARIO(
        "NMU VcArbiter: VcArbiter pending_depth=2. After AW + 1 W beat, "
        "VcArbiter's pending_[0] is full -> push_flit returns false; "
        "credit_avail(0) also returns false. Backpressure visible to "
        "upstream Packetize.");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    VcArbiter arb(noc.req_out(), num_vc, /*pending_depth=*/2);
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_NarrowAw)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_NarrowW, 0, 0, /*wlast=*/0)));
    EXPECT_EQ(arb.pending_size(0), 2u);
    EXPECT_FALSE(arb.credit_avail(0));
    EXPECT_FALSE(arb.push_flit(make_flit(ni::AXI_CH_NarrowW, 0, 0, /*wlast=*/0)));
}

// Round-robin spread: four DISTINCT unbound arids must not all land on VC 0.
// First-available would fix all to VC=0; round-robin walks 0,1,2,3.
TEST(NmuVcArbiterRoundRobin, DistinctReadIdsSpreadAcrossVcs) {
    SCENARIO("NMU VcArbiter: distinct unbound arids round-robin over every VC");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    VcArbiter arb(noc.req_out(), /*num_vc=*/4);
    uint8_t vc_a = push_and_vc(arb, noc, make_flit(ni::AXI_CH_NarrowAr, 0, 0, 0, /*id=*/0x10));
    uint8_t vc_b = push_and_vc(arb, noc, make_flit(ni::AXI_CH_NarrowAr, 0, 0, 0, /*id=*/0x11));
    uint8_t vc_c = push_and_vc(arb, noc, make_flit(ni::AXI_CH_NarrowAr, 0, 0, 0, /*id=*/0x12));
    uint8_t vc_d = push_and_vc(arb, noc, make_flit(ni::AXI_CH_NarrowAr, 0, 0, 0, /*id=*/0x13));
    EXPECT_EQ(vc_a, 0u);
    EXPECT_EQ(vc_b, 1u);
    EXPECT_EQ(vc_c, 2u);
    EXPECT_EQ(vc_d, 3u);
}

// No fixed VC yet: same awid, different dst_id -- the streak broke, so
// round-robin (spread) resumes.
TEST(NmuVcArbiterRoundRobin, SameWriteIdDifferentDestRoundRobins) {
    SCENARIO("NMU VcArbiter: same awid, different dst_id -- no fixed VC yet, round-robins");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    VcArbiter arb(noc.req_out(), /*num_vc=*/4);
    uint8_t a =
        push_and_vc(arb, noc, make_flit(ni::AXI_CH_NarrowAw, /*dst_id=*/0, 0, 0, /*id=*/0x20));
    ASSERT_EQ(push_and_vc(arb, noc, make_flit(ni::AXI_CH_NarrowW, 0, 0, /*wlast=*/1)), a);
    uint8_t b =
        push_and_vc(arb, noc, make_flit(ni::AXI_CH_NarrowAw, /*dst_id=*/1, 0, 0, /*id=*/0x20));
    EXPECT_EQ(a, 0u);
    EXPECT_EQ(b, 1u) << "different dst_id -- no fixed VC yet, round-robin advances";
}

// ordering_req=1 (RoB-owned) flits are order-free by construction -- the RoB
// reorders them, so the fixed VC id logic is skipped and they always
// round-robin, even with a matching (dst, id).
TEST(NmuVcArbiterRoundRobin, RobbedFlitsRoundRobinRegardlessOfDest) {
    SCENARIO("NMU VcArbiter: ordering_req=1 flits round-robin even with same (dst,id)");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    VcArbiter arb(noc.req_out(), /*num_vc=*/4);
    Flit f1 = make_flit(ni::AXI_CH_NarrowAw, /*dst_id=*/0, 0, 0, /*id=*/0x20);
    f1.set_header_field("ordering_req", 1);
    Flit f2 = make_flit(ni::AXI_CH_NarrowAw, /*dst_id=*/0, 0, 0, /*id=*/0x20);
    f2.set_header_field("ordering_req", 1);
    uint8_t a = push_and_vc(arb, noc, f1);
    ASSERT_EQ(push_and_vc(arb, noc, make_flit(ni::AXI_CH_NarrowW, 0, 0, /*wlast=*/1)), a);
    uint8_t b = push_and_vc(arb, noc, f2);
    EXPECT_EQ(a, 0u);
    EXPECT_EQ(b, 1u) << "ordering_req=1 -- always round-robin, never fixed";
}

// NUM_VC==1 short-circuits before the fixed VC id check (select_vc_for_axi_ch's
// num_vc_==1 branch returns early) -- degenerate path unaffected.
TEST(NmuVcArbiterRoundRobin, NumVc1SameIdSameDestUnaffected) {
    SCENARIO("NMU VcArbiter: NUM_VC=1, fixed VC id logic short-circuited, everything -> VC 0");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    VcArbiter arb(noc.req_out(), /*num_vc=*/1);
    uint8_t a =
        push_and_vc(arb, noc, make_flit(ni::AXI_CH_NarrowAr, /*dst_id=*/0, 0, 0, /*id=*/0x20));
    uint8_t b =
        push_and_vc(arb, noc, make_flit(ni::AXI_CH_NarrowAr, /*dst_id=*/1, 0, 0, /*id=*/0x20));
    EXPECT_EQ(a, 0u);
    EXPECT_EQ(b, 0u);
}

// W follows its AW's VC even when that VC came from a fixed VC id reuse, not
// a fresh round-robin pick -- the second AW's W beat must land on the reused
// VC (0), not the next round-robin slot (1).
TEST(NmuVcArbiter, WFollowsAW_ReusedFixedVc) {
    SCENARIO(
        "NMU VcArbiter: second same-(dst,awid) AW reuses its fixed VC; "
        "W beat follows current_aw_vc_ (the reused VC), not round-robin");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    VcArbiter arb(noc.req_out(), /*num_vc=*/4);

    // AW1 (dst=0, id=0x20): first sighting -> round-robin picks VC 0.
    uint8_t aw1_vc =
        push_and_vc(arb, noc, make_flit(ni::AXI_CH_NarrowAw, /*dst_id=*/0, 0, 0, /*id=*/0x20));
    EXPECT_EQ(aw1_vc, 0u);
    uint8_t w1_vc = push_and_vc(arb, noc, make_flit(ni::AXI_CH_NarrowW, 0, 0, /*wlast=*/1));
    EXPECT_EQ(w1_vc, 0u);
    EXPECT_FALSE(arb.has_current_aw());

    // AW2 same (dst,id): fixed VC hit -> reuses VC 0 (round-robin would pick VC 1).
    uint8_t aw2_vc =
        push_and_vc(arb, noc, make_flit(ni::AXI_CH_NarrowAw, /*dst_id=*/0, 0, 0, /*id=*/0x20));
    EXPECT_EQ(aw2_vc, 0u) << "fixed VC hit must reuse VC0, not round-robin to VC1";
    uint8_t w2_vc = push_and_vc(arb, noc, make_flit(ni::AXI_CH_NarrowW, 0, 0, /*wlast=*/1));
    EXPECT_EQ(w2_vc, 0u) << "W must follow AW2's reused VC";
}

INSTANTIATE_TEST_SUITE_P(NumVcMatrix, NmuVcArbParam,
                         ::testing::Values(std::size_t(1), std::size_t(2)),
                         [](const ::testing::TestParamInfo<std::size_t>& info) {
                             return "NumVc" + std::to_string(info.param);
                         });

// ---------------------------------------------------------------------------
// Plain TEST() — not parameterized:
//   Degenerate_NumVc1_AllModesPassthrough  : specifically tests NUM_VC=1 behavior
//   EnabledModeMixedWith_SingleVcTests     : decorator transparency at NUM_VC=1
//   WHeaderFlitTailMatchesWlast             : decorator at NUM_VC=1
//   2 death tests                          : EXPECT_DEATH doesn't compose with TEST_P
// ---------------------------------------------------------------------------

TEST(NmuVcArbiter, Degenerate_NumVc1_AllModesPassthrough) {
    SCENARIO(
        "NMU VcArbiter: NUM_VC=1 routes every axi_ch -> VC=0; "
        "behavior observationally identical to direct Packetize -> ChannelModel");

    ChannelModel noc(/*req*/ 32, /*rsp*/ 32);
    VcArbiter arb(noc.req_out(), /*num_vc=*/1);
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_NarrowAw)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_NarrowW, 0, 0, /*wlast=*/1)));
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_NarrowAr)));
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

TEST(NmuVcArbiter, EnabledModeMixedWith_SingleVcTests) {
    SCENARIO(
        "NMU VcArbiter decorator is transparent to nmu::Packetize: wire "
        "Packetize -> WormholeArbiter -> VcArbiter -> ChannelModel with "
        "NUM_VC=1 and verify Packetize-emitted AW + W flits arrive intact "
        "at NSU side.");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    VcArbiter vc_arb(noc.req_out(), /*num_vc=*/1);
    ni::cmodel::router::WormholeArbiter<ni::cmodel::router::NocReqOut> wh_arb(
        vc_arb, /*num_inputs=*/3, std::vector<ni::cmodel::router::ChannelPairing>{{0, 1}});
    // 16x16 uniform, no rebase: dst = addr/4GB, matching the pre-SAM xy_route mapping.
    auto sam = ni::cmodel::nmu::addr_trans::SamTable::uniform(16, 16, 0x100000000ull);
    ni::cmodel::nmu::Packetize pkt(wh_arb.input(0), wh_arb.input(1), wh_arb.input(2),
                                   wh_arb.input(0), wh_arb.input(1), /*src_id=*/0x12, sam);

    ni::cmodel::axi::AwBeat aw{};
    aw.id = 0x07;
    aw.addr = 0x3400000000;
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
    // SamTable::uniform() with no "space" annotation defaults to data class.
    EXPECT_EQ(f_aw->get_header_field("axi_ch"), ni::AXI_CH_DataAw);
    EXPECT_EQ(f_w->get_header_field("axi_ch"), ni::AXI_CH_DataW);
    EXPECT_EQ(f_aw->get_header_field("dst_id"), 0x34u);
    EXPECT_EQ(f_w->get_header_field("dst_id"), 0x34u);
}

TEST(NmuVcArbiter, WHeaderFlitTailMatchesWlast) {
    SCENARIO(
        "NMU VcArbiter: header.flit_tail on W flits emitted via Packetize -> "
        "WormholeArbiter -> VcArbiter -> downstream matches payload.wlast "
        "(verifies the packetize header.flit_tail fix is preserved end-to-end through the "
        "decorator pipeline).");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    VcArbiter vc_arb(noc.req_out(), /*num_vc=*/1);
    ni::cmodel::router::WormholeArbiter<ni::cmodel::router::NocReqOut> wh_arb(
        vc_arb, /*num_inputs=*/3, std::vector<ni::cmodel::router::ChannelPairing>{{0, 1}});
    // 16x16 uniform, no rebase: dst = addr/4GB, matching the pre-SAM xy_route mapping.
    auto sam = ni::cmodel::nmu::addr_trans::SamTable::uniform(16, 16, 0x100000000ull);
    ni::cmodel::nmu::Packetize pkt(wh_arb.input(0), wh_arb.input(1), wh_arb.input(2),
                                   wh_arb.input(0), wh_arb.input(1), /*src_id=*/0x12, sam);

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
        EXPECT_EQ(f->get_header_field("flit_tail"), expected);
        EXPECT_EQ(f->get_payload_field("NARROW_W", "wlast"), expected);
    }
}

namespace {

class LyingDownstream : public ni::cmodel::router::NocReqOut {
  public:
    bool push_flit(const Flit&) override { return false; }
    bool credit_avail(uint8_t) const override { return true; }
};

}  // namespace

TEST(NmuVcArbDeath, WFollowsAW_WBeforeAW_DeathTest) {
    SCENARIO(
        "NMU VcArbiter: push_flit(W) before any push_flit(AW) violates the "
        "W-follows-AW invariant; current_aw_vc_ has no value so VcArbiter must "
        "assert+abort instead of UB.");
    ChannelModel noc(/*req*/ 64, /*rsp*/ 64);
    VcArbiter arb(noc.req_out(), /*num_vc=*/2);
    EXPECT_DEATH(
        {
            Flit w;
            w.set_header_field("axi_ch", ni::AXI_CH_NarrowW);
            w.set_payload_field("NARROW_W", "wlast", 1);
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
    VcArbiter arb(liar, /*num_vc=*/1);
    ASSERT_TRUE(arb.push_flit(make_flit(ni::AXI_CH_NarrowAr)));
    EXPECT_DEATH({ arb.tick(); }, ".*");
}
