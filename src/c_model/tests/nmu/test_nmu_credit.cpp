// R2 Task 1 conservation test: the opt-in FlooNoC credit at the NMU NoC
// terminal edge. Drives a NmuStandalone with credit ENABLED (the cosim path;
// the default-OFF path is exercised implicitly by every other NmuStandalone
// test, which must stay byte-identical to today).
//
// Three load-bearing properties:
//   (a) Backpressure: with a small seed and no receive_credit, after `seed`
//       req flits drain the sink reports credit_avail=false, pop_req_flit dries
//       up, and the internal queues stay bounded (VcArbiter self-gates — no
//       unbounded growth). This is the conservation guarantee that R2 buys over
//       the old always-grant stub.
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
namespace axi = ni::cmodel::axi;

namespace {

NmuConfig make_cfg(uint8_t src_id) {
    NmuConfig cfg{};
    cfg.src_id = src_id;
    // PortParams has no defaults ("fail loud"); set the depths this test drives.
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
    ar.len = 0;  // 1 beat -> single AR flit
    ar.size = 2;
    ar.burst = axi::Burst::INCR;
    return ar;
}

}  // namespace

// (a) + (b): credit-gated backpressure and re-open. AR transactions each
// produce exactly one AR req flit, so the produced-flit count is directly the
// number of reads pushed.
TEST(NmuCreditConservation, BackpressureStallsAtSeedThenReopens) {
    SCENARIO(
        "NMU credit ON, seed=3: push 8 AR reads, no receive_credit. After 3 "
        "flits drain, credit_avail goes false and pop_req_flit dries up "
        "(VcArbiter self-gates, queues bounded). Then 2 receive_credit pulses "
        "release exactly 2 more flits.");

    constexpr uint8_t kSrcId = 0x12;
    constexpr std::size_t kSeed = 3;
    constexpr int kReads = 8;  // > seed, so the surplus is held by backpressure

    NmuStandalone nmu(make_cfg(kSrcId));
    nmu.enable_noc_credit(kSeed);
    ASSERT_TRUE(nmu.req_credit_avail()) << "fresh seed>0 must grant credit";

    for (int i = 0; i < kReads; ++i) {
        ASSERT_TRUE(nmu.axi_slave_port().push_ar(make_ar(static_cast<uint8_t>(i), 0x100)))
            << "AR queue (depth 16) should accept all " << kReads << " reads";
    }

    // Pump. Without receive_credit the sink grants at most `seed` flits total,
    // ever. Drain everything the sink releases over a generous tick window.
    int drained = 0;
    for (int t = 0; t < 64; ++t) {
        nmu.tick();
        while (auto f = nmu.pop_req_flit()) {
            EXPECT_EQ(f->get_header_field("axi_ch"), ni::AXI_CH_AR) << "reads emit AR flits";
            ++drained;
        }
    }

    EXPECT_EQ(drained, static_cast<int>(kSeed))
        << "credit-gated sink must release exactly `seed` flits with no replenish";
    EXPECT_FALSE(nmu.req_credit_avail())
        << "after draining the full seed the sink must backpressure (credit exhausted)";
    // pop_req_flit is dry — the surplus reads are held UPSTREAM (VcArbiter /
    // wormhole pending), proving backpressure reached into the pipeline rather
    // than the terminal queue growing unbounded.
    EXPECT_FALSE(nmu.pop_req_flit().has_value()) << "no further flits while credit is exhausted";

    // (b) Re-open: each receive_credit pulse releases exactly one more flit.
    nmu.req_receive_credit();
    nmu.req_receive_credit();
    EXPECT_TRUE(nmu.req_credit_avail()) << "receive_credit must re-open the sink";

    int reopened = 0;
    for (int t = 0; t < 64; ++t) {
        nmu.tick();
        while (auto f = nmu.pop_req_flit()) ++reopened;
    }
    EXPECT_EQ(reopened, 2) << "two receive_credit pulses release exactly two held flits";
    EXPECT_FALSE(nmu.req_credit_avail()) << "credit exhausted again after the two pulses";
    EXPECT_EQ(drained + reopened, static_cast<int>(kSeed) + 2)
        << "conservation: total released == seed + pulses (no loss, no double-grant)";
}

// (c) Multi-consume-per-tick: Depacketize drains its whole ingress queue in one
// tick(), so several injected rsp flits are consumed in a single tick. The
// consumer pulse must accumulate ALL of them; take_credit then drains exactly
// one per call.
TEST(NmuCreditConservation, ConsumerPulseAccumulatesMultiConsumePerTick) {
    SCENARIO(
        "Issue 5 AR reads (so the Rob has outstanding entries), then inject the 5 "
        "matching R(last) rsp flits and run a SINGLE nmu.tick(): Depacketize "
        "dequeues all 5 in that one tick. rsp_take_credit must then drain exactly "
        "5 pulses (one per consumed flit) and no more — proving the pulse is an "
        "accumulating counter, not a one-bit latch.");

    constexpr uint8_t kSrcId = 0x12;
    constexpr int kRspFlits = 5;
    NmuStandalone nmu(make_cfg(kSrcId));
    nmu.enable_noc_credit(/*seed=*/kRspFlits);  // enough credit to drain all reads

    // Issue 5 distinct-id reads and drain them to the NoC req-out face. Draining
    // an AR flit registers the read as outstanding in the Rob, so the later
    // R(last) responses are well-formed (no "R(last) with no outstanding read").
    for (int i = 0; i < kRspFlits; ++i) {
        ASSERT_TRUE(nmu.axi_slave_port().push_ar(make_ar(static_cast<uint8_t>(i), 0x100)));
    }
    for (int t = 0; t < 64; ++t) {
        nmu.tick();
        while (nmu.pop_req_flit()) {
        }
    }
    // Drain any consumer pulses incidentally produced by the req-path ticks so
    // the rsp-side pulse count below is clean (req path produces no rsp-in
    // consumption, but be explicit).
    while (nmu.rsp_take_credit()) {
    }

    // Inject all 5 R(last) responses BEFORE ticking, so a single Depacketize.tick()
    // drains the whole ingress queue (depkt_r_q depth 16 > 5).
    for (int i = 0; i < kRspFlits; ++i) {
        Flit r;
        r.set_header_field("axi_ch", ni::AXI_CH_R);
        r.set_header_field("src_id", 0x00);
        r.set_header_field("dst_id", kSrcId);
        r.set_header_field("vc_id", 0);
        r.set_header_field("last", 1);
        r.set_header_field("rob_idx", 0);
        r.set_header_field("rob_req", 0);
        r.set_payload_field("R", "rid", static_cast<uint64_t>(i));
        r.set_payload_field("R", "rresp", static_cast<uint64_t>(axi::Resp::OKAY));
        r.set_payload_field("R", "ruser", 0);
        r.set_payload_field("R", "rlast", 1);
        nmu.inject_rsp_flit(r);
    }

    // Single tick: Depacketize.tick() drains the entire ingress queue in one
    // pass (while-loop). Each consumed flit bumps the consumer pulse counter.
    nmu.tick();

    int pulses = 0;
    while (nmu.rsp_take_credit()) ++pulses;
    EXPECT_EQ(pulses, kRspFlits)
        << "consumer pulse must accumulate every flit consumed in the tick (no loss / "
           "double-count); a one-bit latch would report only 1";
    EXPECT_FALSE(nmu.rsp_take_credit()) << "drained to empty — no double-count";
}
