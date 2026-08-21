#include "ni/wormhole_arbiter.hpp"
#include "common/per_channel_capture.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include <gtest/gtest.h>
#include <vector>

using ni::cmodel::Flit;
using ni::cmodel::router::ChannelPairing;
using ni::cmodel::router::WormholeArbiter;
using ni::cmodel::testing::ReqCapture;

namespace {

Flit make_flit(uint8_t axi_ch, uint64_t flit_tail, uint64_t wlast = 0) {
    Flit f;
    f.set_header_field("axi_ch", axi_ch);
    f.set_header_field("dst_id", 0);
    f.set_header_field("vc_id", 0);
    f.set_header_field("flit_tail", flit_tail);
    if (axi_ch == ni::AXI_CH_NarrowW) {
        f.set_payload_field("NARROW_W", "wlast", wlast);
    }
    return f;
}

}  // namespace

// ---- Functional tests (8) ----

TEST(NocWormholeArbiter, PassThroughNoPairing) {
    ReqCapture down;
    WormholeArbiter<ni::cmodel::router::NocReqOut> arb(down, /*num_inputs=*/2, {});

    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_NarrowAr, /*flit_tail=*/1)));
    ASSERT_TRUE(arb.input(1).push_flit(make_flit(ni::AXI_CH_NarrowAr, /*flit_tail=*/1)));
    arb.tick();
    arb.tick();
    EXPECT_EQ(down.size(), 2u);
    EXPECT_FALSE(arb.is_locked());
}

// Self-lock is the default S3a T5's DatMergeWrap relies on: a multi-flit worm locks the arbiter to
// its own input until the tail drains.
TEST(NocWormholeArbiter, SelfLockNoPairingExcludesOtherInputUntilTail) {
    ReqCapture down;
    WormholeArbiter<ni::cmodel::router::NocReqOut> arb(down, /*num_inputs=*/2, {});

    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_DataAw, /*flit_tail=*/0)));
    ASSERT_TRUE(arb.input(1).push_flit(make_flit(ni::AXI_CH_NarrowAr, /*flit_tail=*/1)));

    arb.tick();  // input 0's head drains, self-locks to input 0 (no pairing target)
    EXPECT_TRUE(arb.is_locked());
    EXPECT_EQ(*arb.locked_to(), 0u);
    EXPECT_EQ(down.size(), 1u);
    EXPECT_EQ(arb.pending_size(1), 1u) << "input 1's flit excluded while locked to input 0";

    arb.tick();  // locked target (input 0) empty this cycle -> idle; input 1 still excluded
    EXPECT_EQ(down.size(), 1u);
    EXPECT_TRUE(arb.is_locked());

    ASSERT_TRUE(
        arb.input(0).push_flit(make_flit(ni::AXI_CH_DataAw, /*flit_tail=*/1)));  // worm tail
    arb.tick();                                                                  // unlocks
    EXPECT_FALSE(arb.is_locked());
    EXPECT_EQ(down.size(), 2u);

    arb.tick();  // now input 1's flit can win arbitration
    EXPECT_EQ(down.size(), 3u);
    EXPECT_EQ(arb.pending_size(1), 0u);
}

TEST(NocWormholeArbiter, AwTriggersLock) {
    ReqCapture down;
    WormholeArbiter<ni::cmodel::router::NocReqOut> arb(down, /*num_inputs=*/3, {{0, 1}});

    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_NarrowAw, /*flit_tail=*/0)));
    arb.tick();
    EXPECT_EQ(down.size(), 1u);
    EXPECT_TRUE(arb.is_locked());
    EXPECT_EQ(*arb.locked_to(), 1u);
}

TEST(NocWormholeArbiter, ArCannotInterleaveDuringLock) {
    ReqCapture down;
    WormholeArbiter<ni::cmodel::router::NocReqOut> arb(down, /*num_inputs=*/3, {{0, 1}});

    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_NarrowAw, /*flit_tail=*/0)));
    arb.tick();  // AW drained, locked to w
    ASSERT_TRUE(arb.input(2).push_flit(make_flit(ni::AXI_CH_NarrowAr, /*flit_tail=*/1)));
    arb.tick();  // locked to w, w pending empty -> idle
    arb.tick();
    EXPECT_EQ(down.size(), 1u);          // only AW; AR still pending
    EXPECT_EQ(arb.pending_size(2), 1u);  // AR sitting
    EXPECT_TRUE(arb.is_locked());
}

TEST(NocWormholeArbiter, MultiBeatWBurstFlowsAndUnlocks) {
    ReqCapture down;
    WormholeArbiter<ni::cmodel::router::NocReqOut> arb(down, /*num_inputs=*/3, {{0, 1}});

    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_NarrowAw, /*flit_tail=*/0)));
    ASSERT_TRUE(
        arb.input(1).push_flit(make_flit(ni::AXI_CH_NarrowW, /*flit_tail=*/0, /*wlast=*/0)));
    ASSERT_TRUE(
        arb.input(1).push_flit(make_flit(ni::AXI_CH_NarrowW, /*flit_tail=*/0, /*wlast=*/0)));
    ASSERT_TRUE(
        arb.input(1).push_flit(make_flit(ni::AXI_CH_NarrowW, /*flit_tail=*/1, /*wlast=*/1)));

    for (int i = 0; i < 4; ++i) arb.tick();
    ASSERT_EQ(down.size(), 4u);
    EXPECT_FALSE(arb.is_locked());

    // Verify emission ORDER + per-flit header.flit_tail is correct
    auto f1 = down.pop();
    ASSERT_TRUE(f1.has_value());
    EXPECT_EQ(f1->get_header_field("axi_ch"), ni::AXI_CH_NarrowAw);
    EXPECT_EQ(f1->get_header_field("flit_tail"), 0u);

    auto f2 = down.pop();
    ASSERT_TRUE(f2.has_value());
    EXPECT_EQ(f2->get_header_field("axi_ch"), ni::AXI_CH_NarrowW);
    EXPECT_EQ(f2->get_header_field("flit_tail"), 0u);

    auto f3 = down.pop();
    ASSERT_TRUE(f3.has_value());
    EXPECT_EQ(f3->get_header_field("axi_ch"), ni::AXI_CH_NarrowW);
    EXPECT_EQ(f3->get_header_field("flit_tail"), 0u);

    auto f4 = down.pop();
    ASSERT_TRUE(f4.has_value());
    EXPECT_EQ(f4->get_header_field("axi_ch"), ni::AXI_CH_NarrowW);
    EXPECT_EQ(f4->get_header_field("flit_tail"), 1u);
}

TEST(NocWormholeArbiter, NocRspOutVariantPassThrough) {
    using ni::cmodel::testing::RspCapture;
    RspCapture down;
    WormholeArbiter<ni::cmodel::router::NocRspOut> arb(down, /*num_inputs=*/2, {});

    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_NarrowB, /*flit_tail=*/1)));
    ASSERT_TRUE(arb.input(1).push_flit(make_flit(ni::AXI_CH_NarrowR, /*flit_tail=*/1)));
    arb.tick();
    arb.tick();
    EXPECT_EQ(down.size(), 2u);
    EXPECT_FALSE(arb.is_locked());
}

TEST(NocWormholeArbiter, BackpressureUpstreamAndDownstream) {
    // Downstream with no room: push_flit returns false. The arbiter does not
    // inspect VC or track credit; push_flit's return value is the authoritative
    // ready signal, and a false return is retried (front flit retained).
    struct FullDown : ni::cmodel::router::NocReqOut {
        bool push_flit(const Flit&) override { return false; }
    } full;
    WormholeArbiter<ni::cmodel::router::NocReqOut> arb(full, /*num_inputs=*/2, {},
                                                       /*per_input_depth=*/2);

    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_NarrowAr, /*flit_tail=*/1)));
    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_NarrowAr, /*flit_tail=*/1)));
    EXPECT_FALSE(arb.input(0).push_flit(make_flit(ni::AXI_CH_NarrowAr, /*flit_tail=*/1)));  // full

    arb.tick();                          // downstream refuses push -> retain -> idle
    EXPECT_EQ(arb.pending_size(0), 2u);  // unchanged
}

TEST(NocWormholeArbiter, DownstreamBackpressureRetriesNoAbort) {
    // Refuses the first 2 push attempts (credit_avail still true), then accepts.
    // Models the multi-VC case: credit_avail(header.vc_id) cannot predict
    // which VC VcAllocator::push_flit actually selects.
    struct FlakyDown : ni::cmodel::router::NocReqOut {
        int refuse_remaining = 2;
        int accepted = 0;
        bool push_flit(const Flit&) override {
            if (refuse_remaining > 0) {
                --refuse_remaining;
                return false;
            }
            ++accepted;
            return true;
        }
        bool credit_avail(uint8_t) const override { return true; }
    } down;
    WormholeArbiter<ni::cmodel::router::NocReqOut> arb(down, /*num_inputs=*/2, {});

    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_NarrowAr, /*flit_tail=*/1)));
    arb.tick();  // refused (1) -> flit retained, no abort
    EXPECT_EQ(arb.pending_size(0), 1u);
    arb.tick();  // refused (2) -> still retained
    EXPECT_EQ(arb.pending_size(0), 1u);
    arb.tick();  // accepted -> drained exactly once
    EXPECT_EQ(arb.pending_size(0), 0u);
    EXPECT_EQ(down.accepted, 1);
}

TEST(NocWormholeArbiter, LockLeakIdleStallNoDeadlock) {
    ReqCapture down;
    WormholeArbiter<ni::cmodel::router::NocReqOut> arb(down, /*num_inputs=*/3, {{0, 1}});

    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_NarrowAw, /*flit_tail=*/0)));
    ASSERT_TRUE(arb.input(2).push_flit(make_flit(ni::AXI_CH_NarrowAr, /*flit_tail=*/1)));
    arb.tick();                               // AW drains, lock to w
    for (int i = 0; i < 10; ++i) arb.tick();  // w pending empty, locked -> idle
    EXPECT_EQ(down.size(), 1u);               // only AW
    EXPECT_EQ(arb.pending_size(2), 1u);       // AR still pending
    EXPECT_TRUE(arb.is_locked());
}

// Per-VC ownership (DatMergeWrap bug, 2026-08-21): input 0 legally interleaves
// two worms ACROSS VCs (per-VC each stream is contiguous). The vc1 worm's tail
// must release vc1 only -- vc0 stays owned by input 0 until ITS tail, so input
// 1's vc0 single-flit packet cannot slip between the vc0 worm's beats.
TEST(NocWormholeArbiter, PerVcLockSurvivesOtherVcTail) {
    ReqCapture down;
    WormholeArbiter<ni::cmodel::router::NocReqOut> arb(down, /*num_inputs=*/2, {});

    auto vc_flit = [](uint8_t vc, uint64_t flit_tail, uint64_t marker) {
        Flit f = make_flit(ni::AXI_CH_DataAw, flit_tail);
        f.set_header_field("vc_id", vc);
        f.set_header_field("dst_id", marker);
        return f;
    };
    // input 0: vc0 worm head, then a complete vc1 worm; the vc0 tail comes later.
    ASSERT_TRUE(arb.input(0).push_flit(vc_flit(0, /*flit_tail=*/0, 10)));
    ASSERT_TRUE(arb.input(0).push_flit(vc_flit(1, /*flit_tail=*/0, 20)));
    ASSERT_TRUE(arb.input(0).push_flit(vc_flit(1, /*flit_tail=*/1, 21)));
    // input 1: single-flit packet on vc0 (the NSU DataR shape).
    ASSERT_TRUE(arb.input(1).push_flit(vc_flit(0, /*flit_tail=*/1, 90)));

    for (int i = 0; i < 3; ++i) arb.tick();  // drains input 0's three flits
    EXPECT_EQ(down.size(), 3u);

    arb.tick();  // vc0 still owned by input 0 (its tail not seen) -> input 1 waits
    EXPECT_EQ(down.size(), 3u) << "vc1 tail must not release input 0's vc0 worm";
    EXPECT_EQ(arb.pending_size(1), 1u);

    ASSERT_TRUE(arb.input(0).push_flit(vc_flit(0, /*flit_tail=*/1, 11)));  // vc0 tail
    arb.tick();                                                            // vc0 worm completes
    arb.tick();  // now input 1's vc0 flit may drain
    EXPECT_EQ(down.size(), 5u);
    for (uint64_t marker : {10u, 20u, 21u, 11u, 90u}) {
        auto f = down.pop();
        ASSERT_TRUE(f.has_value());
        EXPECT_EQ(f->get_header_field("dst_id"), marker);
    }
}

// Cross-VC interleave stays legal: while input 0 owns vc0 mid-worm, input 1's
// flit on a DIFFERENT vc passes -- worm contiguity is per (link, VC), not per
// link (VCs exist to interleave).
TEST(NocWormholeArbiter, OtherVcPassesWhileVcLocked) {
    ReqCapture down;
    WormholeArbiter<ni::cmodel::router::NocReqOut> arb(down, /*num_inputs=*/2, {});

    auto vc_flit = [](uint8_t vc, uint64_t flit_tail, uint64_t marker) {
        Flit f = make_flit(ni::AXI_CH_DataAw, flit_tail);
        f.set_header_field("vc_id", vc);
        f.set_header_field("dst_id", marker);
        return f;
    };
    ASSERT_TRUE(arb.input(0).push_flit(vc_flit(0, /*flit_tail=*/0, 10)));  // vc0 worm head
    ASSERT_TRUE(arb.input(1).push_flit(vc_flit(2, /*flit_tail=*/1, 92)));  // other-vc single

    arb.tick();  // vc0 head drains, vc0 owned by input 0
    arb.tick();  // input 0 empty; input 1's vc2 flit is NOT blocked by the vc0 lock
    EXPECT_EQ(down.size(), 2u);
    EXPECT_EQ(arb.pending_size(1), 0u);
}

// Per-(input, VC) queues: a front flit blocked on ITS VC's downstream credit
// must not hold another VC's flits behind it -- the cross-VC head-of-line
// that closed the 2026-08-21 mode-1 deadlock cycle (merge front blocked on
// vc1 credit, vc1 credit waiting on a router output locked by a vc0 worm
// whose tail sat behind that front).
TEST(NocWormholeArbiter, BlockedVcDoesNotHeadOfLineBlockOtherVc) {
    struct Vc1Refuser : ni::cmodel::router::NocReqOut {
        std::vector<Flit> accepted;
        bool push_flit(const Flit& f) override {
            if (f.get_header_field("vc_id") == 1) return false;
            accepted.push_back(f);
            return true;
        }
        bool credit_avail(uint8_t vc_id) const override { return vc_id != 1; }
    } down;
    WormholeArbiter<ni::cmodel::router::NocReqOut> arb(down, /*num_inputs=*/1, {});

    Flit blocked = make_flit(ni::AXI_CH_DataAw, /*flit_tail=*/1);
    blocked.set_header_field("vc_id", 1);
    Flit ok = make_flit(ni::AXI_CH_DataAw, /*flit_tail=*/1);
    ok.set_header_field("vc_id", 0);
    ASSERT_TRUE(arb.input(0).push_flit(blocked));  // pushed first
    ASSERT_TRUE(arb.input(0).push_flit(ok));

    arb.tick();
    arb.tick();
    ASSERT_EQ(down.accepted.size(), 1u) << "vc0 flit must drain despite blocked vc1 front";
    EXPECT_EQ(down.accepted[0].get_header_field("vc_id"), 0u);
    EXPECT_EQ(arb.pending_size(0), 1u);  // the vc1 flit stays, retried
}

// Work-conserving scan: a candidate refused by the downstream (per-VC credit)
// must not end the tick -- the scan moves on and grants the next eligible
// candidate. Giving up on the first refusal retries it forever (round-robin
// advances only on grant) and starves the grantable input: the 2026-08-21
// mode-1 livelock (merge stuck on a credit-blocked NSU vc1 front while the
// NMU vc0 worm the fabric was waiting for sat ungranted).
TEST(NocWormholeArbiter, RefusedCandidateDoesNotStarveGrantableOne) {
    struct Vc1Refuser : ni::cmodel::router::NocReqOut {
        std::vector<Flit> accepted;
        bool push_flit(const Flit& f) override {
            if (f.get_header_field("vc_id") == 1) return false;
            accepted.push_back(f);
            return true;
        }
        bool credit_avail(uint8_t vc_id) const override { return vc_id != 1; }
    } down;
    WormholeArbiter<ni::cmodel::router::NocReqOut> arb(down, /*num_inputs=*/2, {});

    Flit blocked = make_flit(ni::AXI_CH_DataAw, /*flit_tail=*/1);
    blocked.set_header_field("vc_id", 1);
    Flit ok = make_flit(ni::AXI_CH_DataAw, /*flit_tail=*/1);
    ok.set_header_field("vc_id", 0);
    // Round-robin starts at input 0, whose only front is the refused vc1 flit.
    ASSERT_TRUE(arb.input(0).push_flit(blocked));
    ASSERT_TRUE(arb.input(1).push_flit(ok));

    arb.tick();
    ASSERT_EQ(down.accepted.size(), 1u)
        << "input 1's grantable vc0 flit must drain in the same tick the "
           "refused vc1 candidate was skipped";
    EXPECT_EQ(down.accepted[0].get_header_field("vc_id"), 0u);
    EXPECT_EQ(arb.pending_size(0), 1u);  // refused flit stays, retried later
}

// ---- Death tests (3) ----

TEST(NocWormholeArbiterDeath, WBeforeAW) {
    ReqCapture down;
    WormholeArbiter<ni::cmodel::router::NocReqOut> arb(down, /*num_inputs=*/3, {{0, 1}});
    ASSERT_TRUE(
        arb.input(1).push_flit(make_flit(ni::AXI_CH_NarrowW, /*flit_tail=*/1, /*wlast=*/1)));
    EXPECT_DEATH({ arb.tick(); }, ".*");
}

TEST(NocWormholeArbiterDeath, MalformedAwFlitTailEquals1) {
    ReqCapture down;
    WormholeArbiter<ni::cmodel::router::NocReqOut> arb(down, /*num_inputs=*/3, {{0, 1}});
    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_NarrowAw, /*flit_tail=*/1)));
    EXPECT_DEATH({ arb.tick(); }, ".*");
}

TEST(NocWormholeArbiterDeath, CtorPairingValidation) {
    ReqCapture down;
    // Out of range
    EXPECT_DEATH(
        {
            WormholeArbiter<ni::cmodel::router::NocReqOut> arb(down, /*num_inputs=*/2,
                                                               {{0, 5}});  // to=5 >= num_inputs
        },
        ".*");
    // from == to
    EXPECT_DEATH(
        { WormholeArbiter<ni::cmodel::router::NocReqOut> arb(down, /*num_inputs=*/2, {{1, 1}}); },
        ".*");
    // Duplicate from
    EXPECT_DEATH(
        {
            WormholeArbiter<ni::cmodel::router::NocReqOut> arb(down, /*num_inputs=*/3,
                                                               {{0, 1}, {0, 2}});
        },
        ".*");
    // Nested chain: to of one pairing is from of another
    EXPECT_DEATH(
        {
            WormholeArbiter<ni::cmodel::router::NocReqOut> arb(down, /*num_inputs=*/3,
                                                               {{0, 1}, {1, 2}});
        },
        ".*");
}
