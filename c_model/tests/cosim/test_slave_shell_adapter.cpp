// Unit tests for SlaveShellAdapter — Stage 5b T9.
//
// Tests verify the 3-step pattern (set_inputs / tick / get_outputs) without any
// DPI or SV involvement. Two cases cover the key behavioral invariants:
//   1. Idle adapter asserts awready (empty queue has capacity).
//   2. Single AW+W round trip produces a B response within a few cycles.
#include "common/scenario.hpp"
#include "cosim/slave_shell_adapter.hpp"
#include "cosim/slave_shell_io.hpp"
#include <gtest/gtest.h>

using ni::cmodel::cosim::SlaveInputs;
using ni::cmodel::cosim::SlaveOutputs;
using ni::cmodel::cosim::SlaveShellAdapter;

// ---------------------------------------------------------------------------
// Test 1: idle adapter asserts awready (queue has capacity, no beats pending).
// ---------------------------------------------------------------------------
TEST(SlaveShellAdapter, awready_asserted_when_capacity_available) {
    SCENARIO("Idle SlaveShellAdapter asserts awready (empty queue has capacity)");

    SlaveShellAdapter adapter;
    adapter.init();  // default config: 64 KiB, 1-cycle latency, depth=32

    SlaveInputs in{};  // all valid signals false — nothing presented
    adapter.set_inputs(in);
    adapter.tick();

    SlaveOutputs out{};
    adapter.get_outputs(out);

    EXPECT_TRUE(out.awready) << "slave should accept new AW with empty queue";
    EXPECT_TRUE(out.wready) << "slave should accept new W with empty queue";
    EXPECT_TRUE(out.arready) << "slave should accept new AR with empty queue";
    EXPECT_FALSE(out.bvalid) << "no B response without a prior AW+W";
    EXPECT_FALSE(out.rvalid) << "no R response without a prior AR";
}

// ---------------------------------------------------------------------------
// Test 2: single AW + W round trip — B response arrives within a few cycles.
// Exercises the full write path: AW accepted, W accepted, memory latency,
// B response returned with bvalid asserted.
// ---------------------------------------------------------------------------
TEST(SlaveShellAdapter, single_write_round_trip) {
    SCENARIO("Drive single AW + W, expect B response within 10 cycles");

    SlaveShellAdapter adapter;
    adapter.init(/*memory_base=*/0, /*memory_size=*/65536,
                 /*write_lat=*/1, /*read_lat=*/1,
                 /*queue_depth=*/32);

    SlaveInputs in{};
    SlaveOutputs out{};

    // Cycle 1: drive AW (single-beat INCR write, addr=0x100, size=5 → 32 B/beat).
    in.awvalid = true;
    in.awid = 0;
    in.awaddr = 0x100;
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
    EXPECT_TRUE(out.awready) << "cycle 1: slave should accept AW from empty queue";

    // Cycle 2: drive W (single beat with last=true, strb = all-ones for 32 B).
    in = SlaveInputs{};
    in.wvalid = true;
    in.wdata[0] = 0xAB;  // canary byte
    in.wstrb = 0xFFFF'FFFFu;
    in.wlast = true;
    in.bready = true;  // master is ready to accept B immediately
    adapter.set_inputs(in);
    adapter.tick();
    adapter.get_outputs(out);
    EXPECT_TRUE(out.wready) << "cycle 2: slave should accept W from empty queue";

    // Cycles 3-12: poll for bvalid (memory latency + pipeline drain).
    in = SlaveInputs{};
    in.bready = true;
    bool saw_bvalid = out.bvalid;  // might arrive same cycle as W acceptance

    for (int cycle = 3; cycle <= 12 && !saw_bvalid; ++cycle) {
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
