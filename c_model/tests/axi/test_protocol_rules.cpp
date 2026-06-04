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
#include "common/scenario.hpp"
#include "mock_memory_port.hpp"
#include <gtest/gtest.h>
#include <deque>
#include <map>

namespace axi   = ni::cmodel::axi;
namespace rules = ni::cmodel::axi::rules;
namespace test  = ni::cmodel::axi::testing;

#ifdef NDEBUG

TEST(AxiProtocolDeath, AllSkippedInReleaseBuild) {
  SCENARIO("protocol_rules: NDEBUG build compiles out AXI_PROTOCOL_ASSERT (test suite skipped)");
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
  SCENARIO("protocol_rules: BURST_ENCODING rejects Burst value outside {FIXED,INCR,WRAP}");
  // Burst values 0/1/2 are FIXED/INCR/WRAP; 3 (or any other) is illegal.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(
        rules::check_burst_encoding(static_cast<axi::Burst>(3)),
        "BURST_ENCODING");
  }, ".*");
}

TEST_F(AxiProtocolDeath, BurstEncoding_AcceptsAllThreeLegalValues) {
  SCENARIO("protocol_rules: BURST_ENCODING accepts all three legal Burst values (positive control)");
  // Sanity: legal values do NOT trip the assert (positive control).
  EXPECT_TRUE(rules::check_burst_encoding(axi::Burst::FIXED));
  EXPECT_TRUE(rules::check_burst_encoding(axi::Burst::INCR));
  EXPECT_TRUE(rules::check_burst_encoding(axi::Burst::WRAP));
}

TEST_F(AxiProtocolDeath, SizeBound_RejectsAboveMax) {
  SCENARIO("protocol_rules: SIZE_BOUND rejects size > log2(DATA_BYTES) (>5 at 32B bus)");
  // DATA_BYTES = 32 → max size = 5; size = 6 must trip.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(rules::check_size_bound(6), "SIZE_BOUND");
  }, ".*");
}

TEST_F(AxiProtocolDeath, WrapLen_RejectsLen2) {
  SCENARIO("protocol_rules: WRAP_LEN rejects WRAP len not in {1,3,7,15}");
  // WRAP requires len ∈ {1,3,7,15}; len = 2 is illegal.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(rules::check_wrap_len(axi::Burst::WRAP, 2),
                        "WRAP_LEN");
  }, ".*");
}

TEST_F(AxiProtocolDeath, WrapLen_IgnoresNonWrap) {
  SCENARIO("protocol_rules: WRAP_LEN does not apply to INCR/FIXED (rule is WRAP-specific)");
  // INCR len = 2 must pass (rule is WRAP-specific).
  EXPECT_TRUE(rules::check_wrap_len(axi::Burst::INCR, 2));
}

TEST_F(AxiProtocolDeath, WrapAlign_RejectsUnaligned) {
  SCENARIO("protocol_rules: WRAP_ALIGN rejects WRAP addr not aligned to (1<<size)");
  // WRAP addr 0x1001 with size 2 (4-byte beats) is misaligned.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(
        rules::check_wrap_align(axi::Burst::WRAP, 0x1001, 2),
        "WRAP_ALIGN");
  }, ".*");
}

TEST_F(AxiProtocolDeath, RespEncoding_RejectsInvalid) {
  SCENARIO("protocol_rules: RESP_ENCODING rejects Resp value outside {OKAY,EXOKAY,SLVERR,DECERR}");
  // Resp values 0..3 are legal; 4 is not.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(
        rules::check_resp_encoding(static_cast<axi::Resp>(4)),
        "RESP_ENCODING");
  }, ".*");
}

// ============================================================================
// Stateful intra-burst checks
// ============================================================================

TEST_F(AxiProtocolDeath, WBeatCountWithin_RejectsOverflow) {
  SCENARIO("protocol_rules: W_BEAT_COUNT_WITHIN trips when W beat count > (len+1)");
  // Burst len = 3 (4 beats); submitting a 5th beat must trip.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(rules::check_w_beat_count_within(5, 3),
                        "W_BEAT_COUNT_WITHIN");
  }, ".*");
}

TEST_F(AxiProtocolDeath, WLastTiming_RejectsEarlyLast) {
  SCENARIO("protocol_rules: W_LAST_TIMING trips when WLAST asserted before final beat");
  // len = 3 → WLAST belongs on beat_idx = 3; setting WLAST on beat_idx = 1
  // must trip.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(rules::check_w_last_timing(true, 1, 3),
                        "W_LAST_TIMING");
  }, ".*");
}

TEST_F(AxiProtocolDeath, WLastTiming_RejectsMissingLast) {
  SCENARIO("protocol_rules: W_LAST_TIMING trips when final beat lacks WLAST");
  // Missing WLAST on the final beat must trip.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(rules::check_w_last_timing(false, 3, 3),
                        "W_LAST_TIMING");
  }, ".*");
}

TEST_F(AxiProtocolDeath, RBeatCountWithin_RejectsOverflow) {
  SCENARIO("protocol_rules: R_BEAT_COUNT_WITHIN trips when R beat count > (len+1)");
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(rules::check_r_beat_count_within(5, 3),
                        "R_BEAT_COUNT_WITHIN");
  }, ".*");
}

TEST_F(AxiProtocolDeath, RLastTiming_RejectsEarlyLast) {
  SCENARIO("protocol_rules: R_LAST_TIMING trips when RLAST asserted before final beat");
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(rules::check_r_last_timing(true, 1, 3),
                        "R_LAST_TIMING");
  }, ".*");
}

TEST_F(AxiProtocolDeath, BOneResponsePerWrite_RejectsOverflow) {
  SCENARIO("protocol_rules: B_ONE_RESPONSE_PER_WRITE trips when B count exceeds sub-burst count");
  // Operation has 2 sub-bursts; observing a 3rd B response must trip.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(rules::check_b_one_response_per_write(3, 2),
                        "B_ONE_RESPONSE_PER_WRITE");
  }, ".*");
}

TEST_F(AxiProtocolDeath, StrbValidBits_AcceptsAllOnesAt32Bytes) {
  SCENARIO("protocol_rules: STRB_VALID_BITS is tautological at 32B bus (all 32 lanes legal)");
  // DATA_BYTES = WSTRB_WIDTH = 32 → all 32 bits are valid. The rule is a
  // tautology at this bus width; document via a positive assertion.
  EXPECT_TRUE(rules::check_strb_valid_bits(0xFFFF'FFFFu));
}

TEST_F(AxiProtocolDeath, StrbSparseLegal_RejectsBitsOutsideWindow) {
  SCENARIO("protocol_rules: STRB_SPARSE_LEGAL rejects strb bits outside narrow-burst byte_lane window");
  // beat_addr 0x1000 size 2 → byte_lane 0, window = 0x0000'000F.
  // Setting bit 4 (outside the 4-byte window) must trip.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(
        rules::check_strb_sparse_legal(0x0000'0010u, /*size=*/2,
                                       /*beat_addr=*/0x1000),
        "STRB_SPARSE_LEGAL");
  }, ".*");
}

TEST_F(AxiProtocolDeath, StrbSparseLegal_AcceptsSubsetOfWindow) {
  SCENARIO("protocol_rules: STRB_SPARSE_LEGAL accepts any subset of the byte_lane window (sparse OK)");
  // Within the window: 0x000F at byte_lane 0 is legal; 0x0005 (a subset)
  // is also legal (sparse but bounded).
  EXPECT_TRUE(rules::check_strb_sparse_legal(0x0000'0005u, /*size=*/2,
                                             /*beat_addr=*/0x1000));
}

TEST_F(AxiProtocolDeath, Cross4kb_RejectsIncrCrossing) {
  SCENARIO("protocol_rules: CROSS_4KB rejects INCR burst whose beat range crosses 4KB page boundary");
  // addr 0x0FE0, size 5 (32-byte beats), len 1 (2 beats) →
  // bytes [0x0FE0..0x101F] crosses 0x1000.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(
        rules::check_4kb_cross(0x0FE0, /*len=*/1, /*size=*/5,
                               axi::Burst::INCR),
        "CROSS_4KB");
  }, ".*");
}

TEST_F(AxiProtocolDeath, Cross4kb_AcceptsFixedAndWrap) {
  SCENARIO("protocol_rules: CROSS_4KB exempts FIXED and WRAP per IHI 0022 (rule INCR-specific)");
  // FIXED and WRAP are exempt by spec; rule must return true regardless.
  EXPECT_TRUE(rules::check_4kb_cross(0x0FE0, 1, 5, axi::Burst::FIXED));
  EXPECT_TRUE(rules::check_4kb_cross(0x0FE0, 1, 5, axi::Burst::WRAP));
}

// ============================================================================
// Cross-channel ordering checks
// ============================================================================

TEST_F(AxiProtocolDeath, BIdMatchOutstanding_RejectsUnknownId) {
  SCENARIO("protocol_rules: B_ID_MATCH_OUTSTANDING trips when B id has no outstanding AW with that id");
  std::map<uint8_t, std::deque<int>> outstanding;
  outstanding[7].push_back(1);
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(rules::check_b_id_match_outstanding<std::deque<int>>(
                            /*b_id=*/3, outstanding),
                        "B_ID_MATCH_OUTSTANDING");
  }, ".*");
}

TEST_F(AxiProtocolDeath, RIdMatchOutstanding_RejectsUnknownId) {
  SCENARIO("protocol_rules: R_ID_MATCH_OUTSTANDING trips when R id has no outstanding AR with that id");
  std::map<uint8_t, std::deque<int>> outstanding;
  outstanding[7].push_back(1);
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(rules::check_r_id_match_outstanding<std::deque<int>>(
                            /*r_id=*/3, outstanding),
                        "R_ID_MATCH_OUTSTANDING");
  }, ".*");
}

TEST_F(AxiProtocolDeath, WBeforeB_RejectsEarlyB) {
  SCENARIO("protocol_rules: W_BEFORE_B trips when B response arrives before all W beats forwarded");
  // all_w_done = false → assert trips (B fired before W complete).
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(rules::check_w_before_b(false), "W_BEFORE_B");
  }, ".*");
}

// ============================================================================
// Tautology rules — documented via positive assertions (no death tests).
// These rules are structurally guaranteed by the model: AXI4 W beats have no
// WID (W_NO_INTERLEAVE), AW and W are independent channels (AW_W_INDEPENDENCE),
// and AXI4 permits per-id R interleaving across ids (DIFF_ID_INTERLEAVE).
// Same-id W/R ordering is enforced by per-id FIFO at slave + master.
// ============================================================================

TEST_F(AxiProtocolDeath, Tautology_WNoInterleave) {
  SCENARIO("protocol_rules: W_NO_INTERLEAVE is structurally guaranteed (AXI4 W has no WID)");
  EXPECT_TRUE(rules::check_w_no_interleave());
}
TEST_F(AxiProtocolDeath, Tautology_AwWIndependence) {
  SCENARIO("protocol_rules: AW_W_INDEPENDENCE is structurally guaranteed (AW/W are independent channels)");
  EXPECT_TRUE(rules::check_aw_w_independence());
}
TEST_F(AxiProtocolDeath, Tautology_DiffIdInterleaveAllowed) {
  SCENARIO("protocol_rules: DIFF_ID_INTERLEAVE allowed by AXI4 (different-id R/B can interleave)");
  EXPECT_TRUE(rules::check_diff_id_interleave_allowed());
}
TEST_F(AxiProtocolDeath, Tautology_SameIdWOrder) {
  SCENARIO("protocol_rules: SAME_ID_W_ORDER enforced by per-id FIFO at slave + master");
  EXPECT_TRUE(rules::check_same_id_w_order());
}
TEST_F(AxiProtocolDeath, Tautology_SameIdROrder) {
  SCENARIO("protocol_rules: SAME_ID_R_ORDER enforced by per-id FIFO at slave + master");
  EXPECT_TRUE(rules::check_same_id_r_order());
}

// ============================================================================
// Integration: an end-to-end assert path via AxiSlave::tick().
// Confirms that an inline AXI_PROTOCOL_ASSERT call site in production code
// actually fires when given malformed input. Picks the most easily reachable
// rule (BURST_ENCODING via a corrupted AW.burst).
// ============================================================================

TEST_F(AxiProtocolDeath, AxiSlave_FiresOnInvalidAwBurstEncoding) {
  SCENARIO("protocol_rules: live AXI_PROTOCOL_ASSERT in AxiSlave::tick fires on illegal AW.burst=3");
  test::MockMemoryPort mem;
  axi::AxiSlave slave(mem);
  axi::AwBeat aw{};
  aw.id = 0; aw.addr = 0x1000; aw.len = 0; aw.size = 5;
  aw.burst = static_cast<axi::Burst>(3);  // invalid
  slave.push_aw(aw);
  EXPECT_DEATH({ slave.tick(); }, ".*");
}

// ============================================================================
// Phase C: AXI4 Exclusive Access (IHI 0022 §A7.2.4)
// ============================================================================

TEST_F(AxiProtocolDeath, LockEncoding_RejectsRawTwo) {
  SCENARIO("protocol_rules: LOCK_ENCODING rejects raw lock=2 (AXI3 LOCKED removed in AXI4)");
  // AXI4 AxLOCK is 1-bit; raw 2 (AXI3 LOCKED) is illegal.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(rules::check_lock_encoding(2),
                        "LOCK_ENCODING: invalid raw lock value");
  }, ".*");
}

TEST_F(AxiProtocolDeath, ExclusiveTotalBytes_Rejects256) {
  SCENARIO("protocol_rules: EXCLUSIVE_TOTAL_BYTES rejects exclusive burst > 128 B total");
  // len = 7 (8 beats) × size = 5 (32 B) = 256 B > 128 B max.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(
        rules::check_exclusive_total_bytes_le_max(axi::LockType::Exclusive, 7, 5),
        "EXCLUSIVE_TOTAL_BYTES: exceeds 128");
  }, ".*");
}

TEST_F(AxiProtocolDeath, ExclusiveTotalBeats_Rejects32) {
  SCENARIO("protocol_rules: EXCLUSIVE_TOTAL_BEATS rejects exclusive burst with > 16 beats");
  // len = 31 → 32 beats > 16-beat max.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(
        rules::check_exclusive_total_beats_le_max(axi::LockType::Exclusive, 31),
        "EXCLUSIVE_TOTAL_BEATS: exceeds 16");
  }, ".*");
}

TEST_F(AxiProtocolDeath, ExclusivePow2_RejectsLen2) {
  SCENARIO("protocol_rules: EXCLUSIVE_POW2 rejects exclusive burst whose total beats not power-of-2");
  // len = 2 → 3 beats, not a power of 2.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(
        rules::check_exclusive_total_pow2(axi::LockType::Exclusive, 2),
        "EXCLUSIVE_POW2: total beats not power of 2");
  }, ".*");
}

TEST_F(AxiProtocolDeath, ExclusiveAlign_RejectsUnaligned) {
  SCENARIO("protocol_rules: EXCLUSIVE_ALIGN rejects exclusive addr not aligned to total burst bytes");
  // size = 5 (32 B), len = 0 → total 32 B; addr 0x1004 not aligned to 32.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(
        rules::check_exclusive_addr_aligned_to_total(
            axi::LockType::Exclusive, 0x1004, 0, 5),
        "EXCLUSIVE_ALIGN: addr not aligned to total");
  }, ".*");
}

TEST_F(AxiProtocolDeath, ExclusiveBurstFixed_Rejects) {
  SCENARIO("protocol_rules: EXCLUSIVE_BURST_FIXED rejects FIXED burst for exclusive (INCR/WRAP only)");
  // FIXED is illegal for exclusive.
  EXPECT_DEATH({
    AXI_PROTOCOL_ASSERT(
        rules::check_exclusive_burst_not_fixed(
            axi::LockType::Exclusive, axi::Burst::FIXED),
        "EXCLUSIVE_BURST_FIXED: FIXED not allowed for exclusive");
  }, ".*");
}

// Positive controls: legal exclusive bursts must satisfy every rule.
TEST_F(AxiProtocolDeath, ExclusiveValid_INCR_1Beat_Size5) {
  SCENARIO("protocol_rules: valid 1-beat INCR exclusive (32B, aligned) passes all exclusive rules");
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
  SCENARIO("protocol_rules: valid 4-beat WRAP exclusive (128B boundary, aligned) passes all rules");
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

// ============================================================================
// Phase A: per-id FIFO B/R routing helpers (always compiled — no death tests).
// ============================================================================

namespace {
// Minimal stand-in matching the structural contract the new template helpers
// require: b_count_, sub_bursts (sized), cur_r_sub_idx_, r_beats_in_cur_,
// sub_bursts[i].len. Decoupled from AxiMasterT::OperationContext so the rule
// unit tests don't drag the master into this translation unit.
struct FakeSub { std::size_t len = 0; };
struct FakeOp {
  std::size_t           b_count_ = 0;
  std::vector<FakeSub>  sub_bursts;
  std::size_t           cur_r_sub_idx_ = 0;
  std::size_t           r_beats_in_cur_ = 0;
};
}  // namespace

TEST(ProtocolRulesFifo, CheckBFrontNoOutstandingOrFullyResponded_Rejects) {
  SCENARIO("protocol_rules: check_b_front_can_accept_response rejects unknown id and fully-responded front");
  std::map<uint8_t, std::deque<FakeOp>> m;
  // Case 1: id not present in map at all.
  EXPECT_FALSE(rules::check_b_front_can_accept_response(/*bid=*/0x05, m));
  // Case 2: id present but deque empty (defensive — current code erases empty
  // entries, but the helper must still reject).
  m[0x05];
  EXPECT_FALSE(rules::check_b_front_can_accept_response(0x05, m));
  // Case 3: front op already fully responded (b_count_ == sub_bursts.size()).
  FakeOp op;
  op.sub_bursts.resize(2);
  op.b_count_ = 2;
  m[0x05].push_back(op);
  EXPECT_FALSE(rules::check_b_front_can_accept_response(0x05, m));
  // Positive control: front op with one more B response outstanding.
  FakeOp op_open;
  op_open.sub_bursts.resize(2);
  op_open.b_count_ = 1;
  std::map<uint8_t, std::deque<FakeOp>> m2;
  m2[0x07].push_back(op_open);
  EXPECT_TRUE(rules::check_b_front_can_accept_response(0x07, m2));
}

TEST(ProtocolRulesFifo, CheckRFrontBadBeatTimingOrRlast_Rejects) {
  SCENARIO("protocol_rules: check_r_front_can_accept_beat rejects unknown id and mistimed RLAST");
  std::map<uint8_t, std::deque<FakeOp>> m;
  // Case 1: id not present.
  EXPECT_FALSE(rules::check_r_front_can_accept_beat(/*rid=*/0x05,
                                                    /*rlast=*/false, m));
  // Case 2: rlast=true on an intermediate beat (sub.len=3, so 4 beats; this
  // is beat 1 of 4 — RLAST belongs on beat index 3).
  FakeOp op;
  op.sub_bursts.push_back(FakeSub{3});
  op.r_beats_in_cur_ = 1;
  m[0x05].push_back(op);
  EXPECT_FALSE(rules::check_r_front_can_accept_beat(0x05, /*rlast=*/true, m));
  // Case 3: rlast=false on the FINAL beat (r_beats_in_cur_ == sub.len).
  m[0x05].clear();
  FakeOp op_last;
  op_last.sub_bursts.push_back(FakeSub{3});
  op_last.r_beats_in_cur_ = 3;
  m[0x05].push_back(op_last);
  EXPECT_FALSE(rules::check_r_front_can_accept_beat(0x05, /*rlast=*/false, m));
  // Positive control: rlast=true on the final beat is legal.
  EXPECT_TRUE(rules::check_r_front_can_accept_beat(0x05, /*rlast=*/true, m));
}
