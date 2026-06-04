// TDD smoke tests for AxiDpiAdapter.
//
// Fixture: burst_incr_8beat.yaml — single write + single read, simple INCR
// 8-beat burst. Copied to ./fixtures/ relative to the binary by CMakeLists.
// port_params.yaml is copied to ./config/ by CMakeLists.
//
// Test 1 (drives_aw_after_scenario_load): adapter must assert AWVALID
// (i.e. NMU aw_q non-empty) within 200 cycles of init.
//
// Test 2 (scenario_completes): adapter must reach done() within 5000 cycles
// AND scoreboard must be clean.
#include "common/scenario.hpp"
#include "cosim/axi_dpi_adapter.hpp"
#include "cosim/pin_snapshot.hpp"
#include <gtest/gtest.h>
#include <string>

using ni::cmodel::cosim::AwPins;
using ni::cmodel::cosim::AxiDpiAdapter;

static const std::string kScenarioYaml = "fixtures/burst_incr_8beat.yaml";

// Cycle-budget constants for loop guards.
static constexpr int kMaxAwVisibleCycles = 200;
static constexpr int kMaxScenarioCycles = 5000;

TEST(AxiDpiAdapter, drives_aw_after_scenario_load) {
    SCENARIO("AxiDpiAdapter drives AWVALID on NMU AXI boundary after init");

    AxiDpiAdapter adapter;
    adapter.init(kScenarioYaml);

    AwPins aw{};
    bool saw_awvalid = false;
    for (int cycle = 0; cycle < kMaxAwVisibleCycles && !saw_awvalid; ++cycle) {
        adapter.tick();
        adapter.get_nmu_aw(aw);
        if (aw.valid) saw_awvalid = true;
    }
    EXPECT_TRUE(saw_awvalid) << "no AWVALID asserted in first " << kMaxAwVisibleCycles << " cycles";
}

TEST(AxiDpiAdapter, scenario_completes) {
    SCENARIO("AxiDpiAdapter scoreboard drains after small scenario");

    AxiDpiAdapter adapter;
    adapter.init(kScenarioYaml);
    for (int cycle = 0; cycle < kMaxScenarioCycles && !adapter.done(); ++cycle) {
        adapter.tick();
    }
    EXPECT_TRUE(adapter.done()) << "scenario did not drain within " << kMaxScenarioCycles
                                << " cycles";
    EXPECT_TRUE(adapter.scoreboard_clean()) << "scoreboard reported mismatch";
}
