#include "router/router.hpp"
#include <gtest/gtest.h>

using ni::cmodel::router::Router;
using ni::cmodel::router::RouterConfig;

TEST(RouterConfigGetters, ExposeConfiguredCapacities) {
    RouterConfig c;
    c.x = 0;
    c.y = 0;
    c.mesh_x_dim = 2;
    c.mesh_y_dim = 1;
    c.num_vc = 2;
    c.vc_depth = 4;
    c.output_fifo_depth = 3;
    Router r(c);
    EXPECT_EQ(r.vc_depth(), 4u);
    EXPECT_EQ(r.output_fifo_depth(), 3u);
}
