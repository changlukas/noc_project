#include "common/perf_stats.hpp"
#include <gtest/gtest.h>

using ni::cmodel::testing::Stats;
using ni::cmodel::testing::StatsConfig;

TEST(PerfStats, ScalarsOverKnownSequence) {
    Stats s(StatsConfig{});  // no histogram
    for (uint64_t v : {2u, 4u, 6u, 8u}) s.add(v);
    EXPECT_EQ(s.count(), 4u);
    EXPECT_EQ(s.sum(), 20u);
    EXPECT_EQ(s.min(), 2u);
    EXPECT_EQ(s.max(), 8u);
    EXPECT_DOUBLE_EQ(s.mean(), 5.0);
    EXPECT_DOUBLE_EQ(s.variance(), 5.0);  // ((9+1+1+9)/4) = 5
    EXPECT_TRUE(s.histogram().empty());
}

TEST(PerfStats, EmptyIsSafe) {
    Stats s(StatsConfig{});
    EXPECT_EQ(s.count(), 0u);
    EXPECT_EQ(s.min(), 0u);
    EXPECT_EQ(s.max(), 0u);
    EXPECT_DOUBLE_EQ(s.mean(), 0.0);  // no div-by-zero
    EXPECT_DOUBLE_EQ(s.variance(), 0.0);
}

TEST(PerfStats, ThresholdBinsCountCorrectly) {
    // thresholds {10,20}: bin0=[0,10) bin1=[10,20) bin2=[20,inf)
    Stats s(StatsConfig{{10, 20}});
    for (uint64_t v : {5u, 9u, 10u, 15u, 20u, 99u}) s.add(v);
    ASSERT_EQ(s.histogram().size(), 3u);
    EXPECT_EQ(s.histogram()[0], 2u);  // 5,9
    EXPECT_EQ(s.histogram()[1], 2u);  // 10,15
    EXPECT_EQ(s.histogram()[2], 2u);  // 20,99
}
