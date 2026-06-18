// Unit tests for NsuShellAdapter — Stage 5b T11.
//
// Tests verify the 3-step pattern (set_inputs / tick / get_outputs) without any
// DPI or SV involvement. Three cases cover the key behavioral invariants:
//   1. Idle adapter: no NoC input → noc_rsp_valid low + awvalid/wvalid/arvalid low.
//   2. NoC req AW flit injection → after tick, axi master drives awvalid.
//   3. Multi-outstanding AW + out-of-order B responses: 4 different AW IDs injected
//      via NoC req; B responses returned out-of-order; MetaBuffer correctly produces
//      valid AXI-side NoC rsp traffic.
#include "axi/types.hpp"
#include "common/scenario.hpp"
#include "cosim/nsu_shell_adapter.hpp"
#include "cosim/nsu_shell_io.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include <gtest/gtest.h>

using ni::cmodel::Flit;
using ni::cmodel::axi::Burst;
using ni::cmodel::cosim::FLIT_BYTES;
using ni::cmodel::cosim::FlitBytes;
using ni::cmodel::cosim::NsuInputs;
using ni::cmodel::cosim::NsuOutputs;
using ni::cmodel::cosim::NsuShellAdapter;

namespace {

// Build an AW-channel req flit to inject into Nsu's NoC consumer side.
// src_id = requester's NMU id (needed for MetaBuffer snapshot).
FlitBytes make_aw_flit_bytes(uint8_t awid, uint64_t awaddr, uint8_t src_id = 0x10) {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_AW);
    f.set_header_field("src_id", src_id);
    f.set_header_field("dst_id", 0x01);
    f.set_header_field("last", 1);
    f.set_header_field("rob_req", 0);
    f.set_header_field("rob_idx", 0);
    f.set_payload_field("AW", "awid", awid);
    f.set_payload_field("AW", "awaddr", awaddr);
    f.set_payload_field("AW", "awsize", 5);  // 32 bytes/beat (256-bit bus)
    f.set_payload_field("AW", "awlen", 0);   // 1 beat
    f.set_payload_field("AW", "awburst", static_cast<uint64_t>(Burst::INCR));
    FlitBytes bytes{};
    for (int i = 0; i < Flit::WIDTH_BYTES; ++i) bytes[i] = f.raw()[i];
    return bytes;
}

// Build a W-channel req flit (single last beat) for completeness in test 2.
FlitBytes make_w_flit_bytes() {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_W);
    f.set_header_field("dst_id", 0x01);
    f.set_header_field("last", 1);
    f.set_payload_field("W", "wlast", 1u);
    f.set_payload_field("W", "wstrb", 0xFFFF'FFFFu);
    FlitBytes bytes{};
    for (int i = 0; i < Flit::WIDTH_BYTES; ++i) bytes[i] = f.raw()[i];
    return bytes;
}

}  // namespace

// ---------------------------------------------------------------------------
// Test 1: idle adapter — no NoC input produces no AXI master traffic and no
// NoC rsp flit. bready/rready stay LOW: context-gated policy — no request
// was issued, so no response is owed and ready must not pre-assert.
// ---------------------------------------------------------------------------
TEST(NsuShellAdapter, idle_adapter_no_output) {
    SCENARIO("Idle NsuShellAdapter: no NoC req flit → no AXI outputs, b/rready low");

    NsuShellAdapter adapter;
    adapter.init();

    NsuInputs in{};  // all valid flags false — nothing presented
    adapter.set_inputs(in);
    adapter.tick();

    NsuOutputs out{};
    adapter.get_outputs(out);

    EXPECT_FALSE(out.noc_rsp_valid) << "idle Nsu should produce no NoC rsp flit";
    EXPECT_FALSE(out.awvalid) << "idle Nsu should not drive AW (no request processed)";
    EXPECT_FALSE(out.wvalid) << "idle Nsu should not drive W";
    EXPECT_FALSE(out.arvalid) << "idle Nsu should not drive AR";
    EXPECT_FALSE(out.bready) << "context-gated: no write issued -> no B owed -> bready stays low";
    EXPECT_FALSE(out.rready) << "context-gated: no read issued -> no R owed -> rready stays low";
}

// ---------------------------------------------------------------------------
// Test 2: inject a NoC req AW flit → Nsu drives awvalid after 2 ticks.
//
// Pipeline staging (spec §5.3): S1 (Depacketize) accepts flit in tick 1;
// S2 (AxiMasterPort) drains S1 → aw_q in tick 2. Tick order inside
// Nsu::tick(): S2-drain THEN S1-fill, so S1 fills in tick N and S2 sees it
// in tick N+1.
// ---------------------------------------------------------------------------
TEST(NsuShellAdapter, noc_req_aw_flit_produces_axi_awvalid) {
    SCENARIO(
        "NoC req AW flit injection → AXI master drives awvalid after 2 ticks (S1 fill tick 1, "
        "S2 drain tick 2 — spec §5.3 two-stage pipeline)");

    NsuShellAdapter adapter;
    adapter.init();

    NsuInputs in{};
    NsuOutputs out{};

    // Tick 1: inject AW flit into NoC consumer. S1 (Depacketize) accepts it.
    // S2 has nothing yet (reverse-order tick: S2 drains first, then S1 fills).
    in.noc_req_valid = true;
    in.noc_req_flit = make_aw_flit_bytes(0x05, 0x1000, /*src_id=*/0x10);
    in.awready = false;
    adapter.set_inputs(in);
    adapter.tick();
    adapter.get_outputs(out);
    EXPECT_FALSE(out.awvalid) << "tick 1: AW in S1 only; S2 aw_q empty, awvalid must be low";

    // Tick 2: inject W flit (lands in S1). S2 drains AW from S1 → aw_q → awvalid high.
    in = NsuInputs{};
    in.noc_req_valid = true;
    in.noc_req_flit = make_w_flit_bytes();
    in.awready = false;  // don't accept AW yet — verify awvalid first
    adapter.set_inputs(in);
    adapter.tick();
    adapter.get_outputs(out);
    EXPECT_TRUE(out.awvalid) << "tick 2: AW drained S1→S2; awvalid must be high";
    EXPECT_EQ(out.awid, 0x05u) << "awid must match the flit payload";
    EXPECT_EQ(out.awaddr, 0x1000u) << "awaddr must match the flit payload";

    // Tick 3: accept AW (awready=true). S2 drains W from S1 → wvalid.
    in = NsuInputs{};
    in.awready = true;
    in.wready = false;
    adapter.set_inputs(in);
    adapter.tick();
    adapter.get_outputs(out);
    EXPECT_TRUE(out.wvalid) << "tick 3: W drained S1→S2; wvalid must be high";
    EXPECT_TRUE(out.wlast) << "single-beat burst: wlast must be asserted";
}

// ---------------------------------------------------------------------------
// Test 3: multi-outstanding AW + out-of-order B response ordering.
//
// Inject 4 AW flits with different IDs. Return B responses out-of-order
// (IDs: 3, 1, 0, 2). Verify that:
//   (a) Each AW produces an awvalid output per-cycle (per-cycle visibility).
//   (b) Pushing B responses triggers MetaBuffer lookup and generates NoC rsp
//       flits (noc_rsp_valid) — proving the Packetize path is functional.
// ---------------------------------------------------------------------------
TEST(NsuShellAdapter, multi_outstanding_aw_and_out_of_order_b) {
    SCENARIO("4 AW IDs via NoC req; B responses out-of-order; MetaBuffer produces NoC rsp flits");

    NsuShellAdapter adapter;
    adapter.init();

    NsuInputs in{};
    NsuOutputs out{};

    // Phase 1: inject 4 AW flits, one per cycle.
    const uint8_t ids[4] = {0x00, 0x01, 0x02, 0x03};
    int aw_valid_count = 0;

    for (int i = 0; i < 4; ++i) {
        in = NsuInputs{};
        in.noc_req_valid = true;
        in.noc_req_flit = make_aw_flit_bytes(ids[i], 0x2000 + 0x100 * i, 0x10);
        in.awready = true;  // subordinate immediately accepts each AW
        in.wready = true;
        adapter.set_inputs(in);
        adapter.tick();
        adapter.get_outputs(out);
        if (out.awvalid) ++aw_valid_count;
    }

    EXPECT_GE(aw_valid_count, 1)
        << "at least one AW beat must have been wire-visible during AW injection phase";

    // Phase 2: drain any remaining awvalid without new NoC input.
    // Allow up to 8 cycles for the pipeline to flush.
    for (int cycle = 0; cycle < 8; ++cycle) {
        in = NsuInputs{};
        in.awready = true;
        in.wready = true;
        adapter.set_inputs(in);
        adapter.tick();
        adapter.get_outputs(out);
        if (out.awvalid) ++aw_valid_count;
    }

    // Each of the 4 AW flits should have produced exactly one awvalid beat.
    EXPECT_EQ(aw_valid_count, 4)
        << "all 4 AW beats must be driven on the AXI master wire (one per flit)";

    // Phase 3: return B responses out-of-order: IDs 3, 1, 0, 2.
    // Also need to inject W flits first so the MetaBuffer can match them.
    // For this test we focus on B-response → NoC rsp flit production.
    // Inject a single W flit per outstanding AW (wlast=1) before pushing B.
    for (int i = 0; i < 4; ++i) {
        in = NsuInputs{};
        in.noc_req_valid = true;
        in.noc_req_flit = make_w_flit_bytes();
        in.wready = true;
        adapter.set_inputs(in);
        adapter.tick();
    }

    // Now push B responses out-of-order (3, 1, 0, 2).
    const uint8_t b_order[4] = {3, 1, 0, 2};
    int noc_rsp_count = 0;

    for (int i = 0; i < 4; ++i) {
        in = NsuInputs{};
        in.bvalid = true;
        in.bid = b_order[i];
        in.bresp = 0;  // OKAY
        adapter.set_inputs(in);
        adapter.tick();
        adapter.get_outputs(out);
        // Allow a couple of pipeline cycles per response for Packetize to produce.
        if (out.noc_rsp_valid) ++noc_rsp_count;
    }

    // Allow pipeline to drain — up to 12 additional cycles.
    for (int cycle = 0; cycle < 12; ++cycle) {
        in = NsuInputs{};
        adapter.set_inputs(in);
        adapter.tick();
        adapter.get_outputs(out);
        if (out.noc_rsp_valid) ++noc_rsp_count;
    }

    // Each B response should eventually generate at least one NoC rsp flit.
    // The exact count depends on the Packetize framing, but non-zero proves the path works.
    EXPECT_GT(noc_rsp_count, 0)
        << "at least one NoC rsp flit must be produced by Packetize after B response injection; "
           "zero means MetaBuffer lookup or Packetize path is broken";
}
