#include "common/perf_common.hpp"
#include <gtest/gtest.h>

using ni::cmodel::testing::Phase;
using ni::cmodel::testing::PhaseConfig;
using ni::cmodel::testing::PhaseController;

TEST(PerfPhase, WarmupThenMeasurementThenDrain) {
    uint64_t now = 0;
    PhaseController p(now, PhaseConfig{/*warmup_cycles=*/3});
    now = 0;
    EXPECT_EQ(p.phase(), Phase::Warmup);
    now = 2;
    EXPECT_EQ(p.phase(), Phase::Warmup);
    now = 3;
    EXPECT_EQ(p.phase(), Phase::Measurement);
    now = 9;
    EXPECT_EQ(p.phase(), Phase::Measurement);
    p.begin_drain();
    EXPECT_EQ(p.phase(), Phase::Drain);
}

TEST(PerfPhase, ZeroWarmupStartsInMeasurement) {
    uint64_t now = 0;
    PhaseController p(now, PhaseConfig{});  // warmup_cycles=0
    EXPECT_EQ(p.phase(), Phase::Measurement);
}
