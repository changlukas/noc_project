// Independent design; AXI4 IHI 0022 rule extraction inspired by cocotbext-axi.
//
// Death tests for protocol_rules.hpp. Strategy: each test calls the
// corresponding inline helper through an AXI_PROTOCOL_ASSERT wrapper so the
// failure path is identical to the production call sites in AxiSlave /
// AxiMaster. This isolates the helper under test from the surrounding tick()
// machinery and keeps each death test small and fast.
//
// In release builds (NDEBUG) the macro is a no-op, so every death test
// becomes meaningless — we GTEST_SKIP() the whole suite.
#include "axi/protocol_rules.hpp"
#include "axi/types.hpp"
#include "axi/axi_slave.hpp"
#include "mock_memory_port.hpp"
#include <gtest/gtest.h>
#include <deque>
#include <map>

namespace axi   = ni::cmodel::axi;
namespace rules = ni::cmodel::axi::rules;
namespace test  = ni::cmodel::axi::testing;

#ifdef NDEBUG

TEST(AxiProtocolDeath, AllSkippedInReleaseBuild) {
  GTEST_SKIP() << "AXI_PROTOCOL_ASSERT compiled out under NDEBUG";
}

#else

// Death-test style: threadsafe avoids fork() issues on Windows.
class AxiProtocolDeath : public ::testing::Test {
protected:
  void SetUp() override {
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  }
};

// ============================================================================
// Stateless field checks
// ============================================================================

TEST_F(AxiProtocolDeath, BurstEncoding_RejectsInvalid) {
  // Burst values 0/1/2 are FIXED/INCR/WRAP; 3 (or any other) is illegal.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(
        rules::check_burst_encoding(static_cast<axi::Burst>(3)),
        "BURST_ENCODING");
  }, "BURST_ENCODING");
}

TEST_F(AxiProtocolDeath, BurstEncoding_AcceptsAllThreeLegalValues) {
  // Sanity: legal values do NOT trip the assert (positive control).
  EXPECT_TRUE(rules::check_burst_encoding(axi::Burst::FIXED));
  EXPECT_TRUE(rules::check_burst_encoding(axi::Burst::INCR));
  EXPECT_TRUE(rules::check_burst_encoding(axi::Burst::WRAP));
}

TEST_F(AxiProtocolDeath, SizeBound_RejectsAboveMax) {
  // DATA_BYTES = 32 → max size = 5; size = 6 must trip.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(rules::check_size_bound(6), "SIZE_BOUND");
  }, "SIZE_BOUND");
}

TEST_F(AxiProtocolDeath, WrapLen_RejectsLen2) {
  // WRAP requires len ∈ {1,3,7,15}; len = 2 is illegal.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(rules::check_wrap_len(axi::Burst::WRAP, 2),
                        "WRAP_LEN");
  }, "WRAP_LEN");
}

TEST_F(AxiProtocolDeath, WrapLen_IgnoresNonWrap) {
  // INCR len = 2 must pass (rule is WRAP-specific).
  EXPECT_TRUE(rules::check_wrap_len(axi::Burst::INCR, 2));
}

TEST_F(AxiProtocolDeath, WrapAlign_RejectsUnaligned) {
  // WRAP addr 0x1001 with size 2 (4-byte beats) is misaligned.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(
        rules::check_wrap_align(axi::Burst::WRAP, 0x1001, 2),
        "WRAP_ALIGN");
  }, "WRAP_ALIGN");
}

TEST_F(AxiProtocolDeath, RespEncoding_RejectsInvalid) {
  // Resp values 0..3 are legal; 4 is not.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(
        rules::check_resp_encoding(static_cast<axi::Resp>(4)),
        "RESP_ENCODING");
  }, "RESP_ENCODING");
}

// ============================================================================
// Stateful intra-burst checks
// ============================================================================

TEST_F(AxiProtocolDeath, WBeatCountWithin_RejectsOverflow) {
  // Burst len = 3 (4 beats); submitting a 5th beat must trip.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(rules::check_w_beat_count_within(5, 3),
                        "W_BEAT_COUNT_WITHIN");
  }, "W_BEAT_COUNT_WITHIN");
}

TEST_F(AxiProtocolDeath, WLastTiming_RejectsEarlyLast) {
  // len = 3 → WLAST belongs on beat_idx = 3; setting WLAST on beat_idx = 1
  // must trip.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(rules::check_w_last_timing(true, 1, 3),
                        "W_LAST_TIMING");
  }, "W_LAST_TIMING");
}

TEST_F(AxiProtocolDeath, WLastTiming_RejectsMissingLast) {
  // Missing WLAST on the final beat must trip.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(rules::check_w_last_timing(false, 3, 3),
                        "W_LAST_TIMING");
  }, "W_LAST_TIMING");
}

TEST_F(AxiProtocolDeath, RBeatCountWithin_RejectsOverflow) {
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(rules::check_r_beat_count_within(5, 3),
                        "R_BEAT_COUNT_WITHIN");
  }, "R_BEAT_COUNT_WITHIN");
}

TEST_F(AxiProtocolDeath, RLastTiming_RejectsEarlyLast) {
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(rules::check_r_last_timing(true, 1, 3),
                        "R_LAST_TIMING");
  }, "R_LAST_TIMING");
}

TEST_F(AxiProtocolDeath, BOneResponsePerWrite_RejectsOverflow) {
  // Operation has 2 sub-bursts; observing a 3rd B response must trip.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(rules::check_b_one_response_per_write(3, 2),
                        "B_ONE_RESPONSE_PER_WRITE");
  }, "B_ONE_RESPONSE_PER_WRITE");
}

TEST_F(AxiProtocolDeath, StrbValidBits_AcceptsAllOnesAt32Bytes) {
  // DATA_BYTES = WSTRB_WIDTH = 32 → all 32 bits are valid. The rule is a
  // tautology at this bus width; document via a positive assertion.
  EXPECT_TRUE(rules::check_strb_valid_bits(0xFFFF'FFFFu));
}

TEST_F(AxiProtocolDeath, StrbSparseLegal_RejectsBitsOutsideWindow) {
  // beat_addr 0x1000 size 2 → byte_lane 0, window = 0x0000'000F.
  // Setting bit 4 (outside the 4-byte window) must trip.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(
        rules::check_strb_sparse_legal(0x0000'0010u, /*size=*/2,
                                       /*beat_addr=*/0x1000),
        "STRB_SPARSE_LEGAL");
  }, "STRB_SPARSE_LEGAL");
}

TEST_F(AxiProtocolDeath, StrbSparseLegal_AcceptsSubsetOfWindow) {
  // Within the window: 0x000F at byte_lane 0 is legal; 0x0005 (a subset)
  // is also legal (sparse but bounded).
  EXPECT_TRUE(rules::check_strb_sparse_legal(0x0000'0005u, /*size=*/2,
                                             /*beat_addr=*/0x1000));
}

TEST_F(AxiProtocolDeath, Cross4kb_RejectsIncrCrossing) {
  // addr 0x0FE0, size 5 (32-byte beats), len 1 (2 beats) →
  // bytes [0x0FE0..0x101F] crosses 0x1000.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(
        rules::check_4kb_cross(0x0FE0, /*len=*/1, /*size=*/5,
                               axi::Burst::INCR),
        "CROSS_4KB");
  }, "CROSS_4KB");
}

TEST_F(AxiProtocolDeath, Cross4kb_AcceptsFixedAndWrap) {
  // FIXED and WRAP are exempt by spec; rule must return true regardless.
  EXPECT_TRUE(rules::check_4kb_cross(0x0FE0, 1, 5, axi::Burst::FIXED));
  EXPECT_TRUE(rules::check_4kb_cross(0x0FE0, 1, 5, axi::Burst::WRAP));
}

// ============================================================================
// Cross-channel ordering checks
// ============================================================================

TEST_F(AxiProtocolDeath, BIdMatchOutstanding_RejectsUnknownId) {
  std::map<uint8_t, std::deque<int>> outstanding;
  outstanding[7].push_back(1);
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(rules::check_b_id_match_outstanding<std::deque<int>>(
                            /*b_id=*/3, outstanding),
                        "B_ID_MATCH_OUTSTANDING");
  }, "B_ID_MATCH_OUTSTANDING");
}

TEST_F(AxiProtocolDeath, RIdMatchOutstanding_RejectsUnknownId) {
  std::map<uint8_t, std::deque<int>> outstanding;
  outstanding[7].push_back(1);
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(rules::check_r_id_match_outstanding<std::deque<int>>(
                            /*r_id=*/3, outstanding),
                        "R_ID_MATCH_OUTSTANDING");
  }, "R_ID_MATCH_OUTSTANDING");
}

TEST_F(AxiProtocolDeath, WBeforeB_RejectsEarlyB) {
  // all_w_done = false → assert trips (B fired before W complete).
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(rules::check_w_before_b(false), "W_BEFORE_B");
  }, "W_BEFORE_B");
}

// ============================================================================
// Tautology rules — documented via positive assertions (no death tests).
// These rules are structurally guaranteed by the model: AXI4 W beats have no
// WID (W_NO_INTERLEAVE), AW and W are independent channels (AW_W_INDEPENDENCE),
// and AXI4 permits per-id R interleaving across ids (DIFF_ID_INTERLEAVE).
// Same-id W/R ordering is enforced by per-id FIFO at slave + master.
// ============================================================================

TEST_F(AxiProtocolDeath, Tautology_WNoInterleave) {
  EXPECT_TRUE(rules::check_w_no_interleave());
}
TEST_F(AxiProtocolDeath, Tautology_AwWIndependence) {
  EXPECT_TRUE(rules::check_aw_w_independence());
}
TEST_F(AxiProtocolDeath, Tautology_DiffIdInterleaveAllowed) {
  EXPECT_TRUE(rules::check_diff_id_interleave_allowed());
}
TEST_F(AxiProtocolDeath, Tautology_SameIdWOrder) {
  EXPECT_TRUE(rules::check_same_id_w_order());
}
TEST_F(AxiProtocolDeath, Tautology_SameIdROrder) {
  EXPECT_TRUE(rules::check_same_id_r_order());
}

// ============================================================================
// Integration: an end-to-end assert path via AxiSlave::tick().
// Confirms that an inline AXI_PROTOCOL_ASSERT call site in production code
// actually fires when given malformed input. Picks the most easily reachable
// rule (BURST_ENCODING via a corrupted AW.burst).
// ============================================================================

TEST_F(AxiProtocolDeath, AxiSlave_FiresOnInvalidAwBurstEncoding) {
  test::MockMemoryPort mem;
  axi::AxiSlave slave(mem);
  axi::AwBeat aw{};
  aw.id = 0; aw.addr = 0x1000; aw.len = 0; aw.size = 5;
  aw.burst = static_cast<axi::Burst>(3);  // invalid
  slave.push_aw(aw);
  EXPECT_DEATH({ slave.tick(); }, "BURST_ENCODING");
}

// ============================================================================
// Phase C: AXI4 Exclusive Access (IHI 0022 §A7.2.4)
// ============================================================================

TEST_F(AxiProtocolDeath, LockEncoding_RejectsRawTwo) {
  // AXI4 AxLOCK is 1-bit; raw 2 (AXI3 LOCKED) is illegal.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(rules::check_lock_encoding(2),
                        "LOCK_ENCODING: invalid raw lock value");
  }, "LOCK_ENCODING");
}

TEST_F(AxiProtocolDeath, ExclusiveTotalBytes_Rejects256) {
  // len = 7 (8 beats) × size = 5 (32 B) = 256 B > 128 B max.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(
        rules::check_exclusive_total_bytes_le_max(axi::LockType::Exclusive, 7, 5),
        "EXCLUSIVE_TOTAL_BYTES: exceeds 128");
  }, "EXCLUSIVE_TOTAL_BYTES");
}

TEST_F(AxiProtocolDeath, ExclusiveTotalBeats_Rejects32) {
  // len = 31 → 32 beats > 16-beat max.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(
        rules::check_exclusive_total_beats_le_max(axi::LockType::Exclusive, 31),
        "EXCLUSIVE_TOTAL_BEATS: exceeds 16");
  }, "EXCLUSIVE_TOTAL_BEATS");
}

TEST_F(AxiProtocolDeath, ExclusivePow2_RejectsLen2) {
  // len = 2 → 3 beats, not a power of 2.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(
        rules::check_exclusive_total_pow2(axi::LockType::Exclusive, 2),
        "EXCLUSIVE_POW2: total beats not power of 2");
  }, "EXCLUSIVE_POW2");
}

TEST_F(AxiProtocolDeath, ExclusiveAlign_RejectsUnaligned) {
  // size = 5 (32 B), len = 0 → total 32 B; addr 0x1004 not aligned to 32.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(
        rules::check_exclusive_addr_aligned_to_total(
            axi::LockType::Exclusive, 0x1004, 0, 5),
        "EXCLUSIVE_ALIGN: addr not aligned to total");
  }, "EXCLUSIVE_ALIGN");
}

TEST_F(AxiProtocolDeath, ExclusiveBurstFixed_Rejects) {
  // FIXED is illegal for exclusive.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(
        rules::check_exclusive_burst_not_fixed(
            axi::LockType::Exclusive, axi::Burst::FIXED),
        "EXCLUSIVE_BURST_FIXED: FIXED not allowed for exclusive");
  }, "EXCLUSIVE_BURST_FIXED");
}

// Positive controls: legal exclusive bursts must satisfy every rule.
TEST_F(AxiProtocolDeath, ExclusiveValid_INCR_1Beat_Size5) {
  // 1 beat × 32 B = 32 B, aligned to 32, INCR — valid exclusive.
  EXPECT_TRUE(rules::check_lock_encoding(1));
  EXPECT_TRUE(rules::check_exclusive_total_bytes_le_max(
      axi::LockType::Exclusive, 0, 5));
  EXPECT_TRUE(rules::check_exclusive_total_beats_le_max(
      axi::LockType::Exclusive, 0));
  EXPECT_TRUE(rules::check_exclusive_total_pow2(axi::LockType::Exclusive, 0));
  EXPECT_TRUE(rules::check_exclusive_addr_aligned_to_total(
      axi::LockType::Exclusive, 0x1000, 0, 5));
  EXPECT_TRUE(rules::check_exclusive_burst_not_fixed(
      axi::LockType::Exclusive, axi::Burst::INCR));
}

TEST_F(AxiProtocolDeath, ExclusiveValid_WRAP_4Beat_Size5) {
  // 4 beats × 32 B = 128 B (boundary), aligned to 128, WRAP — valid exclusive.
  EXPECT_TRUE(rules::check_exclusive_total_bytes_le_max(
      axi::LockType::Exclusive, 3, 5));
  EXPECT_TRUE(rules::check_exclusive_total_beats_le_max(
      axi::LockType::Exclusive, 3));
  EXPECT_TRUE(rules::check_exclusive_total_pow2(axi::LockType::Exclusive, 3));
  EXPECT_TRUE(rules::check_exclusive_addr_aligned_to_total(
      axi::LockType::Exclusive, 0x1000, 3, 5));
  EXPECT_TRUE(rules::check_exclusive_burst_not_fixed(
      axi::LockType::Exclusive, axi::Burst::WRAP));
}

#endif  // NDEBUG
