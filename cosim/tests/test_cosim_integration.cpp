// AXI4 cosim integration: runs each AX4-* scenario through Vtb_top (Verilator
// + wb2axip slave), asserts exit 0 AND PASS marker from tb_top.sv. Peer of
// c_model/tests/axi/test_integration.cpp — same scenario set, different
// execution path. wb2axip-blocked scenarios SKIP with a content-derived
// reason from wb2axip_block_reason(); INF scenarios SKIP by id prefix.
#include "axi/scenario_parser.hpp"
#include "common/scenario.hpp"
#include "scenarios_list.hpp"
#include "wb2axip_block.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <gtest/gtest.h>
#include <string>

namespace {

constexpr const char* kCosimBinaryEnv = "COSIM_BIN";
constexpr const char* kPassMarker = "PASS: scenario complete, scoreboard clean";

struct ProcResult {
    int rc;
    std::string output;
};

ProcResult run_and_capture(std::string const& cmd) {
    ProcResult r{};
    auto full = cmd + " 2>&1";
#ifdef _WIN32
    FILE* p = _popen(full.c_str(), "r");
#else
    FILE* p = popen(full.c_str(), "r");
#endif
    if (!p) {
        r.rc = -1;
        return r;
    }
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), p)) {
        r.output += buf;
    }
#ifdef _WIN32
    r.rc = _pclose(p);
#else
    r.rc = pclose(p);
#endif
    return r;
}

class CosimIntegration : public ::testing::TestWithParam<std::string_view> {};

TEST_P(CosimIntegration, ScenarioPassesWb2axip) {
    SCENARIO(("cosim integration: " + std::string{GetParam()}).c_str());
    auto scenario_id = std::string{GetParam()};
    auto scenario_path = std::string(SCENARIO_TREE_ROOT) + scenario_id + "/scenario.yaml";

    // Validate-before-skip.
    auto sc = ni::cmodel::axi::load_scenario(scenario_path);

    if (scenario_id.compare(0, 8, "AX4-INF-") == 0) {
        GTEST_SKIP() << "INF_DEDICATED_TEST";
    }
    if (auto reason = noc::tests::wb2axip_block_reason(sc); reason) {
        GTEST_SKIP() << *reason;
    }

    const char* bin = std::getenv(kCosimBinaryEnv);
    ASSERT_NE(bin, nullptr) << "COSIM_BIN env var not set";
    auto cmd = std::string(bin) + " +scenario=" + scenario_path;
    auto result = run_and_capture(cmd);
    EXPECT_EQ(result.rc, 0) << "scenario " << scenario_id << " failed (exit " << result.rc << ")\n"
                            << result.output;
    EXPECT_NE(result.output.find(kPassMarker), std::string::npos)
        << "no PASS marker for " << scenario_id << "\n"
        << result.output;
}

INSTANTIATE_TEST_SUITE_P(CosimIntegration, CosimIntegration,
                         ::testing::ValuesIn(noc::tests::kAllAxi4Scenarios),
                         [](::testing::TestParamInfo<std::string_view> const& info) {
                             std::string name{info.param};
                             std::replace(name.begin(), name.end(), '-', '_');
                             return name;
                         });

}  // namespace
