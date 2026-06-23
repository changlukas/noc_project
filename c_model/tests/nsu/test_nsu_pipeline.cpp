// NSU pipeline staging tests.
//
// Covers both paths:
//   Req path (spec §5.0/§5.3, Task 3): Depacketize S1 + AxiMasterPort S2 = 2 stages.
//   Rsp path (spec §5.0/§5.1/§5.2, Task 4): AxiMasterPort S1 + Packetize S2 +
//     WormholeArbiter+VcArbiter S3 = 3 stages; arbiter-final-stage pattern
//     prevents same-tick Packetize→NoC escape.
#include "axi/types.hpp"
#include "common/scenario.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include "ni/ni_stage.hpp"
#include "nsu/nsu_standalone.hpp"
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

// Seed the MetaBuffer for a B response with the given AXI id.
// Injects an AW flit and ticks once so Depacketize::tick() snapshots the
// MetaBuffer entry (src_id=0x10, rob_req=0, rob_idx=0). The AW beat may
// land in AxiMasterPort's aw_q on subsequent ticks; it does not affect B.
static void seed_meta_for_b(NsuStandalone& nsu, uint8_t id) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_AW);
    f.set_header_field("src_id", 0x10);  // requester → stored as MetaEntry.src_id
    f.set_header_field("dst_id", 0x00);
    f.set_header_field("last", 0);  // AW opens wormhole (last=0)
    f.set_header_field("rob_req", 0);
    f.set_header_field("rob_idx", 0);
    f.set_header_field("vc_id", 0);
    f.set_payload_field("AW", "awid", id);
    f.set_payload_field("AW", "awaddr", 0x1000);
    f.set_payload_field("AW", "awlen", 0);
    f.set_payload_field("AW", "awsize", 2);
    f.set_payload_field("AW", "awburst", static_cast<uint64_t>(axi::Burst::INCR));
    nsu.inject_req_flit(f);
    nsu.tick();  // Depacketize runs: AW → s1_aw_ + MetaBuffer.snapshot_write(id)
}

// Build a B beat for a given AXI id.
static axi::BBeat make_b_beat(uint8_t id) {
    axi::BBeat b{};
    b.id = id;
    b.resp = axi::Resp::OKAY;
    b.user = 0;
    return b;
}

}  // namespace

// -------------------------------------------------------------------------
// Req path (Task 3): 2-stage latency
// -------------------------------------------------------------------------
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

// -------------------------------------------------------------------------
// Rsp path (Task 4): 3-stage latency + arbiter-final-stage (no same-tick escape)
// -------------------------------------------------------------------------
// Stage model (spec §4, §5.2):
//   Cycle 0: push_b() into AxiMasterPort b_q_ (external accept handshake).
//   Tick 1 (S1): axi_master_port_.tick() → forward_b_to_packetizer_()
//                takes ≤1 from b_q_, calls pkt_.push_b() → writes pkt s1_b_.
//                pop_rsp_flit() MUST be empty (no escape).
//   Tick 2 (S2): packetize_.tick() reads s1_b_, builds Flit, pushes to
//                wormhole.input pending queue (S2→S3 boundary).
//                pop_rsp_flit() MUST be empty (arbiter-final-stage: wormhole
//                tick already ran this tick so flit is queued but not drained).
//   Tick 3 (S3): wormhole_.tick() drains pending to vc_arbiter.
//                vc_arbiter_.tick() drains to NullNocRspOut.
//                pop_rsp_flit() returns the B flit. ✓
TEST(NsuPipeline, RspLatencyIsThreeStages) {
    SCENARIO(
        "NSU rsp path: B beat injected at push_b(), appears on NoC rsp-out only "
        "after 3 ticks (3-stage latency); ticks 1-2 must produce no flit "
        "(no same-tick Packetize→NoC escape, spec §5.2 arbiter-final-stage)");
    NsuConfig cfg = make_nsu_cfg();
    NsuStandalone nsu(cfg);

    // Seed MetaBuffer so Packetize can build the B flit.
    // seed_meta_for_b injects AW flit and runs 1 tick to snapshot MetaBuffer.
    seed_meta_for_b(nsu, /*id=*/3);

    // Push B into AxiMasterPort (external accept handshake = cycle 0).
    ASSERT_TRUE(nsu.axi_master_port().push_b(make_b_beat(/*id=*/3)));

    // Ticks 1-2: rsp flit must NOT escape to NoC (no same-tick escape).
    for (int t = 0; t < 2; ++t) {
        nsu.tick();
        EXPECT_FALSE(nsu.pop_rsp_flit().has_value())
            << "Rsp flit must not escape before tick 3 (no-escape at tick " << (t + 1) << ")";
    }

    // Tick 3: S3 wormhole+vc drains the B flit to NullNocRspOut.
    nsu.tick();
    auto flit = nsu.pop_rsp_flit();
    ASSERT_TRUE(flit.has_value()) << "B flit must appear on NoC rsp-out after tick 3";
    EXPECT_EQ(flit->get_header_field("axi_ch"), static_cast<uint64_t>(ni::AXI_CH_B));
    EXPECT_EQ(flit->get_payload_field("B", "bid"), 3u);

    // Verify S1 stage register occupancy probe (stage 0 = Packetize S1 reg).
    // After tick 3, S1 is empty (B advanced through all stages).
    EXPECT_EQ(nsu.stage_occupancy(NiPath::NsuRsp, 0, ni::AXI_CH_B), 0u)
        << "After tick 3: NsuRsp S1 register must be empty (B has exited pipeline)";
}
