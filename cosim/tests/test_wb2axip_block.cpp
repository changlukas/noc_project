// Unit tests for wb2axip_block_reason predicate (Codex-audited blockers).
#include "wb2axip_block.hpp"
#include <gtest/gtest.h>

namespace {

using ni::cmodel::axi::Burst;
using ni::cmodel::axi::LockType;
using ni::cmodel::axi::Scenario;
using ni::cmodel::axi::ScenarioTransaction;

Scenario base_scenario() {
    Scenario sc{};
    sc.config.memory_base = 0x1000;
    sc.config.memory_size = 0x1000;
    sc.config.max_outstanding_write = 1;
    sc.config.max_outstanding_read = 1;
    ScenarioTransaction t{};
    t.op = ScenarioTransaction::Op::Write;
    t.addr = 0x1000;
    t.id = 0;
    t.len = 0;
    t.size = 5;
    t.burst = Burst::INCR;
    t.lock = LockType::Normal;
    sc.transactions.push_back(t);
    return sc;
}

TEST(Wb2axipBlock, accepts_single_beat_single_outstanding) {
    auto sc = base_scenario();
    EXPECT_FALSE(noc::tests::wb2axip_block_reason(sc).has_value());
}

TEST(Wb2axipBlock, blocks_multi_outstanding_write) {
    auto sc = base_scenario();
    sc.config.max_outstanding_write = 4;
    auto r = noc::tests::wb2axip_block_reason(sc);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "WB2AXIP_MAX_OUT_WRITE");
}

TEST(Wb2axipBlock, blocks_multi_beat) {
    auto sc = base_scenario();
    sc.transactions[0].len = 7;
    auto r = noc::tests::wb2axip_block_reason(sc);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "WB2AXIP_MULTI_BEAT");
}

TEST(Wb2axipBlock, blocks_exclusive) {
    auto sc = base_scenario();
    sc.transactions[0].lock = LockType::Exclusive;
    auto r = noc::tests::wb2axip_block_reason(sc);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "WB2AXIP_EXCLUSIVE");
}

TEST(Wb2axipBlock, blocks_inject_mode) {
    auto sc = base_scenario();
    sc.config.inject.mode = ni::cmodel::axi::InjectConfig::Mode::AwUnstable;
    auto r = noc::tests::wb2axip_block_reason(sc);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "INJECTION_DEDICATED_TEST");
}

TEST(Wb2axipBlock, allows_multi_outstanding_read) {
    // Codex Finding 2: wb2axip does NOT block multi-outstanding-READ.
    auto sc = base_scenario();
    sc.config.max_outstanding_read = 4;
    EXPECT_FALSE(noc::tests::wb2axip_block_reason(sc).has_value());
}

TEST(Wb2axipBlock, allows_non_incr_single_beat) {
    // Codex Finding 2: WRAP/FIXED with len=0 are not structurally blocked.
    auto sc = base_scenario();
    sc.transactions[0].burst = Burst::FIXED;
    EXPECT_FALSE(noc::tests::wb2axip_block_reason(sc).has_value());
}

TEST(Wb2axipBlock, allows_oob_addr) {
    // Codex Finding 2: wb2axip is a protocol checker, not address-map predictor.
    auto sc = base_scenario();
    sc.transactions[0].addr = 0xDEADBEEF;
    EXPECT_FALSE(noc::tests::wb2axip_block_reason(sc).has_value());
}

TEST(Wb2axipBlock, returns_first_blocker_in_priority_order) {
    // Helper short-circuits at first match. Documented order:
    // max_outstanding_write > inject > multi_beat (per-txn) > exclusive (per-txn).
    // Combine 2+ blockers and verify the documented winner.
    auto sc = base_scenario();
    sc.config.max_outstanding_write = 4;
    sc.config.inject.mode = ni::cmodel::axi::InjectConfig::Mode::AwUnstable;
    sc.transactions[0].len = 7;
    sc.transactions[0].lock = ni::cmodel::axi::LockType::Exclusive;
    auto r = noc::tests::wb2axip_block_reason(sc);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "WB2AXIP_MAX_OUT_WRITE")
        << "max_outstanding_write must be first-priority blocker";
}

}  // namespace
