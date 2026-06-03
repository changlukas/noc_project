#include "nmu/rob.hpp"
#include "nmu/packetize.hpp"
#include "nmu/depacketize.hpp"
#include "common/loopback_noc.hpp"
#include "axi/types.hpp"
#include <bitset>
#include <gtest/gtest.h>

using ni::cmodel::testing::LoopbackNoc;
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
struct RobRig {
    LoopbackNoc noc{16, 16};
    Packetize   pkt{noc.req_out(), kSrcId};
    Depacketize depkt{noc.rsp_in(), 16, 16};
    Rob         rob{pkt, depkt, RobMode::Disabled, RobMode::Disabled};
};
}  // namespace

// === ROB-specific core behavior (4 tests) ===

TEST(NmuRob, Disabled_StallSameIdDiffDst) {
    RobRig r;
    // 1st AW: id=5, addr=0x100 -> dst=0
    ASSERT_TRUE(r.rob.push_aw(make_aw(0x05, 0x100)));
    // 2nd AW: same id, addr=0x10100 -> dst=1 -> must stall
    EXPECT_FALSE(r.rob.push_aw(make_aw(0x05, 0x10100)));
}

TEST(NmuRob, Disabled_StallReleaseOnBComplete) {
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
    RobRig r;
    // No push_aw yet -> credit=0 -> push_w must return false
    EXPECT_FALSE(r.rob.push_w(make_w(/*last=*/true)));
}

// === ROB invariants (3 tests) ===

TEST(NmuRob, Disabled_BackpressureAtomicityPushAw) {
    // Force downstream NoC full via small req_depth
    LoopbackNoc noc(/*req*/1, /*rsp*/16);
    Packetize pkt(noc.req_out(), kSrcId);
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
    RobRig r;
    // 2 AWs same id, same dst (both addr in 0x100-0xFFFF range -> dst=0)
    ASSERT_TRUE(r.rob.push_aw(make_aw(0x05, 0x100)));
    ASSERT_TRUE(r.rob.push_aw(make_aw(0x05, 0x200)));
    // No stall - same dst
}

TEST(NmuRob, Disabled_WCreditMultiOutstandingCorrectDecrement) {
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
    // Trigger backpressure: small req_depth fills after AW + W beats
    LoopbackNoc noc(/*req*/2, /*rsp*/16);
    Packetize pkt(noc.req_out(), kSrcId);
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
    RobRig r;
    ASSERT_TRUE(r.rob.push_aw(make_aw(0x05, 0x100)));
    EXPECT_FALSE(r.rob.push_aw(make_aw(0x05, 0x10100)));  // id=5 stalled
    // id=6 should be independent
    EXPECT_TRUE(r.rob.push_aw(make_aw(0x06, 0x100)));
}

// === Defensive (1 test) ===

TEST(NmuRobDeath, Disabled_AbortPaths) {
    RobRig r;
    EXPECT_DEATH(r.rob.push_b(axi::BBeat{}), "Rob: push_b");
    EXPECT_DEATH(r.rob.push_r(axi::RBeat{}), "Rob: push_r");
    EXPECT_DEATH(r.rob.pop_aw(), "Rob: pop_aw");
    EXPECT_DEATH(r.rob.pop_w(),  "Rob: pop_w");
    EXPECT_DEATH(r.rob.pop_ar(), "Rob: pop_ar");
}

// === ROB Enabled mode: push-side tests (Task 2) ===

TEST(NmuRob, Enabled_PushAw_AllocatesSlotAndStampsRobIdx) {
    LoopbackNoc noc(/*req=*/16, /*rsp=*/16);
    Packetize   pkt(noc.req_out(), kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));
    auto f = *noc.req_in().pop_flit();
    EXPECT_EQ(f.get_header_field("axi_ch"), ni::AXI_CH_AW);
    EXPECT_EQ(f.get_header_field("rob_req"), 1u);
    EXPECT_EQ(f.get_header_field("rob_idx"), 0u);  // first allocated slot
}

TEST(NmuRob, Enabled_PushAr_AllocatesConsecutiveSlotsForBurst) {
    LoopbackNoc noc(16, 16);
    Packetize   pkt(noc.req_out(), kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    // AR len=3 → 4 beats → 4 consecutive slots
    axi::ArBeat ar = make_ar(0x05, 0x100);
    ar.len = 3;
    ASSERT_TRUE(rob.push_ar(ar));
    auto f = *noc.req_in().pop_flit();
    EXPECT_EQ(f.get_header_field("axi_ch"), ni::AXI_CH_AR);
    EXPECT_EQ(f.get_header_field("rob_req"), 1u);
    EXPECT_EQ(f.get_header_field("rob_idx"), 0u);  // base = 0
    // Next AR should allocate slot 4 (slots 0-3 occupied)
    axi::ArBeat ar2 = make_ar(0x06, 0x200);
    ar2.len = 1;
    ASSERT_TRUE(rob.push_ar(ar2));
    auto f2 = *noc.req_in().pop_flit();
    EXPECT_EQ(f2.get_header_field("rob_idx"), 4u);
}

TEST(NmuRob, Enabled_FindConsecutiveFree_FragmentedFailNoConsecutiveRun) {
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
    LoopbackNoc noc(16, 16);
    Packetize   pkt(noc.req_out(), kSrcId);
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
    // req queue depth = 1: pkt.push_ar_with_meta will fail after 1st push
    LoopbackNoc noc(/*req=*/1, /*rsp=*/16);
    Packetize   pkt(noc.req_out(), kSrcId);
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
    LoopbackNoc noc(64, 16);
    Packetize   pkt(noc.req_out(), kSrcId);
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
    LoopbackNoc noc(/*req=*/1, /*rsp=*/16);
    Packetize   pkt(noc.req_out(), kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));   // fills req queue 1/1
    EXPECT_FALSE(rob.push_aw(make_aw(0x06, 0x200))); // downstream full, state unchanged
    noc.req_in().pop_flit();                          // drain
    EXPECT_TRUE(rob.push_aw(make_aw(0x06, 0x200))); // retry succeeds with slot still available
}
