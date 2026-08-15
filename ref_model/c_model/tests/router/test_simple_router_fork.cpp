// S4-T3b REQ SimpleRouter multicast fork: the floo_router.sv:344-394
// past_handshakes discipline in its native ready/valid form, the mask-valued
// route latch, fork introspection, the AR-interleave claim, and the §1.2
// overlapping-tree wedge on REQ. Branch geometry cells are hand-computed from
// floo_route_xymask.sv:104-164 the same way test_route_mask.cpp verifies them;
// this file tests the ROUTER's use of the fork set, not the mask math.
#include "router/route_mask.hpp"
#include "router/simple_router.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

using ni::cmodel::Flit;
using ni::cmodel::router::port_bit;
using ni::cmodel::router::PortMask;
using ni::cmodel::router::RouterPort;
using ni::cmodel::router::SimpleRouter;
using ni::cmodel::router::SimpleRouterConfig;
using ni::cmodel::router::SimpleRouterLink;

namespace {

constexpr auto L = static_cast<std::size_t>(RouterPort::LOCAL);
constexpr auto N = static_cast<std::size_t>(RouterPort::NORTH);
constexpr auto E = static_cast<std::size_t>(RouterPort::EAST);
constexpr auto S = static_cast<std::size_t>(RouterPort::SOUTH);
constexpr auto W = static_cast<std::size_t>(RouterPort::WEST);

// Zero-load pipeline depth in direct mode (output_fifo_depth == 0):
// SimpleRouterDatapath.ZeroLoadLatencyDirectModeTwoTicks.
constexpr int kPipelineDepth = 2;

uint8_t make_id(uint8_t x, uint8_t y) {
    return static_cast<uint8_t>((y << ni::width::X_WIDTH) | x);
}

SimpleRouterConfig center_cfg() {
    SimpleRouterConfig cfg;
    cfg.x = 1;
    cfg.y = 1;  // center of default 4x4
    return cfg;
}

struct FlitSink : SimpleRouterLink {
    std::vector<Flit> received;
    bool always_ready = true;
    bool ready(uint8_t /*vc*/) const override { return always_ready; }
    void push_flit(const Flit& f) override { received.push_back(f); }
};

// A collective flit: dst_id is the node the address names, src_id the tree source,
// collective_mask the wildcard (T2 stamps AW and every W beat identically).
// ordering_tag carries the test's beat sequence number.
Flit make_mc_flit(uint8_t dst, uint8_t src, uint8_t cmask, uint8_t vc, uint64_t flit_tail,
                  uint8_t tag) {
    Flit f;
    f.set_header_field("collective_op", ni::COLLECTIVE_OP_MULTICAST);
    f.set_header_field("collective_mask", cmask);
    f.set_header_field("dst_id", dst);
    f.set_header_field("src_id", src);
    f.set_header_field("vc_id", vc);
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

// n-flit worm (head flit_tail=0 .. tail flit_tail=1), tags 0..n-1.
std::vector<Flit> make_mc_worm(uint8_t dst, uint8_t src, uint8_t cmask, uint8_t vc, int n_flits) {
    std::vector<Flit> worm;
    for (int i = 0; i < n_flits; ++i) {
        worm.push_back(make_mc_flit(dst, src, cmask, vc, /*flit_tail=*/i == n_flits - 1 ? 1 : 0,
                                    static_cast<uint8_t>(i)));
    }
    return worm;
}

// Ready/valid-compliant upstream: push the next unfed flit only while the
// receiver asserts ready (one flit/port/tick).
void feed_worm(SimpleRouter& r, const std::vector<Flit>& worm, std::size_t& fed,
               std::size_t in_port, uint8_t vc) {
    if (fed >= worm.size()) return;
    if (!r.ready(in_port, vc)) return;
    r.input(in_port).push_flit(worm[fed]);
    ++fed;
}

void expect_worm_in_order(const FlitSink& sink, int n_flits, const char* label) {
    ASSERT_EQ(sink.received.size(), static_cast<std::size_t>(n_flits)) << label;
    for (int i = 0; i < n_flits; ++i) {
        EXPECT_EQ(sink.received[i].get_header_field("ordering_tag"), static_cast<uint64_t>(i))
            << label << ": beat " << i << " out of order";
    }
}

// --- Fork delivery ---------------------------------------------------------

TEST(SimpleRouterFork, ThreeWayInclLocalReplicatesWholeWorm) {
    SimpleRouter r(center_cfg());
    FlitSink local, north, east;
    r.set_downstream(L, local);
    r.set_downstream(N, north);
    r.set_downstream(E, east);
    // src (0,1), dst (1,1), mask (x=2,y=2): members {1,3}x{1,3} -> fork
    // {L,E,N} at (1,1) (x/y matched -> LOCAL; East spread; North turn).
    constexpr int kFlits = 4;
    const auto worm = make_mc_worm(make_id(1, 1), make_id(0, 1), make_id(2, 2), /*vc=*/0, kFlits);
    std::size_t fed = 0;
    for (int t = 0; t < 30; ++t) {
        feed_worm(r, worm, fed, W, 0);
        r.tick();
    }
    expect_worm_in_order(local, kFlits, "LOCAL");
    expect_worm_in_order(north, kFlits, "NORTH");
    expect_worm_in_order(east, kFlits, "EAST");
    for (int i = 0; i < kFlits; ++i) {
        EXPECT_EQ(local.received[i].raw(), worm[i].raw());
        EXPECT_EQ(north.received[i].raw(), worm[i].raw());
        EXPECT_EQ(east.received[i].raw(), worm[i].raw());
    }
    EXPECT_EQ(r.input_fifo_size(W, 0), 0u) << "input FIFO not released after the worm";
    for (std::size_t o = 0; o < ni::cmodel::router::ROUTER_PORT_COUNT; ++o) {
        EXPECT_FALSE(r.wormhole_locked_input(o).has_value()) << "output " << o;
    }
    EXPECT_EQ(r.fork_done_mask(W, 0), 0u);
    EXPECT_EQ(r.route_locked(W, 0), 0u) << "route latch not released at the tail";
}

// S3a ruling: LOCAL->LOCAL is a real fork branch -- floo_router.sv:383-385's ignore_routes loopback
// exclusion was not ported.
TEST(SimpleRouterFork, LocalInputWithLocalBranch) {
    SimpleRouter r(center_cfg());
    FlitSink local, north;
    r.set_downstream(L, local);
    r.set_downstream(N, north);
    // src (1,1) = this node, dst (1,1), mask (y=2): members (1,1),(1,3).
    constexpr int kFlits = 3;
    const auto worm = make_mc_worm(make_id(1, 1), make_id(1, 1), make_id(0, 2), /*vc=*/0, kFlits);
    std::size_t fed = 0;
    for (int t = 0; t < 24; ++t) {
        feed_worm(r, worm, fed, L, 0);
        r.tick();
    }
    expect_worm_in_order(local, kFlits, "LOCAL");
    expect_worm_in_order(north, kFlits, "NORTH");
}

// --- W replication under branch backpressure -------------------------------

TEST(SimpleRouterFork, MidBurstBranchStallStallsWormWithoutSkipOrReorder) {
    SimpleRouter r(center_cfg());
    FlitSink north, east;
    r.set_downstream(N, north);
    r.set_downstream(E, east);
    // src (0,1), dst (1,3), mask (x=2): members (1,3),(3,3) -> fork {E,N}
    // at (1,1) (no LOCAL: y unmatched).
    constexpr int kFlits = 6;
    const auto worm = make_mc_worm(make_id(1, 3), make_id(0, 1), make_id(2, 0), /*vc=*/0, kFlits);
    std::size_t fed = 0;

    // Phase A: NORTH stops accepting after 2 flits, mid-worm.
    for (int t = 0; t < 30; ++t) {
        if (north.received.size() >= 2) north.always_ready = false;
        feed_worm(r, worm, fed, W, 0);
        r.tick();
    }
    EXPECT_EQ(north.received.size(), 2u);
    EXPECT_EQ(east.received.size(), 3u) << "EAST may lead only by the parked beat";
    EXPECT_EQ(r.fork_expected_mask(W, 0), port_bit(RouterPort::EAST) | port_bit(RouterPort::NORTH));
    EXPECT_EQ(r.fork_done_mask(W, 0), port_bit(RouterPort::EAST));
    EXPECT_EQ(r.wormhole_locked_input(N), std::optional<std::size_t>(W));
    EXPECT_EQ(r.wormhole_locked_input(E), std::optional<std::size_t>(W));

    // Phase B: NORTH accepts again — the worm completes on both branches.
    north.always_ready = true;
    for (int t = 0; t < 30; ++t) {
        feed_worm(r, worm, fed, W, 0);
        r.tick();
    }
    expect_worm_in_order(north, kFlits, "NORTH");
    expect_worm_in_order(east, kFlits, "EAST");
    EXPECT_FALSE(r.wormhole_locked_input(N).has_value());
    EXPECT_FALSE(r.wormhole_locked_input(E).has_value());
    EXPECT_EQ(r.fork_done_mask(W, 0), 0u);
    EXPECT_EQ(r.route_locked(W, 0), 0u);
}

// --- Unicast degeneracy -----------------------------------------------------

TEST(SimpleRouterFork, OneHotForkSetIsBitIdenticalToPlainUnicast) {
    SimpleRouter ra(center_cfg()), rb(center_cfg());
    FlitSink ea, eb;
    ra.set_downstream(E, ea);
    rb.set_downstream(E, eb);
    // A: collective, src (0,1), dst (3,1), mask (y=2): members (3,1),(3,3)
    // -> fork at (1,1) is {EAST} only (x unmatched, East spread).
    // B: plain unicast to (3,1), same src.
    constexpr int kFlits = 3;
    const auto worm_a = make_mc_worm(make_id(3, 1), make_id(0, 1), make_id(0, 2), /*vc=*/0, kFlits);
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
        ra.tick();
        rb.tick();
        ASSERT_EQ(ea.received.size(), eb.received.size()) << "delivery diverged at tick " << t;
        ASSERT_EQ(ra.input_fifo_size(W, 0), rb.input_fifo_size(W, 0)) << "tick " << t;
        ASSERT_EQ(ra.wormhole_locked_input(E), rb.wormhole_locked_input(E)) << "tick " << t;
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

TEST(SimpleRouterFork, DisjointTreesProgressConcurrently) {
    SimpleRouter r(center_cfg());
    FlitSink local, north, east, south;
    r.set_downstream(L, local);
    r.set_downstream(N, north);
    r.set_downstream(E, east);
    r.set_downstream(S, south);
    constexpr int kFlits = 4;
    // M1: src (0,1), dst (1,3), mask (x=2) -> {E,N}.
    const auto m1 = make_mc_worm(make_id(1, 3), make_id(0, 1), make_id(2, 0), 0, kFlits);
    // M2: src (1,2), dst (1,0), mask (y=1): members (1,0),(1,1) -> {L,S}.
    const auto m2 = make_mc_worm(make_id(1, 0), make_id(1, 2), make_id(0, 1), 0, kFlits);
    std::size_t fed1 = 0, fed2 = 0;
    int m1_first = -1, m1_last = -1, m2_first = -1, m2_last = -1;
    for (int t = 0; t < 30; ++t) {
        feed_worm(r, m1, fed1, W, 0);
        feed_worm(r, m2, fed2, N, 0);
        const std::size_t bl = local.received.size(), bn = north.received.size(),
                          be = east.received.size(), bs = south.received.size();
        r.tick();
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
    EXPECT_LE(m1_first, m2_last);
    EXPECT_LE(m2_first, m1_last);
}

// --- AR interleave (design T3b row, Codex-constructed case) ------------------

TEST(SimpleRouterFork, PreFrozenArDelaysABranchButNeverInterleavesTheWorm) {
    SimpleRouter r(center_cfg());
    FlitSink local, north, east;
    r.set_downstream(L, local);
    r.set_downstream(N, north);
    r.set_downstream(E, east);
    east.always_ready = false;  // hold the AR at the EAST output

    // AR (unicast, single flit) from LOCAL to (3,1) -> EAST. Two ticks: admit,
    // then let the unconditional freeze scan claim EAST for it.
    r.input(L).push_flit(make_unicast_flit(make_id(3, 1), make_id(1, 1), 0, /*flit_tail=*/1, 0xA0));
    r.tick();
    r.tick();
    ASSERT_EQ(r.wormhole_locked_input(E), std::optional<std::size_t>(L)) << "AR did not pre-freeze";
    ASSERT_TRUE(east.received.empty());

    // Multicast worm {E,N} arrives at WEST behind the frozen AR.
    constexpr int kFlits = 4;
    const auto worm = make_mc_worm(make_id(1, 3), make_id(0, 1), make_id(2, 0), /*vc=*/0, kFlits);
    std::size_t fed = 0;
    for (int t = 0; t < 8; ++t) {
        feed_worm(r, worm, fed, W, 0);
        r.tick();
    }
    // The AW head reached NORTH; the worm is parked there, idle, because EAST
    // still owes the AR. This is the stall the design predicts — not a break.
    EXPECT_EQ(north.received.size(), 1u) << "NORTH advanced past the parked head";
    EXPECT_EQ(r.fork_done_mask(W, 0), port_bit(RouterPort::NORTH));
    EXPECT_TRUE(east.received.empty());

    // EAST accepts: the AR clears first, then EAST joins the worm.
    east.always_ready = true;
    bool second_ar_sent = false;
    for (int t = 0; t < 30; ++t) {
        feed_worm(r, worm, fed, W, 0);
        // A second AR offered once the worm has STARTED on EAST — it must wait
        // for the tail, never split the worm.
        if (!second_ar_sent && east.received.size() >= 2) {
            r.input(L).push_flit(
                make_unicast_flit(make_id(3, 1), make_id(1, 1), 0, /*flit_tail=*/1, 0xA1));
            second_ar_sent = true;
        }
        r.tick();
    }
    ASSERT_TRUE(second_ar_sent);
    expect_worm_in_order(north, kFlits, "NORTH");
    // EAST sees: AR, the whole worm contiguously, then the second AR.
    ASSERT_EQ(east.received.size(), static_cast<std::size_t>(kFlits + 2));
    EXPECT_EQ(east.received.front().get_header_field("ordering_tag"), 0xA0u);
    EXPECT_EQ(east.received.back().get_header_field("ordering_tag"), 0xA1u);
    for (int i = 0; i < kFlits; ++i) {
        const auto& f = east.received[static_cast<std::size_t>(i) + 1];
        EXPECT_EQ(f.get_header_field("collective_op"), ni::COLLECTIVE_OP_MULTICAST)
            << "an AR interleaved into the started EAST worm at position " << i + 1;
        EXPECT_EQ(f.get_header_field("ordering_tag"), static_cast<uint64_t>(i));
    }
}

// --- Multi-hop traversal (divergent one-hot pass-through hop) ---------------

TEST(SimpleRouterForkChain, MultiHopWormCrossesDivergentOneHotHop) {
    SimpleRouterConfig ca;
    ca.mesh_x_dim = 4;
    ca.mesh_y_dim = 2;
    ca.x = 2;
    ca.y = 0;
    SimpleRouterConfig cb = ca;
    cb.x = 3;
    SimpleRouter ra(ca), rb(cb);
    ra.set_downstream(E, rb.input(W));
    FlitSink na, nb;
    ra.set_downstream(N, na);
    rb.set_downstream(N, nb);

    constexpr int kFlits = 4;
    const auto worm = make_mc_worm(make_id(0, 1), make_id(0, 0), make_id(3, 0), /*vc=*/0, kFlits);
    std::size_t fed = 0;
    for (int t = 0; t < 40; ++t) {
        feed_worm(ra, worm, fed, W, 0);
        ra.tick();
        rb.tick();
    }
    expect_worm_in_order(na, kFlits, "branch hop (2,0)N");
    expect_worm_in_order(nb, kFlits, "divergent one-hot hop (3,0)N");
    // The divergent hop routed by its fork set, not by route_compute (which
    // would have sent it WEST, back the way it came).
    EXPECT_EQ(rb.route_locked(W, 0), 0u) << "latch not released at the tail";
    EXPECT_EQ(ra.fork_done_mask(W, 0), 0u);
    EXPECT_EQ(rb.fork_done_mask(W, 0), 0u);
}

// --- §1.2 wedge on REQ: overlapping trees, opposite acquisition order -------

// spec §1.2/1.3: R1 (class-independent) DEADLOCKS on REQ under opposite-order multicast tree
// acquisition exactly as on DAT; this test passes by detecting the no-progress state, not by
// avoiding it.
TEST(SimpleRouterForkWedge, OverlappingTreesOppositeOrderWedgeDetectedWithinBound) {
    // Row y=0 of a 4x2 mesh. M1 from (0,0) and M2 from (3,0) both multicast to
    // the full row above (dst (0,1), mask x=3): fork {E,N} at every
    // source-row router for M1, {W,N} for M2. M1 locks N@R1 then needs N@R2;
    // M2 locks N@R2 then needs N@R1.
    SimpleRouterConfig cfg1;
    cfg1.mesh_x_dim = 4;
    cfg1.mesh_y_dim = 2;
    cfg1.x = 1;
    cfg1.y = 0;
    cfg1.input_fifo_depth = 4;  // ready deasserts at size 3; worm is 8 flits
    SimpleRouterConfig cfg2 = cfg1;
    cfg2.x = 2;
    SimpleRouter r1(cfg1), r2(cfg2);

    r1.set_downstream(E, r2.input(W));
    r2.set_downstream(W, r1.input(E));
    FlitSink n1, w1, n2, e2;
    r1.set_downstream(N, n1);
    r1.set_downstream(W, w1);
    r2.set_downstream(N, n2);
    r2.set_downstream(E, e2);

    constexpr int kWormFlits = 8;  // AW head + 6 beats + tail, > FIFO depth
    const auto m1 = make_mc_worm(make_id(0, 1), make_id(0, 0), make_id(3, 0), /*vc=*/0, kWormFlits);
    const auto m2 = make_mc_worm(make_id(0, 1), make_id(3, 0), make_id(3, 0), /*vc=*/0, kWormFlits);

    // Wedge tick bound — DERIVED, not guessed. While anything still
    // progresses, at least one flit crosses one pipeline stage somewhere.
    // Pre-wedge work is bounded by: 2 worms x kWormFlits flits x 2 routers x
    // (kPipelineDepth + 1) stage advances each (the two pipeline stages plus
    // the tick in which a backpressured grant re-tries once ready returns),
    // plus 2 x kWormFlits injection ticks (<= 1 flit/tick/input under the
    // ready-gated feed). Past this bound every reachable advance has happened;
    // anything still moving would disprove the wedge.
    constexpr int kWedgeBound =
        2 * kWormFlits * 2 * (kPipelineDepth + 1) + 2 * kWormFlits;  // = 112
    // A live system shows an observable state change at least once per
    // grant -> downstream admission -> re-grant round trip, < 2 x
    // (kPipelineDepth + 1) ticks.
    constexpr int kQuiescentWindow = 2 * (kPipelineDepth + 1);  // = 6

    auto snapshot = [&]() {
        return std::vector<std::size_t>{
            n1.received.size(),       w1.received.size(),       n2.received.size(),
            e2.received.size(),       r1.fork_done_mask(W, 0),  r1.fork_done_mask(E, 0),
            r2.fork_done_mask(W, 0),  r2.fork_done_mask(E, 0),  r1.input_fifo_size(W, 0),
            r1.input_fifo_size(E, 0), r2.input_fifo_size(W, 0), r2.input_fifo_size(E, 0)};
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
    ASSERT_TRUE(wedged) << "trees did not wedge: the §1.2 deadlock no longer reproduces on REQ "
                           "(re-derive the R1 restriction before celebrating)";
    EXPECT_LE(last_progress, kWedgeBound) << "progress past the derived bound";

    // Attribution (§1.3 detection b): the live introspection masks name the
    // wedge — done frozen short of expected, the missing branch being the
    // output the OTHER worm holds, both link FIFOs backpressured.
    const PortMask kEN = port_bit(RouterPort::EAST) | port_bit(RouterPort::NORTH);
    const PortMask kWN = port_bit(RouterPort::WEST) | port_bit(RouterPort::NORTH);
    EXPECT_EQ(r1.fork_expected_mask(W, 0), kEN);
    EXPECT_EQ(r1.fork_done_mask(W, 0), port_bit(RouterPort::NORTH));
    EXPECT_EQ(r2.fork_expected_mask(W, 0), kEN);
    EXPECT_EQ(r2.fork_done_mask(W, 0), port_bit(RouterPort::EAST));
    EXPECT_EQ(r2.fork_expected_mask(E, 0), kWN);
    EXPECT_EQ(r2.fork_done_mask(E, 0), port_bit(RouterPort::NORTH));
    EXPECT_EQ(r1.fork_expected_mask(E, 0), kWN);
    EXPECT_EQ(r1.fork_done_mask(E, 0), port_bit(RouterPort::WEST));
    // The wait-for cycle: M1 holds N@R1, M2 holds N@R2.
    EXPECT_EQ(r1.wormhole_locked_input(N), std::optional<std::size_t>(W));
    EXPECT_EQ(r2.wormhole_locked_input(N), std::optional<std::size_t>(E));
    // Backpressure on both directions of the contended link.
    EXPECT_FALSE(r2.ready(W, 0));
    EXPECT_FALSE(r1.ready(E, 0));
}

// --- Fault injection --------------------------------------------------------

TEST(SimpleRouterForkDeath, EmptyForkSetOnCollectiveHeadAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            SimpleRouter r(center_cfg());
            // src (0,0), dst (0,0), mask (x=2): members (0,0),(2,0) — the
            // tree never touches (1,1) (x unmatched, not on the source row).
            r.input(W).push_flit(make_mc_flit(make_id(0, 0), make_id(0, 0), make_id(2, 0),
                                              /*vc=*/0, /*flit_tail=*/0, 0));
            r.tick();
            r.tick();
        },
        "empty fork set");
}

// Decision 4: a peripheral is never a collective member. The invariant holds
// three layers away (SamTable::packed() stamps port 0 on every tile,
// collective_coords() has no entry for the peripheral space, and a fork replica
// is copied verbatim), so the router that would misbehave states it nowhere
// else: the fork ignores dst_port_id while the CollectB branch above it routes
// on the field, so a non-zero one forks to LOCAL at every member and then
// ejects its B at a face.
TEST(SimpleRouterForkDeath, CollectiveNamingABoundaryPortAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            SimpleRouter r(center_cfg());
            // Legal geometry (dst (1,1), src (0,1), mask y=2 -> {L,N}); only
            // dst_port_id is corrupt.
            Flit f = make_mc_flit(make_id(1, 1), make_id(0, 1), make_id(0, 2), /*vc=*/0,
                                  /*flit_tail=*/0, 0);
            f.set_header_field("dst_port_id", 1);
            r.input(W).push_flit(f);
            r.tick();
            r.tick();
        },
        "names a boundary port");
}

TEST(SimpleRouterForkDeath, ContinuationBranchSetMismatchAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    // Shrunk continuation set: head {L,E,N} (mask x=2,y=2), beat {L,N}.
    EXPECT_DEATH(
        {
            SimpleRouter r(center_cfg());
            FlitSink local;
            FlitSink north;
            FlitSink east;
            r.set_downstream(L, local);
            r.set_downstream(N, north);
            r.set_downstream(E, east);
            r.input(W).push_flit(make_mc_flit(make_id(1, 1), make_id(0, 1), make_id(2, 2), 0,
                                              /*flit_tail=*/0, 0));
            r.tick();
            r.tick();
            r.tick();
            r.input(W).push_flit(make_mc_flit(make_id(1, 1), make_id(0, 1), make_id(0, 2), 0,
                                              /*flit_tail=*/0, 1));
            r.tick();
            r.tick();
        },
        "branch set diverges");
    // Enlarged continuation set: head {L,N} (mask y=2), beat {L,E,N}.
    EXPECT_DEATH(
        {
            SimpleRouter r(center_cfg());
            FlitSink local;
            FlitSink north;
            FlitSink east;
            r.set_downstream(L, local);
            r.set_downstream(N, north);
            r.set_downstream(E, east);
            r.input(W).push_flit(make_mc_flit(make_id(1, 1), make_id(0, 1), make_id(0, 2), 0,
                                              /*flit_tail=*/0, 0));
            r.tick();
            r.tick();
            r.tick();
            r.input(W).push_flit(make_mc_flit(make_id(1, 1), make_id(0, 1), make_id(2, 2), 0,
                                              /*flit_tail=*/0, 1));
            r.tick();
            r.tick();
        },
        "branch set diverges");
    // ONE-HOT corrupted continuation: head {L,N}, beat mask 0 -> {L}. The
    // latch would still route it, silently skipping the owed NORTH branch.
    EXPECT_DEATH(
        {
            SimpleRouter r(center_cfg());
            FlitSink local;
            FlitSink north;
            r.set_downstream(L, local);
            r.set_downstream(N, north);
            r.input(W).push_flit(make_mc_flit(make_id(1, 1), make_id(0, 1), make_id(0, 2), 0,
                                              /*flit_tail=*/0, 0));
            r.tick();
            r.tick();
            r.tick();
            r.input(W).push_flit(make_mc_flit(make_id(1, 1), make_id(0, 1), /*cmask=*/0, 0,
                                              /*flit_tail=*/0, 1));
            r.tick();
            r.tick();
        },
        "branch set diverges");
}

TEST(SimpleRouterForkDeath, UnicastFrontUnderAMultiHotLatchAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            SimpleRouter r(center_cfg());
            FlitSink local;
            FlitSink north;
            r.set_downstream(L, local);
            r.set_downstream(N, north);
            // Head forks {L,N} (mask y=2) and latches both bits; the beat
            // behind it lost collective_op, so it reads as unicast.
            r.input(W).push_flit(make_mc_flit(make_id(1, 1), make_id(0, 1), make_id(0, 2), 0,
                                              /*flit_tail=*/0, 0));
            r.tick();
            r.input(W).push_flit(make_unicast_flit(make_id(1, 1), make_id(0, 1), 0,
                                                   /*flit_tail=*/0, 1));
            r.tick();
            r.tick();
            r.tick();
        },
        "unicast front under a multi-hot route latch");
}

}  // namespace
