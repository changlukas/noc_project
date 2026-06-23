#include "router/two_node_fabric.hpp"
#include <gtest/gtest.h>

using ni::cmodel::router::testing::TwoNodeFabric;

TEST(TwoNodeFabricAt, CoordinateMapsToSameRouterAsNodeIndex) {
    TwoNodeFabric ch(/*num_vc=*/1);
    // 2x1 mesh: (0,0)=node 0, (1,0)=node 1. By-coordinate accessor must alias
    // the by-node accessor (same Router object).
    EXPECT_EQ(&ch.req_router_at(0, 0), &ch.req_router(0));
    EXPECT_EQ(&ch.req_router_at(1, 0), &ch.req_router(1));
    EXPECT_EQ(&ch.rsp_router_at(0, 0), &ch.rsp_router(0));
    EXPECT_EQ(&ch.rsp_router_at(1, 0), &ch.rsp_router(1));
}
