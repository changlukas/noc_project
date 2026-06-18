// AXI4 read-after-write (RAW) ordering test for AxiMaster.
//
// AXI4 IHI 0022 §A5.3.4: there is NO ordering between a write and a read
// to the same address unless the master waits for the write's B response
// before issuing the read's AR. The AxiMaster must enforce this.
//
// Scenario: write 0xAA to addr 0x1000, then read from 0x1000. The master
// must hold AR until the write B arrives. Failure mode (before the fix):
// AR issues in the same tick as AW, before B — stale read returns 0.
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
TEST(AxiMasterRawOrder, ArHeldUntilOverlappingWriteReceivesB) {
    SCENARIO(
        "AxiMaster RAW: write 0xAA to 0x1000 then read 0x1000; AR must not issue until "
        "the write's B response arrives (AXI4 §A5.3.4 master ordering)");

    const std::string scn = write_raw_scenario();
    axi::Memory mem(0x1000, 0x1000, /*write_latency=*/1, /*read_latency=*/0);
    axi::AxiSlave slave(mem);
    slave.set_memory_bounds(0x1000, 0x1000);

    axi::AxiMaster master(scn, slave, std::string(::testing::TempDir()) + "/raw.read.txt",
                          /*max_out_w=*/1, /*max_out_r=*/1);

    // Track: at what cycle did AR issue? At what cycle did B arrive?
    int ar_issue_cycle = -1;
    int b_complete_cycle = -1;
    std::vector<uint8_t> read_data;

    master.on_read_issued([&](const axi::IssueInfo&) {
        // This fires on the cycle the first AR is pushed to the slave.
        // We record it on the next observable state via cycle counter.
        ar_issue_cycle = 0;  // placeholder; will be set via flag below
    });
    master.on_write_completed([&](const axi::WriteResult&) {
        b_complete_cycle = 0;  // placeholder
    });
    master.on_read_observed([&](const axi::ReadResult& rr) { read_data = rr.data; });

    int cycle = 0;
    bool ar_issued = false;
    bool b_received = false;

    for (; cycle < 500 && !master.done(); ++cycle) {
        master.tick();
        slave.tick();
        mem.tick();

        // Detect AR issue: on_read_issued fires during master.tick() when push_ar
        // succeeds for the first sub-burst. We capture the cycle here.
        if (!ar_issued && ar_issue_cycle == 0) {
            ar_issued = true;
            ar_issue_cycle = cycle;
        }
        if (!b_received && b_complete_cycle == 0) {
            b_received = true;
            b_complete_cycle = cycle;
        }
    }

    ASSERT_TRUE(master.done()) << "master did not complete within 500 cycles";

    // AR must NOT issue before B is received.
    ASSERT_GE(ar_issue_cycle, 0) << "AR was never issued";
    ASSERT_GE(b_complete_cycle, 0) << "B was never received";

    EXPECT_GE(ar_issue_cycle, b_complete_cycle)
        << "AR issued on cycle " << ar_issue_cycle << " but write B arrived on cycle "
        << b_complete_cycle << ": master issued read before write committed (RAW hazard)";

    // Data integrity: read must return the written value.
    ASSERT_FALSE(read_data.empty()) << "read returned no data";
    EXPECT_EQ(read_data[0], 0xAA) << "read returned stale data (0x" << std::hex
                                  << static_cast<int>(read_data[0]) << "), expected 0xAA";
}
