#include "common/perf_report.hpp"
#include <gtest/gtest.h>
#include <sstream>
#include <string>

using namespace ni::cmodel::testing;

TEST(PerfReport, JsonContainsSectionEightShape) {
    PerfReport rep;
    rep.set_scenario("AX4-BAS-003");
    rep.add_transaction(TxnRecord{/*line=*/42,
                                  "read",
                                  /*id=*/3,
                                  "NMU0",
                                  "NSU1",
                                  {"NMU0", "R(0,0)", "R(1,0)", "NSU1"},
                                  {"NSU1", "R(1,0)", "R(0,0)", "NMU0"},
                                  /*measured=*/18,
                                  /*zero_load=*/9});
    rep.add_ni(ComponentRecord{"NMU0", "nmu", 2, 3.1, 6, 4, 4});
    rep.add_ni(ComponentRecord{"NSU1", "nsu", 2, 2.4, 5, 3, 32});
    rep.add_router(ComponentRecord{"R(0,0)", "router", 3, 4.0, 9, 3, 4});

    std::ostringstream js;
    rep.write_json(js);
    const std::string j = js.str();
    EXPECT_NE(j.find("\"scenario\":\"AX4-BAS-003\""), std::string::npos);
    EXPECT_NE(j.find("\"queueing_cyc\":9"), std::string::npos);  // 18 - 9
    EXPECT_NE(j.find("\"zero_load_cyc\":9"), std::string::npos);
    EXPECT_NE(j.find("\"kind\":\"nmu\""), std::string::npos);
    EXPECT_NE(j.find("\"kind\":\"nsu\""), std::string::npos);
    EXPECT_NE(j.find("\"capacity\":32"), std::string::npos);
    EXPECT_NE(j.find("\"R(0,0)\""), std::string::npos);

    std::ostringstream os;
    rep.write_summary(os);
    const std::string s = os.str();
    EXPECT_NE(s.find("NMU0"), std::string::npos);
    EXPECT_NE(s.find("R(0,0)"), std::string::npos);
}
