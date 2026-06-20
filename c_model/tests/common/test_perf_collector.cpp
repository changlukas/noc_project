#include "cosim/perf_collector.hpp"

#include <gtest/gtest.h>

using ni::cmodel::cosim::PerfCollector;

namespace {

// One write (lat 42) + one read (lat 38) at the manager slot; the same two at
// the subordinate slot (service latencies 14/12). Mirrors spec §5.1 example.
PerfCollector make_populated() {
    PerfCollector pc;
    pc.set_scenario("AX4-BAS-003");
    pc.set_window(0, 64);
    // addr 0x100000000 routes to node1 via xy_route (bit32 remap).
    pc.add_txn("node0.manager", 3, true, 0x100000000ull, 7, 3, 10, 52);
    pc.add_txn("node0.manager", 5, false, 0x100000000ull, 7, 3, 12, 50);
    pc.add_txn("node1.subordinate", 3, true, 0x100000000ull, 7, 3, 30, 44);
    pc.add_txn("node1.subordinate", 5, false, 0x100000000ull, 7, 3, 28, 40);
    pc.set_slot_backpressure("node0.manager", 2, 0);
    pc.set_slot_backpressure("node1.subordinate", 3, 1);
    pc.set_link("req_0to1", 4, 1);
    pc.sample_router("req.R(0,0)", 2, 2);
    pc.sample_router("req.R(0,0)", 1, 1);  // max must stay 2/2
    return pc;
}

TEST(PerfCollector, ByteCountFromLenSize) {
    // (len+1)<<size = (7+1)<<3 = 64.
    PerfCollector pc = make_populated();
    const std::string j = pc.to_json();
    EXPECT_NE(j.find("\"write_byte_count\":64"), std::string::npos);
}

TEST(PerfCollector, ClassMinMeanMax) {
    PerfCollector pc;
    pc.add_txn("node0.manager", 1, true, 0x100000000ull, 7, 3, 0, 40);
    pc.add_txn("node0.manager", 1, true, 0x100000000ull, 7, 3, 0, 60);
    const std::string j = pc.to_json();
    EXPECT_NE(j.find("\"min\":40"), std::string::npos);
    EXPECT_NE(j.find("\"max\":60"), std::string::npos);
    EXPECT_NE(j.find("\"mean\":50"), std::string::npos);
}

TEST(PerfCollector, ServiceLatencyOnSubordinateOnly) {
    // A manager-only collector emits no service_latency; a subordinate-only one does.
    // Order-independent: tests field presence per slot role, not string position.
    PerfCollector mgr_only;
    mgr_only.add_txn("node0.manager", 3, true, 0x100000000ull, 7, 3, 10, 52);
    EXPECT_EQ(mgr_only.to_json().find("service_latency"), std::string::npos);

    PerfCollector sub_only;
    sub_only.add_txn("node1.subordinate", 3, true, 0x100000000ull, 7, 3, 30, 44);
    EXPECT_NE(sub_only.to_json().find("service_latency"), std::string::npos);
}

TEST(PerfCollector, RouterOccupancyTracksMax) {
    const std::string j = make_populated().to_json();
    EXPECT_NE(j.find("\"in_fifo_occ_max\":2"), std::string::npos);
}

TEST(PerfCollector, HistogramBinsByDefaultLadder) {
    const std::string j = make_populated().to_json();
    // 42 and 38 both fall in [32,64): count 2.
    EXPECT_NE(j.find("\"low\":32,\"high\":64,\"count\":2"), std::string::npos);
}

}  // namespace
