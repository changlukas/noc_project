// Smoke test: Nmu class constructs cleanly + tick() doesn't crash.
// Verifies ctor sequence (member init order, sub-module ref
// dependencies) in isolation. Does
// NOT exercise full e2e flow; that's integration testbench.
#include "axi/types.hpp"
#include "common/channel_model.hpp"
#include "common/scenario.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include "nmu/nmu_standalone.hpp"
#include "router/null_adapters.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <gtest/gtest.h>

using ni::cmodel::Flit;
using ni::cmodel::nmu::Nmu;
using ni::cmodel::nmu::NmuConfig;
using ni::cmodel::nmu::NmuStandalone;
using ni::cmodel::nmu::addr_trans::SamTable;
using ni::cmodel::testing::ChannelModel;
namespace axi = ni::cmodel::axi;

// 16x16 uniform, 4 GB/tile, no rebase: reproduces the retired
// addr_trans::xy_route mapping (dst = addr[39:32], local_addr = addr
// unchanged) so this file's fixed test addresses are unaffected by the
// migration off xy_route.
SamTable legacy_sam() {
    return SamTable::uniform(16, 16, 0x100000000ull);
}

TEST(NmuTopLevel, ConstructsAndTicksWithoutCrash) {
    SCENARIO(
        "Nmu top-level smoke: NUM_VC=1 default config, construct + tick 10x "
        "should not crash. Verifies ctor sequence + member init order.");
    ChannelModel channel(/*req*/ 64, /*rsp*/ 64);
    NmuConfig cfg{};
    cfg.src_id = 0x12;
    cfg.sam = legacy_sam();
    cfg.port_params.aw_queue_depth = 16;
    cfg.port_params.w_queue_depth = 16;
    cfg.port_params.ar_queue_depth = 16;
    cfg.port_params.b_queue_depth = 16;
    cfg.port_params.r_queue_depth = 16;
    cfg.port_params.depkt_b_q_depth = 16;
    cfg.port_params.depkt_r_q_depth = 16;
    // DAT face (S3a T4): unused by this smoke test, wired to the shared
    // null sentinel (router/null_adapters.hpp).
    Nmu nmu(cfg, channel.nmu_req_out(), channel.nmu_rsp_in(), ni::cmodel::router::null_req_out(),
            ni::cmodel::router::null_rsp_in());

    EXPECT_EQ(&nmu.axi_slave_port(), &nmu.axi_slave_port())
        << "axi_slave_port() returns stable reference";

    for (int i = 0; i < 10; ++i) {
        nmu.tick();
        channel.tick();
    }
    SUCCEED();  // reaching here means no abort during ctor or tick
}

// Write round-trip e2e: AW + W in via AxiSlavePort, observe AW + W flits
// on the NoC req-out face, inject a synthetic B flit on the NoC rsp-in
// face, observe the B beat at the AxiSlavePort response side.
//
// Pinpoints: member-declaration order, tick-order (depacketize before
// axi_slave_port, then wormhole, then vc_allocator), Packetize -> Wormhole
// -> VcAllocator -> NocReqOut wiring, and the symmetric
// NocRspIn -> Depacketize -> AxiSlavePort return path inside the
// assembled pipeline. Uses NmuStandalone so the test does not depend on
// the ChannelModel / NSU side; any break in Nmu-internal wiring surfaces
// here even if the integration testbench's harness happens to mask it.
TEST(NmuTopLevel, WriteRoundTripProducesReqFlitsAndObservesBResp) {
    SCENARIO(
        "Nmu write round-trip: push AW+W into AxiSlavePort, drain AW+W "
        "flits from null NoC req-out, inject synthetic B flit into null "
        "NoC rsp-in, expect BBeat at AxiSlavePort.pop_b(). Regression "
        "gate for assembled-pipeline wiring + tick order.");

    constexpr uint8_t kSrcId = 0x12;
    constexpr uint8_t kAxiId = 0x05;
    constexpr uint64_t kAddr = 0x100;  // dst_id = (0x100 >> 32) & 0xff = 0

    NmuConfig cfg{};
    cfg.src_id = kSrcId;
    cfg.sam = legacy_sam();
    // PortParams self-defaults from ni::NMU_QUEUE_DEPTH (see
    // nmu/port_params.hpp); set explicitly here for a hermetic, self-documenting test.
    cfg.port_params.aw_queue_depth = 16;
    cfg.port_params.w_queue_depth = 16;
    cfg.port_params.ar_queue_depth = 16;
    cfg.port_params.b_queue_depth = 16;
    cfg.port_params.r_queue_depth = 16;
    cfg.port_params.depkt_b_q_depth = 16;
    cfg.port_params.depkt_r_q_depth = 16;
    NmuStandalone nmu(cfg);

    // Push one 1-beat write transaction into the upstream AXI face.
    axi::AwBeat aw{};
    aw.id = kAxiId;
    aw.addr = kAddr;
    aw.len = 0;  // 1 beat
    aw.size = 2;
    aw.burst = axi::Burst::INCR;
    ASSERT_TRUE(nmu.axi_slave_port().push_aw(aw));
    axi::WBeat w{};
    w.strb = 0xF;
    w.last = true;
    ASSERT_TRUE(nmu.axi_slave_port().push_w(w));

    // Drain the DAT-out face (S3a T6: Data-class AW/W steer to DAT, spec
    // :348 -- legacy_sam() is data class). Bounded loop: pipeline is
    // AxiSlavePort -> Rob -> Packetize -> WormholeArbiter -> VcAllocator
    // -> QueueNocReqOut, each tick boundary forwards one stage. 32 ticks
    // is generous; any breakage stalls indefinitely and trips the loop bound.
    bool saw_aw_flit = false;
    bool saw_w_flit = false;
    for (int i = 0; i < 32 && !(saw_aw_flit && saw_w_flit); ++i) {
        nmu.tick();
        while (auto f = nmu.pop_dat_req_flit()) {
            uint64_t ch = f->get_header_field("axi_ch");
            uint64_t src = f->get_header_field("src_id");
            EXPECT_EQ(src, kSrcId) << "req flit src_id should match NmuConfig.src_id";
            if (ch == ni::AXI_CH_DataAw) {
                EXPECT_EQ(f->get_payload_field("AW", "awid"), kAxiId);
                EXPECT_EQ(f->get_payload_field("AW", "awaddr"), kAddr);
                saw_aw_flit = true;
            } else if (ch == ni::AXI_CH_DataW) {
                EXPECT_EQ(f->get_payload_field("DATA_W", "wlast"), 1u);
                saw_w_flit = true;
            } else {
                ADD_FAILURE() << "unexpected DAT flit axi_ch=" << ch << " (expected AW or W)";
            }
        }
    }
    ASSERT_TRUE(saw_aw_flit) << "Nmu never produced AW flit on NoC DAT-out face";
    ASSERT_TRUE(saw_w_flit) << "Nmu never produced W flit on NoC DAT-out face";

    // Inject a synthetic B response flit. The NMU Depacketize tick reads
    // axi_ch + bid + bresp + buser; src_id/dst_id/flit_tail are honored but
    // not consumed by the AxiSlavePort drain.
    Flit b_flit;
    b_flit.set_header_field("axi_ch", ni::AXI_CH_DataB);
    b_flit.set_header_field("src_id", 0x00);
    b_flit.set_header_field("dst_id", kSrcId);
    b_flit.set_header_field("vc_id", 0);
    b_flit.set_header_field("flit_tail", 1);
    b_flit.set_payload_field("B", "bid", kAxiId);
    b_flit.set_payload_field("B", "bresp", static_cast<uint64_t>(axi::Resp::OKAY));
    b_flit.set_payload_field("B", "buser", 0);
    nmu.inject_rsp_flit(b_flit);

    // Drain the response side. NMU rsp is a 2-stage (ROBLESS) or 3-stage (ROB)
    // pipeline: Depacketize → (Rob stage) → AxiSlavePort. The flit needs
    // multiple ticks to traverse the stages; loop up to 8 for slack.
    std::optional<axi::BBeat> b_out;
    for (int i = 0; i < 8 && !b_out; ++i) {
        nmu.tick();
        b_out = nmu.axi_slave_port().pop_b();
    }
    ASSERT_TRUE(b_out.has_value()) << "Nmu never surfaced B beat to AxiSlavePort";
    EXPECT_EQ(b_out->id, kAxiId);
    EXPECT_EQ(b_out->resp, axi::Resp::OKAY);
}

// === Transaction retire point, at the assembled-pipeline level ===
//
// The release point is only observable here. Rob-level tests see the id released
// at pop_r(); inside Nmu the response still has to cross s2_rsp_r_, the optional
// extra shift stages and the slave-port output queue before the master sees it
// (nmu.hpp:388,335,321). Releasing at the Rob exit instead of at AXI-side
// acceptance would let the NI admit the next same-id read while the completed
// response is still in flight.

TEST(NmuOutstandingCount, RoblessReadRetiresAtTheAxiSideAndReopensTheId) {
    SCENARIO(
        "Nmu RoBless reads: the per-id single-outstanding interlock survives the pop out of "
        "the Rob and is released when the R beat is accepted at the AXI side, after which "
        "the next AR on that id is admitted.");

    constexpr uint8_t kSrcId = 0x12;
    constexpr uint8_t kAxiId = 0x07;
    constexpr uint64_t kAddr = 0x200;

    NmuConfig cfg{};
    cfg.src_id = kSrcId;
    cfg.sam = legacy_sam();
    cfg.read_rob_mode = ni::cmodel::nmu::RobMode::Disabled;
    cfg.port_params.aw_queue_depth = 16;
    cfg.port_params.w_queue_depth = 16;
    cfg.port_params.ar_queue_depth = 16;
    cfg.port_params.b_queue_depth = 16;
    cfg.port_params.r_queue_depth = 16;
    cfg.port_params.depkt_b_q_depth = 16;
    cfg.port_params.depkt_r_q_depth = 16;
    NmuStandalone nmu(cfg);

    axi::ArBeat ar{};
    ar.id = kAxiId;
    ar.addr = kAddr;
    ar.len = 0;
    ar.size = 2;
    ar.burst = axi::Burst::INCR;
    ASSERT_TRUE(nmu.axi_slave_port().push_ar(ar));

    bool saw_ar = false;
    for (int i = 0; i < 32 && !saw_ar; ++i) {
        nmu.tick();
        while (auto f = nmu.pop_req_flit()) {
            if (f->get_header_field("axi_ch") == ni::AXI_CH_DataAr) saw_ar = true;
        }
    }
    ASSERT_TRUE(saw_ar);
    EXPECT_EQ(nmu.rob().read_txns(), 1u);

    // Response class must match the request's (legacy_sam() -> data class);
    // Rob's RoBless path recovers the AR basis keyed by class, and a mismatch
    // here would abort on a missing narrow AR-meta entry.
    Flit r;
    r.set_header_field("axi_ch", ni::AXI_CH_DataR);
    r.set_header_field("src_id", 0x00);
    r.set_header_field("dst_id", kSrcId);
    r.set_header_field("vc_id", 0);
    r.set_header_field("flit_tail", 1);
    r.set_header_field("ordering_req", 0);
    r.set_header_field("ordering_tag", 0);
    r.set_payload_field("DATA_R", "rid", kAxiId);
    r.set_payload_field("DATA_R", "rresp", static_cast<uint64_t>(axi::Resp::OKAY));
    r.set_payload_field("DATA_R", "rlast", 1u);
    nmu.inject_rsp_flit(r);

    std::optional<axi::RBeat> r_out;
    for (int i = 0; i < 8 && !r_out; ++i) {
        nmu.tick();
        r_out = nmu.axi_slave_port().pop_r();
    }
    ASSERT_TRUE(r_out.has_value()) << "RoBless R never reached the AXI side";
    EXPECT_EQ(nmu.rob().read_txns(), 0u) << "the transaction retires with the AXI-side acceptance";

    // The id's interlock is clear again, so the next read on it is admitted.
    ASSERT_TRUE(nmu.axi_slave_port().push_ar(ar));
    bool saw_second_ar = false;
    for (int i = 0; i < 32 && !saw_second_ar; ++i) {
        nmu.tick();
        while (auto f = nmu.pop_req_flit()) {
            if (f->get_header_field("axi_ch") == ni::AXI_CH_DataAr) saw_second_ar = true;
        }
    }
    EXPECT_TRUE(saw_second_ar) << "the retired transaction never reopened the id";
}

TEST(NmuTopLevel, ReadReorderBufferIsOnByDefault) {
    // docs/noc-target-spec.md section 3 puts a reorder buffer on the response
    // path. A default-constructed config is what every test and the plain DPI
    // entry point get, so this is the one that decides which path they run.
    NmuConfig cfg;
    EXPECT_EQ(cfg.read_rob_mode, ni::cmodel::nmu::RobMode::Enabled);
}
