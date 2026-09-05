#include "wrap/perf_collector.hpp"

#include <gtest/gtest.h>

using ni::cmodel::wrap::PerfCollector;

namespace {

// NoC-side perf: per-link flit/stall counters + per-router fifo occupancy
// (max-tracked across samples).
PerfCollector make_populated() {
    PerfCollector pc;
    pc.set_scenario("AX4-BAS-001");
    pc.begin(11);
    pc.set_link("req_0to1", 4, 1);
    pc.sample_router("req.R(0,0)", 2, 2);
    pc.sample_router("req.R(0,0)", 1, 1);  // max must stay 2/2
    pc.sample_router_dat_input_vc("router_0", "EAST", 1, 6, 8);
    pc.sample_router_dat_input_vc("router_0", "EAST", 1, 4, 8);  // max must stay 6
    pc.sample_router_dat_output_vc("router_0", "NORTH", 1, true);
    pc.sample_router_dat_output_vc("router_0", "NORTH", 1, false);
    pc.end(64);
    return pc;
}

TEST(PerfCollector, SamplesOnlyInsideAnActiveWindow) {
    PerfCollector pc;
    pc.set_link("before", 1, 1);
    pc.sample_router("before", 1, 1);
    pc.sample_router_dat_input_vc("before", "LOCAL", 0, 1, 8);
    pc.sample_router_dat_output_vc("before", "LOCAL", 0, true);

    pc.begin(17);
    EXPECT_TRUE(pc.active());
    pc.set_link("inside", 4, 1);
    pc.sample_router("inside", 2, 3);
    pc.sample_router_dat_input_vc("inside", "NORTH", 0, 2, 8);
    pc.sample_router_dat_output_vc("inside", "EAST", 0, true);
    pc.end(83);
    EXPECT_FALSE(pc.active());

    pc.set_link("after", 1, 1);
    pc.sample_router("after", 1, 1);
    pc.sample_router_dat_input_vc("after", "LOCAL", 0, 1, 8);
    pc.sample_router_dat_output_vc("after", "LOCAL", 0, true);
    const std::string j = pc.to_json();
    EXPECT_EQ(j.find("before"), std::string::npos);
    EXPECT_EQ(j.find("after"), std::string::npos);
    EXPECT_NE(j.find("inside"), std::string::npos);
    EXPECT_NE(j.find("\"window\":{\"start_cyc\":17,\"end_cyc\":83}"), std::string::npos);
}

TEST(PerfCollector, BeginClearsTheEarlierRun) {
    PerfCollector pc;
    pc.begin(3);
    pc.set_link("earlier", 9, 2);
    pc.sample_router("earlier", 4, 5);
    pc.sample_router_dat_input_vc("earlier", "LOCAL", 0, 4, 8);
    pc.sample_router_dat_output_vc("earlier", "LOCAL", 0, true);
    pc.end(8);

    pc.begin(101);
    pc.set_link("current", 7, 1);
    pc.sample_router("current", 2, 3);
    pc.sample_router_dat_input_vc("current", "SOUTH", 1, 2, 8);
    pc.sample_router_dat_output_vc("current", "WEST", 1, true);
    pc.end(211);

    const std::string j = pc.to_json();
    EXPECT_EQ(j.find("earlier"), std::string::npos);
    EXPECT_NE(j.find("current"), std::string::npos);
    EXPECT_NE(j.find("\"window\":{\"start_cyc\":101,\"end_cyc\":211}"), std::string::npos);
}

TEST(PerfCollector, EndMustFollowStart) {
    PerfCollector pc;
    pc.begin(17);
    EXPECT_THROW(pc.end(17), std::invalid_argument);
    EXPECT_TRUE(pc.active());
}

TEST(PerfCollector, RouterOccupancyTracksMax) {
    const std::string j = make_populated().to_json();
    EXPECT_NE(j.find("\"in_fifo_occ_max\":2"), std::string::npos);
    EXPECT_NE(j.find("\"out_fifo_occ_max\":2"), std::string::npos);
}

TEST(PerfCollector, RouterDatDiagnosticsArePerPortAndVc) {
    const std::string j = make_populated().to_json();
    EXPECT_NE(j.find("\"router_dat_input_vcs\":[{\"router\":\"router_0\",\"port\":\"EAST\","
                     "\"vc\":1,\"hwm_flits\":6,\"capacity_flits\":8}"),
              std::string::npos);
    EXPECT_NE(j.find("\"router_dat_output_vcs\":[{\"router\":\"router_0\",\"port\":\"NORTH\","
                     "\"vc\":1,\"credit_block_cycles\":1}"),
              std::string::npos);
}

TEST(PerfCollector, LinkCountersEmitted) {
    const std::string j = make_populated().to_json();
    EXPECT_NE(j.find("\"name\":\"req_0to1\",\"flit_count\":4,\"stall_cyc\":1"), std::string::npos);
}

}  // namespace
