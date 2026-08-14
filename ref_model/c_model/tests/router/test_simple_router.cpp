#include "router/simple_router.hpp"
#include <gtest/gtest.h>
#include <optional>
#include <tuple>
#include <vector>

using ni::cmodel::Flit;
using ni::cmodel::router::port_bit;
using ni::cmodel::router::route_compute;
using ni::cmodel::router::RouterConfig;
using ni::cmodel::router::RouterPort;
using ni::cmodel::router::SimpleRouter;
using ni::cmodel::router::SimpleRouterConfig;
using ni::cmodel::router::SimpleRouterLink;
using ni::cmodel::router::tie_off;

namespace {

SimpleRouterConfig center_cfg() {
    SimpleRouterConfig cfg;
    cfg.x = 1;
    cfg.y = 1;  // center of default 4x4
    return cfg;
}

uint8_t make_dst(uint8_t x, uint8_t y) {
    return static_cast<uint8_t>((y << ni::width::X_WIDTH) | x);
}

struct FlitSink : SimpleRouterLink {
    std::vector<Flit> received;
    bool always_ready = true;
    bool ready(uint8_t /*vc*/) const override { return always_ready; }
    void push_flit(const Flit& f) override { received.push_back(f); }
};

Flit make_flit(uint8_t dst, uint8_t vc, uint64_t flit_tail) {
    Flit f;
    f.set_header_field("dst_id", dst);
    f.set_header_field("vc_id", vc);
    f.set_header_field("flit_tail", flit_tail);
    return f;
}

Flit make_tagged_flit(uint8_t dst, uint8_t vc, uint64_t flit_tail, uint8_t src_id) {
    auto f = make_flit(dst, vc, flit_tail);
    f.set_header_field("src_id", src_id);
    return f;
}

// --- tie_off(): pure-function translate of floo_router.sv:349-357 ----------

// Standing ruling (IMPLEMENTATION_PLAN.md Stage 3b): LOCAL->LOCAL is exempt from tie_off's
// NoLoopback rule -- self-targeted traffic is legal.
TEST(SimpleRouterTieOff, LoopbackAndXYIllegalTurnsSkipped) {
    using RP = RouterPort;
    for (RP p : {RP::NORTH, RP::EAST, RP::SOUTH, RP::WEST}) EXPECT_TRUE(tie_off(p, p));
    EXPECT_FALSE(tie_off(RP::LOCAL, RP::LOCAL))
        << "LOCAL->LOCAL must stay connected (self-transaction path)";
    EXPECT_TRUE(tie_off(RP::SOUTH, RP::EAST));
    EXPECT_TRUE(tie_off(RP::SOUTH, RP::WEST));
    EXPECT_TRUE(tie_off(RP::NORTH, RP::EAST));
    EXPECT_TRUE(tie_off(RP::NORTH, RP::WEST));
    EXPECT_FALSE(tie_off(RP::WEST, RP::EAST));    // straight through X
    EXPECT_FALSE(tie_off(RP::SOUTH, RP::NORTH));  // straight through Y
    EXPECT_FALSE(tie_off(RP::EAST, RP::NORTH));   // X->Y turn: legal
    EXPECT_FALSE(tie_off(RP::WEST, RP::SOUTH));   // X->Y turn: legal
    EXPECT_FALSE(tie_off(RP::LOCAL, RP::EAST));
    EXPECT_FALSE(tie_off(RP::EAST, RP::LOCAL));
}

// --- Construction / discipline guards ---------------------------------------

TEST(SimpleRouterConstructionDeath, BadParametersAbort) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    SimpleRouterConfig bad_vc = center_cfg();
    bad_vc.num_vc = 2;
    EXPECT_DEATH(SimpleRouter r(bad_vc), "num_vc");
    SimpleRouterConfig zero_slack = center_cfg();
    zero_slack.ready_slack = 0;
    EXPECT_DEATH(SimpleRouter r(zero_slack), "ready_slack");
    SimpleRouterConfig bad_slack = center_cfg();
    bad_slack.input_fifo_depth = 2;
    bad_slack.ready_slack = 2;  // needs depth >= slack + 1 == 3
    EXPECT_DEATH(SimpleRouter r(bad_slack), "ready_slack");
    SimpleRouterConfig bad_coord = center_cfg();
    bad_coord.x = 99;
    EXPECT_DEATH(SimpleRouter r(bad_coord), "mesh");
}

TEST(SimpleRouterDatapathDeath, MoreThanOneFlitPerLinkPerCycleAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    SimpleRouter r(center_cfg());
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    r.input(W).push_flit(make_flit(make_dst(3, 1), 0, 1));
    EXPECT_DEATH(r.input(W).push_flit(make_flit(make_dst(3, 1), 0, 1)), "per link per cycle");
}

TEST(SimpleRouterDatapathDeath, OverflowIgnoringReadyAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    SimpleRouterConfig cfg = center_cfg();
    cfg.input_fifo_depth = 2;
    cfg.ready_slack = 1;  // legal minimum (ctor asserts ready_slack >= 1)
    SimpleRouter r(cfg);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    // No downstream attached: nothing ever drains the FIFO. Fill to depth
    // while ready() correctly says so (compliant, never overflows on its
    // own), then push one more UNCONDITIONALLY, ignoring ready()'s now-false
    // value — that ignoring is this test's actual scenario.
    r.input(W).push_flit(make_flit(make_dst(3, 1), 0, 1));
    r.tick();  // size 0 -> 1
    ASSERT_TRUE(r.ready(W, 0));
    r.input(W).push_flit(make_flit(make_dst(3, 1), 0, 1));
    r.tick();  // size 1 -> 2 (== depth)
    ASSERT_FALSE(r.ready(W, 0));
    EXPECT_DEATH(
        {
            r.input(W).push_flit(make_flit(make_dst(3, 1), 0, 1));  // ignores ready() == false
            r.tick();  // size 2 -> would-be 3 > depth 2
        },
        "overflow");
}

// --- Zero-load latency: direct (output_fifo_depth==0) vs buffered ----------

// floo_router.sv:466-470 (gen_no_out_fifo): with output_fifo_depth=0, delivery is 1 tick faster
// than router::Router's registered path.
TEST(SimpleRouterDatapath, ZeroLoadLatencyDirectModeTwoTicks) {
    SimpleRouter r(center_cfg());
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    r.set_downstream(E, east);
    r.input(W).push_flit(make_flit(make_dst(3, 1), 0, /*flit_tail=*/1));
    r.tick();
    EXPECT_TRUE(east.received.empty());
    r.tick();
    ASSERT_EQ(east.received.size(), 1u);
}

// floo_router.sv:448-465 (gen_out_fifo): with output_fifo_depth>0, delivery matches
// router::Router's 3-tick pipeline depth.
TEST(SimpleRouterDatapath, ZeroLoadLatencyBufferedModeThreeTicks) {
    SimpleRouterConfig cfg = center_cfg();
    cfg.output_fifo_depth = 2;
    SimpleRouter r(cfg);
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    r.set_downstream(E, east);
    r.input(W).push_flit(make_flit(make_dst(3, 1), 0, /*flit_tail=*/1));
    r.tick();
    EXPECT_TRUE(east.received.empty());
    r.tick();
    EXPECT_TRUE(east.received.empty());
    r.tick();
    ASSERT_EQ(east.received.size(), 1u);
}

// Regression guard for the S3a T6 node0 hang: tie_off's NoLoopback previously excluded LOCAL->LOCAL
// from arbitration, stranding the flit in the LOCAL input FIFO forever.
TEST(SimpleRouterDatapath, LocalToLocalSelfTrafficDelivers) {
    SimpleRouter r(center_cfg());
    FlitSink local;
    const auto L = static_cast<std::size_t>(RouterPort::LOCAL);
    r.set_downstream(L, local);
    r.input(L).push_flit(make_flit(make_dst(1, 1), 0, /*flit_tail=*/1));  // self-targeted
    r.tick();
    r.tick();
    ASSERT_EQ(local.received.size(), 1u) << "self-targeted flit never delivered LOCAL->LOCAL";
}

// --- Ready/valid backpressure, parameterized over (depth, slack) -----------

class SimpleRouterBackpressure
    : public ::testing::TestWithParam<std::tuple<std::size_t, std::size_t>> {};

TEST_P(SimpleRouterBackpressure, ReadyDeassertsAtAlmostFullNoOverflow) {
    auto [depth, slack] = GetParam();
    SimpleRouterConfig cfg = center_cfg();
    cfg.input_fifo_depth = depth;
    cfg.ready_slack = slack;
    SimpleRouter r(cfg);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    // No downstream attached: occupancy only grows, isolating the admission
    // side of the ready/valid contract from arbitration.
    for (std::size_t t = 0; t < depth + 4; ++t) {
        if (r.ready(W, 0)) r.input(W).push_flit(make_flit(make_dst(3, 1), 0, /*flit_tail=*/1));
        r.tick();
        ASSERT_LE(r.input_fifo_size(W, 0), depth) << "overflow at tick " << t;
    }
    // The almost-full early ready settles one push short of its own margin:
    // ready holds through size == depth - slack, and the compliant sender's
    // last accepted push lands at depth - slack + 1, (slack - 1) slots free
    // of the full depth — never depth itself. That headroom is the point.
    EXPECT_EQ(r.input_fifo_size(W, 0), depth - slack + 1)
        << "compliant sender did not settle at the formula's own high-water mark";
    EXPECT_FALSE(r.ready(W, 0)) << "ready stayed asserted at the settled high-water mark";
}

// slack == 0 is excluded here: the formula's own construction constraint
// (depth >= slack + 1) permits it, but size + 0 <= depth stays true AT size
// == depth, so a compliant sender's very next push overflows by one — the
// margin the formula reserves for a compliant sender is (slack - 1) free
// slots after its last accepted push, not slack, so slack must be >= 1 for
// "never overflows" to hold. slack == 0 (zero round-trip margin) is
// SimpleRouterDatapathDeath.OverflowIgnoringReadyAborts's scenario instead —
// a config that is degenerate by construction (models zero loop latency),
// caught loud by the stage-1 assert rather than silently corrupting state.
INSTANTIATE_TEST_SUITE_P(DepthSlackGrid, SimpleRouterBackpressure,
                         ::testing::Values(std::make_tuple(2, 1), std::make_tuple(4, 1),
                                           std::make_tuple(8, 2), std::make_tuple(8, 4),
                                           std::make_tuple(3, 2), std::make_tuple(5, 3)));

// --- Route lock: floo_route_select.sv:200-220 -------------------------------

// floo_route_select.sv:200-220: a worm's body flit rides the head's latched route, never a fresh
// recompute, even on a malformed mid-worm dst change.
TEST(SimpleRouterRouteLock, LatchedRouteSurvivesAMidWormDstChange) {
    SimpleRouter r(center_cfg());
    FlitSink east, south;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto S = static_cast<std::size_t>(RouterPort::SOUTH);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    r.set_downstream(E, east);
    r.set_downstream(S, south);
    const uint8_t dst_east = make_dst(3, 1);   // routes EAST from center (1,1)
    const uint8_t dst_south = make_dst(1, 0);  // routes SOUTH from center

    // Head: routes EAST, flit_tail=0 (worm continues) -> locks (W,vc0)=EAST.
    r.input(W).push_flit(make_flit(dst_east, 0, /*flit_tail=*/0));
    r.tick();
    r.tick();
    ASSERT_EQ(east.received.size(), 1u);
    EXPECT_EQ(r.route_locked(W, 0), port_bit(RouterPort::EAST));

    // Body: dst now computes to SOUTH, flit_tail=0 still. A recompute would
    // route it SOUTH; the lock must keep it on EAST.
    r.input(W).push_flit(make_flit(dst_south, 0, /*flit_tail=*/0));
    r.tick();
    r.tick();
    EXPECT_EQ(east.received.size(), 2u) << "locked route not honored";
    EXPECT_TRUE(south.received.empty()) << "malformed dst re-routed the worm mid-flight";

    // Tail: closes the worm, releases both locks.
    r.input(W).push_flit(make_flit(dst_east, 0, /*flit_tail=*/1));
    r.tick();
    r.tick();
    EXPECT_EQ(east.received.size(), 3u);
    EXPECT_EQ(r.route_locked(W, 0), 0u);
    EXPECT_EQ(r.wormhole_locked_input(E), std::nullopt);
}

// --- Per-output wormhole/arbiter lock: floo_wormhole_arbiter, LockIn=1'b1 --

struct Packet {
    std::size_t in_port;
    uint8_t src_id;
    int next = 0;  // 0=head, 1=body, 2=tail, 3=done
};
constexpr int kMaxPacketFlits = 3;

void feed_packet(SimpleRouter& r, Packet& pkt, uint8_t dst, uint8_t vc) {
    if (pkt.next >= kMaxPacketFlits) return;
    const uint64_t flit_tail = (pkt.next == kMaxPacketFlits - 1) ? 1 : 0;
    r.input(pkt.in_port).push_flit(make_tagged_flit(dst, vc, flit_tail, pkt.src_id));
    ++pkt.next;
}

// floo_wormhole_arbiter.sv:40 (LockIn=1'b1): the per-output wormhole lock keeps a contending
// packet's flits from interleaving into another packet.
TEST(SimpleRouterWormhole, PacketsDoNotInterleavePerOutput) {
    SimpleRouter r(center_cfg());
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);  // routes EAST from center
    // WEST (straight-through X) and LOCAL (fresh injection) are the only two
    // ports that can legally target EAST under XY tie-off: NORTH/SOUTH -> EAST
    // is the Y->X turn floo_router.sv:349-357 ties off (SimpleRouterTieOff).
    Packet a{static_cast<std::size_t>(RouterPort::WEST), /*src_id=*/0x10};
    Packet b{static_cast<std::size_t>(RouterPort::LOCAL), /*src_id=*/0x20};
    for (int t = 0; t < 20; ++t) {
        feed_packet(r, a, dst, 0);
        if (t >= 1) feed_packet(r, b, dst, 0);
        r.tick();
    }
    ASSERT_EQ(east.received.size(), 6u);
    int runs = 1;
    for (std::size_t i = 1; i < east.received.size(); ++i) {
        const uint8_t s = static_cast<uint8_t>(east.received[i].get_header_field("src_id"));
        const uint8_t prev = static_cast<uint8_t>(east.received[i - 1].get_header_field("src_id"));
        if (s != prev) ++runs;
    }
    EXPECT_EQ(runs, 2) << "packet flits interleaved at the shared output";
}

TEST(SimpleRouterWormhole, LockedEmptyInputIdlesButDoesNotLoseLock) {
    SimpleRouter r(center_cfg());
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto L = static_cast<std::size_t>(RouterPort::LOCAL);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);

    // LOCAL sends a lone head flit (flit_tail=0) then stalls empty.
    r.input(L).push_flit(make_tagged_flit(dst, 0, /*flit_tail=*/0, 0x20));
    r.tick();
    r.tick();
    ASSERT_EQ(east.received.size(), 1u);
    EXPECT_EQ(r.wormhole_locked_input(E), L);

    // WEST offers a complete 3-flit packet at the same output while LOCAL is
    // locked but idle.
    Packet west{W, 0x10};
    for (int t = 0; t < 6; ++t) {
        feed_packet(r, west, dst, 0);
        r.tick();
    }
    for (const auto& f : east.received)
        EXPECT_EQ(static_cast<uint8_t>(f.get_header_field("src_id")), 0x20)
            << "WEST stole the lock while LOCAL held it idle";
    ASSERT_EQ(east.received.size(), 1u);

    // LOCAL's tail releases the lock; WEST's queued packet then drains.
    r.input(L).push_flit(make_tagged_flit(dst, 0, /*flit_tail=*/1, 0x20));
    for (int t = 0; t < 8; ++t) r.tick();
    int count_w = 0, count_l = 0;
    for (const auto& f : east.received) {
        const uint8_t s = static_cast<uint8_t>(f.get_header_field("src_id"));
        if (s == 0x10) ++count_w;
        if (s == 0x20) ++count_l;
    }
    EXPECT_EQ(count_l, 2);  // head + tail
    EXPECT_EQ(count_w, 3);
}

TEST(SimpleRouterWormhole, RrAdvancesPerPacket) {
    SimpleRouter r(center_cfg());
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto WEST = static_cast<std::size_t>(RouterPort::WEST);
    const auto LOCAL = static_cast<std::size_t>(RouterPort::LOCAL);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);

    for (int t = 0; t < 40 && east.received.size() < 8; ++t) {
        if (r.input_fifo_size(WEST, 0) == 0)
            r.input(WEST).push_flit(make_tagged_flit(dst, 0, /*flit_tail=*/1, 0x10));
        if (r.input_fifo_size(LOCAL, 0) == 0)
            r.input(LOCAL).push_flit(make_tagged_flit(dst, 0, /*flit_tail=*/1, 0x20));
        r.tick();
    }
    ASSERT_GE(east.received.size(), 8u);
    for (std::size_t i = 1; i < 8; ++i) {
        const uint8_t prev = static_cast<uint8_t>(east.received[i - 1].get_header_field("src_id"));
        const uint8_t cur = static_cast<uint8_t>(east.received[i].get_header_field("src_id"));
        EXPECT_NE(cur, prev) << "RR did not alternate at grant " << i;
    }
}

// floo_wormhole_arbiter.sv:61-77 (valid_d/valid_q/last_q): the arbitration winner freezes the
// instant a candidate goes valid, so a late-valid input cannot join that round.
TEST(SimpleRouterWormhole, WinnerFrozenBeforeReadyExcludesLaterArrival) {
    SimpleRouter r(center_cfg());
    FlitSink east;
    east.always_ready = false;  // hold backpressure while both candidates arrive
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    const auto L = static_cast<std::size_t>(RouterPort::LOCAL);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);  // routes EAST from center; WEST and LOCAL both legal

    // A (WEST) goes valid first, alone.
    r.input(W).push_flit(make_tagged_flit(dst, 0, /*flit_tail=*/1, 0x10));
    r.tick();  // admits WEST into its input FIFO; still backpressured
    r.tick();  // candidate scan runs (unconditionally): only WEST valid -> freezes on WEST

    // B (LOCAL) goes valid only now, after the freeze. A live re-scan once
    // ready arrives would start at rr=0=LOCAL and pick B over A -- exactly
    // the divergence under review.
    r.input(L).push_flit(make_tagged_flit(dst, 0, /*flit_tail=*/1, 0x20));
    r.tick();  // still backpressured; LOCAL admitted but excluded from this round

    // Backpressure clears; the already-frozen winner (WEST) is what gets granted.
    east.always_ready = true;
    r.tick();

    ASSERT_EQ(east.received.size(), 1u);
    EXPECT_EQ(static_cast<uint8_t>(east.received[0].get_header_field("src_id")), 0x10)
        << "winner must come from the frozen set (WEST, first to go valid), not a live re-scan "
           "once ready arrived (which would find LOCAL first at rr=0)";
}

}  // namespace
