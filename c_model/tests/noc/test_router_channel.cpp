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

TEST(EjectAdapter, BuffersEjectedFlitAndReturnsCredit) {
    SCENARIO("EjectAdapter: router push buffers; pop_flit serves it and returns a credit");
    Router r(cfg_at(0, 0));
    const auto LOCAL = static_cast<std::size_t>(RouterPort::LOCAL);
    InjectAdapter inj(r, LOCAL, 2, 2);
    ni::cmodel::noc::EjectAdapter ej(r, LOCAL, /*depth=*/2);
    r.set_upstream_credit(LOCAL, inj);
    r.set_downstream(LOCAL, ej);
    const std::size_t local_out_seed = r.credit(LOCAL, 0);  // full before any grant
    EXPECT_TRUE(inj.push_flit(req_flit(/*dst=*/0, /*vc=*/0)));
    inj.on_tick();
    r.tick();  // stage1: landing->fifo
    r.tick();  // stage2: grant->output fifo (LOCAL), LOCAL output credit--
    r.tick();  // stage3: output fifo -> downstream (ej buffers)
    EXPECT_EQ(r.credit(LOCAL, 0), local_out_seed - 1) << "LOCAL output credit spent on eject";
    auto out = ej.pop_flit();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->get_header_field("dst_id"), 0u);
    EXPECT_EQ(ej.buffered(), 0u) << "pop drained the only buffered flit";
    EXPECT_EQ(r.credit(LOCAL, 0), local_out_seed)
        << "pop_flit must return the LOCAL output credit to full";
}

TEST(CreditRelay, DecrementThenRelayRestoresUpstreamCredit) {
    SCENARIO(
        "CreditRelay: after the upstream output credit is spent, relay.receive_credit restores it");
    Router up(cfg_at(1, 0));  // node (1,0); a flit to dst=(0,0) routes out WEST
    const auto LOCAL = static_cast<std::size_t>(RouterPort::LOCAL);
    const auto WEST = static_cast<std::size_t>(RouterPort::WEST);
    InjectAdapter inj(up, LOCAL, 2, 2);
    up.set_upstream_credit(LOCAL, inj);
    ni::cmodel::noc::CreditRelay relay(up, WEST);
    const std::size_t seed = up.credit(WEST, 0);
    ASSERT_TRUE(inj.push_flit(req_flit(/*dst=*/0x00, /*vc=*/0)));
    inj.on_tick();
    up.tick();  // stage1
    up.tick();  // stage2: grant -> WEST output FIFO, WEST credit--
    EXPECT_EQ(up.credit(WEST, 0), seed - 1) << "WEST output credit spent on grant";
    relay.receive_credit(0);  // downstream EAST input freed -> relay restores WEST
    EXPECT_EQ(up.credit(WEST, 0), seed) << "relay must restore the upstream WEST output credit";
}

}  // namespace
