#include "common/perf_stats.hpp"
#include <gtest/gtest.h>

using ni::cmodel::testing::Stats;
using ni::cmodel::testing::StatsConfig;

TEST(PerfStats, ScalarsOverKnownSequence) {
    Stats s(StatsConfig{});
    for (uint64_t v : {2u, 4u, 6u, 8u}) s.add(v);
    EXPECT_EQ(s.count(), 4u);
    EXPECT_EQ(s.sum(), 20u);
    EXPECT_EQ(s.min(), 2u);
    EXPECT_EQ(s.max(), 8u);
    EXPECT_DOUBLE_EQ(s.mean(), 5.0);
}

TEST(PerfStats, EmptyIsSafe) {
    Stats s(StatsConfig{});
    EXPECT_EQ(s.count(), 0u);
    EXPECT_EQ(s.min(), 0u);
    EXPECT_EQ(s.max(), 0u);
    EXPECT_DOUBLE_EQ(s.mean(), 0.0);  // no div-by-zero
}
