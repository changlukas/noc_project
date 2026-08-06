// S4-T4 RSP SimpleRouter CollectB join: the floo_reduction_sync /
// floo_reduction_arbiter pair ported into stage 2 — exclusion from the unicast
// scan, the expected-input set from route_mask_join(), all-heads-match fire,
// whole-flit survivor with SLVERR precedence, and the all-pop handshake.
// Expected-input geometry cells are hand-computed from
// floo_route_xymask.sv:200-237 the same way test_route_mask.cpp verifies them;
// this file tests the ROUTER's use of that set, not the mask math.
#include "axi/types.hpp"
#include "common/scenario.hpp"
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

Flit make_unicast_r(uint8_t dst, uint8_t src, uint64_t tag) {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_DataR);
    f.set_header_field("dst_id", dst);
    f.set_header_field("src_id", src);
    f.set_header_field("vc_id", 0);
    f.set_header_field("flit_tail", 1);
    f.set_header_field("ordering_tag", tag);
    return f;
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

TEST(SimpleRouterJoin, PartialArrivalWaitsForEveryExpectedInput) {
    SCENARIO(
        "floo_reduction_sync.sv:39-45 + stream_join_dynamic: the join fires only when EVERY "
        "expected input holds a head of the same collect. With two of three replicas present "
        "nothing is forwarded and both wait in their input FIFOs (no state, no partial merge); "
        "the third arriving completes the merge into exactly ONE B");
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
    SCENARIO(
        "stream_join_dynamic all-ready (inp_ready_o[i] = oup_valid & oup_ready & sel_i[i]): the "
        "merge consumes every contributing head in the SAME handshake — all three input FIFOs "
        "empty, one B out, and no re-fire on later ticks");
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
    SCENARIO(
        "A one-member expected set is a legitimate join, not a unicast: the geometry produces it "
        "routinely at pass-through hops. CollectB is excluded from the unicast candidate scan "
        "outright, so a size-1 set that fell through to that path would be forwarded by nobody — "
        "a hang. Router (2,1), collector (0,1), member set {(1,1),(3,1)}: expected inputs "
        "{EAST}, merged B leaves WEST");
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

TEST(SimpleRouterJoin, AllOkaySurvivorIsTheLowestIndexInput) {
    SCENARIO(
        "floo_reduction_arbiter.sv:100-106,117 — with no error anywhere the survivor is the "
        "lzc-selected input. common_cells lzc defaults to MODE=1'b0 (trailing zero) and is "
        "instantiated with WIDTH only, so cnt_o is the LOWEST set index: the LOCAL replica "
        "(route index 0) survives, forwarded as a whole flit");
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

TEST(SimpleRouterJoin, SingleSlverrAnywhereBecomesTheSurvivor) {
    SCENARIO(
        "floo_reduction_arbiter.sv:116-131: the expected inputs are scanned in route-index order "
        "and an SLVERR replaces the default survivor wholesale — here the EAST member (index 2) "
        "wins over the lzc-selected LOCAL one");
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
    SCENARIO(
        "Two members report SLVERR (NORTH index 1, EAST index 2). The scan breaks at the FIRST "
        "one, so the survivor is deterministic for a given same-cycle valid set: NORTH");
    SimpleRouter r(center_cfg());
    FlitSink local;
    r.set_downstream(L, local);
    push_collector_set(r, Resp::OKAY, Resp::SLVERR, Resp::SLVERR);
    for (int t = 0; t < 10; ++t) r.tick();
    ASSERT_EQ(local.received.size(), 1u);
    EXPECT_EQ(local.received[0].get_header_field("src_id"), kSrcNorth);
}

TEST(SimpleRouterJoin, DecerrIsNotElevated) {
    SCENARIO(
        "Documented divergence from AXI worst-response (design §3.2 step 4): the RTL scan tests "
        "for RESP_SLVERR only, so a DECERR member does NOT win. With DECERR at EAST and OKAY "
        "elsewhere the merged B is the lzc-selected OKAY one");
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

TEST(SimpleRouterJoin, ReductionWinsPriorityWithoutStealingTheFrozenWinner) {
    SCENARIO(
        "floo_output_arbiter.sv:126-139 (prio stream_arbiter, reduction at index 0): a unicast R "
        "has already frozen the LOCAL output while it is backpressured. When the output accepts "
        "again the merged B goes first, but the R is not stolen — its freeze holds and it grants "
        "on the next tick, so the sink sees B then R");
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

// --- Fault injection --------------------------------------------------------

TEST(SimpleRouterJoinDeath, NonBCollectiveOnRspAborts) {
    SCENARIO(
        "Fault injection for the class guard (design §3.1): CollectB is the (opcode, axi_ch) "
        "PAIR, not the opcode alone. A collective flit on a read channel — the only non-B "
        "channel that can reach an RSP router — is fatal; reads are unicast everywhere");
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

TEST(SimpleRouterJoinDeath, EmptyExpectedInputSetAborts) {
    SCENARIO(
        "Fault injection: a CollectB whose expected-input set at this router is EMPTY. Upstream "
        "stream_join_dynamic simply never fires on an empty sel; here it is fatal, because an "
        "all-satisfied-by-vacuity join would swallow the B and hang the write. Router (1,1), "
        "collector (0,0), member set {(2,2),(3,2)} — the collect tree never touches (1,1)");
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
    SCENARIO(
        "Fault injection: a CollectB arriving on a port the geometry does not expect it from "
        "(echoed mask disagrees with the delivery path). Upstream would forward it and never "
        "consume it — sel_i[i] gates inp_ready_o[i] — so it would re-fire every cycle. Same "
        "size-1 fixture as the forward test, but injected at LOCAL instead of EAST");
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
    SCENARIO(
        "Fault injection for the OUR RULE agreement assert on top of floo_reduction_sync.sv:"
        "41-43 (which enforces only dst_id + collective_mask): replicas of ONE multicast AW "
        "carry the NMU's pre-fanout ordering_tag, the same class, and the same bid. Each "
        "disagreement means two different writes were about to become one B");
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
