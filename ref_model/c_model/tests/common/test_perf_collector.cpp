#include "wrap/perf_collector.hpp"

#include <gtest/gtest.h>

using ni::cmodel::wrap::PerfCollector;

namespace {

// NoC-side perf: per-link flit/stall counters + per-router fifo occupancy
// (max-tracked across samples).
PerfCollector make_populated() {
    PerfCollector pc;
    pc.set_scenario("AX4-BAS-001");
    pc.set_window(0, 64);
    pc.set_link("req_0to1", 4, 1);
    pc.sample_router("req.R(0,0)", 2, 2);
    pc.sample_router("req.R(0,0)", 1, 1);  // max must stay 2/2
    return pc;
}

TEST(PerfCollector, RouterOccupancyTracksMax) {
    const std::string j = make_populated().to_json();
    EXPECT_NE(j.find("\"in_fifo_occ_max\":2"), std::string::npos);
    EXPECT_NE(j.find("\"out_fifo_occ_max\":2"), std::string::npos);
}

TEST(PerfCollector, LinkCountersEmitted) {
    const std::string j = make_populated().to_json();
    EXPECT_NE(j.find("\"name\":\"req_0to1\",\"flit_count\":4,\"stall_cyc\":1"), std::string::npos);
}

}  // namespace
