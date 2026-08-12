// DAT-face NI-edge credit conservation test: the FlooNoC-style credit at the
// NMU DAT NoC terminal edge. S3a T5 moved REQ/RSP off credit onto ready/valid
// (spec §4.3) -- DAT is now the only network this mechanism applies to, so
// this file drives NmuStandalone's DAT accessors directly (mock-push egress,
// mock-inject ingress -- steering to Packetize is T6, per test_nmu_dat_face.cpp).
//
// Three load-bearing properties:
//   (a) Backpressure: with a small seed and no receive_credit, after `seed`
//       AW+W pairs drain the sink reports credit_avail=false and the DAT
//       egress dries up (WormholeArbiter/VcAllocator self-gate — no unbounded
//       growth). This is the conservation guarantee this credit mechanism
//       buys over the old always-grant stub.
//   (b) Re-open: receive_credit pulses re-open the sink one flit per pulse.
//   (c) Multi-consume-per-tick: Depacketize drains >1 flit in one tick, so the
//       consumer pulse MUST be an accumulating counter — take_credit then drains
//       exactly one per call with no loss / double-count.
#include "axi/types.hpp"
#include "common/scenario.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include "nmu/nmu_standalone.hpp"
#include <cstdint>
#include <gtest/gtest.h>

using ni::cmodel::Flit;
using ni::cmodel::nmu::NmuConfig;
using ni::cmodel::nmu::NmuStandalone;
using ni::cmodel::nmu::addr_trans::SamTable;
namespace axi = ni::cmodel::axi;

namespace {

NmuConfig make_cfg(uint8_t src_id) {
    NmuConfig cfg{};
    cfg.src_id = src_id;
    // 16x16 uniform, 4 GB/tile, no rebase: reproduces the retired
    // addr_trans::xy_route mapping so this file's fixed test address (0x100)
    // is unaffected by the migration off xy_route.
    cfg.sam = SamTable::uniform(16, 16, 0x100000000ull);
    // PortParams self-defaults from ni::NMU_QUEUE_DEPTH; set explicitly here
    // for a hermetic, self-documenting test.
    cfg.port_params.aw_queue_depth = 16;
    cfg.port_params.w_queue_depth = 16;
    cfg.port_params.ar_queue_depth = 16;
    cfg.port_params.b_queue_depth = 16;
    cfg.port_params.r_queue_depth = 16;
    cfg.port_params.depkt_b_q_depth = 16;
    cfg.port_params.depkt_r_q_depth = 16;
    return cfg;
}

// Data-class AW opening a 1-beat wormhole packet (flit_tail=0, per the AW/W
// pairing lock -- WormholeArbiter's own comment: "AW=0, W=wlast").
Flit make_data_aw(uint8_t awid, uint8_t dst_id) {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_DataAw);
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("flit_tail", 0);
    f.set_payload_field("AW", "awid", awid);
    f.set_payload_field("AW", "awaddr", 0x100);
    f.set_payload_field("AW", "awlen", 0);
    f.set_payload_field("AW", "awsize", 6);
    f.set_payload_field("AW", "awburst", static_cast<uint64_t>(axi::Burst::INCR));
    return f;
}

Flit make_data_w(uint8_t dst_id) {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_DataW);
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("flit_tail", 1);  // wlast closes the wormhole packet
    f.set_payload_field("DATA_W", "wlast", 1);
    f.set_payload_field("DATA_W", "wstrb", 0xFFu);
    return f;
}

Flit make_data_r(uint8_t rid, uint8_t src_id, uint8_t dst_id) {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_DataR);
    f.set_header_field("src_id", src_id);
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("vc_id", 0);
    f.set_header_field("flit_tail", 1);
    f.set_header_field("ordering_tag", 0);
    f.set_header_field("ordering_req", 0);
    f.set_payload_field("DATA_R", "rid", rid);
    f.set_payload_field("DATA_R", "rresp", static_cast<uint64_t>(axi::Resp::OKAY));
    f.set_payload_field("DATA_R", "ruser", 0);
    f.set_payload_field("DATA_R", "rlast", 1);
    return f;
}

}  // namespace

// (a) + (b): credit-gated backpressure and re-open on the DAT egress. Each
// mock-pushed AW+W pair (a 2-flit worm) is one "unit"; the credit seed is
// counted in flits (AW consumes one credit, W consumes the next).
TEST(NmuDatCreditConservation, BackpressureStallsAtSeedThenReopens) {
    SCENARIO(
        "NMU DAT credit ON, seed=3: push 4 AW+W pairs (8 flits) directly into "
        "dat_wormhole_arbiter, no receive_credit. After 3 flits drain, "
        "dat_req_credit_avail goes false and pop_dat_req_flit dries up. Then 2 "
        "receive_credit pulses release exactly 2 more flits.");

    constexpr uint8_t kSrcId = 0x12;
    constexpr std::size_t kSeed = 3;
    constexpr int kPairs = 4;  // 8 flits > seed, so the surplus is held by backpressure

    NmuStandalone nmu(make_cfg(kSrcId));
    nmu.enable_dat_noc_credit(kSeed);
    ASSERT_TRUE(nmu.dat_req_credit_avail()) << "fresh seed>0 must grant credit";

    for (int i = 0; i < kPairs; ++i) {
        ASSERT_TRUE(nmu.nmu().dat_wormhole_arbiter().input(0).push_flit(
            make_data_aw(static_cast<uint8_t>(i), 0x01)));
        ASSERT_TRUE(nmu.nmu().dat_wormhole_arbiter().input(1).push_flit(make_data_w(0x01)));
    }

    // Pump. Without receive_credit the sink grants at most `seed` flits total,
    // ever. Drain everything the sink releases over a generous tick window.
    int drained = 0;
    for (int t = 0; t < 64; ++t) {
        nmu.tick();
        while (nmu.pop_dat_req_flit()) ++drained;
    }

    EXPECT_EQ(drained, static_cast<int>(kSeed))
        << "credit-gated sink must release exactly `seed` flits with no replenish";
    EXPECT_FALSE(nmu.dat_req_credit_avail())
        << "after draining the full seed the sink must backpressure (credit exhausted)";
    EXPECT_FALSE(nmu.pop_dat_req_flit().has_value())
        << "no further flits while credit is exhausted";

    // (b) Re-open: each receive_credit pulse releases exactly one more flit.
    nmu.dat_req_receive_credit();
    nmu.dat_req_receive_credit();
    EXPECT_TRUE(nmu.dat_req_credit_avail()) << "receive_credit must re-open the sink";

    int reopened = 0;
    for (int t = 0; t < 64; ++t) {
        nmu.tick();
        while (nmu.pop_dat_req_flit()) ++reopened;
    }
    EXPECT_EQ(reopened, 2) << "two receive_credit pulses release exactly two held flits";
    EXPECT_FALSE(nmu.dat_req_credit_avail()) << "credit exhausted again after the two pulses";
    EXPECT_EQ(drained + reopened, static_cast<int>(kSeed) + 2)
        << "conservation: total released == seed + pulses (no loss, no double-grant)";
}

// (c) Multi-consume-per-tick: Depacketize drains its whole DAT ingress queue
// in one tick(), so several injected DataR flits are consumed in a single
// tick. The consumer pulse must accumulate ALL of them; take_credit then
// drains exactly one per call.
TEST(NmuDatCreditConservation, ConsumerPulseAccumulatesMultiConsumePerTick) {
    SCENARIO(
        "Issue 5 AR reads on REQ (so the Rob has outstanding entries), then inject the 5 "
        "matching R(last) rsp flits via inject_dat_rsp_flit (DAT ingress) and run a SINGLE "
        "nmu.tick(): Depacketize dequeues all 5 in that one tick. dat_rsp_take_credit must "
        "then drain exactly 5 pulses (one per consumed flit) and no more — proving the pulse "
        "is an accumulating counter, not a one-bit latch.");

    constexpr uint8_t kSrcId = 0x12;
    constexpr int kRspFlits = 5;
    NmuStandalone nmu(make_cfg(kSrcId));

    // Issue 5 distinct-id reads and drain them off the REQ face (unaffected by
    // this file's DAT-credit scope). Draining an AR flit registers the read as
    // outstanding in the Rob, so the later R(last) responses are well-formed.
    for (int i = 0; i < kRspFlits; ++i) {
        axi::ArBeat ar{};
        ar.id = static_cast<uint8_t>(i);
        ar.addr = 0x100;
        ar.len = 0;
        ar.size = 2;
        ar.burst = axi::Burst::INCR;
        ASSERT_TRUE(nmu.axi_slave_port().push_ar(ar));
    }
    for (int t = 0; t < 64; ++t) {
        nmu.tick();
        while (nmu.pop_req_flit()) {
        }
    }
    // Drain any consumer pulses incidentally produced above so the DAT-side
    // pulse count below is clean.
    while (nmu.dat_rsp_take_credit()) {
    }

    // Inject all 5 R(last) responses BEFORE ticking, so a single Depacketize.tick()
    // drains the whole DAT ingress queue (depkt_r_q depth 16 > 5).
    for (int i = 0; i < kRspFlits; ++i) {
        nmu.inject_dat_rsp_flit(make_data_r(static_cast<uint8_t>(i), /*src_id=*/0x00, kSrcId));
    }

    // Single tick: Depacketize.tick() drains the entire ingress queue in one
    // pass (while-loop). Each consumed flit bumps the consumer pulse counter.
    nmu.tick();

    int pulses = 0;
    while (nmu.dat_rsp_take_credit()) ++pulses;
    EXPECT_EQ(pulses, kRspFlits)
        << "consumer pulse must accumulate every flit consumed in the tick (no loss / "
           "double-count); a one-bit latch would report only 1";
    EXPECT_FALSE(nmu.dat_rsp_take_credit()) << "drained to empty — no double-count";
}
