// S4-T4 RSP SimpleRouter CollectB join: the floo_reduction_sync /
// floo_reduction_arbiter pair ported into stage 2 — exclusion from the unicast
// scan, the expected-input set from route_mask_join(), all-heads-match fire,
// whole-flit survivor with SLVERR precedence, and the all-pop handshake.
// Expected-input geometry cells are hand-computed from
// floo_route_xymask.sv:200-237 the same way test_route_mask.cpp verifies them;
// this file tests the ROUTER's use of that set, not the mask math.
#include "axi/types.hpp"
#include "router/route_mask.hpp"
#include "router/simple_router.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

using ni::cmodel::Flit;
using ni::cmodel::axi::Resp;
using ni::cmodel::router::RouterPort;
using ni::cmodel::router::SimpleRouter;
using ni::cmodel::router::SimpleRouterConfig;
using ni::cmodel::router::SimpleRouterLink;

namespace {

constexpr auto L = static_cast<std::size_t>(RouterPort::LOCAL);
constexpr auto N = static_cast<std::size_t>(RouterPort::NORTH);
constexpr auto E = static_cast<std::size_t>(RouterPort::EAST);
constexpr auto W = static_cast<std::size_t>(RouterPort::WEST);

uint8_t make_id(uint8_t x, uint8_t y) {
    return static_cast<uint8_t>((y << ni::width::X_WIDTH) | x);
}

SimpleRouterConfig center_cfg() {
    SimpleRouterConfig cfg;
    cfg.x = 1;
    cfg.y = 1;  // center of the default 4x4
    return cfg;
}

struct FlitSink : SimpleRouterLink {
    std::vector<Flit> received;
    bool always_ready = true;
    bool ready(uint8_t /*vc*/) const override { return always_ready; }
    void push_flit(const Flit& f) override { received.push_back(f); }
};

// One member's B replica. dst_id is the collecting node, src_id the member
// that produced it, collective_mask the wildcard over the member set (T5's NSU
// echo stamps all three). Single-flit by construction — flit_tail=1.
Flit make_collect_b(uint8_t dst, uint8_t src, uint8_t cmask, Resp resp, uint64_t bid = 0x5,
                    uint64_t tag = 0x2a, int axi_ch = ni::AXI_CH_DataB) {
    Flit f;
    f.set_header_field("axi_ch", static_cast<uint64_t>(axi_ch));
    f.set_header_field("collective_op", ni::COLLECTIVE_OP_MULTICAST);
    f.set_header_field("collective_mask", cmask);
    f.set_header_field("dst_id", dst);
    f.set_header_field("src_id", src);
    f.set_header_field("vc_id", 0);
    f.set_header_field("flit_tail", 1);
    f.set_header_field("ordering_tag", tag);
    f.set_payload_field("B", "bid", bid);
    f.set_payload_field("B", "bresp", static_cast<uint64_t>(resp));
    return f;
}

Flit make_unicast_r(uint8_t dst, uint8_t src, uint64_t tag, uint64_t flit_tail = 1) {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_DataR);
    f.set_header_field("dst_id", dst);
    f.set_header_field("src_id", src);
    f.set_header_field("vc_id", 0);
    f.set_header_field("flit_tail", flit_tail);
    f.set_header_field("ordering_tag", tag);
    return f;
}

// Multi-beat R burst: a real RSP worm, tags 0..n-1, tail on the last beat.
std::vector<Flit> make_r_burst(uint8_t dst, uint8_t src, int n_beats) {
    std::vector<Flit> worm;
    for (int i = 0; i < n_beats; ++i) {
        worm.push_back(make_unicast_r(dst, src, static_cast<uint64_t>(i),
                                      /*flit_tail=*/i == n_beats - 1 ? 1 : 0));
    }
    return worm;
}

// Ready/valid-compliant upstream: one flit per port per tick, only while ready.
void feed_worm(SimpleRouter& r, const std::vector<Flit>& worm, std::size_t& fed,
               std::size_t in_port) {
    if (fed >= worm.size()) return;
    if (!r.ready(in_port, 0)) return;
    r.input(in_port).push_flit(worm[fed]);
    ++fed;
}

// --- Geometry of the fixtures ----------------------------------------------
// COLLECTOR fixture: router (1,1) IS the collecting node. Multicast member set
// = mask (x=2,y=2) around (1,1) -> {1,3}x{1,3}. route_mask_join at (1,1):
// x/y matched -> LOCAL; the collector's own column -> NORTH; the collector's
// own row -> EAST. Expected inputs {LOCAL, NORTH, EAST}; the merged B ejects
// LOCAL. (3,3)'s replica merges into (1,3)'s one hop earlier, so only three
// arrive here.
const uint8_t kCollector = make_id(1, 1);
const uint8_t kSetMask = make_id(2, 2);
const uint8_t kSrcLocal = make_id(1, 1);
const uint8_t kSrcNorth = make_id(1, 3);
const uint8_t kSrcEast = make_id(3, 1);

// Feed the three replicas of the COLLECTOR fixture, one per input port, with
// the given per-member responses.
void push_collector_set(SimpleRouter& r, Resp local, Resp north, Resp east) {
    r.input(L).push_flit(make_collect_b(kCollector, kSrcLocal, kSetMask, local));
    r.input(N).push_flit(make_collect_b(kCollector, kSrcNorth, kSetMask, north));
    r.input(E).push_flit(make_collect_b(kCollector, kSrcEast, kSetMask, east));
}

// --- Fire condition ---------------------------------------------------------

// floo_reduction_sync.sv:39-45 (stream_join_dynamic): the join fires only once every expected input
// holds a head of the same collect; partial arrivals wait with no partial merge.
TEST(SimpleRouterJoin, PartialArrivalWaitsForEveryExpectedInput) {
    SimpleRouter r(center_cfg());
    FlitSink local;
    r.set_downstream(L, local);

    r.input(L).push_flit(make_collect_b(kCollector, kSrcLocal, kSetMask, Resp::OKAY));
    r.input(N).push_flit(make_collect_b(kCollector, kSrcNorth, kSetMask, Resp::OKAY));
    for (int t = 0; t < 10; ++t) r.tick();
    EXPECT_TRUE(local.received.empty()) << "join fired without its third replica";
    EXPECT_EQ(r.input_fifo_size(L, 0), 1u);
    EXPECT_EQ(r.input_fifo_size(N, 0), 1u);

    r.input(E).push_flit(make_collect_b(kCollector, kSrcEast, kSetMask, Resp::OKAY));
    for (int t = 0; t < 10; ++t) r.tick();
    ASSERT_EQ(local.received.size(), 1u) << "the completed collect must merge into exactly one B";
}

TEST(SimpleRouterJoin, GrantConsumesEveryContributorExactlyOnce) {
    SimpleRouter r(center_cfg());
    FlitSink local;
    r.set_downstream(L, local);
    push_collector_set(r, Resp::OKAY, Resp::OKAY, Resp::OKAY);
    for (int t = 0; t < 10; ++t) r.tick();
    EXPECT_EQ(local.received.size(), 1u);
    EXPECT_EQ(r.input_fifo_size(L, 0), 0u);
    EXPECT_EQ(r.input_fifo_size(N, 0), 0u);
    EXPECT_EQ(r.input_fifo_size(E, 0), 0u);
    for (int t = 0; t < 10; ++t) r.tick();
    EXPECT_EQ(local.received.size(), 1u) << "the join re-fired on an already-consumed collect";
}

TEST(SimpleRouterJoin, SizeOneExpectedSetForwardsThroughTheJoinPath) {
    SimpleRouterConfig cfg;
    cfg.x = 2;
    cfg.y = 1;
    SimpleRouter r(cfg);
    FlitSink west;
    r.set_downstream(W, west);
    const auto b = make_collect_b(make_id(0, 1), make_id(3, 1), make_id(2, 0), Resp::OKAY);
    r.input(E).push_flit(b);
    for (int t = 0; t < 10; ++t) r.tick();
    ASSERT_EQ(west.received.size(), 1u) << "size-1 join never forwarded (the hang case)";
    EXPECT_EQ(west.received[0].raw(), b.raw()) << "whole flit must pass through untouched";
    EXPECT_EQ(r.input_fifo_size(E, 0), 0u);
}

// --- Survivor selection -----------------------------------------------------

// floo_reduction_arbiter.sv:100-106,117: with no error, the survivor is the lzc-selected
// (lowest-index) input -- common_cells lzc defaults to trailing-zero mode.
TEST(SimpleRouterJoin, AllOkaySurvivorIsTheLowestIndexInput) {
    SimpleRouter r(center_cfg());
    FlitSink local;
    r.set_downstream(L, local);
    push_collector_set(r, Resp::OKAY, Resp::OKAY, Resp::OKAY);
    for (int t = 0; t < 10; ++t) r.tick();
    ASSERT_EQ(local.received.size(), 1u);
    EXPECT_EQ(local.received[0].get_header_field("src_id"), kSrcLocal);
    EXPECT_EQ(local.received[0].get_payload_field("B", "bresp"), static_cast<uint64_t>(Resp::OKAY));
    // Whole-flit forward: header never rebuilt, so the echoed mask and the
    // member src_id survive verbatim.
    EXPECT_EQ(local.received[0].raw(),
              make_collect_b(kCollector, kSrcLocal, kSetMask, Resp::OKAY).raw());
}

// floo_reduction_arbiter.sv:116-131: expected inputs are scanned in route-index order; an SLVERR
// member replaces the default lzc-selected survivor wholesale.
TEST(SimpleRouterJoin, SingleSlverrAnywhereBecomesTheSurvivor) {
    SimpleRouter r(center_cfg());
    FlitSink local;
    r.set_downstream(L, local);
    push_collector_set(r, Resp::OKAY, Resp::OKAY, Resp::SLVERR);
    for (int t = 0; t < 10; ++t) r.tick();
    ASSERT_EQ(local.received.size(), 1u);
    EXPECT_EQ(local.received[0].get_header_field("src_id"), kSrcEast);
    EXPECT_EQ(local.received[0].get_payload_field("B", "bresp"),
              static_cast<uint64_t>(Resp::SLVERR));
}

TEST(SimpleRouterJoin, FirstSlverrInRouteIndexOrderWins) {
    SimpleRouter r(center_cfg());
    FlitSink local;
    r.set_downstream(L, local);
    push_collector_set(r, Resp::OKAY, Resp::SLVERR, Resp::SLVERR);
    for (int t = 0; t < 10; ++t) r.tick();
    ASSERT_EQ(local.received.size(), 1u);
    EXPECT_EQ(local.received[0].get_header_field("src_id"), kSrcNorth);
}

// design §3.2 step 4: documented divergence from AXI worst-response -- the RTL scan tests SLVERR
// only, so a DECERR member does not win the merge.
TEST(SimpleRouterJoin, DecerrIsNotElevated) {
    SimpleRouter r(center_cfg());
    FlitSink local;
    r.set_downstream(L, local);
    push_collector_set(r, Resp::OKAY, Resp::OKAY, Resp::DECERR);
    for (int t = 0; t < 10; ++t) r.tick();
    ASSERT_EQ(local.received.size(), 1u);
    EXPECT_EQ(local.received[0].get_header_field("src_id"), kSrcLocal);
    EXPECT_EQ(local.received[0].get_payload_field("B", "bresp"), static_cast<uint64_t>(Resp::OKAY));
}

// --- Priority vs the frozen unicast winner ----------------------------------

// floo_output_arbiter.sv:126-139: a unicast R that already froze the output keeps its freeze -- an
// arriving merged B does not steal it.
TEST(SimpleRouterJoin, ReductionWinsPriorityWithoutStealingTheFrozenWinner) {
    SimpleRouter r(center_cfg());
    FlitSink local;
    local.always_ready = false;
    r.set_downstream(L, local);

    r.input(W).push_flit(make_unicast_r(kCollector, make_id(0, 1), 0x11));
    for (int t = 0; t < 4; ++t) r.tick();
    ASSERT_EQ(r.wormhole_locked_input(L), std::optional<std::size_t>(W))
        << "the unicast R did not freeze the output";

    push_collector_set(r, Resp::OKAY, Resp::OKAY, Resp::OKAY);
    for (int t = 0; t < 4; ++t) r.tick();
    ASSERT_TRUE(local.received.empty());
    EXPECT_EQ(r.wormhole_locked_input(L), std::optional<std::size_t>(W));

    local.always_ready = true;
    r.tick();
    ASSERT_EQ(local.received.size(), 1u) << "reduction did not take priority";
    EXPECT_EQ(local.received[0].get_header_field("axi_ch"), ni::AXI_CH_DataB);
    EXPECT_EQ(r.wormhole_locked_input(L), std::optional<std::size_t>(W))
        << "the frozen unicast winner was stolen";
    r.tick();
    ASSERT_EQ(local.received.size(), 2u) << "the frozen R never granted";
    EXPECT_EQ(local.received[1].get_header_field("axi_ch"), ni::AXI_CH_DataR);
}

// --- Worm integrity: the join must not preempt a worm in flight -------------

// Deliberate divergence from floo_output_arbiter.sv:126-139 (which arbitrates per beat): a
// completed join inside an in-progress worm waits for the tail so the merged B never splits an R
// burst.
TEST(SimpleRouterJoin, JoinHoldsWhileItsOutputIsMidWorm) {
    SimpleRouter r(center_cfg());
    FlitSink local;
    r.set_downstream(L, local);

    // 4-beat R burst from WEST, ejecting LOCAL — the same output the collect
    // (expected inputs {LOCAL, NORTH, EAST}) merges to.
    constexpr int kBeats = 4;
    const auto burst = make_r_burst(kCollector, make_id(0, 1), kBeats);
    std::size_t fed = 0;
    bool bs_sent = false;
    for (int t = 0; t < 30; ++t) {
        feed_worm(r, burst, fed, W);
        // Offer the whole collect once the burst is already streaming.
        if (!bs_sent && !local.received.empty()) {
            push_collector_set(r, Resp::OKAY, Resp::OKAY, Resp::OKAY);
            bs_sent = true;
        }
        r.tick();
    }
    ASSERT_TRUE(bs_sent);
    ASSERT_EQ(local.received.size(), static_cast<std::size_t>(kBeats) + 1);
    for (int i = 0; i < kBeats; ++i) {
        EXPECT_EQ(local.received[static_cast<std::size_t>(i)].get_header_field("axi_ch"),
                  ni::AXI_CH_DataR)
            << "the merged B interleaved into the R burst at beat " << i;
        EXPECT_EQ(local.received[static_cast<std::size_t>(i)].get_header_field("ordering_tag"),
                  static_cast<uint64_t>(i));
    }
    EXPECT_EQ(local.received.back().get_header_field("axi_ch"), ni::AXI_CH_DataB)
        << "the merged B did not land after the burst tail";
}

TEST(SimpleRouterJoinChain, MidWormHoldKeepsTheDownstreamLatchIntact) {
    SimpleRouterConfig ca = center_cfg();  // (1,1)
    SimpleRouterConfig cb = center_cfg();
    cb.x = 2;  // (2,1)
    SimpleRouter ra(ca), rb(cb);
    ra.set_downstream(E, rb.input(W));
    FlitSink far_sink;
    rb.set_downstream(E, far_sink);

    constexpr int kBeats = 4;
    const auto burst = make_r_burst(make_id(3, 1), make_id(0, 1), kBeats);
    // Collector (2,1), members {(1,1),(1,3)} (mask y=2): expected inputs at
    // (1,1) are {LOCAL} — a size-1 join — and the merged B routes EAST, the
    // burst's output. At (2,1) the collect expects {NORTH, WEST}.
    const auto b = make_collect_b(make_id(2, 1), make_id(1, 1), make_id(0, 2), Resp::OKAY);
    std::size_t fed = 0;
    bool b_sent = false;
    for (int t = 0; t < 40; ++t) {
        feed_worm(ra, burst, fed, W);
        if (!b_sent && !far_sink.received.empty()) {
            ra.input(L).push_flit(b);
            b_sent = true;
        }
        ra.tick();
        rb.tick();
    }
    ASSERT_TRUE(b_sent);
    ASSERT_EQ(far_sink.received.size(), static_cast<std::size_t>(kBeats))
        << "the burst did not cross the chain intact";
    for (int i = 0; i < kBeats; ++i) {
        EXPECT_EQ(far_sink.received[static_cast<std::size_t>(i)].get_header_field("ordering_tag"),
                  static_cast<uint64_t>(i))
            << "burst split at beat " << i;
    }
    EXPECT_EQ(rb.input_fifo_size(W, 0), 1u)
        << "the merged B should be queued at (2,1) awaiting its NORTH sibling";
    EXPECT_EQ(rb.route_locked(W, 0), 0u) << "downstream route latch left held";
}

// --- Fault injection --------------------------------------------------------

// design §3.1: CollectB is the (opcode, axi_ch) pair, not the opcode alone -- a collective flit on
// a read channel is fatal since reads are unicast everywhere.
TEST(SimpleRouterJoinDeath, NonBCollectiveOnRspAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            SimpleRouter r(center_cfg());
            Flit f = make_unicast_r(kCollector, make_id(0, 1), 0x11);
            f.set_header_field("collective_op", ni::COLLECTIVE_OP_MULTICAST);
            f.set_header_field("collective_mask", kSetMask);
            r.input(W).push_flit(f);
            r.tick();
            r.tick();
        },
        "non-B collective flit");
}

// spec §6 :356: collective_op codes 2-3 are reserved; classification keys on `!= UNICAST`, so an
// unrejected reserved code would silently become a CollectB or fork.
TEST(SimpleRouterJoinDeath, ReservedCollectiveOpCodeAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            SimpleRouter r(center_cfg());
            FlitSink local;
            r.set_downstream(L, local);
            Flit f = make_collect_b(kCollector, kSrcLocal, kSetMask, Resp::OKAY);
            f.set_header_field("collective_op", 2);
            r.input(N).push_flit(f);
            r.tick();
            r.tick();
        },
        "reserved collective_op code");
}

TEST(SimpleRouterJoinDeath, EmptyExpectedInputSetAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            SimpleRouter r(center_cfg());
            FlitSink west;
            r.set_downstream(W, west);
            r.input(E).push_flit(
                make_collect_b(make_id(0, 0), make_id(2, 2), make_id(1, 0), Resp::OKAY));
            r.tick();
            r.tick();
        },
        "empty expected-input set");
}

TEST(SimpleRouterJoinDeath, CollectBOffItsOwnExpectedInputSetAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            SimpleRouterConfig cfg;
            cfg.x = 2;
            cfg.y = 1;
            SimpleRouter r(cfg);
            FlitSink west;
            r.set_downstream(W, west);
            r.input(L).push_flit(
                make_collect_b(make_id(0, 1), make_id(3, 1), make_id(2, 0), Resp::OKAY));
            r.tick();
            r.tick();
        },
        "outside its own expected-input set");
}

TEST(SimpleRouterJoinDeath, JoinedReplicasDisagreeAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    // ordering_tag mismatch
    EXPECT_DEATH(
        {
            SimpleRouter r(center_cfg());
            FlitSink local;
            r.set_downstream(L, local);
            r.input(L).push_flit(make_collect_b(kCollector, kSrcLocal, kSetMask, Resp::OKAY));
            r.input(N).push_flit(make_collect_b(kCollector, kSrcNorth, kSetMask, Resp::OKAY,
                                                /*bid=*/0x5, /*tag=*/0x2b));
            r.input(E).push_flit(make_collect_b(kCollector, kSrcEast, kSetMask, Resp::OKAY));
            r.tick();
            r.tick();
        },
        "replicas disagree on");
    // axi_ch (class) mismatch
    EXPECT_DEATH(
        {
            SimpleRouter r(center_cfg());
            FlitSink local;
            r.set_downstream(L, local);
            r.input(L).push_flit(make_collect_b(kCollector, kSrcLocal, kSetMask, Resp::OKAY));
            r.input(N).push_flit(make_collect_b(kCollector, kSrcNorth, kSetMask, Resp::OKAY,
                                                /*bid=*/0x5, /*tag=*/0x2a, ni::AXI_CH_NarrowB));
            r.input(E).push_flit(make_collect_b(kCollector, kSrcEast, kSetMask, Resp::OKAY));
            r.tick();
            r.tick();
        },
        "replicas disagree on");
    // bid mismatch
    EXPECT_DEATH(
        {
            SimpleRouter r(center_cfg());
            FlitSink local;
            r.set_downstream(L, local);
            r.input(L).push_flit(make_collect_b(kCollector, kSrcLocal, kSetMask, Resp::OKAY));
            r.input(N).push_flit(
                make_collect_b(kCollector, kSrcNorth, kSetMask, Resp::OKAY, /*bid=*/0x6));
            r.input(E).push_flit(make_collect_b(kCollector, kSrcEast, kSetMask, Resp::OKAY));
            r.tick();
            r.tick();
        },
        "replicas disagree on");
}

}  // namespace
