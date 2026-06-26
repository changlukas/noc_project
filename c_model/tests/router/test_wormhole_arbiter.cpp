#include "ni/wormhole_arbiter.hpp"
#include "common/per_channel_capture.hpp"
#include "common/scenario.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include <gtest/gtest.h>
#include <vector>

using ni::cmodel::Flit;
using ni::cmodel::router::ChannelPairing;
using ni::cmodel::router::WormholeArbiter;
using ni::cmodel::testing::ReqCapture;

namespace {

Flit make_flit(uint8_t axi_ch, uint64_t last, uint64_t wlast = 0) {
    Flit f;
    f.set_header_field("axi_ch", axi_ch);
    f.set_header_field("dst_id", 0);
    f.set_header_field("vc_id", 0);
    f.set_header_field("last", last);
    if (axi_ch == ni::AXI_CH_W) {
        f.set_payload_field("W", "wlast", wlast);
    }
    return f;
}

}  // namespace

// ---- Functional tests (8) ----

TEST(NocWormholeArbiter, PassThroughNoPairing) {
    SCENARIO(
        "WormholeArbiter NSU mode (2 inputs, no pairing): each pushed flit "
        "is its own packet (last=1), alternating push + tick drain in round-robin order");
    ReqCapture down;
    WormholeArbiter<ni::cmodel::router::NocReqOut> arb(down, /*num_inputs=*/2, {});

    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_AR, /*last=*/1)));
    ASSERT_TRUE(arb.input(1).push_flit(make_flit(ni::AXI_CH_AR, /*last=*/1)));
    arb.tick();
    arb.tick();
    EXPECT_EQ(down.size(), 2u);
    EXPECT_FALSE(arb.is_locked());
}

TEST(NocWormholeArbiter, AwTriggersLock) {
    SCENARIO(
        "WormholeArbiter NMU mode (3 inputs, pairing aw->w): pushing an AW "
        "(header.last=0) to aw input + tick locks the arbiter to the w input");
    ReqCapture down;
    WormholeArbiter<ni::cmodel::router::NocReqOut> arb(down, /*num_inputs=*/3, {{0, 1}});

    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_AW, /*last=*/0)));
    arb.tick();
    EXPECT_EQ(down.size(), 1u);
    EXPECT_TRUE(arb.is_locked());
    EXPECT_EQ(*arb.locked_to(), 1u);
}

TEST(NocWormholeArbiter, ArCannotInterleaveDuringLock) {
    SCENARIO(
        "WormholeArbiter NMU mode: while locked to w input (after AW), an "
        "AR pushed to ar input cannot be drained until W with wlast unlocks");
    ReqCapture down;
    WormholeArbiter<ni::cmodel::router::NocReqOut> arb(down, /*num_inputs=*/3, {{0, 1}});

    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_AW, /*last=*/0)));
    arb.tick();  // AW drained, locked to w
    ASSERT_TRUE(arb.input(2).push_flit(make_flit(ni::AXI_CH_AR, /*last=*/1)));
    arb.tick();  // locked to w, w pending empty -> idle
    arb.tick();
    EXPECT_EQ(down.size(), 1u);          // only AW; AR still pending
    EXPECT_EQ(arb.pending_size(2), 1u);  // AR sitting
    EXPECT_TRUE(arb.is_locked());
}

TEST(NocWormholeArbiter, MultiBeatWBurstFlowsAndUnlocks) {
    SCENARIO(
        "WormholeArbiter NMU mode: AW + 3 W beats (last 2 non-wlast, 3rd "
        "wlast) flow through in ORDER (AW, W, W, W-last) and arbiter "
        "unlocks after W with wlast");
    ReqCapture down;
    WormholeArbiter<ni::cmodel::router::NocReqOut> arb(down, /*num_inputs=*/3, {{0, 1}});

    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_AW, /*last=*/0)));
    ASSERT_TRUE(arb.input(1).push_flit(make_flit(ni::AXI_CH_W, /*last=*/0, /*wlast=*/0)));
    ASSERT_TRUE(arb.input(1).push_flit(make_flit(ni::AXI_CH_W, /*last=*/0, /*wlast=*/0)));
    ASSERT_TRUE(arb.input(1).push_flit(make_flit(ni::AXI_CH_W, /*last=*/1, /*wlast=*/1)));

    for (int i = 0; i < 4; ++i) arb.tick();
    ASSERT_EQ(down.size(), 4u);
    EXPECT_FALSE(arb.is_locked());

    // Verify emission ORDER + per-flit header.last is correct
    auto f1 = down.pop();
    ASSERT_TRUE(f1.has_value());
    EXPECT_EQ(f1->get_header_field("axi_ch"), ni::AXI_CH_AW);
    EXPECT_EQ(f1->get_header_field("last"), 0u);

    auto f2 = down.pop();
    ASSERT_TRUE(f2.has_value());
    EXPECT_EQ(f2->get_header_field("axi_ch"), ni::AXI_CH_W);
    EXPECT_EQ(f2->get_header_field("last"), 0u);

    auto f3 = down.pop();
    ASSERT_TRUE(f3.has_value());
    EXPECT_EQ(f3->get_header_field("axi_ch"), ni::AXI_CH_W);
    EXPECT_EQ(f3->get_header_field("last"), 0u);

    auto f4 = down.pop();
    ASSERT_TRUE(f4.has_value());
    EXPECT_EQ(f4->get_header_field("axi_ch"), ni::AXI_CH_W);
    EXPECT_EQ(f4->get_header_field("last"), 1u);
}

TEST(NocWormholeArbiter, NocRspOutVariantPassThrough) {
    SCENARIO(
        "WormholeArbiter<NocRspOut> NSU instantiation: 2 inputs (B + R), "
        "no pairing, each flit is its own packet; verify template "
        "compiles + behaves identically for NocRspOut downstream type");
    using ni::cmodel::testing::RspCapture;
    RspCapture down;
    WormholeArbiter<ni::cmodel::router::NocRspOut> arb(down, /*num_inputs=*/2, {});

    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_B, /*last=*/1)));
    ASSERT_TRUE(arb.input(1).push_flit(make_flit(ni::AXI_CH_R, /*last=*/1)));
    arb.tick();
    arb.tick();
    EXPECT_EQ(down.size(), 2u);
    EXPECT_FALSE(arb.is_locked());
}

TEST(NocWormholeArbiter, BackpressureUpstreamAndDownstream) {
    SCENARIO(
        "WormholeArbiter backpressure: input pending full -> push_flit returns "
        "false (upstream). A downstream that refuses push_flit (downstream "
        "backpressure) makes tick retain the front flit -> idle, no drain.");
    // Downstream with no room: push_flit returns false. The arbiter does not
    // inspect VC or track credit; push_flit's return value is the authoritative
    // ready signal, and a false return is retried (front flit retained).
    struct FullDown : ni::cmodel::router::NocReqOut {
        bool push_flit(const Flit&) override { return false; }
    } full;
    WormholeArbiter<ni::cmodel::router::NocReqOut> arb(full, /*num_inputs=*/2, {},
                                                       /*per_input_depth=*/2);

    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_AR, /*last=*/1)));
    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_AR, /*last=*/1)));
    EXPECT_FALSE(arb.input(0).push_flit(make_flit(ni::AXI_CH_AR, /*last=*/1)));  // full

    arb.tick();                          // downstream refuses push -> retain -> idle
    EXPECT_EQ(arb.pending_size(0), 2u);  // unchanged
}

TEST(NocWormholeArbiter, DownstreamBackpressureRetriesNoAbort) {
    SCENARIO(
        "WormholeArbiter try-push handshake: a downstream that transiently "
        "refuses push_flit (e.g. NMU VcArbiter where the flit's landing VC is "
        "full while header.vc_id reports VC0) is legitimate backpressure, NOT a "
        "protocol violation. The arbiter must retain the front flit and retry "
        "until accepted -- no abort, no flit loss/duplication.");
    // Refuses the first 2 push attempts (credit_avail still true), then accepts.
    // Models the multi-VC case: credit_avail(header.vc_id) cannot predict the
    // landing VC that VcArbiter::push_flit actually selects.
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

    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_AR, /*last=*/1)));
    arb.tick();  // refused (1) -> flit retained, no abort
    EXPECT_EQ(arb.pending_size(0), 1u);
    arb.tick();  // refused (2) -> still retained
    EXPECT_EQ(arb.pending_size(0), 1u);
    arb.tick();  // accepted -> drained exactly once
    EXPECT_EQ(arb.pending_size(0), 0u);
    EXPECT_EQ(down.accepted, 1);
}

TEST(NocWormholeArbiter, LockLeakIdleStallNoDeadlock) {
    SCENARIO(
        "WormholeArbiter lock-leak / idle stall: AW emits and triggers lock, "
        "but no W ever arrives. tick many times -> arbiter idles (no spurious "
        "emit, no deadlock; AR remains pending, lock held).");
    ReqCapture down;
    WormholeArbiter<ni::cmodel::router::NocReqOut> arb(down, /*num_inputs=*/3, {{0, 1}});

    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_AW, /*last=*/0)));
    ASSERT_TRUE(arb.input(2).push_flit(make_flit(ni::AXI_CH_AR, /*last=*/1)));
    arb.tick();                               // AW drains, lock to w
    for (int i = 0; i < 10; ++i) arb.tick();  // w pending empty, locked -> idle
    EXPECT_EQ(down.size(), 1u);               // only AW
    EXPECT_EQ(arb.pending_size(2), 1u);       // AR still pending
    EXPECT_TRUE(arb.is_locked());
}

// ---- Death tests (3) ----

TEST(NocWormholeArbiterDeath, WBeforeAW) {
    SCENARIO(
        "WormholeArbiter NMU mode: pushing W to w input while unlocked "
        "(no preceding AW) violates upstream serialization; tick must "
        "assert+abort to fail fast");
    ReqCapture down;
    WormholeArbiter<ni::cmodel::router::NocReqOut> arb(down, /*num_inputs=*/3, {{0, 1}});
    ASSERT_TRUE(arb.input(1).push_flit(make_flit(ni::AXI_CH_W, /*last=*/1, /*wlast=*/1)));
    EXPECT_DEATH({ arb.tick(); }, ".*");
}

TEST(NocWormholeArbiterDeath, MalformedAwLastEquals1) {
    SCENARIO(
        "WormholeArbiter NMU mode: AW pushed with header.last=1 is malformed "
        "(violates FlooNoC wormhole AW=0 stamping); tick must assert+abort");
    ReqCapture down;
    WormholeArbiter<ni::cmodel::router::NocReqOut> arb(down, /*num_inputs=*/3, {{0, 1}});
    ASSERT_TRUE(arb.input(0).push_flit(make_flit(ni::AXI_CH_AW, /*last=*/1)));
    EXPECT_DEATH({ arb.tick(); }, ".*");
}

TEST(NocWormholeArbiterDeath, CtorPairingValidation) {
    SCENARIO(
        "WormholeArbiter ctor validates pairings: out-of-range index, "
        "from==to, duplicate from, nested chain (to is also a from). "
        "Each violation triggers assert+abort.");
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
