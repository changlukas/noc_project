#include "common/router_path.hpp"
#include "router/router.hpp"
#include <gtest/gtest.h>

using ni::cmodel::router::RouterPort;
using ni::cmodel::testing::direction;
using ni::cmodel::testing::node_coord;
using ni::cmodel::testing::node_id;
using ni::cmodel::testing::NodeCoord;
using ni::cmodel::testing::opposite;
using ni::cmodel::testing::router_path;

namespace {
constexpr auto EAST = static_cast<std::size_t>(RouterPort::EAST);
constexpr auto WEST = static_cast<std::size_t>(RouterPort::WEST);
constexpr auto NORTH = static_cast<std::size_t>(RouterPort::NORTH);
constexpr auto SOUTH = static_cast<std::size_t>(RouterPort::SOUTH);
}  // namespace

TEST(RouterPath, NodeIdRoundTrip) {
    // X_WIDTH=4: node (x=1,y=0) -> id 0x01; (x=2,y=3) -> id 0x32.
    EXPECT_EQ(node_id(NodeCoord{1, 0}), 0x01u);
    EXPECT_EQ(node_id(NodeCoord{2, 3}), static_cast<uint8_t>(0x02 | (0x03 << 4)));
    EXPECT_TRUE((node_coord(0x32) == NodeCoord{2, 3}));
}

TEST(RouterPath, TwoNodeOneHopRequest) {
    // node 0 (0,0) -> node 1 (1,0): single EAST hop, two coords.
    auto p = router_path(/*src=*/0x00, /*dst=*/0x01, /*mesh_x=*/2, /*mesh_y=*/1);
    ASSERT_EQ(p.size(), 2u);
    EXPECT_TRUE((p[0] == NodeCoord{0, 0}));
    EXPECT_TRUE((p[1] == NodeCoord{1, 0}));
    EXPECT_EQ(direction(p[0], p[1]), EAST);
    EXPECT_EQ(opposite(direction(p[0], p[1])), WEST);
}

TEST(RouterPath, ResponseIsReversePath) {
    auto fwd = router_path(0x00, 0x01, 2, 1);
    auto rev = router_path(0x01, 0x00, 2, 1);
    ASSERT_EQ(rev.size(), 2u);
    EXPECT_TRUE((rev[0] == NodeCoord{1, 0}));
    EXPECT_TRUE((rev[1] == NodeCoord{0, 0}));
    EXPECT_EQ(direction(rev[0], rev[1]), WEST);
}

TEST(RouterPath, SyntheticMultiHopXThenY) {
    // (0,0) -> (2,1) on a 3x2 mesh: X first to column 2, then Y to row 1.
    // Path: (0,0)->(1,0)->(2,0)->(2,1). 3 hops, 4 coords. Proves mesh-agnostic.
    auto p = router_path(/*src=*/node_id(NodeCoord{0, 0}),
                         /*dst=*/node_id(NodeCoord{2, 1}), /*mesh_x=*/3, /*mesh_y=*/2);
    ASSERT_EQ(p.size(), 4u);
    EXPECT_TRUE((p[0] == NodeCoord{0, 0}));
    EXPECT_TRUE((p[1] == NodeCoord{1, 0}));
    EXPECT_TRUE((p[2] == NodeCoord{2, 0}));
    EXPECT_TRUE((p[3] == NodeCoord{2, 1}));
    EXPECT_EQ(direction(p[0], p[1]), EAST);
    EXPECT_EQ(direction(p[1], p[2]), EAST);
    EXPECT_EQ(direction(p[2], p[3]), NORTH);
}

TEST(RouterPath, SameNodeIsSingleCoordNoHop) {
    auto p = router_path(0x00, 0x00, 2, 1);
    ASSERT_EQ(p.size(), 1u);
    EXPECT_TRUE((p[0] == NodeCoord{0, 0}));
}

TEST(RouterPath, NegativeYIsSouth) {
    EXPECT_EQ(direction(NodeCoord{0, 1}, NodeCoord{0, 0}), SOUTH);
}
