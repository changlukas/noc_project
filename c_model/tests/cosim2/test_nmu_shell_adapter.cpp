// Unit tests for NmuShellAdapter — Stage 5b T10.
//
// Tests verify the 3-step pattern (set_inputs / tick / get_outputs) without any
// DPI or SV involvement. Three cases cover the key behavioral invariants:
//   1. Idle adapter asserts awready/wready/arready (empty queues have capacity).
//   2. Single AW + single W beat: awready is asserted, wready is asserted.
//   3. AWLEN=7 (8-beat W burst): every W beat visible on wire one per cycle
//      (KNOWN_LIMITATIONS §2 fix proof — beta-tick model, not snapshot model).
//
// Note on test 3 expectations: NmuStandalone's Packetize stage has internal
// wormhole_per_input_depth=4. The AxiSlavePort w_queue_depth=16 accepts all 8
// beats; they drain one-per-cycle into Packetize. The test verifies that wready
// is asserted each cycle the beat is presented (queue has capacity), proving
// that beats are individually wire-visible, not collapsed into one cycle.
#include "common/scenario.hpp"
#include "cosim2/nmu_shell_adapter.hpp"
#include "cosim2/nmu_shell_io.hpp"
#include <gtest/gtest.h>

using ni::cmodel::cosim2::NmuInputs;
using ni::cmodel::cosim2::NmuOutputs;
using ni::cmodel::cosim2::NmuShellAdapter;

// ---------------------------------------------------------------------------
// Test 1: idle adapter asserts awready/wready/arready (empty queues, no beats).
// ---------------------------------------------------------------------------
TEST(NmuShellAdapter, idle_adapter_asserts_ready_signals) {
    SCENARIO("Idle NmuShellAdapter asserts awready/wready/arready (empty queues have capacity)");

    NmuShellAdapter adapter;
    adapter.init();

    NmuInputs in{};  // all valid signals false — nothing presented
    adapter.set_inputs(in);
    adapter.tick();

    NmuOutputs out{};
    adapter.get_outputs(out);

    EXPECT_TRUE(out.awready) << "idle Nmu should accept new AW (empty queue has capacity)";
    EXPECT_TRUE(out.wready) << "idle Nmu should accept new W (empty queue has capacity)";
    EXPECT_TRUE(out.arready) << "idle Nmu should accept new AR (empty queue has capacity)";
    EXPECT_FALSE(out.bvalid) << "no B response without a prior AW+W";
    EXPECT_FALSE(out.rvalid) << "no R response without a prior AR";
    EXPECT_FALSE(out.noc_req_valid) << "no NoC req flit without any AXI request";
}

// ---------------------------------------------------------------------------
// Test 2: single AW + W beat — awready and wready both asserted this cycle.
// This exercises AXI slave side wire visibility: each channel independently
// accepts its beat when queue has capacity.
// ---------------------------------------------------------------------------
TEST(NmuShellAdapter, single_aw_w_beat_both_accepted) {
    SCENARIO("Drive single AW and W beat; both awready and wready asserted");

    NmuShellAdapter adapter;
    adapter.init();

    NmuInputs in{};
    NmuOutputs out{};

    // Cycle 1: present AW (INCR, len=0 = 1 beat, size=5 = 32 B/beat).
    in.awvalid = true;
    in.awid = 0x01;
    in.awaddr = 0x200;
    in.awlen = 0;    // 1 beat
    in.awsize = 5;   // 32 bytes (256-bit bus)
    in.awburst = 1;  // INCR
    in.awlock = 0;
    in.awcache = 0;
    in.awprot = 0;
    in.awqos = 0;
    adapter.set_inputs(in);
    adapter.tick();
    adapter.get_outputs(out);
    EXPECT_TRUE(out.awready) << "cycle 1: Nmu should accept AW from empty queue";

    // Cycle 2: present W (single beat, last=true).
    in = NmuInputs{};
    in.wvalid = true;
    in.wdata[0] = 0x55;
    in.wstrb = 0xFFFF'FFFFu;
    in.wlast = true;
    in.bready = true;
    adapter.set_inputs(in);
    adapter.tick();
    adapter.get_outputs(out);
    EXPECT_TRUE(out.wready) << "cycle 2: Nmu should accept W from empty queue";
}

// ---------------------------------------------------------------------------
// Test 3: AWLEN=7 (8-beat W burst) — per-cycle wire visibility (KL §2 fix).
//
// The KNOWN_LIMITATIONS §2 broken behavior (snapshot model) would collapse all
// 8 W beats into a single cycle. The beta-tick model (this implementation)
// exposes each beat individually on the wire: wready is asserted each cycle
// because the AxiSlavePort w_queue has capacity (depth=16).
//
// Proof: drive 8 W beats one per cycle; count cycles where wready=true.
// Every cycle with wvalid=true and queue not full should see wready=true.
// ---------------------------------------------------------------------------
TEST(NmuShellAdapter, multi_beat_w_burst_visible_per_cycle) {
    SCENARIO("AWLEN=7 (8-beat W burst): every W beat visible on wire one per cycle (KL §2 fix)");

    NmuShellAdapter adapter;
    adapter.init();

    NmuInputs in{};
    NmuOutputs out{};

    // Cycle 1: drive AW with len=7 (AWLEN=7 → 8 beats).
    in.awvalid = true;
    in.awid = 0x00;
    in.awaddr = 0x100;
    in.awlen = 7;    // 8 beats
    in.awsize = 5;   // 32 bytes/beat (256-bit bus)
    in.awburst = 1;  // INCR
    in.awlock = 0;
    in.awcache = 0;
    in.awprot = 0;
    in.awqos = 0;
    adapter.set_inputs(in);
    adapter.tick();
    adapter.get_outputs(out);
    // AW with awlen=7: NMU's can_accept_aw() is a pure queue-vacancy check (AXI4
    // §A3.3 spec-compliant). AWREADY=1 is correct when the queue has space.
    // wb2axip faxi_slave.v:805-807 enforces AWREADY=0 during W burst — that is a
    // wb2axip-internal simplification, not AXI4 spec (see KNOWN_LIMITATIONS.md §6).
    EXPECT_TRUE(out.awready)
        << "cycle 1: awready should be 1 (queue has space, spec-compliant)";

    // Cycles 2-9: drive 8 W beats one per cycle.
    // Count: (a) beats where wready=true (beat accepted into queue this cycle)
    //        (b) whether wready was ever true (at least one beat visible on wire)
    in = NmuInputs{};
    in.bready = true;
    int beats_accepted = 0;
    int beats_presented = 8;

    for (int beat = 0; beat < beats_presented; ++beat) {
        in.wvalid = true;
        in.wdata.fill(0);
        in.wdata[0] = static_cast<uint8_t>(0x10 + beat);  // unique canary per beat
        in.wstrb = 0xFFFF'FFFFu;
        in.wlast = (beat == 7);
        adapter.set_inputs(in);
        adapter.tick();
        adapter.get_outputs(out);
        if (out.wready) {
            ++beats_accepted;
        }
    }

    // Core KL §2 assertion: at least 1 W beat was wire-visible per cycle.
    // In the broken snapshot model, only beat 0 would ever be visible because
    // all beats are buffered inside the model before tick(), not one per cycle.
    // In the beta-tick model, each beat arrives one-per-cycle at push_w().
    EXPECT_GE(beats_accepted, 1) << "at least one W beat must be accepted per-cycle (KL §2)";

    // Stronger assertion: because AxiSlavePort.w_queue_depth=16 and we drive 8
    // beats, the queue should have capacity for all 8 → every beat accepted.
    EXPECT_EQ(beats_accepted, beats_presented)
        << "all 8 W beats should be accepted (queue depth=16 >> 8 beats); "
           "if <8, the adapter is buffering beats before tick() (KL §2 regression)";
}
