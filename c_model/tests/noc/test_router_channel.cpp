#include "noc/router.hpp"
#include "noc/router_adapters.hpp"
#include "noc/two_node_fabric.hpp"
#include "common/scenario.hpp"
#include <gtest/gtest.h>

using ni::NI_NOC_ROUTER_VC_DEPTH;
using ni::cmodel::Flit;
using ni::cmodel::noc::InjectAdapter;
using ni::cmodel::noc::Router;
using ni::cmodel::noc::RouterConfig;
using ni::cmodel::noc::RouterPort;
using ni::cmodel::noc::testing::TwoNodeFabric;

namespace {

RouterConfig cfg_at(uint8_t x, uint8_t y) {
    RouterConfig c;
    c.x = x;
    c.y = y;
    c.num_vc = 2;
    c.vc_depth = 2;
    return c;
}
Flit req_flit(uint8_t dst, uint8_t vc, uint8_t tag = 0) {
    Flit f;
    f.set_header_field("dst_id", dst);
    f.set_header_field("vc_id", vc);
    f.set_header_field("last", 1);
    f.set_header_field("src_id", tag);
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

TEST(TwoNodeFabric, SingleFlitReqEndToEnd) {
    SCENARIO("TwoNodeFabric: a REQ flit injected at NMU(1,0) ejects at NSU(0,0) through 2 routers");
    TwoNodeFabric ch(/*num_vc=*/2);
    Flit f = req_flit(/*dst=*/0x00, /*vc=*/0);
    f.set_header_field("src_id", 0x10);
    ASSERT_TRUE(ch.nmu_req_out(/*node=*/1).push_flit(f));
    std::optional<Flit> got;
    for (int t = 0; t < 12 && !got; ++t) {
        ch.tick();
        got = ch.nsu_req_in(/*node=*/0).pop_flit();
    }
    ASSERT_TRUE(got.has_value()) << "request did not arrive at NSU(0,0)";
    EXPECT_EQ(got->get_header_field("dst_id"), 0x00u);
    EXPECT_EQ(got->get_header_field("src_id"), 0x10u);
}

TEST(TwoNodeFabric, SingleFlitRspEndToEnd) {
    SCENARIO("TwoNodeFabric: a RSP flit injected at NSU(0,0) ejects at NMU(1,0) (spec both nets)");
    TwoNodeFabric ch(/*num_vc=*/2);
    Flit f = req_flit(/*dst=*/0x01, /*vc=*/0);  // dst=(1,0)
    f.set_header_field("src_id", 0x20);
    ASSERT_TRUE(ch.nsu_rsp_out(/*node=*/0).push_flit(f));
    std::optional<Flit> got;
    for (int t = 0; t < 12 && !got; ++t) {
        ch.tick();
        got = ch.nmu_rsp_in(/*node=*/1).pop_flit();
    }
    ASSERT_TRUE(got.has_value()) << "response did not arrive at NMU(1,0)";
    EXPECT_EQ(got->get_header_field("dst_id"), 0x01u);
}

TEST(TwoNodeFabric, FullBackpressureWhenConsumerStalls) {
    SCENARIO(
        "TwoNodeFabric: NSU never pops -> credit drains -> NMU inject backpressures (no assert)");
    TwoNodeFabric ch(/*num_vc=*/2);
    int accepted = 0;
    for (int t = 0; t < 200; ++t) {
        Flit f = req_flit(0x00, 0);
        if (ch.nmu_req_out(1).push_flit(f)) ++accepted;
        ch.tick();
        // deliberately do NOT pop at nsu_req_in(0)
    }
    EXPECT_GT(accepted, 0);
    EXPECT_LT(accepted, 200) << "with the consumer stalled, inject must eventually backpressure";
}

TEST(TwoNodeFabric, EjectBoundaryCreditConservation) {
    SCENARIO(
        "TwoNodeFabric: credit(LOCAL,vc) + output_fifo_size(LOCAL) + eject_buffered == vc_depth "
        "every tick");
    constexpr std::size_t kDepth = NI_NOC_ROUTER_VC_DEPTH;
    TwoNodeFabric ch(/*num_vc=*/2, /*vc_depth=*/kDepth);
    const auto LOCAL = static_cast<std::size_t>(RouterPort::LOCAL);
    for (int t = 0; t < 300; ++t) {
        ch.nmu_req_out(1).push_flit(req_flit(0x00, 0));  // ok if backpressured
        ch.tick();
        if (t % 3 == 0) ch.nsu_req_in(0).pop_flit();  // drain 1-in-3
        EXPECT_EQ(ch.req_router(0).credit(LOCAL, 0) + ch.req_router(0).output_fifo_size(LOCAL) +
                      ch.req_eject_buffered(0),
                  kDepth)
            << "eject credit conservation violated at tick " << t;
    }
}

TEST(TwoNodeFabric, EjectQueueHoldsAggregateMultiVcCredit) {
    SCENARIO(
        "TwoNodeFabric: with NSU stalled, the shared eject queue must hold num_vc*vc_depth "
        "flits (not just vc_depth)");
    constexpr std::size_t kDepth = 4;
    TwoNodeFabric ch(/*num_vc=*/2, /*vc_depth=*/kDepth);
    // Alternate vc0/vc1 single-flit requests to dst=(0,0); never pop at NSU(0).
    // Each VC can have up to kDepth flits granted out the LOCAL output before its
    // credit is exhausted -> up to 2*kDepth flits accumulate in the shared eject queue.
    std::size_t pushed = 0;
    for (int t = 0; t < 400; ++t) {
        uint8_t vc = static_cast<uint8_t>(t % 2);
        if (ch.nmu_req_out(1).push_flit(req_flit(0x00, vc))) ++pushed;
        ch.tick();  // must NOT abort (eject overflow) once depth is fixed
        // never pop nsu_req_in(0)
    }
    EXPECT_GT(ch.req_eject_buffered(0), kDepth)
        << "eject queue must hold more than one VC's worth (aggregate num_vc*vc_depth)";
}

// ---------------------------------------------------------------------------
// Link adapters (cross-DPI FlooNoC pulse credit).
// ---------------------------------------------------------------------------

TEST(LinkEjectAdapter, FifoOrderAndNoRouterCredit) {
    SCENARIO("LinkEjectAdapter: push N flits, pop returns them FIFO; pop returns no router credit");
    using ni::cmodel::noc::LinkEjectAdapter;
    constexpr std::size_t kDepth = 4;
    LinkEjectAdapter le(kDepth);
    EXPECT_EQ(le.buffered(), 0u);
    EXPECT_FALSE(le.pop_flit().has_value()) << "empty pop yields nullopt";
    for (std::size_t i = 0; i < kDepth; ++i) {
        le.push_flit(req_flit(/*dst=*/0x00, /*vc=*/0, /*tag=*/static_cast<uint8_t>(i)));
        EXPECT_EQ(le.buffered(), i + 1);
    }
    for (std::size_t i = 0; i < kDepth; ++i) {
        auto f = le.pop_flit();
        ASSERT_TRUE(f.has_value());
        EXPECT_EQ(f->get_header_field("src_id"), i) << "FIFO order preserved";
        EXPECT_EQ(le.buffered(), kDepth - i - 1);
    }
    EXPECT_FALSE(le.pop_flit().has_value());
}

TEST(LinkEjectAdapterDeath, OverflowAssertsAtDepth) {
    SCENARIO("LinkEjectAdapter: pushing past depth trips the overflow assert");
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    using ni::cmodel::noc::LinkEjectAdapter;
    LinkEjectAdapter le(/*depth=*/2);
    le.push_flit(req_flit(0x00, 0));
    le.push_flit(req_flit(0x00, 0));
    EXPECT_DEATH(le.push_flit(req_flit(0x00, 0)), "LinkEjectAdapter overflow");
}

TEST(LinkCreditOut, AccumulatesAndDrainsOnePerTake) {
    SCENARIO(
        "LinkCreditOut: receive_credit accumulates pending; take drains one, false when empty");
    using ni::cmodel::noc::LinkCreditOut;
    LinkCreditOut co(/*num_vc=*/2);
    EXPECT_EQ(co.pending(0), 0u);
    EXPECT_FALSE(co.take(0)) << "no pending -> take returns false";
    co.receive_credit(0);
    co.receive_credit(0);
    co.receive_credit(1);
    EXPECT_EQ(co.pending(0), 2u);
    EXPECT_EQ(co.pending(1), 1u);
    EXPECT_TRUE(co.take(0));
    EXPECT_EQ(co.pending(0), 1u) << "take drains exactly one (no double-count)";
    EXPECT_TRUE(co.take(0));
    EXPECT_EQ(co.pending(0), 0u);
    EXPECT_FALSE(co.take(0)) << "drained -> false";
    EXPECT_EQ(co.pending(1), 1u) << "per-VC independent: vc1 untouched";
    EXPECT_TRUE(co.take(1));
    EXPECT_FALSE(co.take(1));
}

// S2 conservation: a single Router with a LinkEjectAdapter on an output port
// (set_downstream) and a LinkCreditOut on an input port (set_upstream_credit).
// Drive many flits LINK-in -> LOCAL-out while stalling credit return, and assert
// (a) no LinkEjectAdapter overflow, (b) LinkCreditOut.pending accumulates the
// input-drain pulses with no double-count, (c) the output port's credit_ drains
// to 0 and no flit is lost or over-pushed.
TEST(LinkAdapterConservation, StalledCreditDrainsOutputAndConservesFlits) {
    SCENARIO(
        "Router + LinkEjectAdapter(out) + LinkCreditOut(in): stalled credit drains output credit "
        "to 0, no eject overflow, pending == drained, flit count conserved");
    using ni::cmodel::noc::LinkCreditOut;
    using ni::cmodel::noc::LinkEjectAdapter;
    // node (1,0): a flit arriving on the WEST link input addressed to this node's
    // own coord dst=(1,0)=0x01 routes to the LOCAL output (XY DOR, dst==self).
    RouterConfig c;
    c.x = 1;
    c.y = 0;
    c.mesh_x_dim = 2;
    c.mesh_y_dim = 1;
    c.num_vc = 1;
    c.vc_depth = 4;
    Router r(c);
    const auto LOCAL = static_cast<std::size_t>(RouterPort::LOCAL);
    const auto WEST = static_cast<std::size_t>(RouterPort::WEST);

    // LOCAL output buffer must cover the aggregate output credit = num_vc*vc_depth.
    const std::size_t out_seed = r.credit(LOCAL, 0);
    LinkEjectAdapter local_out(static_cast<std::size_t>(c.num_vc) * c.vc_depth);
    LinkCreditOut link_credit(c.num_vc);
    r.set_downstream(LOCAL, local_out);
    r.set_upstream_credit(WEST, link_credit);

    // Drive more flits than can drain (LOCAL output credit never returned -> stalls).
    // The WEST input FIFO has vc_depth slots; push one/tick gated by free space.
    std::size_t pushed = 0;
    std::size_t popped = 0;
    for (int t = 0; t < 60; ++t) {
        if (r.input_fifo_size(WEST, 0) < c.vc_depth) {
            r.input(WEST).push_flit(req_flit(/*dst=*/0x01, /*vc=*/0));
            ++pushed;
        }
        r.tick();
        // never pop local_out -> LOCAL output credit stays drained
        // (a) eject buffer never overflows (would have asserted in push_flit)
    }
    // The single WEST input FIFO can hold vc_depth, the LOCAL output FIFO + eject
    // buffer absorb the granted flits, all gated by the LOCAL output credit.
    EXPECT_EQ(r.credit(LOCAL, 0), 0u) << "(c) LOCAL output credit drained to 0 under stall";
    // (b) every grant out of WEST emitted exactly one credit pulse into LinkCreditOut.
    // Grants == flits that left the WEST input FIFO == out_seed (output credit spent).
    EXPECT_EQ(link_credit.pending(0), out_seed)
        << "(b) one credit pulse per WEST-input drain, no double-count";
    // (c) flit conservation: every pushed flit is accounted for, none lost/over-pushed.
    while (local_out.pop_flit().has_value()) ++popped;
    const std::size_t still_in_west = r.input_fifo_size(WEST, 0);
    const std::size_t still_in_out_fifo = r.output_fifo_size(LOCAL);
    EXPECT_EQ(popped + still_in_west + still_in_out_fifo, pushed)
        << "(c) no flit lost or over-pushed across input FIFO + output FIFO + eject buffer";
    EXPECT_GT(pushed, out_seed) << "test drove more than the credit window (real stall)";
}

}  // namespace
