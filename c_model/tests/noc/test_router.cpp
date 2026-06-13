#include "noc/router.hpp"
#include "common/scenario.hpp"
#include <gtest/gtest.h>

using ni::NI_NOC_ROUTER_VC_DEPTH;
using ni::cmodel::noc::route_compute;
using ni::cmodel::noc::Router;
using ni::cmodel::noc::RouterConfig;
using ni::cmodel::noc::RouterPort;

namespace {

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

// Push the next flit of `pkt` into its input port (one flit/port/tick). Returns
// false when the packet is already fully drained.
bool feed_packet(Router& r, Packet& pkt, uint8_t dst, uint8_t vc) {
    if (pkt.next > 2) return false;
    const uint64_t last = (pkt.next == 2) ? 1 : 0;
    r.input(pkt.in_port).push_flit(make_tagged_flit(dst, vc, last, pkt.src_id));
    ++pkt.next;
    return true;
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
    Packet a{static_cast<std::size_t>(RouterPort::WEST), /*src_id=*/0x10};
    Packet b{static_cast<std::size_t>(RouterPort::SOUTH), /*src_id=*/0x20};

    // A starts at tick 0, B offset by one tick; both feed one flit/tick. Return
    // EAST credit each tick so a 6-flit stream never stalls on credit.
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
    // single-flit packet releases the lock the cycle it is granted.
    for (int t = 0; t < 6; ++t) tick_and_return_credit(r, east, E);
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
    // queues behind the held lock.
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

TEST(RouterDatapathDeath, BadVcIdAborts) {
    SCENARIO("Router: input flit vc_id >= num_vc -> assert+abort (spec §9)");
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    Router r(center_cfg());
    EXPECT_DEATH(r.input(static_cast<std::size_t>(RouterPort::WEST))
                     .push_flit(make_flit(make_dst(3, 1), 7, 1)),
                 "vc_id");  // default NUM_VC < 8
}

}  // namespace
