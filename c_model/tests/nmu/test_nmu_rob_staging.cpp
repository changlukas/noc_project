// §6.1 deferral-gap regression: ROB-Enabled notify_drained timing.
//
// The one-beat-release contract (spec §6.1): when an out-of-order R response
// arrives and fills the RoB's committed_r_queue_ (chain-flush), the VcArbiter
// binding and slot are released only at the tick the S2 register drains to
// AxiSlavePort via push_r_staged → commit_r_exit → notify_drained. They must
// NOT release during the chain-flush inside pop_r_staged().
//
// Non-vacuous: the fault-injection variant (build with -DTEST_FAULT_INJECT=1)
// wires notify_drained into pop_r_staged's chain-flush and confirms the test
// FAILS under the pre-§6.1 bug.
//
// References: docs/superpowers/specs/ §6.1; commit history on feat/ni-pipeline-model.
#include "axi/types.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include "nmu/nmu.hpp"
#include <gtest/gtest.h>

using ni::cmodel::Flit;
using ni::cmodel::NiPath;
using ni::cmodel::nmu::NmuConfig;
using ni::cmodel::nmu::NmuStandalone;
using ni::cmodel::nmu::RobMode;
namespace axi = ni::cmodel::axi;

namespace {

NmuConfig make_rob_enabled_cfg() {
    NmuConfig cfg;
    cfg.src_id = 0x12;
    cfg.read_rob_mode = RobMode::Enabled;
    cfg.port_params.aw_queue_depth = 16;
    cfg.port_params.w_queue_depth = 16;
    cfg.port_params.ar_queue_depth = 16;
    cfg.port_params.b_queue_depth = 16;
    cfg.port_params.r_queue_depth = 16;
    cfg.port_params.depkt_b_q_depth = 16;
    cfg.port_params.depkt_r_q_depth = 16;
    return cfg;
}

axi::ArBeat make_ar(uint8_t id, uint64_t addr) {
    axi::ArBeat ar{};
    ar.id = id;
    ar.addr = addr;
    ar.len = 0;
    ar.size = 5;
    ar.burst = axi::Burst::INCR;
    return ar;
}

Flit make_r_flit(uint8_t id, uint8_t rob_idx) {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_R);
    f.set_header_field("src_id", 0x01);
    f.set_header_field("dst_id", 0x12);
    f.set_header_field("vc_id", 0);
    f.set_header_field("last", 1);
    f.set_header_field("rob_req", 1);
    f.set_header_field("rob_idx", rob_idx);
    f.set_payload_field("R", "rid", id);
    f.set_payload_field("R", "rresp", static_cast<uint64_t>(axi::Resp::OKAY));
    f.set_payload_field("R", "ruser", 0);
    f.set_payload_field("R", "rlast", 1);
    return f;
}

}  // namespace

// §6.1 deferral-gap: notify_drained fires at S2-drain tick, not chain-flush tick.
//
// Tick-accurate sequence (Nmu::tick() order: drain_s2 → advance_s2 → depkt.tick):
//   Setup: AR(id=5) admitted, propagates to NoC in 3 ticks.
//   T0: inject R flit. tick() → drain_s2(no-op) → advance_s2(no-op, depkt r_q_ empty)
//       → depkt.tick fills r_q_.
//       After T0: stage_occupancy(NmuRsp,0,R)==1, stage_occupancy(NmuRsp,1,R)==0.
//   T1: tick() → drain_s2(no-op, s2 empty) → advance_s2: pop_r_staged() drains
//       r_q_, does chain-flush into committed_r_queue_, fills s2_rsp_r_.
//       → depkt.tick(no-op). notify_drained NOT yet fired (commit_r_exit not called).
//       After T1: stage_occupancy(NmuRsp,0,R)==0, stage_occupancy(NmuRsp,1,R)==1;
//       pop_r() still empty (beat not yet at AxiSlavePort).
//   T2: tick() → drain_s2: push_rsp_r_to_axi_ → push_r_staged succeeds →
//       commit_r_exit → notify_drained. s2_rsp_r_ cleared.
//       After T2: stage_occupancy(NmuRsp,1,R)==0; pop_r() has the beat.
//
// The key assertion: at T1 end the chain-flush has run (committed_r_queue_ flushed)
// but pop_r() is still empty, proving notify_drained has NOT fired during the
// chain-flush. It fires only at T2 when S1 drains to AxiSlavePort.
TEST(NmuRobStaging, DeferralGapS2DrainIsNotifyBoundary) {
    NmuStandalone nmu(make_rob_enabled_cfg());

    // Issue one AR(id=5). In ROB-Enabled mode this allocates rob_idx=0.
    ASSERT_TRUE(nmu.axi_slave_port().push_ar(make_ar(/*id=*/5, /*addr=*/0x1000)));

    // Propagate through 3-stage req path: S0→S1 (tick1), S1→S2 (tick2),
    // S2→NoC (tick3). The flit should be visible after tick3.
    for (int i = 0; i < 3; ++i) nmu.tick();
    auto req_flit = nmu.pop_req_flit();
    ASSERT_TRUE(req_flit.has_value()) << "AR flit must exit NMU after 3 ticks";
    EXPECT_EQ(req_flit->get_header_field("axi_ch"), static_cast<uint64_t>(ni::AXI_CH_AR));
    // Confirm rob_idx=0 was assigned (slot 0 is first free in empty pool).
    EXPECT_EQ(req_flit->get_header_field("rob_idx"), 0u);

    // T0: inject R flit. depkt fills r_q_ at end of tick (drain/advance run first).
    nmu.inject_rsp_flit(make_r_flit(/*id=*/5, /*rob_idx=*/0));
    nmu.tick();

    // After T0: depacketize r_q_ has the beat (S0 occupied); s2_rsp_r_ still empty.
    EXPECT_EQ(nmu.stage_occupancy(NiPath::NmuRsp, /*stage=*/0, ni::AXI_CH_R), 1u)
        << "Depacketize deque must hold 1 beat after T0";
    EXPECT_EQ(nmu.stage_occupancy(NiPath::NmuRsp, /*stage=*/1, ni::AXI_CH_R), 0u)
        << "S1 register must be empty at T0 (advance_s2 ran before depkt.tick)";
    EXPECT_FALSE(nmu.axi_slave_port().pop_r().has_value())
        << "pop_r must be empty at T0 — beat is in depkt queue, not yet forwarded";

    // T1: advance_s2 calls pop_r_staged() → chain-flush into committed_r_queue_ →
    // s2_rsp_r_ filled. notify_drained NOT yet called (commit_r_exit not reached).
    nmu.tick();

    // After T1: S0 empty (consumed by advance_s2); S1 occupied; pop_r() still empty.
    // This is the critical §6.1 assertion: chain-flush has run but notify_drained
    // has NOT fired yet — beat is in S1 register, not at AxiSlavePort.
    EXPECT_EQ(nmu.stage_occupancy(NiPath::NmuRsp, /*stage=*/0, ni::AXI_CH_R), 0u)
        << "Depacketize deque must be empty after T1 (consumed by advance_s2)";
    EXPECT_EQ(nmu.stage_occupancy(NiPath::NmuRsp, /*stage=*/1, ni::AXI_CH_R), 1u)
        << "S1 register must be occupied after T1 (chain-flush filled it)";
    EXPECT_FALSE(nmu.axi_slave_port().pop_r().has_value())
        << "pop_r must be empty at T1 — S1 has not drained yet, "
           "proving notify_drained has NOT fired during chain-flush";

    // T2: drain_s2 pushes S1 to AxiSlavePort → commit_r_exit → notify_drained.
    nmu.tick();

    // After T2: S1 is empty; pop_r() has the beat — notify_drained fired this tick.
    EXPECT_EQ(nmu.stage_occupancy(NiPath::NmuRsp, /*stage=*/1, ni::AXI_CH_R), 0u)
        << "S1 register must be empty after T2 (drained to AxiSlavePort)";
    auto r = nmu.axi_slave_port().pop_r();
    ASSERT_TRUE(r.has_value()) << "R beat must be visible after T2 (S1 drained)";
    EXPECT_EQ(r->id, 5u);
}
