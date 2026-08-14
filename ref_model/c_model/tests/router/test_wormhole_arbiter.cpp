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
