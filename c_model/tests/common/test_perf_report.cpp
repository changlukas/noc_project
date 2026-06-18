#include "common/perf_report.hpp"
#include <gtest/gtest.h>
#include <sstream>
#include <string>

using namespace ni::cmodel::testing;

namespace {
PerfReport make_report() {
    PerfReport rep;
    rep.set_scenario("AX4-BAS-003");
    rep.set_run_meta(RunMeta{"AX4-BAS-003", /*mesh_x=*/2, /*mesh_y=*/1, /*num_vc=*/1,
                             /*total_cycles=*/123, /*txn_count=*/1,
                             /*json_path=*/"build/cmodel/perf/AX4-BAS-003.json"});
    rep.set_slave_remainder(1);
    rep.add_transaction(TxnRecord{42,
                                  "write",
                                  0,
                                  "NMU0",
                                  "NSU1",
                                  {"NMU0", "R(0,0)", "R(1,0)", "NSU1"},
                                  {"NSU1", "R(1,0)", "R(0,0)", "NMU0"},
                                  18,
                                  9});
    // NMU: occupancy available.
    rep.add_ni(ComponentRecord{"NMU0", "nmu", 2, 2.0, 2, 4, 16, /*occ_available=*/true});
    // NSU: occupancy genuinely unavailable -> n/a, never 0.
    rep.add_ni(ComponentRecord{"NSU1", "nsu", 2, 2.0, 2, 0, 0, /*occ_available=*/false});
    rep.add_router(ComponentRecord{"R(1,0)", "router", 3, 3.0, 3, 2, 4, /*occ_available=*/true});
    return rep;
}
}  // namespace

TEST(PerfReport, StdoutHasRunMetaAndAlignedNamesNoKind) {
    std::ostringstream os;
    make_report().write_summary(os);
    const std::string out = os.str();
    EXPECT_NE(out.find("[perf:run]"), std::string::npos);
    EXPECT_NE(out.find("scenario=AX4-BAS-003"), std::string::npos);
    EXPECT_NE(out.find("mesh=2x1"), std::string::npos);
    EXPECT_NE(out.find("num_vc=1"), std::string::npos);
    EXPECT_NE(out.find("total_cycles=123"), std::string::npos);
    // Aligned field names (JSON-style) on the component line.
    EXPECT_NE(out.find("latency_cyc(min/mean/max)"), std::string::npos);
    EXPECT_NE(out.find("occupancy(max/capacity)"), std::string::npos);
    // kind= dropped from the stdout component line.
    EXPECT_EQ(out.find("] kind="), std::string::npos);
    // NSU occupancy printed as n/a, never as 0.
    EXPECT_NE(out.find("[perf:NSU1]"), std::string::npos);
    EXPECT_NE(out.find("occupancy(max/capacity)=n/a"), std::string::npos);
}

TEST(PerfReport, JsonHasRunBlockAndNaOccupancy) {
    std::ostringstream os;
    make_report().write_json(os);
    const std::string j = os.str();
    EXPECT_NE(j.find("\"run\":{"), std::string::npos);
    EXPECT_NE(j.find("\"mesh_x\":2"), std::string::npos);
    EXPECT_NE(j.find("\"mesh_y\":1"), std::string::npos);
    EXPECT_NE(j.find("\"num_vc\":1"), std::string::npos);
    EXPECT_NE(j.find("\"total_cycles\":123"), std::string::npos);
    EXPECT_NE(j.find("\"transaction_count\":1"), std::string::npos);
    EXPECT_NE(j.find("\"json_path\":\"build/cmodel/perf/AX4-BAS-003.json\""), std::string::npos);
    // NSU occupancy is the JSON null literal, not a measured 0.
    EXPECT_NE(j.find("\"NSU1\":{\"kind\":\"nsu\""), std::string::npos);
    EXPECT_NE(j.find("\"occupancy\":{\"max\":null,\"capacity\":null}"), std::string::npos);
    // Router occupancy still numeric.
    EXPECT_NE(j.find("\"occupancy\":{\"max\":2,\"capacity\":4}"), std::string::npos);
}
