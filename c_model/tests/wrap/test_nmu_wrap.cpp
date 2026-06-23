// Unit tests for NmuWrap — Stage 5b T10, updated for the
// wait_valid / context-gated ready policy (see
// docs/superpowers/specs/2026-06-12-wait-valid-ready-policy-design.md).
//
// Tests verify the 3-step pattern (set_inputs / tick / get_outputs) without any
// DPI or SV involvement. Three cases cover the key behavioral invariants:
//   1. Idle adapter keeps awready/wready/arready LOW (wait_valid: capacity
//      alone never asserts ready).
//   2. Single AW + W: two-phase AW handshake (ready pulses the tick after
//      valid appears), then the W window opens and wready pre-asserts.
//   3. AWLEN=7 (8-beat W burst): wready holds for the whole burst (one bubble
//      at burst start, then full rate). A second AW presented mid-burst still
//      gets its ready pulse — multi-outstanding AW (post addresses ahead of
//      data) is legitimate AXI4 and load-bearing for the RoB/multi-ID paths.
#include "common/scenario.hpp"
#include "wrap/nmu_wrap.hpp"
#include "wrap/nmu_wrap_io.hpp"
#include <gtest/gtest.h>

using ni::cmodel::wrap::NmuInputs;
using ni::cmodel::wrap::NmuOutputs;
using ni::cmodel::wrap::NmuWrap;

// ---------------------------------------------------------------------------
// Test 1: idle adapter keeps all readys LOW (wait_valid policy).
// ---------------------------------------------------------------------------
TEST(NmuWrap, idle_adapter_keeps_readys_low) {
    SCENARIO("Idle NmuWrap keeps awready/wready/arready low (wait_valid)");

    NmuWrap adapter;
    adapter.init();

    NmuInputs in{};  // all valid signals false — nothing presented
    adapter.set_inputs(in);
    adapter.tick();

    NmuOutputs out{};
    adapter.get_outputs(out);

    EXPECT_FALSE(out.awready) << "wait_valid: no AWVALID -> awready must stay low";
    EXPECT_FALSE(out.wready) << "no open W burst window -> wready must stay low";
    EXPECT_FALSE(out.arready) << "wait_valid: no ARVALID -> arready must stay low";
    EXPECT_FALSE(out.bvalid) << "no B response without a prior AW+W";
    EXPECT_FALSE(out.rvalid) << "no R response without a prior AR";
    EXPECT_FALSE(out.noc_req_valid) << "no NoC req flit without any AXI request";
}

// ---------------------------------------------------------------------------
// Test 2: single AW + W beat — two-phase AW handshake, then W window.
// ---------------------------------------------------------------------------
TEST(NmuWrap, single_aw_w_two_phase_handshake) {
    SCENARIO("Two-phase AW handshake; wready pre-asserts after AW, W beat consumed");

    NmuWrap adapter;
    adapter.init();

    NmuInputs in{};
    NmuOutputs out{};

    // Cycle 1: AWVALID first seen — awready pulses; W window still closed.
    in.awvalid = true;
    in.awid = 0x01;
    in.awaddr = 0x200;
    in.awlen = 0;    // 1 beat
    in.awsize = 5;   // 32 bytes (256-bit bus)
    in.awburst = 1;  // INCR
    adapter.set_inputs(in);
    adapter.tick();
    adapter.get_outputs(out);
    EXPECT_TRUE(out.awready) << "cycle 1: AWVALID observed + capacity -> awready pulses";
    EXPECT_FALSE(out.wready) << "cycle 1: AW not yet handshaken -> W window closed";

    // Cycle 2: valid held && prev ready -> AW handshake tick; window opens.
    adapter.set_inputs(in);
    adapter.tick();
    adapter.get_outputs(out);
    EXPECT_FALSE(out.awready) << "cycle 2: handshake done -> awready back low";
    EXPECT_TRUE(out.wready) << "cycle 2: W window open -> wready pre-asserts without wvalid";

    // Cycle 3: drive the single W beat (prev wready=1 -> consumed; WLAST
    // closes the window).
    in = NmuInputs{};
    in.wvalid = true;
    in.wdata[0] = 0x55;
    in.wstrb = 0xFFFF'FFFFu;
    in.wlast = true;
    in.bready = true;
    adapter.set_inputs(in);
    adapter.tick();
    adapter.get_outputs(out);
    EXPECT_FALSE(out.wready) << "cycle 3: WLAST consumed -> W window closed";
}

// ---------------------------------------------------------------------------
// Test 3: AWLEN=7 (8-beat W burst) — burst-hold wready; AW stays available.
//
// After the AW handshake the W window holds wready high for the full burst
// (capacity permitting): one bubble at burst start, then full rate — all 8
// beats accepted back-to-back. A second AWVALID presented mid-burst still
// receives its one-shot ready pulse (multi-outstanding AW preserved; any
// stricter single-outstanding view lives in the scenario skip list, not in
// the model).
// ---------------------------------------------------------------------------
TEST(NmuWrap, multi_beat_w_burst_full_rate_aw_available) {
    SCENARIO("8-beat W burst at full rate; mid-burst AW still gets its ready pulse");

    NmuWrap adapter;
    adapter.init();

    NmuInputs in{};
    NmuOutputs out{};

    // Cycle 1: AW (len=7 -> 8 beats) first seen.
    in.awvalid = true;
    in.awid = 0x00;
    in.awaddr = 0x100;
    in.awlen = 7;    // 8 beats
    in.awsize = 5;   // 32 bytes/beat
    in.awburst = 1;  // INCR
    adapter.set_inputs(in);
    adapter.tick();
    adapter.get_outputs(out);
    EXPECT_TRUE(out.awready) << "cycle 1: awready pulses for the observed AW";

    // Cycle 2: AW handshake tick — window opens (w_expected=8).
    adapter.set_inputs(in);
    adapter.tick();
    adapter.get_outputs(out);
    EXPECT_FALSE(out.awready) << "cycle 2: AW consumed";
    EXPECT_TRUE(out.wready) << "cycle 2: burst window open, wready holds";

    // Cycles 3-10: drive 8 W beats one per cycle. A beat is accepted when the
    // PREVIOUS tick's wready (the wire value this cycle) was high. Present a
    // second AW alongside the first W beat: it must be gated (no awready)
    // while the burst is open.
    int beats_accepted = 0;
    bool prev_wready = out.wready;  // wire value seen by the first W beat
    for (int beat = 0; beat < 8; ++beat) {
        in = NmuInputs{};
        in.bready = true;
        in.wvalid = true;
        in.wdata.fill(0);
        in.wdata[0] = static_cast<uint8_t>(0x10 + beat);
        in.wstrb = 0xFFFF'FFFFu;
        in.wlast = (beat == 7);
        if (beat == 0) {
            in.awvalid = true;  // second AW presented mid-burst
            in.awid = 0x02;
            in.awaddr = 0x300;
            in.awlen = 0;
            in.awsize = 5;
            in.awburst = 1;
        }
        adapter.set_inputs(in);
        adapter.tick();
        adapter.get_outputs(out);
        if (prev_wready) ++beats_accepted;
        prev_wready = out.wready;
        if (beat == 0) {
            EXPECT_TRUE(out.awready) << "second AW presented mid-burst must still get its ready "
                                        "pulse (multi-outstanding AW preserved)";
        }
        if (beat < 7) {
            EXPECT_TRUE(out.wready)
                << "beat " << beat << ": burst window still open -> wready holds";
        }
    }

    EXPECT_EQ(beats_accepted, 8)
        << "all 8 W beats must transfer at full rate after the one-bubble start";
    EXPECT_FALSE(out.wready) << "after WLAST the window closes -> wready low";
}
