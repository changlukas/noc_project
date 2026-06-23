#include "axi/types.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include "nmu/nmu_standalone.hpp"
#include <gtest/gtest.h>

using ni::cmodel::Flit;
using ni::cmodel::NiPath;
using ni::cmodel::nmu::NmuConfig;
using ni::cmodel::nmu::NmuStandalone;
using ni::cmodel::nmu::RobMode;
namespace axi = ni::cmodel::axi;

namespace {

NmuConfig make_nmu_cfg() {
    NmuConfig cfg;
    cfg.src_id = 0x12;
    cfg.port_params.aw_queue_depth = 16;
    cfg.port_params.w_queue_depth = 16;
    cfg.port_params.ar_queue_depth = 16;
    cfg.port_params.b_queue_depth = 16;
    cfg.port_params.r_queue_depth = 16;
    cfg.port_params.depkt_b_q_depth = 16;
    cfg.port_params.depkt_r_q_depth = 16;
    return cfg;
}

axi::ArBeat make_ar_beat(uint8_t id, uint64_t addr) {
    axi::ArBeat ar{};
    ar.id = id;
    ar.addr = addr;
    ar.len = 0;
    ar.size = 5;
    ar.burst = axi::Burst::INCR;
    return ar;
}

Flit make_r_flit(uint8_t id, uint8_t rob_req, uint8_t rob_idx) {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_R);
    f.set_header_field("src_id", 0x01);
    f.set_header_field("dst_id", 0x12);
    f.set_header_field("vc_id", 0);
    f.set_header_field("last", 1);
    f.set_header_field("rob_req", rob_req);
    f.set_header_field("rob_idx", rob_idx);
    f.set_payload_field("R", "rid", id);
    f.set_payload_field("R", "rresp", static_cast<uint64_t>(axi::Resp::OKAY));
    f.set_payload_field("R", "ruser", 0);
    f.set_payload_field("R", "rlast", 1);
    return f;
}

void seed_read_and_inject_r(NmuStandalone& nmu, RobMode mode) {
    constexpr uint8_t kId = 5;
    ASSERT_TRUE(nmu.axi_slave_port().push_ar(make_ar_beat(kId, 0x100)));

    for (int i = 0; i < 3; ++i) {
        nmu.tick();
    }

    ASSERT_TRUE(nmu.pop_req_flit().has_value());
    nmu.inject_rsp_flit(
        make_r_flit(kId, mode == RobMode::Enabled ? 1 : 0, mode == RobMode::Enabled ? 0 : 0));
}

}  // namespace

TEST(NmuPipeline, ReqLatencyIsThreeStages) {
    NmuStandalone nmu(make_nmu_cfg());

    ASSERT_TRUE(nmu.axi_slave_port().push_ar(make_ar_beat(/*id=*/5, /*addr=*/0x100)));

    for (int i = 0; i < 2; ++i) {
        nmu.tick();
        EXPECT_FALSE(nmu.pop_req_flit().has_value())
            << "AR flit must not escape before tick " << (i + 3);
    }

    nmu.tick();
    auto flit = nmu.pop_req_flit();
    ASSERT_TRUE(flit.has_value());
    EXPECT_EQ(flit->get_header_field("axi_ch"), static_cast<uint64_t>(ni::AXI_CH_AR));
    EXPECT_EQ(flit->get_payload_field("AR", "arid"), 5u);
}

TEST(NmuPipeline, RspLatencyRobIsThree) {
    NmuConfig cfg = make_nmu_cfg();
    cfg.read_rob_mode = RobMode::Enabled;
    NmuStandalone nmu(cfg);
    seed_read_and_inject_r(nmu, RobMode::Enabled);

    for (int i = 0; i < 2; ++i) {
        nmu.tick();
        EXPECT_FALSE(nmu.axi_slave_port().pop_r().has_value())
            << "R beat must not be visible before tick " << (i + 3);
    }

    nmu.tick();
    auto r = nmu.axi_slave_port().pop_r();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->id, 5u);
}

TEST(NmuPipeline, RspLatencyRoblessIsTwo) {
    NmuConfig cfg = make_nmu_cfg();
    cfg.read_rob_mode = RobMode::Disabled;
    NmuStandalone nmu(cfg);
    seed_read_and_inject_r(nmu, RobMode::Disabled);

    nmu.tick();
    EXPECT_FALSE(nmu.axi_slave_port().pop_r().has_value());

    nmu.tick();
    auto r = nmu.axi_slave_port().pop_r();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->id, 5u);
}

TEST(NmuPipeline, DepthKnobAddsLatency) {
    NmuConfig cfg = make_nmu_cfg();
    cfg.read_rob_mode = RobMode::Disabled;
    cfg.ni_rsp_extra_depth = 2;
    NmuStandalone nmu(cfg);
    seed_read_and_inject_r(nmu, RobMode::Disabled);

    for (int i = 0; i < 3; ++i) {
        nmu.tick();
        EXPECT_FALSE(nmu.axi_slave_port().pop_r().has_value())
            << "R beat must not be visible before tick " << (i + 2);
    }

    nmu.tick();
    auto r = nmu.axi_slave_port().pop_r();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->id, 5u);
}

// Verify stage_occupancy tracks per-stage advance for the ROB-Enabled rsp path.
// Tick-accurate (Nmu::tick order: drain_s2 → advance_s2 → depkt.tick):
//   T0 inject: depkt.tick fills S0 (depacketize r_q_); S1 still empty.
//   T1: advance_s2 consumes S0 → fills S1; depkt.tick no-op; S0 empty, S1 full.
//   T2: drain_s2 pushes S1 to AxiSlavePort; S1 empty, pop_r() has beat.
TEST(NmuPipeline, StageOccupancyTracksRspAdvance) {
    NmuConfig cfg = make_nmu_cfg();
    cfg.read_rob_mode = RobMode::Enabled;
    NmuStandalone nmu(cfg);

    // Issue AR(id=5), propagate to NoC.
    ASSERT_TRUE(nmu.axi_slave_port().push_ar(make_ar_beat(/*id=*/5, /*addr=*/0x100)));
    for (int i = 0; i < 3; ++i) nmu.tick();
    ASSERT_TRUE(nmu.pop_req_flit().has_value());

    // All rsp stages empty before injection.
    EXPECT_EQ(nmu.stage_occupancy(NiPath::NmuRsp, 0, ni::AXI_CH_R), 0u);
    EXPECT_EQ(nmu.stage_occupancy(NiPath::NmuRsp, 1, ni::AXI_CH_R), 0u);

    // Inject R flit (rob_req=1, rob_idx=0).
    nmu.inject_rsp_flit(make_r_flit(/*id=*/5, /*rob_req=*/1, /*rob_idx=*/0));
    nmu.tick();  // T0: drain/advance no-op (depkt empty); depkt.tick fills S0.

    // After T0: S0 occupied; S1 empty (advance ran before depkt.tick).
    EXPECT_EQ(nmu.stage_occupancy(NiPath::NmuRsp, 0, ni::AXI_CH_R), 1u)
        << "Depacketize deque must hold beat after T0";
    EXPECT_EQ(nmu.stage_occupancy(NiPath::NmuRsp, 1, ni::AXI_CH_R), 0u)
        << "S1 register must be empty at T0 (advance ran before depkt.tick)";

    nmu.tick();  // T1: advance_s2 consumes S0 → fills S1; depkt.tick no-op.
    EXPECT_EQ(nmu.stage_occupancy(NiPath::NmuRsp, 0, ni::AXI_CH_R), 0u)
        << "Depacketize deque must be empty after T1";
    EXPECT_EQ(nmu.stage_occupancy(NiPath::NmuRsp, 1, ni::AXI_CH_R), 1u)
        << "S1 register must be occupied after T1";
    EXPECT_FALSE(nmu.axi_slave_port().pop_r().has_value())
        << "pop_r must be empty at T1 (beat in S1, not yet at AxiSlavePort)";

    nmu.tick();  // T2: drain_s2 pushes S1 to AxiSlavePort; commit_r_exit fires.
    EXPECT_EQ(nmu.stage_occupancy(NiPath::NmuRsp, 1, ni::AXI_CH_R), 0u)
        << "S1 register must be empty after T2";
    EXPECT_TRUE(nmu.axi_slave_port().pop_r().has_value())
        << "R beat must be visible at AxiSlavePort after T2";
}
