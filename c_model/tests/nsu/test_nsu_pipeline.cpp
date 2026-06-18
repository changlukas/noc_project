// NSU request path register-parked staging test.
//
// Spec §5.0/§5.3: Depacketize decodes <=1 req flit/channel into S1
// PipelineStage registers; AxiMasterPort consumes <=1 beat/channel from S1
// (S2). Nsu::tick() req portion runs reverse-order: S2 advance then S1.
//
// ReqLatencyIsTwoStages: inject one AR flit, tick once -> S1 holds AR (not
// yet at slave); tick again -> AR drivable at slave port.
#include "axi/types.hpp"
#include "common/scenario.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include "nmu/ni_stage.hpp"
#include "nsu/nsu.hpp"
#include <gtest/gtest.h>

using ni::cmodel::NiPath;
using ni::cmodel::nsu::NsuConfig;
using ni::cmodel::nsu::NsuStandalone;
namespace axi = ni::cmodel::axi;

namespace {

static NsuConfig make_nsu_cfg() {
    NsuConfig cfg;
    cfg.src_id = 0;
    cfg.port_params.aw_queue_depth = 16;
    cfg.port_params.w_queue_depth = 16;
    cfg.port_params.ar_queue_depth = 16;
    cfg.port_params.b_queue_depth = 16;
    cfg.port_params.r_queue_depth = 16;
    cfg.port_params.depkt_aw_q_depth = 16;
    cfg.port_params.depkt_w_q_depth = 16;
    cfg.port_params.depkt_ar_q_depth = 16;
    cfg.port_params.meta_buffer_per_id_depth = 4;
    return cfg;
}

// Build a minimal AR flit and inject it into the NSU's NoC req-in.
// Models the pattern from test_nsu_depacketize.cpp::make_ar_flit.
static void inject_single_ar_flit(NsuStandalone& nsu, uint8_t id, uint64_t addr) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_AR);
    f.set_header_field("src_id", 0x10);
    f.set_header_field("dst_id", 0x02);
    f.set_header_field("last", 1);
    f.set_header_field("rob_req", 0);
    f.set_header_field("rob_idx", 0);
    f.set_header_field("vc_id", 0);
    f.set_payload_field("AR", "arid", id);
    f.set_payload_field("AR", "araddr", addr);
    f.set_payload_field("AR", "arsize", 5);
    f.set_payload_field("AR", "arburst", static_cast<uint64_t>(axi::Burst::INCR));
    nsu.inject_req_flit(f);
}

}  // namespace

// After one tick: AR flit decoded into S1 register (Depacketize stage).
// stage_occupancy(NsuReq, 0, AXI_CH_AR) == 1; slave port cannot pop_ar yet.
// After second tick: S2 advance moves AR from S1 to AxiMasterPort output;
// slave port can now pop_ar.
TEST(NsuPipeline, ReqLatencyIsTwoStages) {
    SCENARIO(
        "NSU req path: AR flit sits in S1 register after first tick, becomes "
        "poppable at slave port only after second tick (2-stage latency)");
    NsuConfig cfg = make_nsu_cfg();
    NsuStandalone nsu(cfg);

    inject_single_ar_flit(nsu, /*id=*/5, /*addr=*/0x40);

    nsu.tick();  // S1: Depacketize register holds AR; not yet at slave
    EXPECT_EQ(nsu.stage_occupancy(NiPath::NsuReq, 0, ni::AXI_CH_AR), 1u)
        << "After tick 1: S1 register must hold the AR flit";
    EXPECT_FALSE(nsu.axi_master_port().pop_ar().has_value())
        << "After tick 1: AR must not yet be poppable at slave port";

    nsu.tick();  // S2: AR advances from S1 to AxiMasterPort output queue
    EXPECT_TRUE(nsu.axi_master_port().pop_ar().has_value())
        << "After tick 2: AR must be poppable at slave port";
}
