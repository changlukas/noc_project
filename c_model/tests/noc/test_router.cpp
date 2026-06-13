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

TEST(RouterDatapathDeath, BadVcIdAborts) {
    SCENARIO("Router: input flit vc_id >= num_vc -> assert+abort (spec §9)");
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    Router r(center_cfg());
    EXPECT_DEATH(r.input(static_cast<std::size_t>(RouterPort::WEST))
                     .push_flit(make_flit(make_dst(3, 1), 7, 1)),
                 "vc_id");  // default NUM_VC < 8
}

}  // namespace
