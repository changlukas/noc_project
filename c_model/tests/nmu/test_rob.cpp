#include "nmu/rob.hpp"
#include "nmu/packetize.hpp"
#include "nmu/depacketize.hpp"
#include "common/loopback_noc.hpp"
#include "axi/types.hpp"
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
