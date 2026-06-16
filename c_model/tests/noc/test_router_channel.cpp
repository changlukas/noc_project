#include "noc/router_channel.hpp"
#include "noc/router.hpp"
#include "common/scenario.hpp"
#include <gtest/gtest.h>

using ni::cmodel::Flit;
using ni::cmodel::noc::InjectAdapter;
using ni::cmodel::noc::Router;
using ni::cmodel::noc::RouterConfig;
using ni::cmodel::noc::RouterPort;

namespace {

RouterConfig cfg_at(uint8_t x, uint8_t y) {
    RouterConfig c;
    c.x = x;
    c.y = y;
    c.num_vc = 2;
    c.vc_depth = 2;
    return c;
}
Flit req_flit(uint8_t dst, uint8_t vc) {
    Flit f;
    f.set_header_field("dst_id", dst);
    f.set_header_field("vc_id", vc);
    f.set_header_field("last", 1);
    return f;
}

TEST(InjectAdapter, CreditMirrorGatesPush) {
    SCENARIO("InjectAdapter: push_flit honors the per-VC credit mirror (seeded to vc_depth)");
    Router r(cfg_at(0, 0));
    InjectAdapter inj(r, static_cast<std::size_t>(RouterPort::LOCAL), /*num_vc=*/2, /*depth=*/2);
    r.set_upstream_credit(static_cast<std::size_t>(RouterPort::LOCAL), inj);
    EXPECT_TRUE(inj.credit_avail(0));
    EXPECT_TRUE(inj.push_flit(req_flit(0, 0)));
    EXPECT_FALSE(inj.push_flit(req_flit(0, 0)))
        << "second push in the same tick must backpressure, not hit the router assert";
}

TEST(InjectAdapter, LandingGuardResetsOnTick) {
    SCENARIO("InjectAdapter: the per-tick push flag resets so the next tick accepts again");
    Router r(cfg_at(0, 0));
    InjectAdapter inj(r, static_cast<std::size_t>(RouterPort::LOCAL), 2, 2);
    r.set_upstream_credit(static_cast<std::size_t>(RouterPort::LOCAL), inj);
    EXPECT_TRUE(inj.push_flit(req_flit(0, 0)));
    EXPECT_FALSE(inj.push_flit(req_flit(0, 0)));
    inj.on_tick();
    r.tick();
    EXPECT_TRUE(inj.push_flit(req_flit(0, 0))) << "new tick: landing free, one push allowed";
}

}  // namespace
