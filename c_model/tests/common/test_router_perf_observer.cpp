#include "common/router_perf_observer.hpp"
#include "common/perf_common.hpp"
#include "noc/router.hpp"
#include "flit.hpp"
#include <gtest/gtest.h>

namespace noc = ni::cmodel::noc;
using ni::cmodel::Flit;
using ni::cmodel::testing::PhaseConfig;
using ni::cmodel::testing::PhaseController;
using ni::cmodel::testing::RouterPerfConfig;
using ni::cmodel::testing::RouterPerfObserver;

static Flit make_flit(uint8_t dst_id) {
    Flit f;
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("vc_id", 0);
    f.set_header_field("axi_ch", 0);
    f.set_header_field("last", 1);
    return f;
}

TEST(RouterPerfObserver, IdleRouterZeroStall) {
    noc::RouterConfig c;
    c.x = 0;
    c.y = 0;
    c.mesh_x_dim = 2;
    c.mesh_y_dim = 1;
    c.num_vc = 1;
    noc::Router r(c);
    uint64_t now = 0;
    PhaseController phase(now, PhaseConfig{});
    RouterPerfObserver obs(now, phase, {{"req0", &r}}, RouterPerfConfig{});
    obs.sample();
    EXPECT_EQ(obs.credit_stall_cycles(), 0u);
}

TEST(RouterPerfObserver, FrontFlitNoCreditAccruesStall) {
    // Deterministic stall: vc_depth=1, output_fifo_depth=1, EAST has no
    // downstream wired. flit1 consumes the single EAST credit (1->0) and parks
    // in the output FIFO (stage-3 finds no downstream, so no credit return).
    // flit2 then sits in the input FIFO routed EAST with credit(EAST,0)==0.
    noc::RouterConfig c;
    c.x = 0;
    c.y = 0;
    c.mesh_x_dim = 2;
    c.mesh_y_dim = 1;
    c.num_vc = 1;
    c.vc_depth = 1;
    c.output_fifo_depth = 1;
    noc::Router r(c);
    const auto LOCAL = static_cast<std::size_t>(noc::RouterPort::LOCAL);

    r.input(LOCAL).push_flit(make_flit(0x01));  // flit1, dst (1,0) -> EAST
    r.tick();                                   // stage1: flit1 landing -> input FIFO
    r.tick();  // stage2: flit1 granted, EAST credit 1->0, parks in output FIFO

    uint64_t now = 0;
    PhaseController phase(now, PhaseConfig{});
    RouterPerfObserver obs(now, phase, {{"req0", &r}}, RouterPerfConfig{});

    r.input(LOCAL).push_flit(make_flit(0x01));  // flit2, dst EAST
    r.tick();  // flit2 -> input FIFO; cannot be granted (credit(EAST,0)==0)
    obs.sample();
    EXPECT_GT(obs.credit_stall_cycles(), 0u);
}
