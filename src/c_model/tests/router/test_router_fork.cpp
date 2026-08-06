// S4-T3 DAT router multicast fork: F1-F10 composition (design §1.1), fork
// introspection (§1.3), and the §1.2 overlapping-tree wedge. Port/branch
// geometry cells are hand-computed from floo_route_xymask.sv:104-164 the same
// way test_route_mask.cpp verifies them; this file tests the ROUTER's use of
// the fork set, not the mask math.
#include "common/scenario.hpp"
#include "router/route_mask.hpp"
#include "router/router.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

using ni::cmodel::Flit;
using ni::cmodel::router::port_bit;
using ni::cmodel::router::PortMask;
using ni::cmodel::router::Router;
using ni::cmodel::router::RouterConfig;
using ni::cmodel::router::RouterPort;

namespace {

constexpr auto L = static_cast<std::size_t>(RouterPort::LOCAL);
constexpr auto N = static_cast<std::size_t>(RouterPort::NORTH);
constexpr auto E = static_cast<std::size_t>(RouterPort::EAST);
constexpr auto S = static_cast<std::size_t>(RouterPort::SOUTH);
constexpr auto W = static_cast<std::size_t>(RouterPort::WEST);

// Router zero-load pipeline depth (see test_router.cpp).
constexpr int kPipelineDepth = 3;

uint8_t make_id(uint8_t x, uint8_t y) {
    return static_cast<uint8_t>((y << ni::width::X_WIDTH) | x);
}

RouterConfig center_cfg() {
    RouterConfig cfg;
    cfg.x = 1;
    cfg.y = 1;  // center of default 4x4
    return cfg;
}

struct FlitSink : ni::cmodel::router::RouterLink {
    std::vector<Flit> received;
    void push_flit(const Flit& f) override { received.push_back(f); }
};

struct CreditCounter : ni::cmodel::router::RouterCreditSink {
    std::vector<uint8_t> pulses;
    void receive_credit(uint8_t vc) override { pulses.push_back(vc); }
};

// A collective flit: dst_id is the set anchor, src_id the tree source,
// collective_mask the wildcard (T2 stamps AW and every W beat identically).
// ordering_tag carries the test's beat sequence number (src_id is taken).
Flit make_mc_flit(uint8_t dst, uint8_t src, uint8_t cmask, uint8_t vc, uint64_t flit_tail,
                  uint64_t fixed_vc, uint8_t tag) {
    Flit f;
    f.set_header_field("collective_op", ni::COLLECTIVE_OP_MULTICAST);
    f.set_header_field("collective_mask", cmask);
    f.set_header_field("dst_id", dst);
    f.set_header_field("src_id", src);
    f.set_header_field("vc_id", vc);
    f.set_header_field("fixed_vc", fixed_vc);
    f.set_header_field("flit_tail", flit_tail);
    f.set_header_field("ordering_tag", tag);
    return f;
}

Flit make_unicast_flit(uint8_t dst, uint8_t src, uint8_t vc, uint64_t flit_tail, uint8_t tag) {
    Flit f;
    f.set_header_field("dst_id", dst);
    f.set_header_field("src_id", src);
    f.set_header_field("vc_id", vc);
    f.set_header_field("flit_tail", flit_tail);
    f.set_header_field("ordering_tag", tag);
    return f;
}

// Build an n-flit worm (head flit_tail=0 .. tail flit_tail=1), tags 0..n-1.
std::vector<Flit> make_mc_worm(uint8_t dst, uint8_t src, uint8_t cmask, uint8_t vc,
                               uint64_t fixed_vc, int n_flits) {
    std::vector<Flit> worm;
    for (int i = 0; i < n_flits; ++i) {
        worm.push_back(make_mc_flit(dst, src, cmask, vc, /*flit_tail=*/i == n_flits - 1 ? 1 : 0,
                                    fixed_vc, static_cast<uint8_t>(i)));
    }
    return worm;
}

// Push the next unfed worm flit into `in_port` when the input VC FIFO has
// room (credit-aware upstream model; one flit/port/tick).
void feed_worm(Router& r, const std::vector<Flit>& worm, std::size_t& fed, std::size_t in_port,
               uint8_t vc) {
    if (fed >= worm.size()) return;
    if (r.input_fifo_size(in_port, vc) >= r.vc_depth()) return;
    r.input(in_port).push_flit(worm[fed]);
    ++fed;
}

// Return one credit to `r` for every flit delivered to `sink` since `before`,
// on the delivered flit's vc_id (models a freely draining downstream).
void return_credit(Router& r, FlitSink& sink, std::size_t out_port, std::size_t before) {
    for (std::size_t i = before; i < sink.received.size(); ++i) {
        r.receive_credit(out_port,
                         static_cast<uint8_t>(sink.received[i].get_header_field("vc_id")));
    }
}

void expect_worm_in_order(const FlitSink& sink, int n_flits, const char* label) {
    ASSERT_EQ(sink.received.size(), static_cast<std::size_t>(n_flits)) << label;
    for (int i = 0; i < n_flits; ++i) {
        EXPECT_EQ(sink.received[i].get_header_field("ordering_tag"), static_cast<uint64_t>(i))
            << label << ": beat " << i << " out of order";
    }
}

// --- Fork delivery ---------------------------------------------------------

TEST(RouterFork, ThreeWayInclLocalReplicatesWholeWorm) {
    SCENARIO(
        "3-way fork {LOCAL,EAST,NORTH} at (1,1): a 4-flit collective worm is replicated "
        "byte-for-byte and in order to all three branches; ONE upstream credit pulse per "
        "flit (F10); all branch locks released after the tail (per-branch WLAST release)");
    Router r(center_cfg());
    FlitSink local, north, east;
    CreditCounter west_up;
    r.set_downstream(L, local);
    r.set_downstream(N, north);
    r.set_downstream(E, east);
    r.set_upstream_credit(W, west_up);
    // src (0,1), anchor (1,1), mask (x=2,y=2): members {1,3}x{1,3} -> fork
    // {L,E,N} at (1,1) (x/y matched -> LOCAL; East spread; North turn).
    constexpr int kFlits = 4;
    const auto worm = make_mc_worm(make_id(1, 1), make_id(0, 1), make_id(2, 2), /*vc=*/0,
                                   /*fixed_vc=*/1, kFlits);
    std::size_t fed = 0;
    for (int t = 0; t < 30; ++t) {
        feed_worm(r, worm, fed, W, 0);
        const std::size_t bl = local.received.size(), bn = north.received.size(),
                          be = east.received.size();
        r.tick();
        return_credit(r, local, L, bl);
        return_credit(r, north, N, bn);
        return_credit(r, east, E, be);
    }
    expect_worm_in_order(local, kFlits, "LOCAL");
    expect_worm_in_order(north, kFlits, "NORTH");
    expect_worm_in_order(east, kFlits, "EAST");
    // Byte-for-byte replication: every branch carries the injected flit
    // unmodified (fixed_vc=1 keeps vc_id, so raw equality holds end-to-end).
    for (int i = 0; i < kFlits; ++i) {
        EXPECT_EQ(local.received[i].raw(), worm[i].raw());
        EXPECT_EQ(north.received[i].raw(), worm[i].raw());
        EXPECT_EQ(east.received[i].raw(), worm[i].raw());
    }
    // F10: one upstream pulse per flit, never per branch.
    EXPECT_EQ(west_up.pulses.size(), static_cast<std::size_t>(kFlits));
    // Per-branch tail release: no branch lock survives the worm.
    for (std::size_t o = 0; o < ni::cmodel::router::ROUTER_PORT_COUNT; ++o) {
        EXPECT_FALSE(r.wormhole_locked_input(o).has_value()) << "output " << o;
    }
    EXPECT_EQ(r.fork_done_mask(W, 0), 0u);
}

TEST(RouterFork, LocalInputWithLocalBranch) {
    SCENARIO(
        "LOCAL->LOCAL is a real branch (S3a ruling; F3 OUR RULE keeps LOCAL in "
        "expected_mask — no ignore_routes port): a worm injected at the LOCAL input with "
        "fork {LOCAL,NORTH} ejects locally AND spreads north");
    Router r(center_cfg());
    FlitSink local, north;
    r.set_downstream(L, local);
    r.set_downstream(N, north);
    // src (1,1) = this node, anchor (1,1), mask (y=2): members (1,1),(1,3).
    constexpr int kFlits = 3;
    const auto worm = make_mc_worm(make_id(1, 1), make_id(1, 1), make_id(0, 2), /*vc=*/0,
                                   /*fixed_vc=*/1, kFlits);
    std::size_t fed = 0;
    for (int t = 0; t < 24; ++t) {
        feed_worm(r, worm, fed, L, 0);
        const std::size_t bl = local.received.size(), bn = north.received.size();
        r.tick();
        return_credit(r, local, L, bl);
        return_credit(r, north, N, bn);
    }
    expect_worm_in_order(local, kFlits, "LOCAL");
    expect_worm_in_order(north, kFlits, "NORTH");
}

// --- W replication under branch starvation ---------------------------------

TEST(RouterFork, MidBurstBranchStarvationStallsWormWithoutSkipOrReorder) {
    SCENARIO(
        "2-way fork {E,N}: NORTH credit dries mid-worm — the worm stalls at the beat NORTH "
        "cannot take (EAST leads by at most the one parked beat, F2/F3), no beat is skipped "
        "or reordered, introspection shows the live {expected,done} masks and both branch "
        "locks; returning NORTH credit completes the worm and releases per-branch at WLAST");
    RouterConfig cfg = center_cfg();
    cfg.vc_depth = 2;  // NORTH holds 2 credits -> dries after head+1 beat
    Router r(cfg);
    FlitSink north, east;
    CreditCounter west_up;
    r.set_downstream(N, north);
    r.set_downstream(E, east);
    r.set_upstream_credit(W, west_up);
    // src (0,1), anchor (1,3), mask (x=2): members (1,3),(3,3) -> fork {E,N}
    // at (1,1) (no LOCAL: y unmatched).
    constexpr int kFlits = 6;
    const uint8_t dst = make_id(1, 3), src = make_id(0, 1), cmask = make_id(2, 0);
    const auto worm = make_mc_worm(dst, src, cmask, /*vc=*/0, /*fixed_vc=*/1, kFlits);
    std::size_t fed = 0;

    // Phase A: EAST credit returned per delivery, NORTH credit NEVER returned.
    for (int t = 0; t < 30; ++t) {
        feed_worm(r, worm, fed, W, 0);
        const std::size_t be = east.received.size();
        r.tick();
        return_credit(r, east, E, be);
    }
    // NORTH took its 2 credits (head + beat 1) and dried; the worm is parked
    // on beat 2 with only EAST's grant in — EAST leads by exactly the parked
    // beat, no beat skipped past the stall.
    EXPECT_EQ(north.received.size(), 2u);
    EXPECT_EQ(east.received.size(), 3u) << "EAST may lead only by the parked beat";
    // Introspection (§1.3 detection b): live masks + both branch locks held.
    EXPECT_EQ(r.fork_expected_mask(W, 0), port_bit(RouterPort::EAST) | port_bit(RouterPort::NORTH));
    EXPECT_EQ(r.fork_done_mask(W, 0), port_bit(RouterPort::EAST));
    EXPECT_EQ(r.wormhole_locked_input(N), std::optional<std::size_t>(W));
    EXPECT_EQ(r.wormhole_locked_input(E), std::optional<std::size_t>(W));

    // Phase B: downstream NORTH drains its two held flits (credits come
    // home), then keeps draining — the worm completes, both branches carry
    // every beat in order, one credit pulse per flit total.
    r.receive_credit(N, 0);
    r.receive_credit(N, 0);
    for (int t = 0; t < 30; ++t) {
        feed_worm(r, worm, fed, W, 0);
        const std::size_t bn = north.received.size(), be = east.received.size();
        r.tick();
        return_credit(r, north, N, bn);
        return_credit(r, east, E, be);
    }
    expect_worm_in_order(north, kFlits, "NORTH");
    expect_worm_in_order(east, kFlits, "EAST");
    EXPECT_EQ(west_up.pulses.size(), static_cast<std::size_t>(kFlits));
    EXPECT_FALSE(r.wormhole_locked_input(N).has_value());
    EXPECT_FALSE(r.wormhole_locked_input(E).has_value());
    EXPECT_EQ(r.fork_done_mask(W, 0), 0u);
}

TEST(RouterFork, BranchStarvationStallsOnlyTheWorm) {
    SCENARIO(
        "While a fork worm is credit-starved on its NORTH branch, unicast traffic through "
        "an uninvolved output (SOUTH->WEST) keeps flowing — the work-conserving stage-2 "
        "scan is unaffected (D7); the starved worm itself stays frozen");
    RouterConfig cfg = center_cfg();
    cfg.vc_depth = 2;
    Router r(cfg);
    FlitSink north, east, west;
    r.set_downstream(N, north);
    r.set_downstream(E, east);
    r.set_downstream(W, west);
    const auto worm = make_mc_worm(make_id(1, 3), make_id(0, 1), make_id(2, 0), /*vc=*/0,
                                   /*fixed_vc=*/1, /*n_flits=*/6);
    std::size_t fed = 0;
    // Wedge the worm first: NORTH credit never returned.
    for (int t = 0; t < 20; ++t) {
        feed_worm(r, worm, fed, W, 0);
        const std::size_t be = east.received.size();
        r.tick();
        return_credit(r, east, E, be);
    }
    ASSERT_EQ(north.received.size(), 2u) << "worm not starved as arranged";
    const std::size_t frozen_east = east.received.size();

    // Unicast SOUTH -> WEST output while the worm is frozen.
    int sent = 0, got = 0;
    for (int t = 0; t < 20; ++t) {
        if (r.input_fifo_size(S, 0) < cfg.vc_depth) {
            r.input(S).push_flit(make_unicast_flit(make_id(0, 1), make_id(1, 0), /*vc=*/0,
                                                   /*flit_tail=*/1, static_cast<uint8_t>(sent)));
            ++sent;
        }
        const std::size_t bw = west.received.size();
        r.tick();
        return_credit(r, west, W, bw);
        got = static_cast<int>(west.received.size());
    }
    EXPECT_GE(got, 8) << "unicast starved by a stalled fork worm on other outputs";
    EXPECT_EQ(east.received.size(), frozen_east) << "starved worm advanced without credit";
    EXPECT_EQ(north.received.size(), 2u);
}

// --- Unicast degeneracy -----------------------------------------------------

TEST(RouterFork, OneHotForkSetIsBitIdenticalToPlainUnicast) {
    SCENARIO(
        "Unicast-degenerate (§1.1 rule 5): a collective worm whose fork set at this router "
        "is one-hot ({EAST} pass-through) takes the immediate-pop path — lockstep-identical "
        "to a plain unicast worm in per-tick deliveries, credits and FIFO occupancy, and "
        "byte-for-byte identical after clearing the two collective header fields");
    RouterConfig cfg = center_cfg();
    cfg.num_vc = 2;  // VA restamp active (fixed_vc=0): identity must survive it
    Router ra(cfg), rb(cfg);
    FlitSink ea, eb;
    ra.set_downstream(E, ea);
    rb.set_downstream(E, eb);
    // A: collective, src (0,1), anchor (3,1), mask (y=2): members (3,1),(3,3)
    // -> fork at (1,1) is {EAST} only (x unmatched, East spread).
    // B: plain unicast to (3,1), same src.
    constexpr int kFlits = 3;
    const auto worm_a = make_mc_worm(make_id(3, 1), make_id(0, 1), make_id(0, 2), /*vc=*/0,
                                     /*fixed_vc=*/0, kFlits);
    std::vector<Flit> worm_b;
    for (int i = 0; i < kFlits; ++i) {
        worm_b.push_back(make_unicast_flit(make_id(3, 1), make_id(0, 1), /*vc=*/0,
                                           /*flit_tail=*/i == kFlits - 1 ? 1 : 0,
                                           static_cast<uint8_t>(i)));
    }
    std::size_t fed_a = 0, fed_b = 0;
    for (int t = 0; t < 24; ++t) {
        feed_worm(ra, worm_a, fed_a, W, 0);
        feed_worm(rb, worm_b, fed_b, W, 0);
        const std::size_t ba = ea.received.size(), bb = eb.received.size();
        ra.tick();
        rb.tick();
        return_credit(ra, ea, E, ba);
        return_credit(rb, eb, E, bb);
        // Lockstep identity, every tick.
        ASSERT_EQ(ea.received.size(), eb.received.size()) << "delivery diverged at tick " << t;
        ASSERT_EQ(ra.input_fifo_size(W, 0), rb.input_fifo_size(W, 0)) << "tick " << t;
        for (uint8_t v = 0; v < cfg.num_vc; ++v) {
            ASSERT_EQ(ra.credit(E, v), rb.credit(E, v)) << "credit diverged, tick " << t;
        }
    }
    ASSERT_EQ(ea.received.size(), static_cast<std::size_t>(kFlits));
    for (int i = 0; i < kFlits; ++i) {
        Flit norm = ea.received[i];
        norm.set_header_field("collective_op", 0);
        norm.set_header_field("collective_mask", 0);
        EXPECT_EQ(norm.raw(), eb.received[i].raw()) << "flit " << i;
    }
}

// --- Concurrent trees -------------------------------------------------------

TEST(RouterFork, DisjointTreesProgressConcurrently) {
    SCENARIO(
        "Two forks with disjoint branch sets through one router ({E,N} from WEST, {L,S} "
        "from NORTH) both complete, and their delivery windows overlap in time — neither "
        "tree serializes behind the other");
    Router r(center_cfg());
    FlitSink local, north, east, south;
    r.set_downstream(L, local);
    r.set_downstream(N, north);
    r.set_downstream(E, east);
    r.set_downstream(S, south);
    constexpr int kFlits = 4;
    // M1: src (0,1), anchor (1,3), mask (x=2) -> {E,N}.
    const auto m1 = make_mc_worm(make_id(1, 3), make_id(0, 1), make_id(2, 0), 0, 1, kFlits);
    // M2: src (1,2), anchor (1,0), mask (y=1): members (1,0),(1,1) -> {L,S}.
    const auto m2 = make_mc_worm(make_id(1, 0), make_id(1, 2), make_id(0, 1), 0, 1, kFlits);
    std::size_t fed1 = 0, fed2 = 0;
    int m1_first = -1, m1_last = -1, m2_first = -1, m2_last = -1;
    for (int t = 0; t < 30; ++t) {
        feed_worm(r, m1, fed1, W, 0);
        feed_worm(r, m2, fed2, N, 0);
        const std::size_t bl = local.received.size(), bn = north.received.size(),
                          be = east.received.size(), bs = south.received.size();
        r.tick();
        return_credit(r, local, L, bl);
        return_credit(r, north, N, bn);
        return_credit(r, east, E, be);
        return_credit(r, south, S, bs);
        if (north.received.size() > bn || east.received.size() > be) {
            if (m1_first < 0) m1_first = t;
            m1_last = t;
        }
        if (local.received.size() > bl || south.received.size() > bs) {
            if (m2_first < 0) m2_first = t;
            m2_last = t;
        }
    }
    expect_worm_in_order(north, kFlits, "M1/NORTH");
    expect_worm_in_order(east, kFlits, "M1/EAST");
    expect_worm_in_order(local, kFlits, "M2/LOCAL");
    expect_worm_in_order(south, kFlits, "M2/SOUTH");
    // Concurrency: the two trees' delivery windows overlap.
    EXPECT_LE(m1_first, m2_last);
    EXPECT_LE(m2_first, m1_last);
}

// --- Per-branch VA (F7) and fixed_vc pinning (F8) ---------------------------

TEST(RouterFork, PerBranchVaAssignsEachBranchItsOwnPreferredVc) {
    SCENARIO(
        "F7: a fixed_vc=0 fork head runs vc_assignment per branch — EAST's next hop "
        "prefers vc1 while NORTH's straight run prefers vc0; each branch restamps and "
        "locks its own output VC and consumes its own credit");
    RouterConfig cfg = center_cfg();
    cfg.num_vc = 2;
    Router r(cfg);
    FlitSink north, east;
    r.set_downstream(N, north);
    r.set_downstream(E, east);
    // {E,N} fork, fixed_vc=0. EAST branch: next hop from (2,1) to (1,3) is
    // WEST -> preferred 1 (floo_vc_assignment.sv:93). NORTH branch: straight
    // N -> preferred 0 (:90).
    const auto worm = make_mc_worm(make_id(1, 3), make_id(0, 1), make_id(2, 0), /*vc=*/0,
                                   /*fixed_vc=*/0, /*n_flits=*/3);
    std::size_t fed = 0;
    bool checked_locks = false;
    for (int t = 0; t < 24; ++t) {
        feed_worm(r, worm, fed, W, 0);
        const std::size_t bn = north.received.size(), be = east.received.size();
        r.tick();
        if (!checked_locks && r.wormhole_locked_output_vc(E).has_value() &&
            r.wormhole_locked_output_vc(N).has_value()) {
            EXPECT_EQ(*r.wormhole_locked_output_vc(E), 1u) << "EAST branch locked_output_vc";
            EXPECT_EQ(*r.wormhole_locked_output_vc(N), 0u) << "NORTH branch locked_output_vc";
            checked_locks = true;
        }
        return_credit(r, north, N, bn);
        return_credit(r, east, E, be);
    }
    EXPECT_TRUE(checked_locks) << "branch locks never observed mid-worm";
    ASSERT_EQ(east.received.size(), 3u);
    ASSERT_EQ(north.received.size(), 3u);
    for (const auto& f : east.received) {
        EXPECT_EQ(f.get_header_field("vc_id"), 1u) << "EAST branch not restamped to its VA vc";
    }
    for (const auto& f : north.received) {
        EXPECT_EQ(f.get_header_field("vc_id"), 0u) << "NORTH branch not restamped to its VA vc";
    }
}

class RouterForkPinnedVc : public ::testing::TestWithParam<int> {};

TEST_P(RouterForkPinnedVc, PinnedVcRidesEveryBranchAcrossNumVc) {
    SCENARIO(
        "F8/D8: a fixed_vc=1 collective worm keeps its NI-pinned vc_id on EVERY branch "
        "(num_vc parameterized 1/2/4); only the pinned VC's credit is consumed per branch");
    const int num_vc = GetParam();
    RouterConfig cfg = center_cfg();
    cfg.num_vc = static_cast<uint8_t>(num_vc);
    Router r(cfg);
    FlitSink north, east;
    r.set_downstream(N, north);
    r.set_downstream(E, east);
    const auto pinned = static_cast<uint8_t>(num_vc - 1);
    constexpr int kFlits = 4;
    const auto worm = make_mc_worm(make_id(1, 3), make_id(0, 1), make_id(2, 0), pinned,
                                   /*fixed_vc=*/1, kFlits);
    std::size_t fed = 0;
    for (int t = 0; t < 30; ++t) {
        feed_worm(r, worm, fed, W, pinned);
        const std::size_t bn = north.received.size(), be = east.received.size();
        r.tick();
        return_credit(r, north, N, bn);
        return_credit(r, east, E, be);
    }
    expect_worm_in_order(north, kFlits, "NORTH");
    expect_worm_in_order(east, kFlits, "EAST");
    for (const auto& f : north.received) EXPECT_EQ(f.get_header_field("vc_id"), pinned);
    for (const auto& f : east.received) EXPECT_EQ(f.get_header_field("vc_id"), pinned);
    // Off-pinned VCs untouched: their credit never moved.
    for (uint8_t v = 0; v < cfg.num_vc; ++v) {
        if (v == pinned) continue;
        EXPECT_EQ(r.credit(N, v), cfg.vc_depth) << "vc " << static_cast<int>(v);
        EXPECT_EQ(r.credit(E, v), cfg.vc_depth) << "vc " << static_cast<int>(v);
    }
}

INSTANTIATE_TEST_SUITE_P(NumVc, RouterForkPinnedVc, ::testing::Values(1, 2, 4));

// --- Fault injection --------------------------------------------------------

TEST(RouterForkDeath, EmptyForkSetOnCollectiveHeadAborts) {
    SCENARIO(
        "Fault injection (T3 hard rule 2): a collective flit whose fork set at this router "
        "is EMPTY (misrouted multicast) is fatal — F3's pop condition would otherwise be "
        "trivially true and silently drop + credit the flit");
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            Router r(center_cfg());
            // src (0,0), anchor (0,0), mask (x=2): members (0,0),(2,0) — the
            // tree never touches (1,1) (x unmatched, not on the source row).
            r.input(W).push_flit(make_mc_flit(make_id(0, 0), make_id(0, 0), make_id(2, 0),
                                              /*vc=*/0, /*flit_tail=*/0, /*fixed_vc=*/1, 0));
            r.tick();
            r.tick();
        },
        "empty fork set");
}

TEST(RouterForkDeath, ContinuationBranchSetMismatchAborts) {
    SCENARIO(
        "Fault injection (F9, src-anchored): a W continuation whose recomputed branch set "
        "differs from the head's — shrunk ({L,E,N} head, {L,N} beat) or enlarged ({L,N} "
        "head, {L,E,N} beat) — trips the branch-set divergence assert; the enlarged case "
        "also proves an unlocked output cannot join mid-worm");
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    // Shrunk continuation set.
    EXPECT_DEATH(
        {
            Router r(center_cfg());
            FlitSink local;
            FlitSink north;
            FlitSink east;
            r.set_downstream(L, local);
            r.set_downstream(N, north);
            r.set_downstream(E, east);
            // Head: {L,E,N} (mask x=2,y=2). Grant on all three branches, pop.
            r.input(W).push_flit(make_mc_flit(make_id(1, 1), make_id(0, 1), make_id(2, 2), 0,
                                              /*flit_tail=*/0, 1, 0));
            r.tick();
            r.tick();
            r.tick();
            // Corrupted beat: mask (y=2) -> {L,N} at this router.
            r.input(W).push_flit(make_mc_flit(make_id(1, 1), make_id(0, 1), make_id(0, 2), 0,
                                              /*flit_tail=*/0, 1, 1));
            r.tick();
            r.tick();
        },
        "branch set diverges");
    // Enlarged continuation set.
    EXPECT_DEATH(
        {
            Router r(center_cfg());
            FlitSink local;
            FlitSink north;
            FlitSink east;
            r.set_downstream(L, local);
            r.set_downstream(N, north);
            r.set_downstream(E, east);
            // Head: {L,N} (mask y=2, anchor (1,1), src (0,1)).
            r.input(W).push_flit(make_mc_flit(make_id(1, 1), make_id(0, 1), make_id(0, 2), 0,
                                              /*flit_tail=*/0, 1, 0));
            r.tick();
            r.tick();
            r.tick();
            // Corrupted beat: mask (x=2,y=2) -> {L,E,N}.
            r.input(W).push_flit(make_mc_flit(make_id(1, 1), make_id(0, 1), make_id(2, 2), 0,
                                              /*flit_tail=*/0, 1, 1));
            r.tick();
            r.tick();
        },
        "branch set diverges");
}

}  // namespace
