#include "router/router.hpp"
#include "router/two_node_fabric.hpp"
#include "common/flit_builders.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <ios>
#include <tuple>
#include <vector>

using ni::NOC_ROUTER_OUTPUT_FIFO_DEPTH;
using ni::NOC_ROUTER_VC_DEPTH;
using ni::cmodel::router::route_compute;
using ni::cmodel::router::Router;
using ni::cmodel::router::RouterConfig;
using ni::cmodel::router::RouterPort;

namespace {

// Router zero-load latency for a HEAD flit (a single-flit packet is one): a
// flit pushed at tick T is delivered at T+4 (4-stage reverse-order pipeline;
// verified by RouterDatapath.ZeroLoadLatencyIsFourTicks). A body or tail flit
// skips RC+VA and follows its head one cycle apart
// (RouterDatapath.BodyFlitsFollowHeadOneCycleApart).
constexpr int kPipelineDepth = 4;

RouterConfig center_cfg() {
    RouterConfig cfg;
    cfg.x = 1;
    cfg.y = 1;  // center of default 4x4
    // Most legacy datapath cases below isolate vc0. Tests that exercise
    // multi-VC arbitration override this explicitly through two_vc_cfg() or
    // their parameterized RouterConfig.
    cfg.num_vc = 1;
    return cfg;
}

// dst_port_id naming the tile on the router's LOCAL port, for the routing
// cases below that have no peripheral destination.
constexpr uint8_t kTilePort = 0;

uint8_t make_dst(uint8_t x, uint8_t y) {
    return static_cast<uint8_t>((y << ni::width::X_WIDTH) | x);
}

struct FlitSink : ni::cmodel::router::RouterLink {
    std::vector<ni::cmodel::Flit> received;
    void push_flit(const ni::cmodel::Flit& f) override { received.push_back(f); }
};

struct CreditCounter : ni::cmodel::router::RouterCreditSink {
    std::vector<uint8_t> pulses;
    void receive_credit(uint8_t vc) override { pulses.push_back(vc); }
};

using ni::cmodel::testing::make_flit;

// The XY walk itself: dst_port_id 0 throughout, the tile on LOCAL.
TEST(RouterRouteCompute, XyDimensionOrder) {
    const auto cfg = center_cfg();
    EXPECT_EQ(route_compute(make_dst(3, 1), kTilePort, cfg), RouterPort::EAST);
    EXPECT_EQ(route_compute(make_dst(0, 1), kTilePort, cfg), RouterPort::WEST);
    EXPECT_EQ(route_compute(make_dst(1, 3), kTilePort, cfg), RouterPort::NORTH);
    EXPECT_EQ(route_compute(make_dst(1, 0), kTilePort, cfg), RouterPort::SOUTH);
    EXPECT_EQ(route_compute(make_dst(1, 1), kTilePort, cfg), RouterPort::LOCAL);
    // X precedence: both differ -> X resolved first
    EXPECT_EQ(route_compute(make_dst(3, 3), kTilePort, cfg), RouterPort::EAST);
}

TEST(RouterRouteComputeDeath, DstOutsideMeshAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    const auto cfg = center_cfg();
    EXPECT_DEATH(route_compute(make_dst(5, 1), kTilePort, cfg), "outside mesh");
}

TEST(RouterCreditSeed, LocalUsesNiDepthAndCompassPortsUseRouterDepth) {
    RouterConfig cfg = center_cfg();
    cfg.vc_depth = 8;
    cfg.local_vc_depth = 2;
    Router r(cfg);

    for (uint8_t vc = 0; vc < cfg.num_vc; ++vc) {
        EXPECT_EQ(r.credit(static_cast<std::size_t>(RouterPort::LOCAL), vc), 2u);
        for (const auto port : {RouterPort::NORTH, RouterPort::EAST, RouterPort::SOUTH,
                                RouterPort::WEST}) {
            EXPECT_EQ(r.credit(static_cast<std::size_t>(port), vc), 8u);
        }
    }
}

TEST(RouteCompute, PortZeroEjectsLocalAtTheDestinationCoordinate) {
    RouterConfig cfg{};
    cfg.x = 1;
    cfg.y = 1;
    cfg.mesh_x_dim = 2;
    cfg.mesh_y_dim = 2;
    EXPECT_EQ(route_compute(/*dst_id=*/0x11, /*dst_port_id=*/0, cfg), RouterPort::LOCAL);
}

TEST(RouteCompute, XFaceResolvesByTheRoutersOwnEdge) {
    RouterConfig cfg{};
    cfg.mesh_x_dim = 2;
    cfg.mesh_y_dim = 2;
    cfg.x = 0;
    cfg.y = 0;
    EXPECT_EQ(route_compute(0x00, /*dst_port_id=*/1, cfg), RouterPort::WEST);
    cfg.x = 1;
    EXPECT_EQ(route_compute(0x01, /*dst_port_id=*/1, cfg), RouterPort::EAST);
}

TEST(RouteCompute, YFaceResolvesByTheRoutersOwnEdge) {
    RouterConfig cfg{};
    cfg.mesh_x_dim = 2;
    cfg.mesh_y_dim = 2;
    cfg.x = 0;
    cfg.y = 0;
    EXPECT_EQ(route_compute(0x00, /*dst_port_id=*/2, cfg), RouterPort::SOUTH);
    cfg.y = 1;
    EXPECT_EQ(route_compute(0x10, /*dst_port_id=*/2, cfg), RouterPort::NORTH);
}

TEST(RouteComputeDeath, AnInteriorRouterHasNoFace) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    RouterConfig cfg{};
    cfg.x = 1;
    cfg.y = 1;
    cfg.mesh_x_dim = 4;
    cfg.mesh_y_dim = 4;
    EXPECT_DEATH(route_compute(0x11, /*dst_port_id=*/1, cfg), "no x face");
}

TEST(RouteComputeDeath, AnInteriorRouterHasNoYFace) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    RouterConfig cfg{};
    cfg.x = 1;
    cfg.y = 1;
    cfg.mesh_x_dim = 4;
    cfg.mesh_y_dim = 4;
    EXPECT_DEATH(route_compute(0x11, /*dst_port_id=*/2, cfg), "no y face");
}

TEST(RouteComputeDeath, TheReservedPortEncodingAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    RouterConfig cfg{};
    cfg.x = 0;
    cfg.y = 0;
    cfg.mesh_x_dim = 2;
    cfg.mesh_y_dim = 2;
    EXPECT_DEATH(route_compute(0x00, /*dst_port_id=*/3, cfg), "reserved");
}

// An ejection has no next hop, so a flit leaving by a boundary face takes VC 0
// exactly as a LOCAL ejection does (floo_vc_assignment.sv:86).
//
// Without that guard the fallback is not merely different, it is nonsense:
// next_hop_route steps x == 0 west to 255, route_compute reads that as a WEST
// next hop, and preferred_vc(WEST, WEST) yields 2 -- a lookahead value the RTL
// never stores, which is a co-sim mismatch rather than a performance wobble.
// num_vc = 4 keeps 0 and 2 distinct; at num_vc = 1 (every chain fixture) the
// two collapse and the guard is unobservable. fixed_vc = 0 so VA actually runs
// instead of taking the NI-pinned bypass.
TEST(RouterEjectionVc, BoundaryFaceEjectionTakesVcZero) {
    RouterConfig cfg;
    cfg.x = 0;  // west edge, so dst_port_id 1 resolves to WEST
    cfg.y = 0;
    cfg.mesh_x_dim = 2;
    cfg.mesh_y_dim = 2;
    cfg.num_vc = 4;
    Router r(cfg);
    FlitSink west;
    r.set_downstream(static_cast<std::size_t>(RouterPort::WEST), west);

    auto f = make_flit(make_dst(0, 0), /*vc=*/0, /*flit_tail=*/1);
    f.set_header_field("dst_port_id", 1);  // the peripheral on this router's x face
    f.set_header_field("fixed_vc", 0);
    r.input(static_cast<std::size_t>(RouterPort::LOCAL)).push_flit(f);
    for (int t = 0; t < kPipelineDepth; ++t) r.tick();

    ASSERT_EQ(west.received.size(), 1u) << "peripheral-bound flit never ejected to the x face";
    EXPECT_EQ(west.received[0].get_header_field("vc_id"), 0u)
        << "boundary-face ejection took a VC derived from a next hop that does not exist";
}

TEST(RouterConstructionDeath, BadParametersAbort) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    RouterConfig bad_vc = center_cfg();
    bad_vc.num_vc = 9;  // > 8 = 2^VC_ID_WIDTH
    EXPECT_DEATH(Router r(bad_vc), "num_vc");
    RouterConfig bad_depth = center_cfg();
    bad_depth.vc_depth = 0;
    EXPECT_DEATH(Router r(bad_depth), "depth");
    RouterConfig non_power_of_two_vc_depth = center_cfg();
    non_power_of_two_vc_depth.vc_depth = 3;
    EXPECT_DEATH(Router r(non_power_of_two_vc_depth), "power of two");
    RouterConfig non_power_of_two_local_vc_depth = center_cfg();
    non_power_of_two_local_vc_depth.local_vc_depth = 3;
    EXPECT_DEATH(Router r(non_power_of_two_local_vc_depth), "local_vc_depth");
    RouterConfig non_power_of_two_output_depth = center_cfg();
    non_power_of_two_output_depth.output_fifo_depth = 3;
    EXPECT_DEATH(Router r(non_power_of_two_output_depth), "power of two");
}

TEST(RouterConstruction, DefaultDatVcCountMatchesGeneratedContract) {
    RouterConfig cfg{};
    EXPECT_EQ(cfg.num_vc, ni::NOC_DAT_NUM_VC);
    EXPECT_EQ(cfg.num_vc, 2u);
}

TEST(RouterDatapath, ZeroLoadLatencyIsFourTicks) {
    Router r(center_cfg());
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    r.set_downstream(E, east);
    r.input(W).push_flit(make_flit(make_dst(3, 1), 0, /*flit_tail=*/1));
    for (int t = 0; t < 3; ++t) {  // T+1 BW, T+2 RC+VA, T+3 SA+ST
        r.tick();
        EXPECT_TRUE(east.received.empty()) << "tick " << t;
    }
    r.tick();  // T+4 LT
    ASSERT_EQ(east.received.size(), 1u);
}

// Head pays VA, body and tail do not: a 3-flit worm delivers at ticks 4, 5, 6.
TEST(RouterDatapath, BodyFlitsFollowHeadOneCycleApart) {
    Router r(center_cfg());
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);
    std::vector<std::size_t> arrivals;
    for (int t = 1; t <= 8; ++t) {
        if (t <= 3) r.input(W).push_flit(make_flit(dst, 0, t == 3 ? 1 : 0));
        const std::size_t before = east.received.size();
        r.tick();
        if (east.received.size() > before) arrivals.push_back(static_cast<std::size_t>(t));
    }
    EXPECT_EQ(arrivals, (std::vector<std::size_t>{4, 5, 6}));
}

// A tail frees its VC in SA and the next head takes it in VA of the same
// cycle: eight single-flit packets on one input VC arrive one per cycle.
TEST(RouterDatapath, BackToBackSingleFlitsOnePerCycle) {
    Router r(center_cfg());
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);
    std::vector<std::size_t> arrivals;
    for (int t = 1; t <= 14; ++t) {
        if (t <= 8) r.input(W).push_flit(make_flit(dst, 0, 1));
        const std::size_t before = east.received.size();
        r.tick();
        for (std::size_t i = before; i < east.received.size(); ++i) {
            r.receive_credit(E, static_cast<uint8_t>(east.received[i].get_header_field("vc_id")));
            arrivals.push_back(static_cast<std::size_t>(t));
        }
    }
    ASSERT_EQ(arrivals.size(), 8u);
    for (std::size_t i = 1; i < arrivals.size(); ++i) EXPECT_EQ(arrivals[i], arrivals[i - 1] + 1);
}

TEST(RouterDatapath, HeaderTransparency) {
    Router r(center_cfg());
    FlitSink east;
    r.set_downstream(static_cast<std::size_t>(RouterPort::EAST), east);
    auto f = make_flit(make_dst(3, 1), /*vc=*/0, /*flit_tail=*/1);
    f.set_header_field("ordering_req", 1);
    f.set_header_field("ordering_tag", 7);
    f.set_header_field("src_id", make_dst(0, 2));
    r.input(static_cast<std::size_t>(RouterPort::WEST)).push_flit(f);
    for (int t = 0; t < kPipelineDepth; ++t) r.tick();
    ASSERT_EQ(east.received.size(), 1u);
    EXPECT_EQ(east.received[0].raw(), f.raw());  // byte-for-byte, whole flit
}

TEST(RouterDatapath, CreditDecrementAtGrantAndPulseAfterDequeue) {
    Router r(center_cfg());
    FlitSink east;
    CreditCounter west_up;
    r.set_downstream(static_cast<std::size_t>(RouterPort::EAST), east);
    r.set_upstream_credit(static_cast<std::size_t>(RouterPort::WEST), west_up);
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    EXPECT_EQ(r.credit(E, 0), NOC_ROUTER_VC_DEPTH);  // seeded
    r.input(W).push_flit(make_flit(make_dst(3, 1), 0, 1));
    r.tick();  // stage 1 BW
    EXPECT_EQ(r.credit(E, 0), NOC_ROUTER_VC_DEPTH);
    r.tick();  // stage 2 VA: the VC is held, its credit is NOT reserved
    ASSERT_EQ(r.va_out_vc(W, 0), std::optional<uint8_t>(0));
    EXPECT_EQ(r.credit(E, 0), NOC_ROUTER_VC_DEPTH);
    r.tick();  // stage 3 SA: grant
    EXPECT_EQ(r.credit(E, 0), NOC_ROUTER_VC_DEPTH - 1);
    EXPECT_TRUE(west_up.pulses.empty());  // registered
    r.tick();                             // stage 4 LT + pulse delivered
    ASSERT_EQ(west_up.pulses.size(), 1u);
    EXPECT_EQ(west_up.pulses[0], 0);
    r.receive_credit(E, 0);  // downstream returns
    EXPECT_EQ(r.credit(E, 0), NOC_ROUTER_VC_DEPTH);
}

// --- Wormhole locking helpers --------------------------------------------
// A 3-flit packet (head flit_tail=0, body flit_tail=0, tail flit_tail=1) tagged by src_id so
// flits of concurrent packets can be told apart at the sink. dst routes EAST.
struct Packet {
    std::size_t in_port;
    uint8_t src_id;
    int next = 0;  // 0=head, 1=body, 2=tail, 3=done
};

constexpr int kMaxPacketFlits = 3;  // head + body + tail; see feed_packet()

ni::cmodel::Flit make_tagged_flit(uint8_t dst, uint8_t vc, uint64_t flit_tail, uint8_t src_id) {
    auto f = make_flit(dst, vc, flit_tail);
    f.set_header_field("src_id", src_id);
    return f;
}

// fixed_vc=1: the flit's header vc_id is pinned through the VA stage (D8
// bypass) — the output VC always equals the input VC. Used by tests whose
// premise is that 1:1 mapping (per-VC independence, per-VC order); the VA
// restamp path is covered by the RouterVa* tests.
ni::cmodel::Flit make_pinned_flit(uint8_t dst, uint8_t vc, uint64_t flit_tail, uint8_t src_id = 0) {
    auto f = make_tagged_flit(dst, vc, flit_tail, src_id);
    f.set_header_field("fixed_vc", 1);
    return f;
}

// Push the next flit of `pkt` into its input port (one flit/port/tick). No-op
// once the packet (head/body/tail) is already fully drained.
void feed_packet(Router& r, Packet& pkt, uint8_t dst, uint8_t vc) {
    if (pkt.next >= kMaxPacketFlits) return;
    const uint64_t flit_tail = (pkt.next == kMaxPacketFlits - 1) ? 1 : 0;
    r.input(pkt.in_port).push_flit(make_tagged_flit(dst, vc, flit_tail, pkt.src_id));
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

// nmu::VcAllocator hashes every AW of one (dst_id, awid) onto one VC, so the
// same-ID write ordering argument reduces to this: two consecutive pinned worms
// on one VC keep issue order at a shared output while another VC's worms
// contend for it. Worm 2's head must not precede worm 1's tail, and neither
// worm's flits may be split by the other's (the noise VC may interleave, that
// is WormsOnDifferentVcsInterleavePerOutput's property).
TEST(RouterWormhole, SameVcWormsKeepIssueOrderUnderCrossVcContention) {
    RouterConfig cfg = center_cfg();
    cfg.num_vc = 2;
    Router r(cfg);
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    const auto S = static_cast<std::size_t>(RouterPort::SOUTH);
    // The two worms share src_id 0x10 (one write stream); ordering_tag carries
    // the worm number, a header field no fabric stage reads.
    constexpr int kWormFlits = 3, kStreamFlits = 2 * kWormFlits;
    std::vector<uint64_t> stream_tags, stream_tails;
    int vc1_delivered = 0, fed = 0, noise = 0;
    for (int t = 0; t < 200 && stream_tags.size() < static_cast<std::size_t>(kStreamFlits); ++t) {
        if (fed < kStreamFlits && r.input_fifo_size(W, 0) == 0) {
            auto f =
                make_pinned_flit(dst, /*vc=*/0, (fed % kWormFlits == kWormFlits - 1) ? 1 : 0, 0x10);
            f.set_header_field("ordering_tag", fed / kWormFlits + 1);
            r.input(W).push_flit(f);
            ++fed;
        }
        if (r.input_fifo_size(S, 1) == 0) {  // continuous 3-flit worms on vc1
            r.input(S).push_flit(make_pinned_flit(
                dst, /*vc=*/1, (noise % kWormFlits == kWormFlits - 1) ? 1 : 0, 0x20));
            ++noise;
        }
        const std::size_t before = east.received.size();
        r.tick();
        for (std::size_t i = before; i < east.received.size(); ++i) {
            const auto& f = east.received[i];
            const auto vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
            if (vc == 0) {
                stream_tags.push_back(f.get_header_field("ordering_tag"));
                stream_tails.push_back(f.get_header_field("flit_tail"));
            } else {
                ++vc1_delivered;
            }
            r.receive_credit(E, vc);
        }
    }
    EXPECT_GT(vc1_delivered, 0) << "no cross-VC contention generated";
    ASSERT_EQ(stream_tags.size(), static_cast<std::size_t>(kStreamFlits));
    EXPECT_EQ(stream_tags, (std::vector<uint64_t>{1, 1, 1, 2, 2, 2}))
        << "worm 2 flits reached the link before worm 1 finished";
    EXPECT_EQ(stream_tails, (std::vector<uint64_t>{0, 0, 1, 0, 0, 1}));
}

// Two pinned 3-flit worms from different inputs, different VCs, same output:
// both hold a lock at once and the link carries their flits interleaved.
// Each VC's own stream stays contiguous.
TEST(RouterWormhole, WormsOnDifferentVcsInterleavePerOutput) {
    RouterConfig cfg = center_cfg();
    cfg.num_vc = 2;
    Router r(cfg);
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    const auto S = static_cast<std::size_t>(RouterPort::SOUTH);
    int next_a = 0, next_b = 0;
    uint64_t grants = 0, waiting_vc_cycles = 0;
    for (int t = 0; t < 24; ++t) {
        if (next_a < 3) {
            r.input(W).push_flit(make_pinned_flit(dst, 0, next_a == 2 ? 1 : 0, 0x10));
            ++next_a;
        }
        if (next_b < 3) {
            r.input(S).push_flit(make_pinned_flit(dst, 1, next_b == 2 ? 1 : 0, 0x20));
            ++next_b;
        }
        const std::size_t before = east.received.size();
        r.tick();
        const auto& activity = r.switch_activity(E);
        ASSERT_GE(activity.eligible_vcs, activity.grants);
        ASSERT_LE(activity.grants, 1u);
        grants += activity.grants;
        waiting_vc_cycles += activity.eligible_vcs - activity.grants;
        for (std::size_t i = before; i < east.received.size(); ++i)
            r.receive_credit(E, static_cast<uint8_t>(east.received[i].get_header_field("vc_id")));
    }
    ASSERT_EQ(east.received.size(), 6u);
    EXPECT_EQ(grants, 6u);
    EXPECT_GT(waiting_vc_cycles, 0u);
    EXPECT_EQ(r.switch_activity(E).eligible_vcs, 0u);
    int runs = 1;
    for (std::size_t i = 1; i < east.received.size(); ++i) {
        if (east.received[i].get_header_field("src_id") !=
            east.received[i - 1].get_header_field("src_id"))
            ++runs;
    }
    EXPECT_GT(runs, 2) << "worms on different VCs serialized on EAST";
    std::vector<uint64_t> tails0, tails1;
    for (const auto& f : east.received) {
        const auto vc = f.get_header_field("vc_id");
        EXPECT_EQ(f.get_header_field("src_id"), vc == 0 ? 0x10u : 0x20u);
        (vc == 0 ? tails0 : tails1).push_back(f.get_header_field("flit_tail"));
    }
    EXPECT_EQ(tails0, (std::vector<uint64_t>{0, 0, 1}));
    EXPECT_EQ(tails1, (std::vector<uint64_t>{0, 0, 1}));
}

// An unclosed worm on vc0 holds only vc0's lock: a complete worm on vc1 is
// delivered in full.
TEST(RouterWormhole, OpenWormOnVc0DoesNotBlockVc1) {
    RouterConfig cfg = center_cfg();
    cfg.num_vc = 2;
    Router r(cfg);
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    const auto S = static_cast<std::size_t>(RouterPort::SOUTH);
    r.input(W).push_flit(make_pinned_flit(dst, 0, /*flit_tail=*/0, 0x10));  // head, never closed
    int next_b = 0;
    for (int t = 0; t < 24; ++t) {
        if (next_b < 3) {
            r.input(S).push_flit(make_pinned_flit(dst, 1, next_b == 2 ? 1 : 0, 0x20));
            ++next_b;
        }
        const std::size_t before = east.received.size();
        r.tick();
        for (std::size_t i = before; i < east.received.size(); ++i)
            r.receive_credit(E, static_cast<uint8_t>(east.received[i].get_header_field("vc_id")));
    }
    ASSERT_EQ(east.received.size(), 4u);
    EXPECT_EQ(r.wormhole_locked_input(E, 0), std::optional<std::size_t>(W));
    EXPECT_FALSE(r.wormhole_locked_input(E, 1).has_value());
}

// A locked VC with no credit idles only itself (floo_vc_arbiter LockIn=0
// re-arbitrates every cycle among the VCs that can send).
TEST(RouterWormhole, CreditStarvedVcDoesNotIdleTheOutput) {
    RouterConfig cfg = center_cfg();
    cfg.num_vc = 2;
    Router r(cfg);
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    const auto S = static_cast<std::size_t>(RouterPort::SOUTH);
    // vc0: an open worm fed one body flit per tick; its credit is never returned,
    // so after NOC_ROUTER_VC_DEPTH grants vc0 is starved while still locked.
    int next_b = 0;
    for (int t = 0; t < 40; ++t) {
        if (r.input_fifo_size(W, 0) == 0) r.input(W).push_flit(make_pinned_flit(dst, 0, 0, 0x10));
        if (t >= 10 && next_b < 3) {
            r.input(S).push_flit(make_pinned_flit(dst, 1, next_b == 2 ? 1 : 0, 0x20));
            ++next_b;
        }
        const std::size_t before = east.received.size();
        r.tick();
        for (std::size_t i = before; i < east.received.size(); ++i) {
            const auto vc = static_cast<uint8_t>(east.received[i].get_header_field("vc_id"));
            if (vc == 1) r.receive_credit(E, vc);
        }
    }
    int vc0 = 0, vc1 = 0;
    for (const auto& f : east.received) (f.get_header_field("vc_id") == 0 ? vc0 : vc1)++;
    EXPECT_EQ(vc0, static_cast<int>(NOC_ROUTER_VC_DEPTH));
    EXPECT_EQ(vc1, 3);
    EXPECT_EQ(r.wormhole_locked_input(E, 0), std::optional<std::size_t>(W));
}

// A credit-blocked worm's tail must wait for its own VC's credit. A tail is
// the one flit vc_assignment lets overflow to another VC; granting it from
// another VC's slot would leave the worm's lock set forever.
TEST(RouterWormhole, CreditBlockedTailDoesNotOverflowToAnotherVc) {
    RouterConfig cfg = center_cfg();
    cfg.num_vc = 2;
    Router r(cfg);
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    r.set_downstream(E, east);
    // (2,3) routes EAST here and NORTH at the next hop, so the worm's preferred
    // output VC is 0 (floo_vc_assignment.sv:91) and vc1 is the FVADA overflow.
    const uint8_t dst = make_dst(2, 3);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    // fixed_vc=0 worm: head + NOC_ROUTER_VC_DEPTH-1 bodies use up vc0's credit,
    // then the tail arrives with credit_[E][0] == 0 and credit left on vc1.
    const int bodies = static_cast<int>(NOC_ROUTER_VC_DEPTH) - 1;
    int fed = 0;
    for (int t = 0; t < 40; ++t) {
        if (r.input_fifo_size(W, 0) == 0 && fed <= bodies + 1) {
            const uint64_t tail = (fed == bodies + 1) ? 1 : 0;
            r.input(W).push_flit(make_tagged_flit(dst, 0, tail, 0x10));
            ++fed;
        }
        r.tick();  // no credit ever returned on vc0
    }
    EXPECT_EQ(east.received.size(), static_cast<std::size_t>(NOC_ROUTER_VC_DEPTH));  // tail stuck
    EXPECT_TRUE(r.wormhole_locked_input(E, 0).has_value());
    EXPECT_FALSE(r.wormhole_locked_input(E, 1).has_value());
    for (const auto& f : east.received) EXPECT_EQ(f.get_header_field("vc_id"), 0u);
}

TEST(RouterWormhole, SingleFlitPacketLocksAndReleasesSameCycle) {
    Router r(center_cfg());
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);
    const auto WEST = static_cast<std::size_t>(RouterPort::WEST);
    const auto SOUTH = static_cast<std::size_t>(RouterPort::SOUTH);

    // Tick 0: WEST single-flit (flit_tail=1) lands; SOUTH single-flit lands too.
    r.input(WEST).push_flit(make_tagged_flit(dst, 0, /*flit_tail=*/1, 0x10));
    r.input(SOUTH).push_flit(make_tagged_flit(dst, 0, /*flit_tail=*/1, 0x20));
    // S1 latch (tick 1), then one VA grant per output per tick: one input
    // allocates (EAST, vc) at tick 2 and is granted at tick 3, releasing the VC
    // on that same grant; the other allocates at tick 3 and is granted at
    // tick 4. Stage 4 pushes each to the sink the tick after its grant, so
    // 3 + kPipelineDepth ticks settle both single-flit packets.
    for (int t = 0; t < 3 + kPipelineDepth; ++t) tick_and_return_credit(r, east, E);
    // Both single-flit packets must have been delivered; the second did not wait
    // for a stale lock to clear.
    ASSERT_EQ(east.received.size(), 2u);
    const uint8_t s0 = static_cast<uint8_t>(east.received[0].get_header_field("src_id"));
    const uint8_t s1 = static_cast<uint8_t>(east.received[1].get_header_field("src_id"));
    EXPECT_NE(s0, s1) << "both inputs granted; no stale single-flit lock";
}

TEST(RouterWormhole, LockedEmptyVcIdlesButDoesNotLoseLock) {
    Router r(center_cfg());
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);
    const auto WEST = static_cast<std::size_t>(RouterPort::WEST);
    const auto SOUTH = static_cast<std::size_t>(RouterPort::SOUTH);

    // SOUTH establishes the lock alone (head, flit_tail=0) with no competitor present,
    // then stalls its FIFO empty. WEST (the competitor) then offers a complete
    // 3-flit packet on the same (EAST, vc0). The locked-but-empty SOUTH must idle
    // the arbiter — WEST cannot steal until SOUTH's tail releases the lock.
    Packet west{WEST, 0x10};

    // Tick 0: SOUTH head lands alone -> wins arbitration -> locks (EAST, vc0).
    r.input(SOUTH).push_flit(make_tagged_flit(dst, 0, /*flit_tail=*/0, 0x20));
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

    // SOUTH sends its tail (flit_tail=1) to release the lock.
    r.input(SOUTH).push_flit(make_tagged_flit(dst, 0, /*flit_tail=*/1, 0x20));
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
    Router r(center_cfg());
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);
    const auto WEST = static_cast<std::size_t>(RouterPort::WEST);
    const auto SOUTH = static_cast<std::size_t>(RouterPort::SOUTH);

    // Both inputs continuously offer single-flit packets (flit_tail=1) to EAST vc0.
    // Keep at most one flit queued per input (refill only when its FIFO is empty)
    // so the input FIFO never overflows; the output grants one input per cycle, so
    // a saturated single-flit stream from both inputs exercises the packet-level
    // RR pointer. Replenish credit per delivered flit so the stream never stalls.
    // The real exit is the size() < 8 guard (collect 8 grants); bound 40 is a
    // generous safety cap (8 grants + kPipelineDepth drain fit well inside it) so
    // a regression that stops granting fails fast instead of spinning forever.
    for (int t = 0; t < 40 && east.received.size() < 8; ++t) {
        if (r.input_fifo_size(WEST, 0) == 0)
            r.input(WEST).push_flit(make_tagged_flit(dst, 0, /*flit_tail=*/1, 0x10));
        if (r.input_fifo_size(SOUTH, 0) == 0)
            r.input(SOUTH).push_flit(make_tagged_flit(dst, 0, /*flit_tail=*/1, 0x20));
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

TEST(RouterWormhole, LockedOutputIsLocalAndOtherOutputProceeds) {
    RouterConfig cfg = center_cfg();
    cfg.num_vc = 2;
    Router r(cfg);
    FlitSink east, north;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto N = static_cast<std::size_t>(RouterPort::NORTH);
    r.set_downstream(E, east);
    r.set_downstream(N, north);
    Packet a{static_cast<std::size_t>(RouterPort::WEST), 0x10};   // -> EAST
    Packet b{static_cast<std::size_t>(RouterPort::SOUTH), 0x20};  // -> NORTH
    for (int t = 0; t < 24; ++t) {
        feed_packet(r, a, make_dst(3, 1), /*vc=*/0);  // EAST
        feed_packet(r, b, make_dst(1, 2), /*vc=*/0);  // NORTH
        const std::size_t be = east.received.size(), bn = north.received.size();
        r.tick();
        // Return credit on the DELIVERED vc_id: VA restamps these fixed_vc=0
        // flits to the preferred output VC, not their input VC.
        for (std::size_t i = be; i < east.received.size(); ++i)
            r.receive_credit(E, static_cast<uint8_t>(east.received[i].get_header_field("vc_id")));
        for (std::size_t i = bn; i < north.received.size(); ++i)
            r.receive_credit(N, static_cast<uint8_t>(north.received[i].get_header_field("vc_id")));
    }
    EXPECT_EQ(east.received.size(), 3u);
    EXPECT_EQ(north.received.size(), 3u);  // other output not blocked by EAST's lock
}

// --- Per-VC independence --------------------------------------------------
// All three tests explicitly require two VCs. Keep that requirement local even
// though the generated NOC_DAT_NUM_VC default is currently 2. The two per-VC tests drive
// fixed_vc=1 (pinned) flits: their premise is a 1:1 input-VC/output-VC
// mapping, which post-VA only the fixed_vc bypass guarantees.

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
    Router r(two_vc_cfg());
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto WEST = static_cast<std::size_t>(RouterPort::WEST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);  // routes EAST from center

    // First, exhaust EAST vc0 credit. Feed one vc0 single-flit packet per tick
    // from WEST; each grant decrements credit (4 -> 0) and we NEVER return vc0
    // credit. Stop feeding once vc0 credit is gone and 2 extras sit queued (the
    // input VC FIFO is vc_depth=4 deep, so 2 extras can never overflow it).
    for (int t = 0; t < 30; ++t) {
        // Keep feeding vc0 while credit remains, plus exactly 2 extras afterward.
        const bool credit_left = r.credit(E, 0) > 0;
        const bool want_extra = !credit_left && r.input_fifo_size(WEST, 0) < 2;
        if (credit_left || want_extra)
            r.input(WEST).push_flit(make_pinned_flit(dst, /*vc=*/0, /*flit_tail=*/1, 0x10));
        r.tick();  // never return vc0 credit
        if (!credit_left && r.input_fifo_size(WEST, 0) >= 2) break;
    }
    ASSERT_EQ(r.credit(E, 0), 0u) << "vc0 credit not exhausted";
    ASSERT_GE(r.input_fifo_size(WEST, 0), 1u) << "no vc0 backlog left head-blocked";
    const std::size_t vc0_blocked = r.input_fifo_size(WEST, 0);
    const std::size_t sink_after_vc0 = east.received.size();

    // Then feed vc1 single-flit packets from WEST (one/tick) and return vc1
    // credit on delivery. vc1 has full, fresh credit, so it must flow despite vc0
    // being permanently blocked on the same input/output ports.
    int vc1_received = 0;
    // Each packet is a head, so each costs one more tick than the pre-VA-stage
    // pipeline: 4 packets, bound 12 -> 16. The real exit is vc1_received.
    for (int t = 0; t < 16 && vc1_received < 4; ++t) {
        if (r.input_fifo_size(WEST, 1) == 0)
            r.input(WEST).push_flit(make_pinned_flit(dst, /*vc=*/1, /*flit_tail=*/1, 0x20));
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
    Router r(two_vc_cfg());
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto WEST = static_cast<std::size_t>(RouterPort::WEST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);

    // One flit/input-port/tick (input register), so "sustained two-VC load from
    // the same input" is fed by alternating vc0 / vc1 on successive ticks. Both
    // VCs keep a flit queued; the output grants one flit/cycle and round-robins
    // across VCs, so delivered vc_ids must alternate. Ample credit (returned per
    // delivery) keeps neither VC credit-starved.
    uint8_t feed_vc = 0;
    for (int t = 0; t < 40 && east.received.size() < 8; ++t) {
        if (r.input_fifo_size(WEST, feed_vc) == 0)
            r.input(WEST).push_flit(make_pinned_flit(dst, feed_vc, /*flit_tail=*/1));
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
    Router r(two_vc_cfg());
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto WEST = static_cast<std::size_t>(RouterPort::WEST);
    const uint8_t dst = make_dst(3, 1);

    // Fill the EAST output FIFO to its depth (NOC_ROUTER_OUTPUT_FIFO_DEPTH)
    // WITHOUT a downstream attached, so stage 4 LT cannot drain it. Keep a backlog queued
    // in the input FIFO so a grant is available on every later tick. Feed one vc0 flit/tick.
    for (int t = 0; t < 12; ++t) {
        if (r.input_fifo_size(WEST, 0) < NOC_ROUTER_OUTPUT_FIFO_DEPTH + 1)
            r.input(WEST).push_flit(make_flit(dst, /*vc=*/0, /*flit_tail=*/1));
        r.tick();
        if (r.output_fifo_size(E) >= NOC_ROUTER_OUTPUT_FIFO_DEPTH &&
            r.input_fifo_size(WEST, 0) >= 1)
            break;
    }
    ASSERT_EQ(r.output_fifo_size(E), static_cast<std::size_t>(NOC_ROUTER_OUTPUT_FIFO_DEPTH))
        << "output FIFO not filled to depth";
    ASSERT_GE(r.input_fifo_size(WEST, 0), 1u) << "no input backlog to supply a same-tick grant";

    // Attach the downstream now: stage 4 LT can drain one flit this tick, and
    // stage 3 SA (running after LT in the same tick) can grant one from the
    // input backlog. Net output-FIFO occupancy stays at
    // NOC_ROUTER_OUTPUT_FIFO_DEPTH; the sink gains exactly one.
    r.set_downstream(E, east);
    ASSERT_TRUE(east.received.empty());
    const std::size_t backlog_before = r.input_fifo_size(WEST, 0);
    r.tick();
    EXPECT_EQ(r.output_fifo_size(E), static_cast<std::size_t>(NOC_ROUTER_OUTPUT_FIFO_DEPTH))
        << "deq+enq in the same tick did not hold occupancy at the FIFO depth";
    EXPECT_EQ(east.received.size(), 1u) << "stage 4 LT did not drain one flit";
    EXPECT_EQ(r.input_fifo_size(WEST, 0), backlog_before - 1)
        << "stage 3 SA did not grant one from the backlog the same tick";
}

// --- Credit conservation + error behaviors ---------------------------------

TEST(RouterCredit, ConservationAcrossChainedRouters) {
    // Chain: A(1,1) EAST -> B(2,1) WEST; B ejects LOCAL into a sink.
    RouterConfig acfg = center_cfg();
    RouterConfig bcfg = center_cfg();
    bcfg.x = 2;  // B is east of A
    Router a(acfg), b(bcfg);

    // B's WEST upstream-credit pulse returns one (A.EAST, vc0) credit to A. This
    // fires (registered, one tick late) when B GRANTS a flit out of its WEST/vc0
    // input FIFO — i.e. the credit comes home only once B has consumed the flit.
    struct CreditRelay : ni::cmodel::router::RouterCreditSink {
        Router* target;
        std::size_t port;
        void receive_credit(uint8_t vc) override { target->receive_credit(port, vc); }
    } relay;
    relay.target = &a;
    relay.port = static_cast<std::size_t>(RouterPort::EAST);

    // A's stage 4 LT push lands in B's WEST input register. It becomes FIFO-visible
    // only after B's next stage-1. `wire_inflight` counts flits A has pushed but B
    // has not yet absorbed into its WEST FIFO. We increment here and decrement once
    // per b.tick() (stage 1 always drains a present input register). Since A pushes <=1
    // flit/tick on EAST and B absorbs its input register every tick, wire_inflight is 0 or
    // 1 at any post-tick sampling point.
    struct CountingLink : ni::cmodel::router::RouterLink {
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
    struct EjectSink : ni::cmodel::router::RouterLink {
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
    // + a.output_fifo_size(A.EAST)            granted, awaiting A's stage-4 push
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
    for (int t = 0; t < 200 && (injected < kPackets || a.credit(E, 0) < NOC_ROUTER_VC_DEPTH); ++t) {
        if (injected < kPackets && a.credit(E, 0) > 0 && a.input_fifo_size(W, 0) == 0) {
            a.input(W).push_flit(make_flit(dst_b_local, /*vc=*/0, /*flit_tail=*/1));
            ++injected;
        }
        a.tick();  // A may push <=1 flit onto the wire (a_to_b.push_flit)
        b.tick();  // B absorbs its WEST input register this tick
        if (a_to_b.in_flight > 0) --a_to_b.in_flight;  // input register consumed by B stage 1

        // No credit created: the observable occupancy never exceeds DEPTH.
        EXPECT_LE(occupancy_lower(), static_cast<std::size_t>(NOC_ROUTER_VC_DEPTH))
            << "credit created at tick " << t;
        EXPECT_LE(a_to_b.in_flight, 1u) << "more than one flit on the wire at tick " << t;
    }

    EXPECT_EQ(injected, kPackets) << "did not inject all packets (credit deadlock?)";
    // At quiescence: every credit restored (none destroyed) and every flit ejected
    // (none lost or duplicated).
    EXPECT_EQ(a.credit(E, 0), static_cast<std::size_t>(NOC_ROUTER_VC_DEPTH))
        << "credit not fully restored at drain";
    EXPECT_EQ(a_to_b.in_flight, 0u);
    EXPECT_EQ(b.input_fifo_size(W, 0), 0u);
    EXPECT_EQ(local_sink.received.size(), static_cast<std::size_t>(kPackets))
        << "flits created or lost in transit";
}

TEST(RouterCreditDeath, OverflowAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    Router r(center_cfg());
    EXPECT_DEATH(r.receive_credit(static_cast<std::size_t>(RouterPort::EAST), 0), "overflow");
}

// --- All-to-one fairness + parameterized grid -----------------------------

TEST(RouterFairness, AllToOneNoStarvation) {
    Router r(center_cfg());
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);  // routes EAST from center (1,1)

    // The four contending inputs: every non-EAST port. LOCAL->EAST is a valid
    // turn, so LOCAL is a legitimate sender here. EAST is the shared output; its
    // FlitSink drains freely (credit relayed per delivered flit by
    // tick_and_return_credit), so there is no downstream backpressure and the
    // only contention is for the single EAST grant/cycle.
    constexpr int kInputs = 4;
    // feed_packet emits a head(flit_tail=0)/body(flit_tail=0)/tail(flit_tail=1) stream, so each
    // packet here is MAX_PACKET_FLITS = 3 flits. The §5 fairness bound scales with
    // this packet length.
    constexpr int kPacketFlits = kMaxPacketFlits;
    const std::size_t in_ports[kInputs] = {
        static_cast<std::size_t>(RouterPort::LOCAL),
        static_cast<std::size_t>(RouterPort::NORTH),
        static_cast<std::size_t>(RouterPort::SOUTH),
        static_cast<std::size_t>(RouterPort::WEST),
    };
    // Distinct sink labels (carried in src_id) so each delivered flit names its
    // input. One Packet object per input; refill with a fresh 3-flit packet as
    // soon as the previous one is fully pushed, keeping all four backlogged.
    const uint8_t labels[kInputs] = {0x10, 0x20, 0x30, 0x40};
    Packet pkt[kInputs];
    for (int i = 0; i < kInputs; ++i) {
        pkt[i] = Packet{in_ports[i], labels[i]};
    }

    // Feed at most one flit/input/tick, and only when that input's vc0 FIFO has
    // room (credit-aware: never overflow the vc_depth-deep input FIFO). Refill a
    // finished packet so every input stays continuously backlogged. Collect a
    // generous number of delivered flits to span many full RR rounds (each round
    // is kInputs x kPacketFlits = 12 flits).
    constexpr std::size_t kCollect = 72u;  // 6 full RR rounds
    for (int t = 0; t < 400 && east.received.size() < kCollect; ++t) {
        for (int i = 0; i < kInputs; ++i) {
            if (pkt[i].next >= kMaxPacketFlits) pkt[i] = Packet{in_ports[i], labels[i]};  // refill
            if (r.input_fifo_size(in_ports[i], 0) < NOC_ROUTER_VC_DEPTH)
                feed_packet(r, pkt[i], dst, 0);
        }
        tick_and_return_credit(r, east, E);
    }
    ASSERT_GE(east.received.size(), kCollect) << "router stopped granting under all-to-one load";

    // All four inputs must make progress (none starved entirely).
    int per_src[kInputs] = {0, 0, 0, 0};
    for (const auto& f : east.received) {
        const uint8_t s = static_cast<uint8_t>(f.get_header_field("src_id"));
        for (int i = 0; i < kInputs; ++i)
            if (s == labels[i]) ++per_src[i];
    }
    for (int i = 0; i < kInputs; ++i)
        EXPECT_GT(per_src[i], 0) << "input label 0x" << std::hex << static_cast<int>(labels[i])
                                 << " starved (zero grants)";

    // Fairness bound: between two consecutive grants of the SAME input,
    // at most (inputs-1) x MAX_PACKET_FLITS flits from OTHER inputs may interpose.
    // Packet-level RR serves each of the other 3 inputs one full kPacketFlits
    // packet before returning, so the worst case is exactly 3 x 3 = 9 intervening
    // flits.
    constexpr int kBound = (kInputs - 1) * kPacketFlits;  // 9
    int last_index[kInputs];
    for (int i = 0; i < kInputs; ++i) last_index[i] = -1;
    int max_intervening = 0;
    for (std::size_t idx = 0; idx < east.received.size(); ++idx) {
        const uint8_t s = static_cast<uint8_t>(east.received[idx].get_header_field("src_id"));
        for (int i = 0; i < kInputs; ++i) {
            if (s != labels[i]) continue;
            if (last_index[i] >= 0) {
                // Flits from other inputs strictly between this and the previous
                // grant of the same input = index gap minus 1.
                const int intervening = static_cast<int>(idx) - last_index[i] - 1;
                if (intervening > max_intervening) max_intervening = intervening;
            }
            last_index[i] = static_cast<int>(idx);
        }
    }
    EXPECT_LE(max_intervening, kBound)
        << "starvation: " << max_intervening << " flits interposed between two grants of one input"
        << " (bound " << kBound << ")";
}

class RouterGrid : public ::testing::TestWithParam<std::tuple<int, int>> {};

TEST_P(RouterGrid, EndToEndTrafficAcrossParameterSpace) {
    auto [num_vc, depth] = GetParam();
    RouterConfig cfg = center_cfg();
    cfg.num_vc = static_cast<uint8_t>(num_vc);
    cfg.vc_depth = static_cast<std::size_t>(depth);
    Router r(cfg);
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto WEST = static_cast<std::size_t>(RouterPort::WEST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);  // routes EAST from center

    // Drive kPacketsPerVc single-flit packets per VC from WEST->EAST. Tag each
    // with a per-vc-increasing src_id so per-VC FIFO order can be checked at the
    // sink. All flits are pinned (fixed_vc=1) so the per-VC order check keys on
    // a stable vc_id across VA — this grid doubles as fixed_vc-bypass coverage
    // of the whole (num_vc, depth) space; the VA restamp path is covered by
    // RouterVa*. Single-flit packets keep the wormhole lock trivial; the point
    // of this grid is parameter-space coverage (num_vc, depth), not multi-flit
    // locking (covered by RouterWormhole.*).
    constexpr int kPacketsPerVc = 3;
    // src_id label per (vc, packet): high nibble = vc, low nibble = sequence. With
    // num_vc<=8 and 3 packets this stays inside a byte and is unambiguous.
    auto label = [](int vc, int seq) -> uint8_t {
        return static_cast<uint8_t>((vc << 4) | (seq & 0x0F));
    };

    // Feeding must be credit-aware so depth=1 never overflows the vc_depth-deep
    // input FIFO: push a vc's next packet only when that vc's input FIFO has room.
    // One flit/input-port/tick (input register), so per-vc injection naturally
    // interleaves across ticks. Track the next packet sequence to push per vc.
    std::vector<int> fed(num_vc, 0);
    const int total = num_vc * kPacketsPerVc;
    for (int t = 0;
         t < total * (kPipelineDepth + 4) + 50 && static_cast<int>(east.received.size()) < total;
         ++t) {
        for (int vc = 0; vc < num_vc; ++vc) {
            if (fed[vc] >= kPacketsPerVc) continue;
            if (r.input_fifo_size(WEST, static_cast<uint8_t>(vc)) <
                static_cast<std::size_t>(depth)) {
                r.input(WEST).push_flit(make_pinned_flit(dst, static_cast<uint8_t>(vc),
                                                         /*flit_tail=*/1, label(vc, fed[vc])));
                ++fed[vc];
                break;  // one flit/input-port/tick (input register)
            }
        }
        const std::size_t before = east.received.size();
        r.tick();
        for (std::size_t i = before; i < east.received.size(); ++i) {
            const auto vc = static_cast<uint8_t>(east.received[i].get_header_field("vc_id"));
            r.receive_credit(E, vc);
        }
    }

    // Every flit arrived (none lost or duplicated).
    ASSERT_EQ(static_cast<int>(east.received.size()), total)
        << "num_vc=" << num_vc << " depth=" << depth << ": not all flits delivered";

    // Per-VC order preserved: within each vc, src_id sequence must be the
    // monotonic 0,1,2 injection order (wormhole + FIFO keep per-vc order; the
    // router may interleave ACROSS vcs, which is allowed).
    std::vector<int> next_seq(num_vc, 0);
    for (const auto& f : east.received) {
        const auto vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
        const auto s = static_cast<uint8_t>(f.get_header_field("src_id"));
        ASSERT_LT(static_cast<int>(vc), num_vc) << "delivered an out-of-range vc";
        EXPECT_EQ(s, label(vc, next_seq[vc]))
            << "vc=" << static_cast<int>(vc) << " out of order (num_vc=" << num_vc
            << " depth=" << depth << ")";
        // Header intact: dst and vc survive unchanged end-to-end.
        EXPECT_EQ(static_cast<uint8_t>(f.get_header_field("dst_id")), dst);
        ++next_seq[vc];
    }
    for (int vc = 0; vc < num_vc; ++vc)
        EXPECT_EQ(next_seq[vc], kPacketsPerVc)
            << "vc=" << vc << " did not deliver all packets (num_vc=" << num_vc << ")";
}

INSTANTIATE_TEST_SUITE_P(NumVcDepthGrid, RouterGrid,
                         ::testing::Combine(::testing::Values(1, 2, 4, 8),
                                            ::testing::Values(2, 4, 8, 32)));

// --- VA stage (deprecated vc_router_util port) ------------------------------
// Preferred-VC map, FVADA selection/overflow, worm no-overflow, fixed_vc
// bypass, credit consume/return split. RTL provenance:
// hw/deprecated/vc_router_util/ + hw/deprecated/floo_vc_router.sv.

// floo_vc_assignment.sv:86-93: preferred_vc follows the XY-optimized (output, next-hop) map, taken
// modulo num_vc.
TEST(RouterVaPreferredMap, AllCellsAllNumVc) {
    using ni::cmodel::router::preferred_vc;
    constexpr RouterPort L = RouterPort::LOCAL, N = RouterPort::NORTH, E = RouterPort::EAST,
                         S = RouterPort::SOUTH, W = RouterPort::WEST;
    // (this-hop output, next-hop route, pre-% preferred) — verbatim design table.
    const struct {
        RouterPort out, next;
        uint32_t pref;
    } kCells[] = {
        {L, L, 0}, {L, N, 0}, {L, E, 0}, {L, S, 0}, {L, W, 0},  // :86 eject output
        {N, L, 1}, {S, L, 1},                                   // :88 N/S -> eject
        {E, L, 3}, {W, L, 3},                                   // :89 E/W -> eject
        {N, N, 0}, {N, S, 0}, {S, N, 0}, {S, S, 0},             // :90 straight N/S
        {E, N, 0}, {W, N, 0},                                   // :91
        {E, S, 2}, {W, S, 1},                                   // :92
        {E, E, 1}, {W, W, 2},                                   // :93
    };
    for (uint8_t num_vc = 1; num_vc <= 8; ++num_vc) {
        for (const auto& c : kCells) {
            EXPECT_EQ(preferred_vc(c.out, c.next, num_vc), static_cast<uint8_t>(c.pref % num_vc))
                << "out=" << static_cast<int>(c.out) << " next=" << static_cast<int>(c.next)
                << " num_vc=" << static_cast<int>(num_vc);
        }
    }
}

class RouterVaFvada : public ::testing::TestWithParam<int> {};

// floo_vc_selection.sv:37-45: overflow picks the HIGHEST-index non-full VC -- a faithful quirk of
// the ported overwrite scan, not a bug to fix.
TEST_P(RouterVaFvada, PreferredThenHighestIndexOverflowThenStall) {
    const int num_vc = GetParam();
    RouterConfig cfg = center_cfg();
    cfg.num_vc = static_cast<uint8_t>(num_vc);
    cfg.vc_depth = 2;
    Router r(cfg);
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto WEST = static_cast<std::size_t>(RouterPort::WEST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);  // EAST; next hop EAST -> preferred 1 % num_vc
    const uint8_t pref = static_cast<uint8_t>(1 % num_vc);

    // Expected delivered out-VC sequence with credit never returned: preferred
    // first, then the remaining VCs from the highest index down.
    std::vector<uint8_t> expect{pref, pref};
    for (int v = num_vc - 1; v >= 0; --v) {
        if (v != pref) {
            expect.push_back(static_cast<uint8_t>(v));
            expect.push_back(static_cast<uint8_t>(v));
        }
    }
    for (std::size_t k = 0; k < expect.size(); ++k) {
        r.input(WEST).push_flit(make_flit(dst, /*vc=*/0, /*flit_tail=*/1));  // fixed_vc=0
        for (int t = 0; t < 6 && east.received.size() < k + 1; ++t) r.tick();
        ASSERT_EQ(east.received.size(), k + 1)
            << "grant " << k << " missing (num_vc=" << num_vc << ")";
        EXPECT_EQ(static_cast<uint8_t>(east.received[k].get_header_field("vc_id")), expect[k])
            << "FVADA pick order wrong at grant " << k << " (num_vc=" << num_vc << ")";
    }

    // Every output VC exhausted: the next flit is not grantable (mech 7).
    r.input(WEST).push_flit(make_flit(dst, /*vc=*/0, /*flit_tail=*/1));
    for (int t = 0; t < 6; ++t) r.tick();
    EXPECT_EQ(east.received.size(), expect.size()) << "granted with zero credit on every VC";
    EXPECT_EQ(r.input_fifo_size(WEST, 0), 1u);

    // Returning the preferred VC's credit releases the stalled flit onto it.
    r.receive_credit(E, pref);
    for (int t = 0; t < 6 && east.received.size() < expect.size() + 1; ++t) r.tick();
    ASSERT_EQ(east.received.size(), expect.size() + 1);
    EXPECT_EQ(static_cast<uint8_t>(east.received.back().get_header_field("vc_id")), pref);
}

INSTANTIATE_TEST_SUITE_P(NumVc, RouterVaFvada, ::testing::Values(2, 4, 8));

// floo_vc_router.sv:295, floo_vc_assignment.sv:110-112: a worm head is granted only on its
// preferred VC -- a full preferred VC stalls it even with other VCs free.
TEST(RouterVaWorm, HeadWaitsForPreferredFullBodyTailFollowLockedOutputVc) {
    RouterConfig cfg = center_cfg();
    cfg.num_vc = 2;
    cfg.vc_depth = 4;  // fits the 3-flit worm in one input VC FIFO
    Router r(cfg);
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto WEST = static_cast<std::size_t>(RouterPort::WEST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);  // EAST, preferred 1

    // Exhaust EAST vc1 with pinned single-flit packets, credit withheld.
    for (std::size_t k = 0; k < cfg.vc_depth; ++k) {
        r.input(WEST).push_flit(make_pinned_flit(dst, /*vc=*/1, /*flit_tail=*/1));
        for (int t = 0; t < 6 && east.received.size() < k + 1; ++t) r.tick();
        ASSERT_EQ(east.received.size(), k + 1);
    }
    ASSERT_EQ(r.credit(E, 1), 0u);

    // 3-flit fixed_vc=0 worm on input vc0: the head must idle while vc1 (its
    // preferred VC) is full, even though vc0 has full credit.
    r.input(WEST).push_flit(make_flit(dst, /*vc=*/0, /*flit_tail=*/0));  // head
    r.tick();
    r.input(WEST).push_flit(make_flit(dst, /*vc=*/0, /*flit_tail=*/0));  // body
    r.tick();
    r.input(WEST).push_flit(make_flit(dst, /*vc=*/0, /*flit_tail=*/1));  // tail
    for (int t = 0; t < 4; ++t) r.tick();
    EXPECT_EQ(east.received.size(), 4u) << "worm head overflowed off its preferred VC";
    EXPECT_EQ(r.input_fifo_size(WEST, 0), 3u);
    EXPECT_EQ(r.credit(E, 0), 4u) << "head consumed the wrong VC's credit";

    // Return vc1 credit per delivery: the worm flows, every flit stamped vc1.
    r.receive_credit(E, 1);
    for (int t = 0; t < 16 && east.received.size() < 7; ++t) {
        const std::size_t before = east.received.size();
        r.tick();
        for (std::size_t i = before; i < east.received.size(); ++i)
            r.receive_credit(E, static_cast<uint8_t>(east.received[i].get_header_field("vc_id")));
    }
    ASSERT_EQ(east.received.size(), 7u);
    for (std::size_t i = 4; i < 7; ++i)
        EXPECT_EQ(static_cast<uint8_t>(east.received[i].get_header_field("vc_id")), 1u)
            << "worm flit " << i << " left locked_output_vc";
}

TEST(RouterVaWorm, PinnedWormRidesNonPreferredVcNoAssert) {
    RouterConfig cfg = center_cfg();
    cfg.num_vc = 2;
    Router r(cfg);
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto WEST = static_cast<std::size_t>(RouterPort::WEST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);  // EAST, preferred 1; worm pinned to vc0

    r.input(WEST).push_flit(make_pinned_flit(dst, /*vc=*/0, /*flit_tail=*/0));  // head
    r.tick();
    r.input(WEST).push_flit(make_pinned_flit(dst, /*vc=*/0, /*flit_tail=*/0));  // body
    r.tick();
    r.input(WEST).push_flit(make_pinned_flit(dst, /*vc=*/0, /*flit_tail=*/1));  // tail
    for (int t = 0; t < 8; ++t) tick_and_return_credit(r, east, E);
    ASSERT_EQ(east.received.size(), 3u);
    for (const auto& f : east.received)
        EXPECT_EQ(static_cast<uint8_t>(f.get_header_field("vc_id")), 0u) << "pin not honored";
    EXPECT_EQ(r.credit(E, 1), static_cast<std::size_t>(NOC_ROUTER_VC_DEPTH))
        << "pinned worm consumed the preferred VC's credit";
}

// VA needs a free AND credited VC. Preferred VC held by another worm: a
// single-flit packet takes the highest-index other free credited VC; a worm
// head stays idle and takes nothing.
TEST(RouterVa, HeldPreferredVcSingleFlitOverflowsWormHeadWaits) {
    RouterConfig cfg = center_cfg();
    cfg.num_vc = 2;
    Router r(cfg);
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    const auto S = static_cast<std::size_t>(RouterPort::SOUTH);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(2, 3);  // EAST here, next hop NORTH -> preferred vc0
    r.input(S).push_flit(make_tagged_flit(dst, 0, /*flit_tail=*/0, 0x20));  // open worm holds vc0
    r.tick();
    r.tick();
    ASSERT_EQ(r.wormhole_locked_input(E, 0), std::optional<std::size_t>(S));

    r.input(W).push_flit(make_tagged_flit(dst, 0, 1, 0x10));  // single-flit packet
    r.tick();                                                 // stage 1 BW
    r.tick();                                                 // stage 2 VA
    EXPECT_EQ(r.va_out_vc(W, 0), std::optional<uint8_t>(1)) << "overflowed to vc1";
    for (int t = 0; t < 4; ++t) r.tick();  // it departs and frees vc1 again

    r.input(W).push_flit(make_tagged_flit(dst, 0, 0, 0x30));  // worm head
    for (int t = 0; t < 4; ++t) r.tick();
    EXPECT_EQ(r.input_fifo_size(W, 0), 1u) << "worm head still parked, waiting on VA";
    EXPECT_FALSE(r.va_out_vc(W, 0).has_value()) << "worm head overflowed off its preferred VC";
    EXPECT_FALSE(r.wormhole_locked_input(E, 1).has_value());
}

// A free VC with credit 0 is never held: the head stays idle until credit
// returns, then allocates and departs.
TEST(RouterVa, ZeroCreditVcIsNeverHeld) {
    Router r(center_cfg());  // num_vc 1
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    r.set_downstream(E, east);
    const uint8_t dst = make_dst(3, 1);
    for (std::size_t i = 0; i < NOC_ROUTER_VC_DEPTH; ++i) {
        r.input(W).push_flit(make_flit(dst, 0, 1));
        r.tick();
    }
    for (int t = 0; t < 8; ++t) r.tick();  // credit_[E][0] is now 0, no returns
    ASSERT_EQ(r.credit(E, 0), 0u);
    ASSERT_EQ(east.received.size(), static_cast<std::size_t>(NOC_ROUTER_VC_DEPTH));

    r.input(W).push_flit(make_flit(dst, 0, 1));
    for (int t = 0; t < 4; ++t) r.tick();
    EXPECT_FALSE(r.va_out_vc(W, 0).has_value());
    EXPECT_FALSE(r.wormhole_locked_input(E, 0).has_value());
    ASSERT_EQ(r.allocation_waits().size(), 1u);
    EXPECT_FALSE(r.allocation_waits().front().occupied);
    r.receive_credit(E, 0);
    for (int t = 0; t < 4; ++t) r.tick();
    EXPECT_EQ(east.received.size(), NOC_ROUTER_VC_DEPTH + 1);
    EXPECT_TRUE(r.allocation_waits().empty());
}

TEST(RouterDiagnostics, AllocationWaitIdentifiesOccupiedOutputAndFullInput) {
    Router r(center_cfg());
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    const auto N = static_cast<std::size_t>(RouterPort::NORTH);
    const auto dst = make_dst(3, 1);
    r.set_downstream(E, east);
    r.tick();
    EXPECT_TRUE(r.allocation_waits().empty());
    r.input(W).push_flit(make_flit(dst, 0, 0));
    for (int t = 0; t < kPipelineDepth; ++t) r.tick();
    for (std::size_t beat = 0; beat < r.vc_depth(); ++beat) {
        r.input(N).push_flit(make_flit(dst, 0, 1));
        r.tick();
    }
    r.tick();
    ASSERT_EQ(r.allocation_waits().size(), 1u);
    const auto& wait = r.allocation_waits().front();
    EXPECT_EQ(wait.input, N);
    EXPECT_EQ(wait.output, E);
    EXPECT_EQ(wait.vc, 0);
    EXPECT_TRUE(wait.occupied);
    EXPECT_TRUE(wait.input_full);
    EXPECT_GT(r.credit(E, 0), 0u);
    r.input(W).push_flit(make_flit(dst, 0, 1));
    r.tick();
    r.tick();
    EXPECT_TRUE(r.allocation_waits().empty());
    EXPECT_EQ(r.wormhole_locked_input(E, 0), N);
}

TEST(RouterDiagnostics, IdleZeroCreditVcIsNotBlocked) {
    RouterConfig cfg = center_cfg();
    cfg.num_vc = 2;
    Router r(cfg);
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    r.set_downstream(E, east);

    for (std::size_t k = 0; k < cfg.vc_depth; ++k) {
        r.input(W).push_flit(make_pinned_flit(make_dst(3, 1), 0, 1));
        for (int t = 0; t < 6 && east.received.size() < k + 1; ++t) r.tick();
        ASSERT_EQ(east.received.size(), k + 1);
    }
    ASSERT_EQ(r.credit(E, 0), 0u);

    EXPECT_FALSE(r.output_vc_credit_blocked(E, 0));
    EXPECT_EQ(r.switch_activity(E).eligible_vcs, 0u);
    EXPECT_EQ(r.switch_activity(E).grants, 0u);
}

TEST(RouterDiagnostics, UnrelatedZeroCreditVcIsNotBlocked) {
    RouterConfig cfg = center_cfg();
    cfg.num_vc = 2;
    Router r(cfg);
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    r.set_downstream(E, east);

    for (std::size_t k = 0; k < cfg.vc_depth; ++k) {
        r.input(W).push_flit(make_pinned_flit(make_dst(3, 1), 0, 1));
        for (int t = 0; t < 6 && east.received.size() < k + 1; ++t) r.tick();
        ASSERT_EQ(east.received.size(), k + 1);
    }
    ASSERT_EQ(r.credit(E, 0), 0u);

    r.input(W).push_flit(make_pinned_flit(make_dst(3, 1), 1, 0));
    r.tick();  // stage 1 BW: the vc1 request is now visible at the FIFO front
    EXPECT_FALSE(r.output_vc_credit_blocked(E, 0));
}

TEST(RouterDiagnostics, UnallocatedHeadCountsWhenSelectedVcHasZeroCreditAndOutputFifoIsFull) {
    RouterConfig cfg = center_cfg();
    cfg.vc_depth = 2;
    cfg.output_fifo_depth = 2;
    Router r(cfg);
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);

    for (std::size_t k = 0; k <= cfg.vc_depth; ++k) {
        r.input(W).push_flit(make_pinned_flit(make_dst(3, 1), 0, 1));
        r.tick();
    }
    r.tick();  // fill the output FIFO; the third head retries VA at zero credit
    ASSERT_EQ(r.output_fifo_size(E), cfg.output_fifo_depth);
    ASSERT_EQ(r.credit(E, 0), 0u);
    ASSERT_EQ(r.input_fifo_size(W, 0), 1u);
    ASSERT_FALSE(r.va_out_vc(W, 0).has_value());

    EXPECT_TRUE(r.output_vc_credit_blocked(E, 0));
}

TEST(RouterDiagnostics, RequestingWormReportsItsSelectedZeroCreditVc) {
    RouterConfig cfg = center_cfg();
    cfg.num_vc = 2;
    Router r(cfg);
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto W = static_cast<std::size_t>(RouterPort::WEST);
    r.set_downstream(E, east);

    for (std::size_t k = 0; k <= cfg.vc_depth; ++k) {
        r.input(W).push_flit(make_pinned_flit(make_dst(3, 1), 1, 0));
        r.tick();
    }
    r.tick();  // drain the fourth flit; the fifth remains at the locked FIFO front
    ASSERT_EQ(r.credit(E, 1), 0u);
    ASSERT_EQ(r.input_fifo_size(W, 1), 1u);

    EXPECT_TRUE(r.output_vc_credit_blocked(E, 1));
    EXPECT_FALSE(r.output_vc_credit_blocked(E, 0));
}

TEST(RouterVaWorkConserving, HeadVaFailAlternateCandidateGrantedSameTick) {
    RouterConfig cfg = center_cfg();
    cfg.num_vc = 2;
    Router r(cfg);
    FlitSink east;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto NORTH = static_cast<std::size_t>(RouterPort::NORTH);
    const auto SOUTH = static_cast<std::size_t>(RouterPort::SOUTH);
    const auto WEST = static_cast<std::size_t>(RouterPort::WEST);
    r.set_downstream(E, east);

    // An open worm from WEST holds (EAST, vc1) -- dst (3,1) routes EAST here,
    // next hop EAST -> preferred vc1. vc0 stays free.
    r.input(WEST).push_flit(make_flit(make_dst(3, 1), /*vc=*/0, /*flit_tail=*/0));
    r.tick();
    r.tick();
    ASSERT_EQ(r.wormhole_locked_input(E, 1), std::optional<std::size_t>(WEST));

    // Candidate A: worm head, preferred vc1 -> held, and a worm head never
    // overflows, so its VA fails.
    r.input(NORTH).push_flit(make_flit(make_dst(3, 1), /*vc=*/0, /*flit_tail=*/0));
    // Candidate B: single-flit packet, dst (2,3) -> out EAST, next hop NORTH ->
    // preferred vc0 (free and credited).
    r.input(SOUTH).push_flit(make_flit(make_dst(2, 3), /*vc=*/0, /*flit_tail=*/1));
    r.tick();  // stage 1: both land in their input-side vc0 FIFOs
    r.tick();  // stage 2 VA: A scanned first, fails; the scan continues to B (D7)
    EXPECT_FALSE(r.va_out_vc(NORTH, 0).has_value()) << "candidate A must not be allocated";
    EXPECT_EQ(r.va_out_vc(SOUTH, 0), std::optional<uint8_t>(0))
        << "candidate B must be allocated this SAME tick";
    EXPECT_EQ(r.wormhole_locked_input(E, 0), std::optional<std::size_t>(SOUTH));

    r.tick();  // stage 3 SA: B granted
    EXPECT_EQ(r.input_fifo_size(NORTH, 0), 1u) << "candidate A must not be granted";
    EXPECT_EQ(r.credit(E, 0), static_cast<std::size_t>(NOC_ROUTER_VC_DEPTH) - 1)
        << "B's preferred VC credit not consumed";
    r.tick();                             // stage 4 LT: B reaches the sink
    ASSERT_EQ(east.received.size(), 2u);  // the holder's head + B
    EXPECT_EQ(static_cast<uint8_t>(east.received[1].get_header_field("dst_id")), make_dst(2, 3));
    EXPECT_EQ(static_cast<uint8_t>(east.received[1].get_header_field("vc_id")), 0u);
}

// Neither VA pointer moves on a failed VA, so the candidate that could not
// allocate keeps first priority: when the VC frees it wins over a candidate
// that arrived later on another input.
TEST(RouterVaWorkConserving, FailedVaLeavesTheArbitrationPointers) {
    RouterConfig cfg = center_cfg();
    cfg.num_vc = 2;
    Router r(cfg);
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto NORTH = static_cast<std::size_t>(RouterPort::NORTH);
    const auto SOUTH = static_cast<std::size_t>(RouterPort::SOUTH);
    const auto WEST = static_cast<std::size_t>(RouterPort::WEST);
    // dst (3,1) routes EAST here, next hop EAST -> preferred vc1 for every
    // candidate below. A worm head never overflows, so vc1 is the only VC any
    // of them can take.
    const uint8_t dst = make_dst(3, 1);

    // An open worm from (WEST, input vc0) holds (EAST, vc1). Its VA grant left
    // in_vc_rr_[EAST] = 1 and rr_[EAST] = LOCAL, so input vc1 is scanned first
    // and NORTH is reached before SOUTH.
    r.input(WEST).push_flit(make_flit(dst, /*vc=*/0, /*flit_tail=*/0));
    r.tick();
    r.tick();
    ASSERT_EQ(r.wormhole_locked_input(E, 1), std::optional<std::size_t>(WEST));

    // Candidate A on (NORTH, input vc1): worm head, preferred vc1 held -> VA
    // fails, and it is the only candidate, so EAST grants nothing this tick.
    r.input(NORTH).push_flit(make_flit(dst, /*vc=*/1, /*flit_tail=*/0));
    r.tick();  // stage 1 BW
    r.tick();  // stage 2 VA: A fails
    ASSERT_FALSE(r.va_out_vc(NORTH, 1).has_value());

    // Candidate B on (SOUTH, input vc1) arrives later and fails the same way.
    r.input(SOUTH).push_flit(make_flit(dst, /*vc=*/1, /*flit_tail=*/0));
    r.tick();  // stage 1 BW
    r.tick();  // stage 2 VA: both fail, vc1 still held
    ASSERT_FALSE(r.va_out_vc(NORTH, 1).has_value());
    ASSERT_FALSE(r.va_out_vc(SOUTH, 1).has_value());

    // Free vc1: the holder's tail passes SA, and VA allocates in the same tick
    // because SA runs first.
    r.input(WEST).push_flit(make_flit(dst, /*vc=*/0, /*flit_tail=*/1));
    r.tick();  // stage 1 BW files the tail behind the still-held vc1
    ASSERT_EQ(r.wormhole_locked_input(E, 1), std::optional<std::size_t>(WEST));
    r.tick();  // stage 3 SA grants the tail and frees vc1; stage 2 VA re-allocates

    EXPECT_EQ(r.va_out_vc(NORTH, 1), std::optional<uint8_t>(1))
        << "the candidate whose VA failed lost its place in the rotation";
    EXPECT_FALSE(r.va_out_vc(SOUTH, 1).has_value())
        << "the later arrival was allocated ahead of it";
    EXPECT_EQ(r.wormhole_locked_input(E, 1), std::optional<std::size_t>(NORTH));
}

TEST(RouterVaCredit, ConsumeStampedVcReturnInputVc) {
    RouterConfig cfg = center_cfg();
    cfg.num_vc = 2;
    Router r(cfg);
    FlitSink east;
    CreditCounter west_up;
    const auto E = static_cast<std::size_t>(RouterPort::EAST);
    const auto WEST = static_cast<std::size_t>(RouterPort::WEST);
    r.set_downstream(E, east);
    r.set_upstream_credit(WEST, west_up);
    // fixed_vc=0, input vc0, dst next-hop EAST -> assigned output vc1.
    r.input(WEST).push_flit(make_flit(make_dst(3, 1), /*vc=*/0, /*flit_tail=*/1));
    r.tick();  // stage 1 BW
    r.tick();  // stage 2 VA: output vc1 assigned
    ASSERT_EQ(r.va_out_vc(WEST, 0), std::optional<uint8_t>(1));
    r.tick();  // stage 3 SA: grant
    EXPECT_EQ(r.credit(E, 1), static_cast<std::size_t>(NOC_ROUTER_VC_DEPTH) - 1)
        << "credit not consumed on the assigned VC";
    EXPECT_EQ(r.credit(E, 0), static_cast<std::size_t>(NOC_ROUTER_VC_DEPTH))
        << "credit consumed on the input VC";
    r.tick();  // stage 4 LT + registered pulse
    ASSERT_EQ(east.received.size(), 1u);
    EXPECT_EQ(static_cast<uint8_t>(east.received[0].get_header_field("vc_id")), 1u)
        << "header not restamped with the assigned VC";
    ASSERT_EQ(west_up.pulses.size(), 1u);
    EXPECT_EQ(west_up.pulses[0], 0u) << "upstream pulse must carry the input-side VC";
}

TEST(RouterVaDeath, LockedOutputVcVsPreferredMismatchAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            RouterConfig cfg = center_cfg();
            cfg.num_vc = 4;
            Router r(cfg);
            FlitSink east;
            r.set_downstream(static_cast<std::size_t>(RouterPort::EAST), east);
            const auto WEST = static_cast<std::size_t>(RouterPort::WEST);
            // Head dst (3,1): EAST out, next-hop EAST -> preferred 1. Locks EAST.
            r.input(WEST).push_flit(make_flit(make_dst(3, 1), /*vc=*/0, /*flit_tail=*/0));
            r.tick();
            r.tick();
            // Continuation dst (2,1): still routes EAST here (route-check assert
            // passes) but its next hop is LOCAL -> preferred 3 != locked 1.
            r.input(WEST).push_flit(make_flit(make_dst(2, 1), /*vc=*/0, /*flit_tail=*/1));
            r.tick();
            r.tick();
        },
        "diverges");
}

// --- VA at fabric level (two routers, two VA stages per flit) ---------------

using ni::cmodel::router::testing::TwoNodeFabric;

TEST(RouterVaFabric, MultiHopPinnedVcPreservedUnderContention) {
    TwoNodeFabric ch(/*num_vc=*/2);
    constexpr uint8_t kPinnedTag = 0x50;  // low nibble carries the pinned vc
    constexpr uint8_t kNoiseTag = 0xCC;
    constexpr int kPinned = 8;
    int pinned_sent = 0, noise_sent = 0, pinned_seen = 0;
    for (int t = 0; t < 300 && pinned_seen < kPinned; ++t) {
        if (pinned_sent < kPinned) {
            const uint8_t vc = static_cast<uint8_t>(pinned_sent % 2);
            if (ch.nmu_req_out(1).push_flit(make_pinned_flit(
                    make_dst(0, 0), vc, /*flit_tail=*/1, static_cast<uint8_t>(kPinnedTag | vc))))
                ++pinned_sent;
        }
        // Contending traffic: node0 self-addressed fixed_vc=0 packets fight for
        // node0's LOCAL output (and get restamped by VA there).
        if (ch.nmu_req_out(0).push_flit(
                make_tagged_flit(make_dst(0, 0), /*vc=*/0, /*flit_tail=*/1, kNoiseTag)))
            ++noise_sent;
        ch.tick();
        while (auto got = ch.nsu_req_in(0).pop_flit()) {
            const auto s = static_cast<uint8_t>(got->get_header_field("src_id"));
            if ((s & 0xF0) == kPinnedTag) {
                ++pinned_seen;
                EXPECT_EQ(static_cast<uint8_t>(got->get_header_field("vc_id")),
                          static_cast<uint8_t>(s & 0x0F))
                    << "pinned vc_id lost across the 2-hop VA path";
            }
        }
    }
    EXPECT_EQ(pinned_seen, kPinned) << "pinned packets did not all arrive";
    EXPECT_GT(noise_sent, 0) << "no contention generated";
}

TEST(RouterVaFabric, SameStreakPacketOrderAcrossHopUnderContention) {
    TwoNodeFabric ch(/*num_vc=*/2);
    constexpr uint8_t kP1 = 0xA1, kP2 = 0xA2, kNoiseTag = 0xCC;
    // node1 injection schedule: noise around and between the streak members.
    std::vector<ni::cmodel::Flit> node1_feed;
    node1_feed.push_back(make_tagged_flit(make_dst(0, 0), 0, 1, kNoiseTag));
    node1_feed.push_back(make_pinned_flit(make_dst(0, 0), /*vc=*/1, /*flit_tail=*/1, kP1));
    node1_feed.push_back(make_tagged_flit(make_dst(0, 0), 0, 1, kNoiseTag));
    node1_feed.push_back(make_pinned_flit(make_dst(0, 0), /*vc=*/1, /*flit_tail=*/1, kP2));
    node1_feed.push_back(make_tagged_flit(make_dst(0, 0), 0, 1, kNoiseTag));
    std::size_t fed = 0;
    std::vector<uint8_t> eject_order;
    for (int t = 0; t < 300 && eject_order.size() < node1_feed.size(); ++t) {
        if (fed < node1_feed.size() && ch.nmu_req_out(1).push_flit(node1_feed[fed])) ++fed;
        ch.nmu_req_out(0).push_flit(
            make_tagged_flit(make_dst(0, 0), 0, 1, kNoiseTag));  // ok if refused
        ch.tick();
        while (auto got = ch.nsu_req_in(0).pop_flit()) {
            const auto s = static_cast<uint8_t>(got->get_header_field("src_id"));
            if (s == kP1 || s == kP2) eject_order.push_back(s);
        }
    }
    auto p1 = std::find(eject_order.begin(), eject_order.end(), kP1);
    auto p2 = std::find(eject_order.begin(), eject_order.end(), kP2);
    ASSERT_TRUE(p1 != eject_order.end() && p2 != eject_order.end())
        << "streak packets did not both arrive";
    EXPECT_LT(p1 - eject_order.begin(), p2 - eject_order.begin())
        << "second streak packet overtook the first";
}

TEST(RouterVaFabric, NoInputFifoOverflowUnderVaDivergenceAtMinimumVcDepth) {
    TwoNodeFabric ch(/*num_vc=*/2, /*vc_depth=*/2);
    int accepted = 0, ejected = 0;
    // Phase 1: eject stalled (never popped) — occupancy pushed to the credit bound.
    for (int t = 0; t < 60; ++t) {
        if (ch.nmu_req_out(1).push_flit(make_flit(make_dst(0, 0), /*vc=*/0, /*flit_tail=*/1)))
            ++accepted;
        ch.tick();
    }
    EXPECT_GT(accepted, 0);
    // Phase 2: drain — every accepted flit must arrive (none lost, no abort).
    for (int t = 0; t < 200 && ejected < accepted; ++t) {
        ch.tick();
        while (ch.nsu_req_in(0).pop_flit().has_value()) ++ejected;
    }
    EXPECT_EQ(ejected, accepted) << "flits lost under VA divergence at minimum vc_depth";
}

TEST(RouterDatapathDeath, BadVcIdAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    Router r(center_cfg());
    EXPECT_DEATH(r.input(static_cast<std::size_t>(RouterPort::WEST))
                     .push_flit(make_flit(make_dst(3, 1), 7, 1)),
                 "vc_id");  // default NUM_VC < 8
}

}  // namespace
