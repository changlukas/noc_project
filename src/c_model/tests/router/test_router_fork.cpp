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

// --- §1.2 wedge: overlapping trees, opposite acquisition order --------------

struct WireLink : ni::cmodel::router::RouterLink {
    Router* target = nullptr;
    std::size_t port = 0;
    void push_flit(const Flit& f) override { target->input(port).push_flit(f); }
};

struct CreditRelay : ni::cmodel::router::RouterCreditSink {
    Router* target = nullptr;
    std::size_t out_port = 0;
    void receive_credit(uint8_t vc) override { target->receive_credit(out_port, vc); }
};

// Sink that returns the credit inline at delivery (stage 3 runs first in
// tick(), so the credit is usable the same tick — the EjectSink pattern of
// test_router.cpp). Models a freely absorbing submesh row.
struct DrainingSink : ni::cmodel::router::RouterLink {
    Router* router = nullptr;
    std::size_t out_port = 0;
    std::vector<Flit> received;
    void push_flit(const Flit& f) override {
        received.push_back(f);
        router->receive_credit(out_port, static_cast<uint8_t>(f.get_header_field("vc_id")));
    }
};

TEST(RouterForkWedge, OverlappingTreesOppositeOrderWedgeDetectedWithinBound) {
    SCENARIO(
        "§1.2/§1.3 detection a: two overlapping multicast trees acquire N@R1 and N@R2 in "
        "opposite orders under credit exhaustion (burst >> vc_depth). The ported F2/F3 "
        "discipline DEADLOCKS — this test PASSES by detecting the no-progress state within "
        "the derived tick bound and attributing it through the fork introspection: both "
        "fork done_masks frozen short of expected with the missing branches' outputs "
        "locked to the other worm. This is the R1 restriction's documented hazard "
        "demonstration, not a bug in the fork");
    // Row y=0 of a 4x2 mesh. M1 from (0,0) and M2 from (3,0) both multicast
    // to the full row above (anchor (0,1), mask x=3): fork {E,N} at every
    // source-row router for M1, {W,N} for M2. M1 locks N@R1 then needs N@R2;
    // M2 locks N@R2 then needs N@R1. vc_depth 4 << worm length 8 exhausts
    // the R1<->R2 link credits, freezing both worms mid-burst.
    RouterConfig cfg1;
    cfg1.mesh_x_dim = 4;
    cfg1.mesh_y_dim = 2;
    cfg1.x = 1;
    cfg1.y = 0;
    cfg1.vc_depth = 4;
    RouterConfig cfg2 = cfg1;
    cfg2.x = 2;
    Router r1(cfg1), r2(cfg2);

    // R1.EAST <-> R2.WEST wire + credit return; N/W/E edges drain freely.
    WireLink r1_to_r2, r2_to_r1;
    r1_to_r2.target = &r2;
    r1_to_r2.port = W;
    r2_to_r1.target = &r1;
    r2_to_r1.port = E;
    r1.set_downstream(E, r1_to_r2);
    r2.set_downstream(W, r2_to_r1);
    CreditRelay r2w_credit, r1e_credit;
    r2w_credit.target = &r1;  // R2 pops its WEST input -> credit home to R1.EAST
    r2w_credit.out_port = E;
    r2.set_upstream_credit(W, r2w_credit);
    r1e_credit.target = &r2;  // R1 pops its EAST input -> credit home to R2.WEST
    r1e_credit.out_port = W;
    r1.set_upstream_credit(E, r1e_credit);
    DrainingSink n1, w1, n2, e2;
    n1.router = &r1;
    n1.out_port = N;
    w1.router = &r1;
    w1.out_port = W;
    n2.router = &r2;
    n2.out_port = N;
    e2.router = &r2;
    e2.out_port = E;
    r1.set_downstream(N, n1);
    r1.set_downstream(W, w1);
    r2.set_downstream(N, n2);
    r2.set_downstream(E, e2);

    constexpr int kWormFlits = 8;  // AW head + 6 beats + tail, > vc_depth
    const auto m1 = make_mc_worm(make_id(0, 1), make_id(0, 0), make_id(3, 0), /*vc=*/0,
                                 /*fixed_vc=*/1, kWormFlits);
    const auto m2 = make_mc_worm(make_id(0, 1), make_id(3, 0), make_id(3, 0), /*vc=*/0,
                                 /*fixed_vc=*/1, kWormFlits);

    // Wedge tick bound — DERIVED, not guessed. While anything still
    // progresses, at least one flit crosses one pipeline stage somewhere.
    // Pre-wedge work is bounded by: 2 worms x kWormFlits flits x 2 routers x
    // (kPipelineDepth + 1) stage advances each (pipeline stages plus the
    // one-tick registered credit return a grant may wait on), plus
    // 2 x kWormFlits injection ticks (<= 1 flit/tick/input under the
    // FIFO-room feed gate). Past this bound every reachable advance has
    // happened; anything still moving would disprove the wedge.
    constexpr int kWedgeBound =
        2 * kWormFlits * 2 * (kPipelineDepth + 1) + 2 * kWormFlits;  // = 144
    // A live system shows an observable state change at least once per
    // grant -> stage-3 push -> registered credit pulse -> re-grant round
    // trip, < 2 x (kPipelineDepth + 1) ticks.
    constexpr int kQuiescentWindow = 2 * (kPipelineDepth + 1);  // = 8

    auto snapshot = [&]() {
        return std::vector<std::size_t>{
            n1.received.size(),       w1.received.size(),       n2.received.size(),
            e2.received.size(),       r1.fork_done_mask(W, 0),  r1.fork_done_mask(E, 0),
            r2.fork_done_mask(W, 0),  r2.fork_done_mask(E, 0),  r1.input_fifo_size(W, 0),
            r1.input_fifo_size(E, 0), r2.input_fifo_size(W, 0), r2.input_fifo_size(E, 0),
            r1.credit(E, 0),          r2.credit(W, 0)};
    };

    std::size_t fed1 = 0, fed2 = 0;
    std::vector<std::size_t> prev;
    int last_progress = 0;
    bool wedged = false;
    for (int t = 0; t < kWedgeBound + kQuiescentWindow + 1; ++t) {
        feed_worm(r1, m1, fed1, W, 0);
        feed_worm(r2, m2, fed2, E, 0);
        r1.tick();
        r2.tick();
        auto sig = snapshot();
        if (sig != prev) {
            last_progress = t;
            prev = std::move(sig);
        }
        if (t - last_progress >= kQuiescentWindow) {
            wedged = true;  // the no-progress detection — this IS the pass
            break;
        }
    }
    ASSERT_TRUE(wedged) << "trees did not wedge: the §1.2 deadlock no longer reproduces "
                           "(re-derive the R1 restriction before celebrating)";
    EXPECT_LE(last_progress, kWedgeBound) << "progress past the derived bound";
    EXPECT_EQ(fed1, static_cast<std::size_t>(kWormFlits));
    EXPECT_EQ(fed2, static_cast<std::size_t>(kWormFlits));

    // Attribution (§1.3 detection b): the live introspection masks name the
    // wedge — done frozen short of expected, missing branch = the output the
    // OTHER worm locked, both link credits exhausted.
    const PortMask kEN = port_bit(RouterPort::EAST) | port_bit(RouterPort::NORTH);
    const PortMask kWN = port_bit(RouterPort::WEST) | port_bit(RouterPort::NORTH);
    // M1 mid-worm at R1: EAST branch starved, NORTH taken.
    EXPECT_EQ(r1.fork_expected_mask(W, 0), kEN);
    EXPECT_EQ(r1.fork_done_mask(W, 0), port_bit(RouterPort::NORTH));
    // M1 head at R2: EAST taken, NORTH held by M2.
    EXPECT_EQ(r2.fork_expected_mask(W, 0), kEN);
    EXPECT_EQ(r2.fork_done_mask(W, 0), port_bit(RouterPort::EAST));
    // M2 mid-worm at R2: WEST branch starved, NORTH taken.
    EXPECT_EQ(r2.fork_expected_mask(E, 0), kWN);
    EXPECT_EQ(r2.fork_done_mask(E, 0), port_bit(RouterPort::NORTH));
    // M2 head at R1: WEST taken, NORTH held by M1.
    EXPECT_EQ(r1.fork_expected_mask(E, 0), kWN);
    EXPECT_EQ(r1.fork_done_mask(E, 0), port_bit(RouterPort::WEST));
    // The wait-for cycle: M1 holds N@R1, M2 holds N@R2.
    EXPECT_EQ(r1.wormhole_locked_input(N), std::optional<std::size_t>(W));
    EXPECT_EQ(r2.wormhole_locked_input(N), std::optional<std::size_t>(E));
    // Credit exhaustion on both directions of the contended link.
    EXPECT_EQ(r1.credit(E, 0), 0u);
    EXPECT_EQ(r2.credit(W, 0), 0u);
}

// --- Multi-hop traversal (divergent one-hot pass-through hop) ---------------

TEST(RouterForkChain, MultiHopWormCrossesDivergentOneHotHop) {
    SCENARIO(
        "2-router chain: a multicast worm (anchor (0,1), src (0,0), mask x=3) forks {E,N} "
        "at (2,0) and {N} at (3,0) — at the spread-end hop the one-hot fork direction "
        "(NORTH) diverges from the anchor's XY route (WEST). The worm must traverse "
        "cleanly end to end: the locked continuation check keys on the fork set, never "
        "route_compute, for ANY collective flit (review fix)");
    RouterConfig ca;
    ca.mesh_x_dim = 4;
    ca.mesh_y_dim = 2;
    ca.x = 2;
    ca.y = 0;
    RouterConfig cb = ca;
    cb.x = 3;
    Router ra(ca), rb(cb);
    WireLink a_to_b;
    a_to_b.target = &rb;
    a_to_b.port = W;
    ra.set_downstream(E, a_to_b);
    CreditRelay b_credit;
    b_credit.target = &ra;
    b_credit.out_port = E;
    rb.set_upstream_credit(W, b_credit);
    DrainingSink na, nb;
    na.router = &ra;
    na.out_port = N;
    nb.router = &rb;
    nb.out_port = N;
    ra.set_downstream(N, na);
    rb.set_downstream(N, nb);

    constexpr int kFlits = 4;
    const auto worm = make_mc_worm(make_id(0, 1), make_id(0, 0), make_id(3, 0), /*vc=*/0,
                                   /*fixed_vc=*/1, kFlits);
    std::size_t fed = 0;
    for (int t = 0; t < 40; ++t) {
        feed_worm(ra, worm, fed, W, 0);
        ra.tick();
        rb.tick();
    }
    ASSERT_EQ(na.received.size(), static_cast<std::size_t>(kFlits)) << "branch hop (2,0)N";
    ASSERT_EQ(nb.received.size(), static_cast<std::size_t>(kFlits)) << "divergent hop (3,0)N";
    for (int i = 0; i < kFlits; ++i) {
        EXPECT_EQ(na.received[i].get_header_field("ordering_tag"), static_cast<uint64_t>(i));
        EXPECT_EQ(nb.received[i].get_header_field("ordering_tag"), static_cast<uint64_t>(i));
    }
    for (std::size_t o = 0; o < ni::cmodel::router::ROUTER_PORT_COUNT; ++o) {
        EXPECT_FALSE(ra.wormhole_locked_input(o).has_value()) << "ra output " << o;
        EXPECT_FALSE(rb.wormhole_locked_input(o).has_value()) << "rb output " << o;
    }
    EXPECT_EQ(ra.fork_done_mask(W, 0), 0u);
    EXPECT_EQ(rb.fork_done_mask(W, 0), 0u);
}

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

TEST(RouterForkDeath, ReservedCollectiveOpAborts) {
    SCENARIO(
        "Fault injection (spec §6 :356 reserved codes 2-3): a flit stamped with a reserved "
        "collective_op is fatal — every collective classification keys on `!= UNICAST`, so "
        "an unrejected reserved code would silently fork as a multicast");
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            Router r(center_cfg());
            // Otherwise-legal collective geometry (anchor (1,1), src (0,1),
            // mask y=2 -> {L,N}); only the opcode is corrupt.
            Flit f = make_mc_flit(make_id(1, 1), make_id(0, 1), make_id(0, 2), /*vc=*/0,
                                  /*flit_tail=*/0, /*fixed_vc=*/1, 0);
            f.set_header_field("collective_op", 2);
            r.input(W).push_flit(f);
            r.tick();
            r.tick();
        },
        "reserved collective_op");
}

TEST(RouterForkDeath, CollectiveOnReadChannelAborts) {
    SCENARIO(
        "Fault injection (design §3.1): a collective flit on a read channel is fatal — "
        "ARUSER has no collective surface (spec §6 :324), so reads are unicast everywhere "
        "and such a header is mis-stamped");
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            Router r(center_cfg());
            Flit f = make_mc_flit(make_id(1, 1), make_id(0, 1), make_id(0, 2), /*vc=*/0,
                                  /*flit_tail=*/0, /*fixed_vc=*/1, 0);
            f.set_header_field("axi_ch", ni::AXI_CH_DataR);
            r.input(W).push_flit(f);
            r.tick();
            r.tick();
        },
        "read channel");
}

TEST(RouterForkDeath, ContinuationBranchSetMismatchAborts) {
    SCENARIO(
        "Fault injection (F9, src-anchored): a W continuation whose recomputed branch set "
        "differs from the head's — shrunk ({L,E,N} head, {L,N} beat), enlarged ({L,N} head, "
        "{L,E,N} beat), or corrupted to the ONE-HOT set {L} whose route_compute(dst) "
        "coincidentally passes the unicast check — each trips the branch-set divergence "
        "assert; the enlarged case also proves an unlocked output cannot join mid-worm, and "
        "the one-hot case proves a collective continuation never takes the unicast locked "
        "path");
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
    // ONE-HOT corrupted continuation (review Important): anchor (1,1),
    // mask 0 -> set {L} at this router, and route_compute(dst)==LOCAL
    // coincidentally passes — the pre-fix unicast locked path would grant
    // and pop it, silently skipping the still-locked NORTH branch. The
    // collective locked path must run the F9 set check instead: {L} !=
    // locked|done {L,N} -> abort.
    EXPECT_DEATH(
        {
            Router r(center_cfg());
            FlitSink local;
            FlitSink north;
            r.set_downstream(L, local);
            r.set_downstream(N, north);
            // Head: {L,N} (mask y=2, anchor (1,1), src (0,1)).
            r.input(W).push_flit(make_mc_flit(make_id(1, 1), make_id(0, 1), make_id(0, 2), 0,
                                              /*flit_tail=*/0, 1, 0));
            r.tick();
            r.tick();
            r.tick();
            // Corrupted one-hot beat: mask 0 -> {L}.
            r.input(W).push_flit(make_mc_flit(make_id(1, 1), make_id(0, 1), /*cmask=*/0, 0,
                                              /*flit_tail=*/0, 1, 1));
            r.tick();
            r.tick();
        },
        "branch set diverges");
}

}  // namespace
