// Unit tests for SlaveShellAdapter — Stage 5b T9, updated for the
// wait_valid / context-gated ready policy (see
// docs/superpowers/specs/2026-06-12-wait-valid-ready-policy-design.md).
//
// Tests verify the 3-step pattern (set_inputs / tick / get_outputs) without any
// DPI or SV involvement. Cases cover the key behavioral invariants:
//   1. Idle adapter keeps ALL readys low (no valid → no ready).
//   2. Two-phase AW handshake (ready pulses the tick after valid appears),
//      W context window (wready pre-asserts after the AW handshake), and the
//      full write round trip producing a B response.
#include "common/scenario.hpp"
#include "cosim/slave_shell_adapter.hpp"
#include "cosim/slave_shell_io.hpp"
#include <gtest/gtest.h>

using ni::cmodel::cosim::SlaveInputs;
using ni::cmodel::cosim::SlaveOutputs;
using ni::cmodel::cosim::SlaveShellAdapter;

// ---------------------------------------------------------------------------
// Test 1: idle adapter keeps all readys LOW — wait_valid policy: capacity
// alone never asserts ready; a valid must be observed first.
// ---------------------------------------------------------------------------
TEST(SlaveShellAdapter, idle_adapter_keeps_readys_low) {
    SCENARIO("Idle SlaveShellAdapter keeps awready/wready/arready low (wait_valid)");

    SlaveShellAdapter adapter;
    adapter.init();  // default config: 64 KiB, 1-cycle latency, depth=32

    SlaveInputs in{};  // all valid signals false — nothing presented
    adapter.set_inputs(in);
    adapter.tick();

    SlaveOutputs out{};
    adapter.get_outputs(out);

    EXPECT_FALSE(out.awready) << "wait_valid: no AWVALID -> awready must stay low";
    EXPECT_FALSE(out.wready) << "no open W burst window -> wready must stay low";
    EXPECT_FALSE(out.arready) << "wait_valid: no ARVALID -> arready must stay low";
    EXPECT_FALSE(out.bvalid) << "no B response without a prior AW+W";
    EXPECT_FALSE(out.rvalid) << "no R response without a prior AR";
}

// ---------------------------------------------------------------------------
// Test 2: single AW + W round trip under the two-phase handshake.
// Phase pattern per address beat: tick N (valid first seen) -> ready=1 on the
// output (wire next cycle); tick N+1 (valid && prev ready) -> beat consumed,
// ready returns low. After the AW handshake the W burst window opens and
// wready pre-asserts WITHOUT wvalid.
// ---------------------------------------------------------------------------
TEST(SlaveShellAdapter, single_write_round_trip) {
    SCENARIO("Two-phase AW handshake, W context window, B response within 12 cycles");

    SlaveShellAdapter adapter;
    adapter.init(/*memory_base=*/0, /*memory_size=*/65536,
                 /*write_lat=*/1, /*read_lat=*/1,
                 /*queue_depth=*/32);

    SlaveInputs in{};
    SlaveOutputs out{};

    // Cycle 1: AWVALID first seen — ready must assert in response (one-shot).
    in.awvalid = true;
    in.awid = 0;
    in.awaddr = 0x100;
    in.awlen = 0;    // 1 beat
    in.awsize = 5;   // 32 bytes (256-bit bus)
    in.awburst = 1;  // INCR
    adapter.set_inputs(in);
    adapter.tick();
    adapter.get_outputs(out);
    EXPECT_TRUE(out.awready) << "cycle 1: AWVALID observed + capacity -> awready pulses";
    EXPECT_FALSE(out.wready) << "cycle 1: AW not yet handshaken -> W window still closed";

    // Cycle 2: valid still high && prev ready -> the AW handshake tick.
    // The beat is consumed, awready returns low, and the W window opens.
    adapter.set_inputs(in);  // master holds AW until it sees ready
    adapter.tick();
    adapter.get_outputs(out);
    EXPECT_FALSE(out.awready) << "cycle 2: handshake done -> awready back to low";
    EXPECT_TRUE(out.wready) << "cycle 2: AW handshaken -> wready pre-asserts (no wvalid!)";

    // Cycle 3: drive the single W beat — wready was already high (window),
    // so this is the W handshake tick; WLAST closes the window.
    in = SlaveInputs{};
    in.wvalid = true;
    in.wdata[0] = 0xAB;  // canary byte
    in.wstrb = 0xFFFF'FFFFu;
    in.wlast = true;
    in.bready = true;
    adapter.set_inputs(in);
    adapter.tick();
    adapter.get_outputs(out);
    EXPECT_FALSE(out.wready) << "cycle 3: WLAST consumed -> W window closed, wready low";

    // Cycles 4-12: poll for bvalid (memory latency + pipeline drain).
    in = SlaveInputs{};
    in.bready = true;
    bool saw_bvalid = out.bvalid;

    for (int cycle = 4; cycle <= 12 && !saw_bvalid; ++cycle) {
        adapter.set_inputs(in);
        adapter.tick();
        adapter.get_outputs(out);
        if (out.bvalid) saw_bvalid = true;
    }

    EXPECT_TRUE(saw_bvalid) << "B response never arrived within 12 cycles";
    if (saw_bvalid) {
        EXPECT_EQ(out.bid, 0u) << "bid must match awid";
        EXPECT_EQ(out.bresp, 0u) << "bresp must be OKAY (0)";
    }
}
