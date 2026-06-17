#include "common/perf_report.hpp"
#include <gtest/gtest.h>
#include <sstream>
#include <string>

using namespace ni::cmodel::testing;

TEST(PerfReport, SummaryLineContainsLabels) {
    uint64_t now = 0;
    NIPerfObserver ni(now, "flowA");
    now = 1;
    ni.on_issue(true, 1);
    now = 4;
    ni.on_complete(true, 1);

    std::ostringstream os;
    PerfReport rep;
    rep.add_ni(&ni);
    rep.write_summary(os);
    const std::string out = os.str();
    EXPECT_NE(out.find("[perf:flowA]"), std::string::npos);
    EXPECT_NE(out.find("wr_lat"), std::string::npos);
}
