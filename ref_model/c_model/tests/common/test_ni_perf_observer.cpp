#include "common/ni_perf_observer.hpp"
#include <gtest/gtest.h>

using ni::cmodel::testing::NIPerfObserver;

TEST(NIPerfObserver, RecordsWriteAndReadLatency) {
    uint64_t now = 0;
    NIPerfObserver obs(now, "NI");

    now = 5;
    obs.on_issue(/*is_write=*/true, /*line=*/1);
    now = 6;
    obs.on_issue(true, 2);
    now = 10;
    obs.on_complete(true, 1);  // 10 - 5 = 5
    now = 12;
    obs.on_complete(true, 2);  // 12 - 6 = 6

    EXPECT_EQ(obs.write_latency().count(), 2u);
    EXPECT_EQ(obs.write_latency().min(), 5u);
    EXPECT_EQ(obs.write_latency().max(), 6u);
    EXPECT_EQ(obs.stuck_count(), 0u);
}

TEST(NIPerfObserver, StuckTransactionSurfaced) {
    uint64_t now = 0;
    NIPerfObserver obs(now, "NI");
    now = 1;
    obs.on_issue(false, 7);  // read issued, never completed
    EXPECT_EQ(obs.stuck_count(), 1u);
    EXPECT_EQ(obs.read_latency().count(), 0u);
}
