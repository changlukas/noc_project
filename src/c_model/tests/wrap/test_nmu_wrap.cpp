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
#include "common/tmp_path.hpp"
#include "ni_flit_constants.h"
#include "ni_params.h"
#include "wrap/flit_byte_conv.hpp"
#include "wrap/nmu_wrap.hpp"
#include "wrap/nmu_wrap_io.hpp"
#include <fstream>
#include <gtest/gtest.h>

using ni::cmodel::wrap::flit_from_bytes;
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

// ---------------------------------------------------------------------------
// Task 6: init(config_path) loads the topology YAML's address_map into
// NmuConfig.sam instead of the legacy 16x16-uniform default.
// ---------------------------------------------------------------------------
TEST(NmuWrap, init_with_config_path_loads_sam_from_yaml) {
    SCENARIO(
        "NmuWrap::init(config_path) loads a hand-written topology YAML's "
        "address_map (4 KB tiles, 2x2 mesh) into NmuConfig.sam; "
        "the legacy default (4 GB tiles) would resolve the same "
        "address to a different dst_id/local_addr, so observing the "
        "YAML-mapped values proves the config path was actually loaded.");

    auto path = ni::cmodel::testing::unique_temp_path("nmu_wrap_sam.yaml");
    std::ofstream(path) << "topology: { name: t, x_dim: 2, y_dim: 2, num_vc: 1 }\n"
                           "address_map:\n"
                           "  tile_size: 0x1000\n";

    NmuWrap adapter;
    adapter.init(/*src_id=*/0, /*num_vc=*/1, ni::NMU_QUEUE_DEPTH,
                 ni::cmodel::nmu::RobMode::Disabled, path.c_str());

    NmuInputs in{};
    NmuOutputs out{};

    // Global 0x1040 -> under the 4 KB/tile 2x2 SAM this is tile (x=1,y=0),
    // dst_id = (y<<X_WIDTH)|x = 1, rebased local_addr = 0x40. Under the
    // legacy 4 GB/tile default it resolves to dst 0, local 0x1040 (tile 0,
    // base 0, so rebase is a no-op) -- the two SAMs disagree on this address,
    // so this pins the YAML load.
    in.awvalid = true;
    in.awid = 0x01;
    in.awaddr = 0x1040;
    in.awlen = 0;
    in.awsize = 2;   // 4 B/beat: keeps the burst inside the 4 KB tile
    in.awburst = 1;  // INCR
    adapter.set_inputs(in);
    adapter.tick();
    adapter.get_outputs(out);
    ASSERT_TRUE(out.awready) << "cycle 1: AWVALID observed -> awready pulses";

    adapter.set_inputs(in);  // valid held; prev ready -> AW handshake tick
    adapter.tick();
    adapter.get_outputs(out);

    // Drain the NoC req-out face for the AW flit (bounded loop mirrors
    // test_nmu.cpp's WriteRoundTrip pipeline-drain pattern).
    bool saw_aw_flit = false;
    in = NmuInputs{};
    for (int i = 0; i < 32 && !saw_aw_flit; ++i) {
        adapter.set_inputs(in);
        adapter.tick();
        adapter.get_outputs(out);
        if (out.noc_req_valid) {
            auto flit = flit_from_bytes(out.noc_req_flit);
            if (flit.get_header_field("axi_ch") == ni::AXI_CH_AW) {
                EXPECT_EQ(flit.get_header_field("dst_id"), 0x01u);
                EXPECT_EQ(flit.get_payload_field("AW", "awaddr"), 0x40ull);
                saw_aw_flit = true;
            }
        }
    }
    ASSERT_TRUE(saw_aw_flit) << "NmuWrap never produced an AW flit from the config-path SAM";
}

// ---------------------------------------------------------------------------
// Death test: odd num_vc rejected at the wrap init boundary.
// derive_vc_pools asserts on any odd num_vc > 1; NmuWrap::init calls it, so
// the abort must propagate through the wrap path.
// ---------------------------------------------------------------------------
TEST(WrapOddNumVcDeath, NmuWrapRejectsOddNumVc) {
    NmuWrap nmu;
    EXPECT_DEATH({ nmu.init(/*src_id=*/0, /*num_vc=*/3); }, "num_vc must be 1 or even");
}
