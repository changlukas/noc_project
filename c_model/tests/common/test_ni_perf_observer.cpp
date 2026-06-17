#include "common/ni_perf_observer.hpp"
#include "common/perf_common.hpp"
#include <gtest/gtest.h>

using ni::cmodel::testing::NIPerfConfig;
using ni::cmodel::testing::NIPerfObserver;
using ni::cmodel::testing::PhaseConfig;
using ni::cmodel::testing::PhaseController;

TEST(NIPerfObserver, LatencyAndOutstandingPeak) {
    uint64_t now = 0;
    PhaseController phase(now, PhaseConfig{});  // measurement from cycle 0
    std::size_t fake_rob = 0;
    NIPerfObserver obs(now, phase, [&] { return fake_rob; }, NIPerfConfig{"NI"});

    now = 5;
    obs.on_issue(/*is_write=*/true, /*line=*/1);  // w outstanding=1
    now = 6;
    obs.on_issue(true, 2);  // w outstanding=2 (peak)
    now = 10;
    obs.on_complete(true, 1);  // latency 10-5=5
    now = 12;
    obs.on_complete(true, 2);  // latency 12-6=6

    EXPECT_EQ(obs.outstanding_peak(), 2u);
    EXPECT_EQ(obs.write_latency().count(), 2u);
    EXPECT_EQ(obs.write_latency().min(), 5u);
    EXPECT_EQ(obs.write_latency().max(), 6u);
    EXPECT_EQ(obs.stuck_count(), 0u);
}

TEST(NIPerfObserver, WarmupIssuedNotCountedButOutstandingBalances) {
    uint64_t now = 0;
    PhaseController phase(now, PhaseConfig{/*warmup_cycles=*/10});
    NIPerfObserver obs(now, phase, [] { return std::size_t{0}; }, NIPerfConfig{"NI"});

    now = 2;
    obs.on_issue(true, 1);  // issued in Warmup -> not eligible
    now = 15;
    obs.on_complete(true, 1);                    // completes in Measurement
    EXPECT_EQ(obs.write_latency().count(), 0u);  // not recorded
    EXPECT_EQ(obs.stuck_count(), 0u);            // outstanding still balanced
}

TEST(NIPerfObserver, StuckTransactionSurfaced) {
    uint64_t now = 0;
    PhaseController phase(now, PhaseConfig{});
    NIPerfObserver obs(now, phase, [] { return std::size_t{0}; }, NIPerfConfig{"NI"});
    now = 1;
    obs.on_issue(false, 7);  // read issued, never completed
    EXPECT_EQ(obs.stuck_count(), 1u);
}

// H4: a transaction issued during Measurement but completing during Drain is
// still recorded, because eligibility is latched at issue, not re-checked at
// completion.
TEST(NIPerfObserver, MeasurementIssuedDrainCompletedStillRecorded) {
    uint64_t now = 0;
    PhaseController phase(now, PhaseConfig{});  // measurement from cycle 0
    NIPerfObserver obs(now, phase, [] { return std::size_t{0}; }, NIPerfConfig{"NI"});
    now = 3;
    obs.on_issue(true, 1);  // issued in Measurement -> eligible latched
    phase.begin_drain();    // now draining
    now = 9;
    obs.on_complete(true, 1);                    // completes in Drain
    EXPECT_EQ(obs.write_latency().count(), 1u);  // still recorded
    EXPECT_EQ(obs.write_latency().max(), 6u);    // 9 - 3
}
