#include "noc/router.hpp"
#include "common/scenario.hpp"
#include <gtest/gtest.h>

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

}  // namespace
