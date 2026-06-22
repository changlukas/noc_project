// AXI4 cosim integration: runs each AX4-* scenario through Vtb_top, asserts
// exit 0 AND PASS marker from tb_top.sv. Peer of
// c_model/tests/axi/test_integration.cpp — same scenario set, different
// execution path. INF scenarios SKIP by id prefix.
#include "axi/scenario_parser.hpp"
#include "common/scenario.hpp"
#include "scenarios_list.hpp"
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

    // tb_top's non-vacuous PASS guard requires reads_checked > 0; write-only
    // scenarios would FAIL it. Skip them — the bidirectional path needs a read.
    bool has_read = std::any_of(
        sc.transactions.begin(), sc.transactions.end(),
        [](const ni::cmodel::axi::ScenarioTransaction& t) {
            return t.op == ni::cmodel::axi::ScenarioTransaction::Op::Read;
        });
    if (!has_read) GTEST_SKIP() << "BIDIR_REQUIRES_READ";

    const char* bin = std::getenv(kCosimBinaryEnv);
    ASSERT_NE(bin, nullptr) << "COSIM_BIN env var not set";
    // Drive both nodes from this scenario's coordinate variants: node0 =
    // identity (low addr), node1 = +0x1_0000_0000 (coordinate (1,0)). Variants
    // are materialized + committed under tests/scenarios/<id>/node{0,1}/ only
    // for the bidirectional bring-up subset; a qualifying scenario outside that
    // subset has no variant tree — skip rather than fail on a missing file.
    // COORD_VARIANT_ROOT == the scenario-tree root (trailing slash).
    auto base = std::string(COORD_VARIANT_ROOT) + scenario_id;
    auto node0_path = base + "/node0/scenario.yaml";
    if (FILE* f = std::fopen(node0_path.c_str(), "r")) {
        std::fclose(f);
    } else {
        GTEST_SKIP() << "NOT_IN_BIDIR_SUBSET";
    }
    auto cmd = std::string(bin) + " +scenario_node0=" + base + "/node0/scenario.yaml" +
               " +scenario_node1=" + base + "/node1/scenario.yaml";
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
