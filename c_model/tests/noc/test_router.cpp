#include "noc/router.hpp"
#include "common/scenario.hpp"
#include <gtest/gtest.h>

using ni::NI_NOC_ROUTER_OUTPUT_FIFO_DEPTH;
using ni::NI_NOC_ROUTER_VC_DEPTH;
using ni::cmodel::noc::route_compute;
using ni::cmodel::noc::Router;
using ni::cmodel::noc::RouterConfig;
using ni::cmodel::noc::RouterPort;

namespace {

// Router zero-load latency: a flit pushed at tick T is delivered at T+3
// (3-stage reverse-order pipeline; pinned by RouterDatapath.ZeroLoadLatencyIsThreeTicks).
constexpr int kPipelineDepth = 3;

RouterConfig center_cfg() {
    RouterConfig cfg;
    cfg.x = 1;
    cfg.y = 1;  // center of default 4x4
    return cfg;
}

uint8_t make_dst(uint8_t x, uint8_t y) {
    return static_cast<uint8_t>((y << ni::width::X_WIDTH) | x);
}

struct FlitSink : ni::cmodel::noc::RouterLink {
    std::vector<ni::cmodel::Flit> received;
    void push_flit(const ni::cmodel::Flit& f) override { received.push_back(f); }
};

struct CreditCounter : ni::cmodel::noc::RouterCreditSink {
    std::vector<uint8_t> pulses;
    void receive_credit(uint8_t vc) override { pulses.push_back(vc); }
};

ni::cmodel::Flit make_flit(uint8_t dst, uint8_t vc, uint64_t last) {
    ni::cmodel::Flit f;
    f.set_header_field("dst_id", dst);
    f.set_header_field("vc_id", vc);
    f.set_header_field("last", last);
    f.set_header_field("route_par", ni::cmodel::route_parity(dst, last));
    return f;
}

TEST(RouterRouteCompute, XyDimensionOrder) {
    SCENARIO("Router RC: XY DOR — X first, then Y, both-equal ejects LOCAL");
    const auto cfg = center_cfg();
    EXPECT_EQ(route_compute(make_dst(3, 1), cfg), RouterPort::EAST);
    EXPECT_EQ(route_compute(make_dst(0, 1), cfg), RouterPort::WEST);
    EXPECT_EQ(route_compute(make_dst(1, 3), cfg), RouterPort::NORTH);
    EXPECT_EQ(route_compute(make_dst(1, 0), cfg), RouterPort::SOUTH);
    EXPECT_EQ(route_compute(make_dst(1, 1), cfg), RouterPort::LOCAL);
    // X precedence: both differ -> X resolved first
    EXPECT_EQ(route_compute(make_dst(3, 3), cfg), RouterPort::EAST);
}

TEST(RouterRouteComputeDeath, DstOutsideMeshAborts) {
    SCENARIO("Router RC: dst outside MESH_X_DIM x MESH_Y_DIM -> assert+abort");
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    const auto cfg = center_cfg();
    EXPECT_DEATH(route_compute(make_dst(5, 1), cfg), "outside mesh");
}

TEST(RouterConstructionDeath, BadParametersAbort) {
    SCENARIO("Router: construction asserts — num_vc bound, nonzero depths");
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    RouterConfig bad_vc = center_cfg();
    bad_vc.num_vc = 9;  // > 8 = 2^VC_ID_WIDTH
    EXPECT_DEATH(Router r(bad_vc), "num_vc");
    RouterConfig bad_depth = center_cfg();
    bad_depth.vc_depth = 0;
    EXPECT_DEATH(Router r(bad_depth), "depth");
}

TEST(RouterDatapath, ZeroLoadLatencyIsThreeTicks) {
    SCENARIO("Router: flit pushed at T reaches downstream.push_flit during tick T+3 (spec §12.5)");
    Router r(center_cfg());
    FlitSink east;
    r.set_downstream(static_cast<std::size_t>(RouterPort::EAST), east);
    auto f = make_flit(make_dst(3, 1), /*vc=*/0, /*last=*/1);
    r.input(static_cast<std::size_t>(RouterPort::WEST)).push_flit(f);  // T
    r.tick();
    EXPECT_TRUE(east.received.empty());  // T+1: stage 1
    r.tick();
    EXPECT_TRUE(east.received.empty());  // T+2: stage 2
    r.tick();
    ASSERT_EQ(east.received.size(), 1u);  // T+3: stage 3
}

TEST(RouterDatapath, HeaderTransparency) {
    SCENARIO("Router: header bits identical at ingress and egress, incl. seq (spec §12.8)");
    Router r(center_cfg());
    FlitSink east;
    r.set_downstream(static_cast<std::size_t>(RouterPort::EAST), east);
    auto f = make_flit(make_dst(3, 1), /*vc=*/0, /*last=*/1);
    f.set_header_field("seq", 21);
    f.set_header_field("noc_qos", 5);
    f.set_header_field("rob_req", 1);
    f.set_header_field("rob_idx", 7);
    f.set_header_field("src_id", make_dst(0, 2));
    r.input(static_cast<std::size_t>(RouterPort::WEST)).push_flit(f);
    r.tick();
    r.tick();
    r.tick();
    ASSERT_EQ(east.received.size(), 1u);
    EXPECT_EQ(east.received[0].raw(), f.raw());  // byte-for-byte, whole flit
}

TEST(RouterDatapath, CreditDecrementAtGrantAndPulseAfterDequeue) {
    SCENARIO(
        "Router: credit-- at output-FIFO admission; upstream pulse 1 cycle after input dequeue");
    Router r(center_cfg());
    FlitSink east;
    CreditCounter west_up;
    r.set_downstream(static_cast<std::size_t>(RouterPort::EAST), east);
    r.set_upstream_credit(static_cast<std::size_t>(RouterPort::WEST), west_up);
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    EXPECT_EQ(r.credit(E, 0), NI_NOC_ROUTER_VC_DEPTH);  // seeded
    r.input(static_cast<std::size_t>(RouterPort::WEST)).push_flit(make_flit(make_dst(3, 1), 0, 1));
    r.tick();  // stage 1
    EXPECT_EQ(r.credit(E, 0), NI_NOC_ROUTER_VC_DEPTH);
    r.tick();  // stage 2: grant
    EXPECT_EQ(r.credit(E, 0), NI_NOC_ROUTER_VC_DEPTH - 1);
    EXPECT_TRUE(west_up.pulses.empty());  // registered
    r.tick();                             // pulse delivered
    ASSERT_EQ(west_up.pulses.size(), 1u);
    EXPECT_EQ(west_up.pulses[0], 0);
    r.receive_credit(E, 0);  // downstream returns
    EXPECT_EQ(r.credit(E, 0), NI_NOC_ROUTER_VC_DEPTH);
}

// --- Wormhole locking helpers (Task 6) ----------------------------------
// A 3-flit packet (head last=0, body last=0, tail last=1) tagged by src_id so
// flits of concurrent packets can be told apart at the sink. dst routes EAST.
struct Packet {
    std::size_t in_port;
    uint8_t src_id;
    int next = 0;  // 0=head, 1=body, 2=tail, 3=done
};

ni::cmodel::Flit make_tagged_flit(uint8_t dst, uint8_t vc, uint64_t last, uint8_t src_id) {
    auto f = make_flit(dst, vc, last);
    f.set_header_field("src_id", src_id);
    return f;
}

// Push the next flit of `pkt` into its input port (one flit/port/tick). No-op
// once the packet (head/body/tail) is already fully drained.
void feed_packet(Router& r, Packet& pkt, uint8_t dst, uint8_t vc) {
    if (pkt.next > 2) return;
    const uint64_t last = (pkt.next == 2) ? 1 : 0;
    r.input(pkt.in_port).push_flit(make_tagged_flit(dst, vc, last, pkt.src_id));
    ++pkt.next;
}

// tick() then, for every flit delivered to `sink` this cycle, return one EAST
// vc0 credit (models the downstream node draining its buffer). Returning credit
// only on actual delivery keeps the counter from overflowing past vc_depth.
void tick_and_return_credit(Router& r, FlitSink& sink, std::size_t out_port) {
    const std::size_t before = sink.received.size();
    r.tick();
    for (std::size_t i = before; i < sink.received.size(); ++i) r.receive_credit(out_port, 0);
}

TEST(RouterWormhole, PacketsDoNotInterleavePerOutputVc) {
    SCENARIO(
        "Router: two inputs, same (output, vc) — flits of packet B never appear "
        "inside packet A (spec §12.2)");
    Router r(center_cfg());
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);  // routes EAST from center
    // 0x10 / 0x20: arbitrary distinct sink labels so each packet's flits can be
    // told apart at the shared EAST sink (carried in the src_id header field).
    Packet a{static_cast<std::size_t>(RouterPort::WEST), /*src_id=*/0x10};
    Packet b{static_cast<std::size_t>(RouterPort::SOUTH), /*src_id=*/0x20};

    // A starts at tick 0, B offset by one tick; both feed one flit/tick. Return
    // EAST credit each tick so a 6-flit stream never stalls on credit. Bound 20 is
    // generous slack (two 3-flit packets serialized through a kPipelineDepth-deep
    // pipe drain in well under 20 ticks); the real settle point is the size()==6
    // assertion below.
    for (int t = 0; t < 20; ++t) {
        feed_packet(r, a, dst, 0);
        if (t >= 1) feed_packet(r, b, dst, 0);
        tick_and_return_credit(r, east, E);  // downstream drains immediately
    }
    ASSERT_EQ(east.received.size(), 6u);

    // Count contiguous src_id runs: exactly two (AAA BBB) means no interleave;
    // >2 runs means packet flits mixed on the shared (EAST, vc0).
    int runs = 1;
    for (std::size_t i = 1; i < east.received.size(); ++i) {
        const uint8_t s = static_cast<uint8_t>(east.received[i].get_header_field("src_id"));
        const uint8_t prev = static_cast<uint8_t>(east.received[i - 1].get_header_field("src_id"));
        if (s != prev) ++runs;
    }
    EXPECT_EQ(runs, 2) << "packet flits interleaved on (EAST, vc0)";
    // And each src contributes exactly 3 flits.
    int count_a = 0, count_b = 0;
    for (const auto& f : east.received) {
        const uint8_t s = static_cast<uint8_t>(f.get_header_field("src_id"));
        if (s == 0x10) ++count_a;
        if (s == 0x20) ++count_b;
    }
    EXPECT_EQ(count_a, 3);
    EXPECT_EQ(count_b, 3);
}

TEST(RouterWormhole, SingleFlitPacketLocksAndReleasesSameCycle) {
    SCENARIO(
        "Router: single-flit packet (last=1 at grant) — next packet from another "
        "input can win the very next arbitration");
    Router r(center_cfg());
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);
    const auto WEST = static_cast<std::size_t>(RouterPort::WEST);
    const auto SOUTH = static_cast<std::size_t>(RouterPort::SOUTH);

    // Tick 0: WEST single-flit (last=1) lands; SOUTH single-flit lands too.
    r.input(WEST).push_flit(make_tagged_flit(dst, 0, /*last=*/1, 0x10));
    r.input(SOUTH).push_flit(make_tagged_flit(dst, 0, /*last=*/1, 0x20));
    // S1 latch (tick 1) -> two back-to-back grants (ticks 2,3) since each
    // single-flit packet releases the lock the cycle it is granted. The later
    // grant (tick 3) lands at the sink kPipelineDepth ticks afterward, so 3 +
    // kPipelineDepth ticks suffice to settle both single-flit packets.
    for (int t = 0; t < 3 + kPipelineDepth; ++t) tick_and_return_credit(r, east, E);
    // Both single-flit packets must have been delivered; the second did not wait
    // for a stale lock to clear.
    ASSERT_EQ(east.received.size(), 2u);
    const uint8_t s0 = static_cast<uint8_t>(east.received[0].get_header_field("src_id"));
    const uint8_t s1 = static_cast<uint8_t>(east.received[1].get_header_field("src_id"));
    EXPECT_NE(s0, s1) << "both inputs granted; no stale single-flit lock";
}

TEST(RouterWormhole, LockedEmptyVcIdlesButDoesNotLoseLock) {
    SCENARIO(
        "Router: locked input VC with empty FIFO idles the (output,vc) arbiter; "
        "competitor cannot steal (spec §5)");
    Router r(center_cfg());
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);
    const auto WEST = static_cast<std::size_t>(RouterPort::WEST);
    const auto SOUTH = static_cast<std::size_t>(RouterPort::SOUTH);

    // SOUTH establishes the lock alone (head, last=0) with no competitor present,
    // then stalls its FIFO empty. WEST (the competitor) then offers a complete
    // 3-flit packet on the same (EAST, vc0). The locked-but-empty SOUTH must idle
    // the arbiter — WEST cannot steal until SOUTH's tail releases the lock.
    Packet west{WEST, 0x10};

    // Tick 0: SOUTH head lands alone -> wins arbitration -> locks (EAST, vc0).
    r.input(SOUTH).push_flit(make_tagged_flit(dst, 0, /*last=*/0, 0x20));
    tick_and_return_credit(r, east, E);

    // SOUTH stalls (FIFO drains to empty). WEST starts pushing its packet, which
    // queues behind the held lock. 4 ticks = 3 WEST flits pushed + 1 settle tick
    // through the kPipelineDepth-deep pipe; long enough for any stolen flit to
    // surface at the sink were the lock not held.
    for (int t = 0; t < 4; ++t) {
        feed_packet(r, west, dst, 0);  // WEST head / body / tail
        tick_and_return_credit(r, east, E);
    }
    // SOUTH owns (EAST, vc0): only its head should have drained so far. No WEST
    // flit may pass while SOUTH holds the lock with an empty FIFO.
    for (const auto& f : east.received) {
        EXPECT_EQ(static_cast<uint8_t>(f.get_header_field("src_id")), 0x20)
            << "WEST stole a locked (EAST, vc0) while SOUTH held the lock";
    }
    ASSERT_EQ(east.received.size(), 1u);  // exactly SOUTH's head delivered

    // SOUTH sends its tail (last=1) to release the lock.
    r.input(SOUTH).push_flit(make_tagged_flit(dst, 0, /*last=*/1, 0x20));
    // Drain SOUTH's tail + WEST's 3 queued flits through the kPipelineDepth-deep
    // pipe. Bound 10 is generous slack; the real settle point is the
    // count_w==3 / count_s==2 assertions below.
    for (int t = 0; t < 10; ++t) {
        tick_and_return_credit(r, east, E);
    }
    // After release, WEST's 3 queued flits drain. SOUTH contributed head+tail = 2.
    int count_w = 0, count_s = 0;
    for (const auto& f : east.received) {
        const uint8_t s = static_cast<uint8_t>(f.get_header_field("src_id"));
        if (s == 0x10) ++count_w;
        if (s == 0x20) ++count_s;
    }
    EXPECT_EQ(count_w, 3);
    EXPECT_EQ(count_s, 2);
    // SOUTH's full packet (head then tail) must precede any WEST flit.
    EXPECT_EQ(static_cast<uint8_t>(east.received[0].get_header_field("src_id")), 0x20);
    EXPECT_EQ(static_cast<uint8_t>(east.received[1].get_header_field("src_id")), 0x20);
}

TEST(RouterWormhole, RrAdvancesPerPacket) {
    SCENARIO(
        "Router: packet-level RR pointer advances on release — alternating grants "
        "under sustained load");
    Router r(center_cfg());
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);
    const auto WEST = static_cast<std::size_t>(RouterPort::WEST);
    const auto SOUTH = static_cast<std::size_t>(RouterPort::SOUTH);

    // Both inputs continuously offer single-flit packets (last=1) to EAST vc0.
    // Keep at most one flit queued per input (refill only when its FIFO is empty)
    // so the input FIFO never overflows; the output grants one input per cycle, so
    // a saturated single-flit stream from both inputs exercises the packet-level
    // RR pointer. Replenish credit per delivered flit so the stream never stalls.
    // The real exit is the size() < 8 guard (collect 8 grants); bound 40 is a
    // generous safety cap (8 grants + kPipelineDepth drain fit well inside it) so
    // a regression that stops granting fails fast instead of spinning forever.
    for (int t = 0; t < 40 && east.received.size() < 8; ++t) {
        if (r.input_fifo_size(WEST, 0) == 0)
            r.input(WEST).push_flit(make_tagged_flit(dst, 0, /*last=*/1, 0x10));
        if (r.input_fifo_size(SOUTH, 0) == 0)
            r.input(SOUTH).push_flit(make_tagged_flit(dst, 0, /*last=*/1, 0x20));
        tick_and_return_credit(r, east, E);
    }
    ASSERT_GE(east.received.size(), 8u);

    // After the first grant, single-flit packets release immediately so the RR
    // pointer advances every grant -> strict alternation between WEST and SOUTH.
    for (std::size_t i = 1; i < 8; ++i) {
        const uint8_t prev = static_cast<uint8_t>(east.received[i - 1].get_header_field("src_id"));
        const uint8_t cur = static_cast<uint8_t>(east.received[i].get_header_field("src_id"));
        EXPECT_NE(cur, prev) << "RR did not alternate at grant " << i;
    }
}

// --- Per-VC independence (Task 7) ---------------------------------------
// All three tests need >=2 VCs; the generated default NI_NOC_NUM_VC is 1, so
// each builds its RouterConfig with num_vc = 2.

RouterConfig two_vc_cfg() {
    RouterConfig cfg = center_cfg();
    cfg.num_vc = 2;
    return cfg;
}

// tick() then return one EAST credit only for flits delivered on `keep_vc`.
// Flits on the other VC drain to the sink but their credit is deliberately
// withheld, so that VC's downstream stays "full" (head-of-line blocked).
void tick_return_credit_for_vc(Router& r, FlitSink& sink, std::size_t out_port, uint8_t keep_vc) {
    const std::size_t before = sink.received.size();
    r.tick();
    for (std::size_t i = before; i < sink.received.size(); ++i) {
        const auto vc = static_cast<uint8_t>(sink.received[i].get_header_field("vc_id"));
        if (vc == keep_vc) r.receive_credit(out_port, vc);
    }
}

TEST(RouterVcArbitration, BlockedVcDoesNotStallOthers) {
    SCENARIO(
        "Router: vc0 head-blocked (credits exhausted, none returned) — vc1 traffic flows "
        "(spec §12.3)");
    Router r(two_vc_cfg());
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto WEST = static_cast<std::size_t>(RouterPort::WEST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);  // routes EAST from center

    // Phase A — exhaust EAST vc0 credit. Feed one vc0 single-flit packet per tick
    // from WEST; each grant decrements credit (4 -> 0) and we NEVER return vc0
    // credit. Stop feeding once vc0 credit is gone and 2 extras sit queued (the
    // input VC FIFO is vc_depth=4 deep, so 2 extras can never overflow it).
    for (int t = 0; t < 30; ++t) {
        // Keep feeding vc0 while credit remains, plus exactly 2 extras afterward.
        const bool credit_left = r.credit(E, 0) > 0;
        const bool want_extra = !credit_left && r.input_fifo_size(WEST, 0) < 2;
        if (credit_left || want_extra)
            r.input(WEST).push_flit(make_tagged_flit(dst, /*vc=*/0, /*last=*/1, 0x10));
        r.tick();  // never return vc0 credit
        if (!credit_left && r.input_fifo_size(WEST, 0) >= 2) break;
    }
    ASSERT_EQ(r.credit(E, 0), 0u) << "vc0 credit not exhausted";
    ASSERT_GE(r.input_fifo_size(WEST, 0), 1u) << "no vc0 backlog left head-blocked";
    const std::size_t vc0_blocked = r.input_fifo_size(WEST, 0);
    const std::size_t sink_after_vc0 = east.received.size();

    // Phase B — feed vc1 single-flit packets from WEST (one/tick) and return vc1
    // credit on delivery. vc1 has full, fresh credit, so it must flow despite vc0
    // being permanently blocked on the same input/output ports.
    int vc1_received = 0;
    for (int t = 0; t < 12 && vc1_received < 4; ++t) {
        if (r.input_fifo_size(WEST, 1) == 0)
            r.input(WEST).push_flit(make_tagged_flit(dst, /*vc=*/1, /*last=*/1, 0x20));
        const std::size_t before = east.received.size();
        tick_return_credit_for_vc(r, east, E, /*keep_vc=*/1);
        for (std::size_t i = before; i < east.received.size(); ++i)
            if (static_cast<uint8_t>(east.received[i].get_header_field("vc_id")) == 1)
                ++vc1_received;
    }

    EXPECT_GE(vc1_received, 4) << "vc1 starved by head-blocked vc0";
    // vc0's backlog must still be stuck in the input FIFO: its credit never
    // returned, so no further vc0 flit could have been granted.
    EXPECT_EQ(r.credit(E, 0), 0u);
    EXPECT_EQ(r.input_fifo_size(WEST, 0), vc0_blocked) << "blocked vc0 drained unexpectedly";
    // Every flit delivered during phase B was vc1 (vc0 could not advance).
    for (std::size_t i = sink_after_vc0; i < east.received.size(); ++i)
        EXPECT_EQ(static_cast<uint8_t>(east.received[i].get_header_field("vc_id")), 1)
            << "a blocked-vc0 flit slipped through";
}

TEST(RouterVcArbitration, FlitLevelRrAcrossVcs) {
    SCENARIO("Router: per-output flit-level RR across VCs under sustained two-VC load");
    Router r(two_vc_cfg());
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto WEST = static_cast<std::size_t>(RouterPort::WEST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);

    // One flit/input-port/tick (landing register), so "sustained two-VC load from
    // the same input" is fed by alternating vc0 / vc1 on successive ticks. Both
    // VCs keep a flit queued; the output grants one flit/cycle and round-robins
    // across VCs, so delivered vc_ids must alternate. Ample credit (returned per
    // delivery) keeps neither VC credit-starved.
    uint8_t feed_vc = 0;
    for (int t = 0; t < 40 && east.received.size() < 8; ++t) {
        if (r.input_fifo_size(WEST, feed_vc) == 0)
            r.input(WEST).push_flit(make_flit(dst, feed_vc, /*last=*/1));
        feed_vc ^= 1;  // alternate the input we top up each tick
        const std::size_t before = east.received.size();
        r.tick();
        for (std::size_t i = before; i < east.received.size(); ++i) {
            const auto vc = static_cast<uint8_t>(east.received[i].get_header_field("vc_id"));
            r.receive_credit(E, vc);
        }
    }
    ASSERT_GE(east.received.size(), 8u);

    // Per-output VC RR: consecutive grants must alternate vc0 / vc1.
    for (std::size_t i = 1; i < 8; ++i) {
        const auto prev = static_cast<uint8_t>(east.received[i - 1].get_header_field("vc_id"));
        const auto cur = static_cast<uint8_t>(east.received[i].get_header_field("vc_id"));
        EXPECT_NE(cur, prev) << "VC RR did not alternate at grant " << i;
    }
}

TEST(RouterVcArbitration, SameCycleOutputFifoEnqueueDequeue) {
    SCENARIO(
        "Router: full output FIFO frees one slot at stage 3 and accepts a new grant the same "
        "tick (spec §5)");
    Router r(two_vc_cfg());
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto WEST = static_cast<std::size_t>(RouterPort::WEST);
    const uint8_t dst = make_dst(3, 1);

    // Phase A — fill the EAST output FIFO to its depth (NI_NOC_ROUTER_OUTPUT_FIFO_DEPTH)
    // WITHOUT a downstream attached, so stage 3 cannot drain it. Keep a backlog queued
    // in the input FIFO so a grant is available on every later tick. Feed one vc0 flit/tick.
    for (int t = 0; t < 12; ++t) {
        if (r.input_fifo_size(WEST, 0) < NI_NOC_ROUTER_OUTPUT_FIFO_DEPTH + 1)
            r.input(WEST).push_flit(make_flit(dst, /*vc=*/0, /*last=*/1));
        r.tick();
        if (r.output_fifo_size(E) >= NI_NOC_ROUTER_OUTPUT_FIFO_DEPTH &&
            r.input_fifo_size(WEST, 0) >= 1)
            break;
    }
    ASSERT_EQ(r.output_fifo_size(E), static_cast<std::size_t>(NI_NOC_ROUTER_OUTPUT_FIFO_DEPTH))
        << "output FIFO not filled to depth";
    ASSERT_GE(r.input_fifo_size(WEST, 0), 1u) << "no input backlog to supply a same-tick grant";

    // Attach the downstream now: stage 3 can drain one flit this tick, and stage 2
    // (running after stage 3 in the same tick) can grant one from the input
    // backlog. Net output-FIFO occupancy stays at NI_NOC_ROUTER_OUTPUT_FIFO_DEPTH;
    // the sink gains exactly one.
    r.set_downstream(E, east);
    ASSERT_TRUE(east.received.empty());
    const std::size_t backlog_before = r.input_fifo_size(WEST, 0);
    r.tick();
    EXPECT_EQ(r.output_fifo_size(E), static_cast<std::size_t>(NI_NOC_ROUTER_OUTPUT_FIFO_DEPTH))
        << "deq+enq in the same tick did not hold occupancy at the FIFO depth";
    EXPECT_EQ(east.received.size(), 1u) << "stage 3 did not drain one flit";
    EXPECT_EQ(r.input_fifo_size(WEST, 0), backlog_before - 1)
        << "stage 2 did not grant one from the backlog the same tick";
}

// --- Credit conservation + error behaviors + route_par fault injection (Task 8) ---

TEST(RouterCredit, ConservationAcrossChainedRouters) {
    SCENARIO(
        "Router: credit + in-flight + downstream occupancy == depth, every tick (spec §6/§12.1)");
    // Chain: A(1,1) EAST -> B(2,1) WEST; B ejects LOCAL into a sink.
    RouterConfig acfg = center_cfg();
    RouterConfig bcfg = center_cfg();
    bcfg.x = 2;  // B is east of A
    Router a(acfg), b(bcfg);

    // B's WEST upstream-credit pulse returns one (A.EAST, vc0) credit to A. This
    // fires (registered, one tick late) when B GRANTS a flit out of its WEST/vc0
    // input FIFO — i.e. the credit comes home only once B has consumed the flit.
    struct CreditRelay : ni::cmodel::noc::RouterCreditSink {
        Router* target;
        std::size_t port;
        void receive_credit(uint8_t vc) override { target->receive_credit(port, vc); }
    } relay;
    relay.target = &a;
    relay.port = static_cast<std::size_t>(RouterPort::EAST);

    // A's stage-3 push lands in B's WEST landing register; it becomes FIFO-visible
    // only after B's next stage-1. `wire_inflight` counts flits A has pushed but B
    // has not yet absorbed into its WEST FIFO. We increment here and decrement once
    // per b.tick() (stage 1 always drains a present landing). Since A pushes <=1
    // flit/tick on EAST and B absorbs its landing every tick, wire_inflight is 0 or
    // 1 at any post-tick sampling point.
    struct CountingLink : ni::cmodel::noc::RouterLink {
        Router* target;
        std::size_t port;
        std::size_t in_flight = 0;
        void push_flit(const ni::cmodel::Flit& f) override {
            target->input(port).push_flit(f);
            ++in_flight;
        }
    } a_to_b;
    a_to_b.target = &b;
    a_to_b.port = static_cast<std::size_t>(RouterPort::WEST);

    const std::size_t E = static_cast<std::size_t>(RouterPort::EAST);
    const std::size_t W = static_cast<std::size_t>(RouterPort::WEST);
    const std::size_t L = static_cast<std::size_t>(RouterPort::LOCAL);
    a.set_downstream(E, a_to_b);
    b.set_upstream_credit(W, relay);

    // B's LOCAL ejection node: record the flit AND immediately return one B.LOCAL
    // vc0 credit, modelling the destination draining its buffer each cycle. Without
    // this, B's LOCAL credit (seeded at DEPTH) is never replenished and B stalls
    // after DEPTH ejections — which would starve A's returning credit too. This
    // ejection sink is outside the (A.EAST, vc0) conservation domain under test.
    struct EjectSink : ni::cmodel::noc::RouterLink {
        std::vector<ni::cmodel::Flit> received;
        Router* router;
        std::size_t port;
        void push_flit(const ni::cmodel::Flit& f) override {
            received.push_back(f);
            router->receive_credit(port, 0);
        }
    } local_sink;
    local_sink.router = &b;
    local_sink.port = L;
    b.set_downstream(L, local_sink);

    const uint8_t dst_b_local = make_dst(2, 1);  // routes EAST at A, LOCAL at B
    constexpr int kPackets = 20;
    int injected = 0;

    // Conservation domain for (A.EAST, vc0): a credit leaves A at its grant and
    // returns only after B grants the flit onward. The full accounting of slots
    // that the DEPTH credits map to, at any sampling instant, is:
    //   credit(A.E,0)                         available
    // + a.output_fifo_size(A.EAST)            granted, awaiting A's stage-3 push
    // + wire_inflight                         pushed by A, not yet in B's WEST FIFO
    // + b.input_fifo_size(B.WEST,0)           buffered at B, awaiting B's grant
    // + in-flight B->A return pulse           B granted, credit not yet home
    // The last term we cannot read directly, so we assert the SOUND upper bound
    // (everything except the unobservable return-pulse term) <= DEPTH every tick
    // (proves no credit is created), and prove exact restoration + zero loss at
    // quiescence (proves none is destroyed). Those three together = conservation.
    auto occupancy_lower = [&]() -> std::size_t {
        return a.credit(E, 0) + a.output_fifo_size(E) + a_to_b.in_flight + b.input_fifo_size(W, 0);
    };

    // Drive: model the NI-side credit mirror — only push a new packet into A's WEST
    // when A still has EAST/vc0 credit (the sender never overruns the receiver).
    for (int t = 0; t < 200 && (injected < kPackets || a.credit(E, 0) < NI_NOC_ROUTER_VC_DEPTH);
         ++t) {
        if (injected < kPackets && a.credit(E, 0) > 0 && a.input_fifo_size(W, 0) == 0) {
            a.input(W).push_flit(make_flit(dst_b_local, /*vc=*/0, /*last=*/1));
            ++injected;
        }
        a.tick();  // A may push <=1 flit onto the wire (a_to_b.push_flit)
        b.tick();  // B absorbs its WEST landing this tick
        if (a_to_b.in_flight > 0) --a_to_b.in_flight;  // landing consumed by B stage 1

        // No credit created: the observable occupancy never exceeds DEPTH.
        EXPECT_LE(occupancy_lower(), static_cast<std::size_t>(NI_NOC_ROUTER_VC_DEPTH))
            << "credit created at tick " << t;
        EXPECT_LE(a_to_b.in_flight, 1u) << "more than one flit on the wire at tick " << t;
    }

    EXPECT_EQ(injected, kPackets) << "did not inject all packets (credit deadlock?)";
    // At quiescence: every credit restored (none destroyed) and every flit ejected
    // (none lost or duplicated).
    EXPECT_EQ(a.credit(E, 0), static_cast<std::size_t>(NI_NOC_ROUTER_VC_DEPTH))
        << "credit not fully restored at drain";
    EXPECT_EQ(a_to_b.in_flight, 0u);
    EXPECT_EQ(b.input_fifo_size(W, 0), 0u);
    EXPECT_EQ(local_sink.received.size(), static_cast<std::size_t>(kPackets))
        << "flits created or lost in transit";
}

TEST(RouterCreditDeath, OverflowAborts) {
    SCENARIO("Router: spurious credit return beyond depth -> assert+abort (spec §9)");
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    Router r(center_cfg());
    EXPECT_DEATH(
        {
            for (int i = 0; i <= static_cast<int>(NI_NOC_ROUTER_VC_DEPTH); ++i)
                r.receive_credit(static_cast<std::size_t>(RouterPort::EAST), 0);
        },
        "overflow");
}

TEST(RouterRoutePar, FaultInjectionDropsAndCounts) {
    SCENARIO(
        "Router: corrupted route_par -> flit dropped, drop counter ++, credit still returned, "
        "stream continues (spec §12.7, checker-first)");
    RouterConfig cfg = center_cfg();
    cfg.route_par_check = true;
    Router r(cfg);
    FlitSink east;
    CreditCounter west_up;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto WEST = static_cast<std::size_t>(RouterPort::WEST);
    r.set_downstream(E, east);
    r.set_upstream_credit(WEST, west_up);

    // The corrupted flit MUST be a single-flit packet (last=1): a mid-packet tail
    // drop under a wormhole lock is a documented non-recovered case (spec §9).
    auto bad = make_flit(make_dst(3, 1), 0, 1);
    bad.set_header_field("route_par", bad.get_header_field("route_par") ^ 1);  // corrupt
    r.input(WEST).push_flit(bad);
    r.tick();  // stage 1: parity screen drops it, queues a WEST credit pulse
    r.tick();  // registered WEST credit pulse delivered upstream
    r.tick();
    r.tick();
    EXPECT_EQ(r.route_par_drop_count(), 1u);
    EXPECT_TRUE(east.received.empty()) << "dropped flit must not reach the output";
    EXPECT_EQ(west_up.pulses.size(), 1u) << "credit leaked (or double-returned) on drop";

    // Stream continues: a well-formed flit still passes end-to-end.
    r.input(WEST).push_flit(make_flit(make_dst(3, 1), 0, 1));
    for (int t = 0; t < kPipelineDepth; ++t) r.tick();
    EXPECT_EQ(r.route_par_drop_count(), 1u) << "good flit wrongly dropped";
    ASSERT_EQ(east.received.size(), 1u) << "good flit did not pass after a drop";
}

TEST(RouterRoutePar, CleanStreamNotDropped) {
    SCENARIO(
        "Router: route_par check enabled, correct parity — zero drops (checker does not "
        "over-fire)");
    RouterConfig cfg = center_cfg();
    cfg.route_par_check = true;
    Router r(cfg);
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto WEST = static_cast<std::size_t>(RouterPort::WEST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);  // routes EAST from center

    // make_flit stamps correct parity. Feed several well-formed single-flit packets
    // one/tick, returning EAST credit on delivery so the stream never stalls.
    constexpr int kFlits = 6;
    int fed = 0;
    for (int t = 0; t < kFlits + 4 * kPipelineDepth && east.received.size() < kFlits; ++t) {
        if (fed < kFlits && r.input_fifo_size(WEST, 0) == 0) {
            r.input(WEST).push_flit(make_flit(dst, /*vc=*/0, /*last=*/1));
            ++fed;
        }
        tick_and_return_credit(r, east, E);
    }
    EXPECT_EQ(r.route_par_drop_count(), 0u) << "checker over-fired on a clean stream";
    EXPECT_EQ(east.received.size(), static_cast<std::size_t>(kFlits)) << "clean flits lost";
}

TEST(RouterDatapathDeath, BadVcIdAborts) {
    SCENARIO("Router: input flit vc_id >= num_vc -> assert+abort (spec §9)");
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    Router r(center_cfg());
    EXPECT_DEATH(r.input(static_cast<std::size_t>(RouterPort::WEST))
                     .push_flit(make_flit(make_dst(3, 1), 7, 1)),
                 "vc_id");  // default NUM_VC < 8
}

}  // namespace
