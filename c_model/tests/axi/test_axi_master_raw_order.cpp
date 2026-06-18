// AXI4 read-after-write (RAW) ordering test for AxiMaster.
//
// AXI4 IHI 0022 §A5.3.4: there is NO ordering between a write and a read
// to the same address unless the master waits for the write's B response
// before issuing the read's AR. The AxiMaster must enforce this.
//
// Scenario: write 0xAA to addr 0x1000, then read from 0x1000. The master
// must hold AR until the write B arrives. Failure mode (before the fix):
// AR issues in the same tick as AW, before B — stale read returns 0.
//
// Non-vacuous design:
//   write_latency=6 keeps the write's B response delayed for several cycles.
//   Both write and read are admitted in tick 0 (max_out_w=1, max_out_r=1).
//   The RAW guard must actively hold AR for the intervening cycles; without
//   the guard AR issues at tick 0 and the stale-read data check catches it.
//
// Sentinel discipline:
//   ar_issue_cycle and b_complete_cycle are initialised to -1 (never-set).
//   Separate boolean flags (ar_issued, b_received) gate first-occurrence
//   recording to avoid same-tick aliasing when both events happen in one tick.
#include "axi/axi_master.hpp"
#include "axi/axi_slave.hpp"
#include "axi/memory.hpp"
#include "common/scenario.hpp"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <vector>

namespace axi = ni::cmodel::axi;

namespace {

// Write temp scenario files. Returns yaml path.
// Write 0xAA to 0x1000, then read 0x1000.
std::string write_raw_scenario() {
    const std::string dir = std::string(::testing::TempDir()) + "/raw_order_test";
    const std::string data_path = dir + ".data";
    const std::string dump_path = dir + ".rdump";
    const std::string yaml_path = dir + ".yaml";

    std::ofstream(data_path) << "AA\n";
    {
        std::ofstream dummy(dump_path);
    }  // empty placeholder

    std::ofstream yf(yaml_path);
    yf << "config:\n"
       << "  memory_base: 0x1000\n"
       << "  memory_size: 0x1000\n"
       << "transactions:\n"
       << "  - op: write\n"
       << "    id: 0\n"
       << "    addr: 0x1000\n"
       << "    size: 0\n"
       << "    len: 0\n"
       << "    burst: INCR\n"
       << "    data_file: " << data_path << "\n"
       << "  - op: read\n"
       << "    id: 0\n"
       << "    addr: 0x1000\n"
       << "    size: 0\n"
       << "    len: 0\n"
       << "    burst: INCR\n"
       << "    dump_file: " << dump_path << "\n";
    return yaml_path;
}

}  // namespace

// AxiMaster must not issue AR for a read whose address overlaps an outstanding
// (AW issued, B not yet received) write. The read must wait for the write's B.
//
// write_latency=6 ensures the write's B response is delayed by 6+ cycles.
// Both the write and the read are admitted in tick 0 (max_out_w=1, max_out_r=1),
// so the RAW guard must actively suppress AR for cycles 0..5 and release it
// only when B arrives. Without the guard, AR issues at tick 0 (before memory
// is written) and the stale-data assertion catches the violation.
TEST(AxiMasterRawOrder, ArHeldUntilOverlappingWriteReceivesB) {
    const std::string scn = write_raw_scenario();
    // write_latency=6: B delayed 6+ cycles after AW/W. read_latency=0: once AR
    // is admitted, R returns immediately so total latency variation is minimal.
    axi::Memory mem(0x1000, 0x1000, /*write_latency=*/6, /*read_latency=*/0);
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);

    // max_out_w=1, max_out_r=1: both write and read are admitted at tick 0.
    // The RAW guard is the only mechanism preventing AR from issuing immediately.
    axi::AxiMaster master(scn, slave, std::string(::testing::TempDir()) + "/raw.read.txt",
                          /*max_out_w=*/1, /*max_out_r=*/1);

    // Sentinels: -1 = never-set. Separate boolean gates guard first-occurrence
    // recording so that a same-tick B+AR event (B drains first in the tick,
    // then AR issues in the same tick) is correctly captured as B_cycle < AR_cycle.
    int ar_issue_cycle = -1;
    int b_complete_cycle = -1;
    std::vector<uint8_t> read_data;

    bool ar_issued = false;
    bool b_received = false;

    master.on_read_issued([&](const axi::IssueInfo&) { ar_issued = true; });
    master.on_write_completed([&](const axi::WriteResult&) { b_received = true; });
    master.on_read_observed([&](const axi::ReadResult& rr) { read_data = rr.data; });

    for (int cycle = 0; cycle < 500 && !master.done(); ++cycle) {
        // Capture B and AR events before ticking, using the per-tick flags.
        // Callbacks fire during master.tick(); capture cycle index immediately after.
        master.tick();
        slave.tick();
        mem.tick();

        // Record first-occurrence cycle for each event.
        // Both checks are inside the same post-tick block so cycle is consistent.
        if (b_received && b_complete_cycle == -1) {
            b_complete_cycle = cycle;
        }
        if (ar_issued && ar_issue_cycle == -1) {
            ar_issue_cycle = cycle;
        }
    }

    ASSERT_TRUE(master.done()) << "master did not complete within 500 cycles";

    // AR must NOT issue before B is received.
    ASSERT_GE(ar_issue_cycle, 0) << "AR was never issued";
    ASSERT_GE(b_complete_cycle, 0) << "B was never received";

    // With write_latency=6, B arrives several cycles after tick 0.
    // The guard must hold AR; it must issue STRICTLY AFTER B (same tick is
    // acceptable: drain_b runs before push_ar in the same master.tick()).
    EXPECT_GE(ar_issue_cycle, b_complete_cycle)
        << "AR issued on cycle " << ar_issue_cycle << " but write B arrived on cycle "
        << b_complete_cycle << ": master issued read before write committed (RAW hazard)";

    // B must have been delayed (guard was needed): B must not arrive at tick 0.
    EXPECT_GT(b_complete_cycle, 0)
        << "B arrived at tick 0 — write_latency too small, guard not exercised";

    // Data integrity: read must return the written value.
    ASSERT_FALSE(read_data.empty()) << "read returned no data";
    EXPECT_EQ(read_data[0], 0xAA) << "read returned stale data (0x" << std::hex
                                  << static_cast<int>(read_data[0]) << "), expected 0xAA";
}
