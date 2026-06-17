#include "noc/router.hpp"
#include "flit.hpp"
#include <gtest/gtest.h>

namespace noc = ni::cmodel::noc;
using ni::cmodel::Flit;

static Flit make_flit(uint8_t dst_id, uint8_t vc) {
    Flit f;
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("vc_id", vc);
    f.set_header_field("axi_ch", 0);
    f.set_header_field("last", 1);
    return f;
}

TEST(RouterFrontRoute, EmptyReturnsNullopt) {
    noc::RouterConfig c;
    c.x = 0;
    c.y = 0;
    c.mesh_x_dim = 2;
    c.mesh_y_dim = 1;
    c.num_vc = 1;
    noc::Router r(c);
    EXPECT_EQ(r.num_vc(), 1u);
    EXPECT_FALSE(r.front_route(static_cast<std::size_t>(noc::RouterPort::LOCAL), 0).has_value());
}

TEST(RouterFrontRoute, FrontFlitRoutesEast) {
    noc::RouterConfig c;
    c.x = 0;
    c.y = 0;
    c.mesh_x_dim = 2;
    c.mesh_y_dim = 1;
    c.num_vc = 1;
    noc::Router r(c);
    r.input(static_cast<std::size_t>(noc::RouterPort::LOCAL))
        .push_flit(make_flit(/*dst=(1,0)*/ 0x01, 0));
    r.tick();  // landing -> input FIFO
    auto out = r.front_route(static_cast<std::size_t>(noc::RouterPort::LOCAL), 0);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, noc::RouterPort::EAST);
}
