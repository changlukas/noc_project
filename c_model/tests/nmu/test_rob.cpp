#include "nmu/rob.hpp"
#include "nmu/packetize.hpp"
#include "nmu/depacketize.hpp"
#include "common/loopback_noc.hpp"
#include "common/per_channel_capture.hpp"
#include "common/scenario.hpp"
#include "axi/types.hpp"
#include <array>
#include <bitset>
#include <set>
#include <vector>
#include <gtest/gtest.h>

using ni::cmodel::testing::LoopbackNoc;
using ni::cmodel::testing::ReqCapture;
using ni::cmodel::nmu::Packetize;
using ni::cmodel::nmu::Depacketize;
using ni::cmodel::nmu::Rob;
using ni::cmodel::nmu::RobMode;
namespace axi = ni::cmodel::axi;

namespace {
constexpr uint8_t kSrcId = 0x01;

axi::AwBeat make_aw(uint8_t id, uint64_t addr, uint8_t len = 0) {
    axi::AwBeat b{};
    b.id = id; b.addr = addr; b.len = len; b.size = 5;
    b.burst = axi::Burst::INCR;
    return b;
}
axi::ArBeat make_ar(uint8_t id, uint64_t addr) {
    axi::ArBeat b{};
    b.id = id; b.addr = addr; b.size = 5;
    b.burst = axi::Burst::INCR;
    return b;
}
axi::WBeat make_w(bool last) {
    axi::WBeat b{};
    b.last = last;
    return b;
}
// Helper to construct + feed a B flit to nmu::Depacketize for pop_b to return
ni::cmodel::Flit make_b_flit(uint8_t bid) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_B);
    f.set_header_field("dst_id", 0x01);
    f.set_header_field("last",   1);
    f.set_payload_field("B", "bid",   bid);
    f.set_payload_field("B", "bresp", 0);
    return f;
}
ni::cmodel::Flit make_r_flit(uint8_t rid, bool rlast) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_R);
    f.set_header_field("dst_id", 0x01);
    f.set_header_field("last",   1);
    f.set_payload_field("R", "rid",   rid);
    f.set_payload_field("R", "rlast", rlast ? 1u : 0u);
    return f;
}

// Test rig: Rob wraps Packetize + Depacketize over LoopbackNoc.
// PerChannelCapture is used for w/ar outputs; aw_out is LoopbackNoc.req_out()
// so the rsp side (noc.rsp_out) still works for injecting B/R flits.
struct RobRig {
    LoopbackNoc noc{16, 16};
    ReqCapture  w_cap, ar_cap;
    Packetize   pkt{noc.req_out(), w_cap, ar_cap, kSrcId};
    Depacketize depkt{noc.rsp_in(), 16, 16};
    Rob         rob{pkt, depkt, RobMode::Disabled, RobMode::Disabled};
};
}  // namespace

// === ROB-specific core behavior (4 tests) ===

TEST(NmuRob, Disabled_StallSameIdDiffDst) {
    SCENARIO("Rob Disabled: same-id AW to different dst stalls until 1st AW's B response returns");
    RobRig r;
    // 1st AW: id=5, addr=0x100 -> dst=0
    ASSERT_TRUE(r.rob.push_aw(make_aw(0x05, 0x100)));
    // 2nd AW: same id, addr=0x10100 -> dst=1 -> must stall
    EXPECT_FALSE(r.rob.push_aw(make_aw(0x05, 0x10100)));
}

TEST(NmuRob, Disabled_StallReleaseOnBComplete) {
    SCENARIO("Rob Disabled: stall on same-id-diff-dst AW released when matching B arrives via pop_b");
    RobRig r;
    ASSERT_TRUE(r.rob.push_aw(make_aw(0x05, 0x100)));
    EXPECT_FALSE(r.rob.push_aw(make_aw(0x05, 0x10100)));
    // Inject a B flit for id=5 via rsp_in
    ASSERT_TRUE(r.noc.rsp_out().push_flit(make_b_flit(0x05)));
    r.depkt.tick();  // demux into B queue
    auto b = r.rob.pop_b();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->id, 0x05);
    // After pop_b, outstanding for id=5 is empty -> next push_aw should pass
    EXPECT_TRUE(r.rob.push_aw(make_aw(0x05, 0x10100)));
}

TEST(NmuRob, Disabled_StallReleaseOnRlast) {
    SCENARIO("Rob Disabled: AR stall on same-id-diff-dst released when matching R(rlast=1) arrives");
    RobRig r;
    ASSERT_TRUE(r.rob.push_ar(make_ar(0x05, 0x100)));
    EXPECT_FALSE(r.rob.push_ar(make_ar(0x05, 0x10100)));
    // Inject R(last=true)
    ASSERT_TRUE(r.noc.rsp_out().push_flit(make_r_flit(0x05, /*rlast=*/true)));
    r.depkt.tick();
    auto rb = r.rob.pop_r();
    ASSERT_TRUE(rb.has_value());
    EXPECT_TRUE(rb->last);
    EXPECT_TRUE(r.rob.push_ar(make_ar(0x05, 0x10100)));
}

TEST(NmuRob, Disabled_WCreditBlocksWBeforeAw) {
    SCENARIO("Rob Disabled: W-burst credit starts at 0; push_w before any push_aw returns false");
    RobRig r;
    // No push_aw yet -> credit=0 -> push_w must return false
    EXPECT_FALSE(r.rob.push_w(make_w(/*last=*/true)));
}

// === ROB invariants (3 tests) ===

TEST(NmuRob, Disabled_BackpressureAtomicityPushAw) {
    SCENARIO("Rob Disabled: push_aw failing on downstream backpressure leaves ROB state unchanged");
    // Force downstream NoC full via small req_depth.
    // All 3 Packetize outputs share the same LoopbackNoc req_out.
    LoopbackNoc noc(/*req*/1, /*rsp*/16);
    Packetize pkt(noc.req_out(), noc.req_out(), noc.req_out(), kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Disabled, RobMode::Disabled);

    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));   // 1st fills req queue
    // 2nd push: should also stall on downstream backpressure, but ROB state must NOT mutate
    EXPECT_FALSE(rob.push_aw(make_aw(0x06, 0x200)));  // different id, no ROB stall; downstream full
    // Drain
    noc.req_in().pop_flit();
    // Retry now succeeds (state was atomic - second push didn't leave dangling outstanding)
    EXPECT_TRUE(rob.push_aw(make_aw(0x06, 0x200)));
}

TEST(NmuRob, Disabled_MultiOutstandingSameIdSameDst_NoFalseStall) {
    SCENARIO("Rob Disabled: two same-id same-dst AWs both admitted (no false stall when dst matches)");
    RobRig r;
    // 2 AWs same id, same dst (both addr in 0x100-0xFFFF range -> dst=0)
    ASSERT_TRUE(r.rob.push_aw(make_aw(0x05, 0x100)));
    ASSERT_TRUE(r.rob.push_aw(make_aw(0x05, 0x200)));
    // No stall - same dst
}

TEST(NmuRob, Disabled_WCreditMultiOutstandingCorrectDecrement) {
    SCENARIO("Rob Disabled: W credit increments per AW, decrements per wlast=1; 3rd wlast fails at 0");
    RobRig r;
    ASSERT_TRUE(r.rob.push_aw(make_aw(0x05, 0x100)));
    ASSERT_TRUE(r.rob.push_aw(make_aw(0x05, 0x200)));
    // credit=2; push wlast twice
    ASSERT_TRUE(r.rob.push_w(make_w(/*last=*/true)));  // credit-- to 1
    ASSERT_TRUE(r.rob.push_w(make_w(/*last=*/true)));  // credit-- to 0
    // Now credit=0 -> next push_w must fail
    EXPECT_FALSE(r.rob.push_w(make_w(/*last=*/true)));
}

// === Edge cases (2 tests) ===

TEST(NmuRob, Disabled_WBackpressureDoesNotConsumeCredit) {
    SCENARIO("Rob Disabled: push_w failing on downstream backpressure preserves W credit (no decrement)");
    // Trigger backpressure: small req_depth fills after AW + W beats.
    // All 3 Packetize outputs share the same LoopbackNoc req_out so depth
    // limits apply regardless of which channel (AW or W) is being pushed.
    LoopbackNoc noc(/*req*/2, /*rsp*/16);
    Packetize pkt(noc.req_out(), noc.req_out(), noc.req_out(), kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Disabled, RobMode::Disabled);

    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));  // AW flit (req queue 1/2)
    ASSERT_TRUE(rob.push_w(make_w(/*last=*/false)));  // W beat 1 (req queue 2/2 full)
    // credit was 1, still 1 (wlast=false didn't decrement)
    EXPECT_FALSE(rob.push_w(make_w(/*last=*/false)));  // downstream full -> false; credit unchanged
    // Drain + retry - verify credit still 1, succeeds without becoming negative
    noc.req_in().pop_flit();  // drain AW
    EXPECT_TRUE(rob.push_w(make_w(/*last=*/true)));  // now succeeds, credit-- to 0
}

TEST(NmuRob, Disabled_DifferentIdsIndependentNoInterference) {
    SCENARIO("Rob Disabled: id=5 stalled does not block id=6; per-id state is independent");
    RobRig r;
    ASSERT_TRUE(r.rob.push_aw(make_aw(0x05, 0x100)));
    EXPECT_FALSE(r.rob.push_aw(make_aw(0x05, 0x10100)));  // id=5 stalled
    // id=6 should be independent
    EXPECT_TRUE(r.rob.push_aw(make_aw(0x06, 0x100)));
}

// === Defensive (1 test) ===

TEST(NmuRobDeath, Disabled_AbortPaths) {
    SCENARIO("Rob: push_b/push_r and pop_aw/pop_w/pop_ar all abort (wrong-direction APIs)");
    RobRig r;
    EXPECT_DEATH(r.rob.push_b(axi::BBeat{}), "nmu::Rob::push_b");
    EXPECT_DEATH(r.rob.push_r(axi::RBeat{}), "nmu::Rob::push_r");
    EXPECT_DEATH(r.rob.pop_aw(), "nmu::Rob::pop_aw");
    EXPECT_DEATH(r.rob.pop_w(),  "nmu::Rob::pop_w");
    EXPECT_DEATH(r.rob.pop_ar(), "nmu::Rob::pop_ar");
}

// === ROB Enabled mode: push-side tests (Task 2) ===

TEST(NmuRob, Enabled_PushAw_AllocatesSlotAndStampsRobIdx) {
    SCENARIO("Rob Enabled: push_aw allocates ROB slot, stamps rob_req=1 + rob_idx on AW header");
    LoopbackNoc noc(/*req=*/16, /*rsp=*/16);
    ReqCapture  w_cap, ar_cap;
    Packetize   pkt(noc.req_out(), w_cap, ar_cap, kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));
    auto f = *noc.req_in().pop_flit();
    EXPECT_EQ(f.get_header_field("axi_ch"), ni::AXI_CH_AW);
    EXPECT_EQ(f.get_header_field("rob_req"), 1u);
    EXPECT_EQ(f.get_header_field("rob_idx"), 0u);  // first allocated slot
}

TEST(NmuRob, Enabled_PushAr_AllocatesConsecutiveSlotsForBurst) {
    SCENARIO("Rob Enabled: AR len=3 (4 beats) -> 4 consecutive ROB slots, base rob_idx stamped to AR header");
    LoopbackNoc noc(16, 16);
    ReqCapture  w_cap, ar_cap;
    Packetize   pkt(noc.req_out(), w_cap, ar_cap, kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    // AR len=3 → 4 beats → 4 consecutive slots
    axi::ArBeat ar = make_ar(0x05, 0x100);
    ar.len = 3;
    ASSERT_TRUE(rob.push_ar(ar));
    auto f = *ar_cap.pop();
    EXPECT_EQ(f.get_header_field("axi_ch"), ni::AXI_CH_AR);
    EXPECT_EQ(f.get_header_field("rob_req"), 1u);
    EXPECT_EQ(f.get_header_field("rob_idx"), 0u);  // base = 0
    // Next AR should allocate slot 4 (slots 0-3 occupied)
    axi::ArBeat ar2 = make_ar(0x06, 0x200);
    ar2.len = 1;
    ASSERT_TRUE(rob.push_ar(ar2));
    auto f2 = *ar_cap.pop();
    EXPECT_EQ(f2.get_header_field("rob_idx"), 4u);
}

TEST(NmuRob, Enabled_FindConsecutiveFree_FragmentedFailNoConsecutiveRun) {
    SCENARIO("Rob Enabled: find_consecutive_free returns -1 when fragmented free pool lacks a run of n");
    std::bitset<Rob::ROB_CAPACITY> free;
    // Fragmented free state: bits 1 at positions 0, 2, 4, 6 only.
    free.set(0); free.set(2); free.set(4); free.set(6);

    // 4 total free bits, but no run of 2+ consecutive.
    EXPECT_EQ(Rob::find_consecutive_free(free, 3), -1);
    EXPECT_EQ(Rob::find_consecutive_free(free, 2), -1);

    // n=1 always finds position 0 first.
    EXPECT_EQ(Rob::find_consecutive_free(free, 1), 0);

    // All free (32 free bits): n up to 32 succeeds at base=0; n=33 fails (over capacity).
    free.set();
    EXPECT_EQ(Rob::find_consecutive_free(free, 1), 0);
    EXPECT_EQ(Rob::find_consecutive_free(free, 32), 0);
    EXPECT_EQ(Rob::find_consecutive_free(free, 33), -1);
}

TEST(NmuRob, Enabled_PushAr_OversizedBurst_ReturnFalse) {
    SCENARIO("Rob Enabled: AR burst > ROB_CAPACITY rejected (return false), no state mutation");
    LoopbackNoc noc(16, 16);
    ReqCapture  w_cap, ar_cap;
    Packetize   pkt(noc.req_out(), w_cap, ar_cap, kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    // AR len=31 → 32 beats = ROB_CAPACITY; len=32 → 33 beats > ROB_CAPACITY.
    // Use len=255 (AXI4 INCR max) for an obviously-oversized burst.
    axi::ArBeat ar = make_ar(0x05, 0x100);
    ar.len = 255;   // 256 beats, exceeds ROB_CAPACITY (32)
    EXPECT_FALSE(rob.push_ar(ar));
    // No state mutation: a normal-sized AR should still succeed.
    axi::ArBeat ar_ok = make_ar(0x06, 0x200);
    ar_ok.len = 3;
    EXPECT_TRUE(rob.push_ar(ar_ok));
}

TEST(NmuRob, Enabled_PushAr_DownstreamBackpressure_AtomicRollback) {
    SCENARIO("Rob Enabled: push_ar rolled back on downstream backpressure; slots stay free for retry");
    // req queue depth = 1: pkt.push_ar_with_meta will fail after 1st push.
    // All 3 Packetize outputs share the same LoopbackNoc req_out so the
    // depth limit applies to AR pushes as well as AW.
    LoopbackNoc noc(/*req=*/1, /*rsp=*/16);
    Packetize   pkt(noc.req_out(), noc.req_out(), noc.req_out(), kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    axi::ArBeat ar = make_ar(0x05, 0x100);
    ar.len = 3;   // 4 beats
    ASSERT_TRUE(rob.push_ar(ar));   // fills req queue 1/1
    // Drain to allow next push to find consecutive free + downstream available
    noc.req_in().pop_flit();
    // Refill: push to fill queue again, then push another AR — downstream rejects
    axi::ArBeat ar2 = make_ar(0x06, 0x200);
    ar2.len = 3;
    ASSERT_TRUE(rob.push_ar(ar2));
    // Now downstream full. Next push_ar must return false WITHOUT touching free_read_entries_.
    axi::ArBeat ar3 = make_ar(0x07, 0x300);
    ar3.len = 1;   // 2 beats
    EXPECT_FALSE(rob.push_ar(ar3));
    // Drain, then ar3 retry must succeed (proving state was atomic — slots 8-9 still available)
    noc.req_in().pop_flit();
    EXPECT_TRUE(rob.push_ar(ar3));
}

TEST(NmuRob, Enabled_PushAw_PoolFull_ReturnFalseAtomic) {
    SCENARIO("Rob Enabled: 32 AWs fill write pool to ROB_CAPACITY; 33rd push_aw returns false");
    LoopbackNoc noc(64, 16);
    ReqCapture  w_cap, ar_cap;
    Packetize   pkt(noc.req_out(), w_cap, ar_cap, kSrcId);  // aw uses noc; w/ar use captures
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    // 32 single-beat AWs fill the write pool entirely
    for (int i = 0; i < 32; ++i) {
        ASSERT_TRUE(rob.push_aw(make_aw(static_cast<uint8_t>(i & 0xFF), 0x100)));
    }
    // 33rd AW must fail
    EXPECT_FALSE(rob.push_aw(make_aw(0x33, 0x200)));
}

TEST(NmuRob, Enabled_PushAw_DownstreamBackpressure_AtomicRollback) {
    SCENARIO("Rob Enabled: push_aw rolled back on downstream backpressure; slot stays free for retry");
    LoopbackNoc noc(/*req=*/1, /*rsp=*/16);
    ReqCapture  w_cap, ar_cap;
    Packetize   pkt(noc.req_out(), w_cap, ar_cap, kSrcId);  // aw uses noc for backpressure
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));   // fills req queue 1/1
    EXPECT_FALSE(rob.push_aw(make_aw(0x06, 0x200))); // downstream full, state unchanged
    noc.req_in().pop_flit();                          // drain
    EXPECT_TRUE(rob.push_aw(make_aw(0x06, 0x200))); // retry succeeds with slot still available
}

// === ROB Enabled mode: pop-side tests (Task 3) ===

TEST(NmuRob, Enabled_PopB_InOrder_ImmediateCommit) {
    SCENARIO("Rob Enabled: B for rob_idx=0 (per-id head) commits immediately on pop_b");
    LoopbackNoc noc(16, 16);
    ReqCapture  w_cap, ar_cap;
    Packetize   pkt(noc.req_out(), w_cap, ar_cap, kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));   // allocates slot 0
    // Inject B with rob_idx=0, matching the head of id=5's sequence
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch",  ni::AXI_CH_B);
    f.set_header_field("dst_id",  kSrcId);
    f.set_header_field("last",    1);
    f.set_header_field("rob_req", 1);
    f.set_header_field("rob_idx", 0);
    f.set_payload_field("B", "bid",   0x05);
    f.set_payload_field("B", "bresp", 0);
    ASSERT_TRUE(noc.rsp_out().push_flit(f));
    depkt.tick();
    auto b = rob.pop_b();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->id, 0x05u);
}

TEST(NmuRob, Enabled_PopB_OutOfOrder_HeldUntilHeadReady) {
    SCENARIO("Rob Enabled: out-of-order B (slot 1 before 0) held; chain-flushes when head (0) arrives");
    LoopbackNoc noc(16, 16);
    ReqCapture  w_cap, ar_cap;
    Packetize   pkt(noc.req_out(), w_cap, ar_cap, kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    // id=5: two AWs in flight, slots 0 + 1
    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));
    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x10100)));
    auto push_b = [&](uint8_t rob_idx, uint8_t bresp) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch",  ni::AXI_CH_B);
        f.set_header_field("dst_id",  kSrcId);
        f.set_header_field("last",    1);
        f.set_header_field("rob_req", 1);
        f.set_header_field("rob_idx", rob_idx);
        f.set_payload_field("B", "bid",   0x05);
        f.set_payload_field("B", "bresp", bresp);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
    };
    push_b(/*rob_idx=*/1, /*bresp=*/0);   // B for AW2 arrives first
    depkt.tick();
    EXPECT_FALSE(rob.pop_b().has_value());  // not head, held
    push_b(/*rob_idx=*/0, /*bresp=*/0);   // B for AW1 arrives second
    depkt.tick();
    auto b1 = rob.pop_b();
    ASSERT_TRUE(b1.has_value());           // chain-flush: AW1's B
    auto b2 = rob.pop_b();
    ASSERT_TRUE(b2.has_value());           // then AW2's B
    EXPECT_FALSE(rob.pop_b().has_value()); // empty
}

TEST(NmuRob, Enabled_PopR_MultiBeatBurstCommitInOrder) {
    SCENARIO("Rob Enabled: AR1 4-beat then AR2 2-beat R flits arrive reversed; commit in submission order");
    LoopbackNoc noc(16, 16);
    ReqCapture  w_cap, ar_cap;
    Packetize   pkt(noc.req_out(), w_cap, ar_cap, kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    // id=5: AR1 len=3 -> slots 0..3; AR2 len=1 -> slots 4..5
    axi::ArBeat ar1 = make_ar(0x05, 0x100); ar1.len = 3;
    axi::ArBeat ar2 = make_ar(0x05, 0x200); ar2.len = 1;
    ASSERT_TRUE(rob.push_ar(ar1));
    ASSERT_TRUE(rob.push_ar(ar2));
    auto push_r = [&](uint8_t rob_idx, bool rlast, uint8_t marker) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch",  ni::AXI_CH_R);
        f.set_header_field("dst_id",  kSrcId);
        f.set_header_field("last",    1);
        f.set_header_field("rob_req", 1);
        f.set_header_field("rob_idx", rob_idx);
        f.set_payload_field("R", "rid",   0x05);
        f.set_payload_field("R", "rresp", 0);
        f.set_payload_field("R", "rlast", rlast ? 1u : 0u);
        std::array<uint8_t, 32> d{};
        d[0] = marker;
        f.set_payload_bytes("R", "rdata", d.data(), 256);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
    };
    // Arrive in order: slot 4, 5 (AR2), then 0, 1, 2, 3 (AR1)
    push_r(4, false, 0xB0); push_r(5, true,  0xB1);
    push_r(0, false, 0xA0); push_r(1, false, 0xA1);
    push_r(2, false, 0xA2); push_r(3, true,  0xA3);
    depkt.tick();
    // pop_r pulls one downstream flit per call; commits happen when a range
    // is fully ready. Drain by polling, collecting all returned beats in order.
    // Bound iterations to avoid infinite loop on bug.
    std::vector<uint8_t> got;
    for (int i = 0; i < 32 && got.size() < 6; ++i) {
        auto r = rob.pop_r();
        if (r.has_value()) got.push_back(r->data[0]);
    }
    // AR1 must commit first (it was issued first); markers 0xA0..0xA3,
    // then AR2: markers 0xB0..0xB1
    ASSERT_EQ(got.size(), 6u);
    EXPECT_EQ(got[0], 0xA0u);
    EXPECT_EQ(got[1], 0xA1u);
    EXPECT_EQ(got[2], 0xA2u);
    EXPECT_EQ(got[3], 0xA3u);
    EXPECT_EQ(got[4], 0xB0u);
    EXPECT_EQ(got[5], 0xB1u);
    EXPECT_FALSE(rob.pop_r().has_value());
}

TEST(NmuRob, Enabled_DifferentIdsInterleaveAtTransactionBoundary) {
    SCENARIO("Rob Enabled: different-id Rs may commit interleaved (per-id order preserved within each id)");
    LoopbackNoc noc(16, 16);
    ReqCapture  w_cap, ar_cap;
    Packetize   pkt(noc.req_out(), w_cap, ar_cap, kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    // id=5 AR slot 0; id=6 AR slot 1
    ASSERT_TRUE(rob.push_ar(make_ar(0x05, 0x100)));   // len=0 -> 1 beat
    ASSERT_TRUE(rob.push_ar(make_ar(0x06, 0x100)));   // slot 1
    auto push_r = [&](uint8_t rob_idx, uint8_t rid) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch",  ni::AXI_CH_R);
        f.set_header_field("dst_id",  kSrcId);
        f.set_header_field("last",    1);
        f.set_header_field("rob_req", 1);
        f.set_header_field("rob_idx", rob_idx);
        f.set_payload_field("R", "rid",   rid);
        f.set_payload_field("R", "rresp", 0);
        f.set_payload_field("R", "rlast", 1u);
        std::array<uint8_t, 32> d{}; d[0] = rid;
        f.set_payload_bytes("R", "rdata", d.data(), 256);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
    };
    push_r(1, 0x06);   // id=6 R arrives first
    push_r(0, 0x05);   // id=5 R arrives second
    depkt.tick();
    // Both committable (each is head of its own per-id sequence).
    auto r1 = rob.pop_r();
    ASSERT_TRUE(r1.has_value());
    auto r2 = rob.pop_r();
    ASSERT_TRUE(r2.has_value());
    // Both 0x05 and 0x06 must appear (order between ids is implementation-defined
    // but each id's beats must be in submission order -- here each id has 1 beat).
    std::set<uint8_t> ids{r1->id, r2->id};
    EXPECT_EQ(ids.size(), 2u);
    EXPECT_TRUE(ids.count(0x05) && ids.count(0x06));
}

TEST(NmuRobDeath, Enabled_PopBWithUnallocatedRobIdx_Abort) {
    SCENARIO("Rob Enabled: pop_b on B flit with unallocated rob_idx aborts (defensive assert)");
    LoopbackNoc noc(16, 16);
    ReqCapture  w_cap, ar_cap;
    Packetize   pkt(noc.req_out(), w_cap, ar_cap, kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    // Inject B with rob_idx=7, but no AW allocated that slot -> assert fires
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch",  ni::AXI_CH_B);
    f.set_header_field("dst_id",  kSrcId);
    f.set_header_field("last",    1);
    f.set_header_field("rob_req", 1);
    f.set_header_field("rob_idx", 7);
    f.set_payload_field("B", "bid",   0x05);
    f.set_payload_field("B", "bresp", 0);
    ASSERT_TRUE(noc.rsp_out().push_flit(f));
    depkt.tick();
    EXPECT_DEATH(rob.pop_b(), "");
}

TEST(NmuRobDeath, Enabled_PopBWithDisabledFlit_Abort) {
    SCENARIO("Rob Enabled: pop_b on Disabled-mode flit (rob_req=0) aborts (mode mismatch)");
    LoopbackNoc noc(16, 16);
    ReqCapture  w_cap, ar_cap;
    Packetize   pkt(noc.req_out(), w_cap, ar_cap, kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));   // allocates slot 0
    // Inject B with rob_req=0 (Disabled-mode flit) into Enabled Rob -> assert
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch",  ni::AXI_CH_B);
    f.set_header_field("dst_id",  kSrcId);
    f.set_header_field("last",    1);
    f.set_header_field("rob_req", 0);
    f.set_header_field("rob_idx", 0);
    f.set_payload_field("B", "bid",   0x05);
    f.set_payload_field("B", "bresp", 0);
    ASSERT_TRUE(noc.rsp_out().push_flit(f));
    depkt.tick();
    EXPECT_DEATH(rob.pop_b(), "");
}
