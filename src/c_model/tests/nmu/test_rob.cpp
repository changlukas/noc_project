#include "nmu/rob.hpp"
#include "nmu/packetize.hpp"
#include "nmu/depacketize.hpp"
#include "common/channel_model.hpp"
#include "common/per_channel_capture.hpp"
#include "common/scenario.hpp"
#include "axi/types.hpp"
#include <array>
#include <set>
#include <utility>
#include <vector>
#include <gtest/gtest.h>

using ni::cmodel::nmu::Depacketize;
using ni::cmodel::nmu::Packetize;
using ni::cmodel::nmu::Rob;
using ni::cmodel::nmu::RobMode;
using ni::cmodel::testing::ChannelModel;
using ni::cmodel::testing::ReqCapture;
namespace axi = ni::cmodel::axi;
namespace addr_trans = ni::cmodel::nmu::addr_trans;

namespace {
constexpr uint8_t kSrcId = 0x01;

// 16x16 uniform, 4 GB/tile, no rebase: dst = addr/4GB, local_addr = addr
// unchanged -- reproduces the retired addr_trans::xy_route mapping exactly,
// so every legacy dst-from-addr[39:32] expectation below holds unchanged.
addr_trans::SamTable legacy_sam() {
    return addr_trans::SamTable::uniform(16, 16, 0x100000000ull);
}

axi::AwBeat make_aw(uint8_t id, uint64_t addr, uint8_t len = 0) {
    axi::AwBeat b{};
    b.id = id;
    b.addr = addr;
    b.len = len;
    b.size = 5;
    b.burst = axi::Burst::INCR;
    return b;
}
axi::ArBeat make_ar(uint8_t id, uint64_t addr) {
    axi::ArBeat b{};
    b.id = id;
    b.addr = addr;
    b.size = 5;
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
    f.set_header_field("flit_tail", 1);
    f.set_payload_field("B", "bid", bid);
    f.set_payload_field("B", "bresp", 0);
    return f;
}
ni::cmodel::Flit make_r_flit(uint8_t rid, bool rlast) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_R);
    f.set_header_field("dst_id", 0x01);
    f.set_header_field("flit_tail", 1);
    f.set_payload_field("R", "rid", rid);
    f.set_payload_field("R", "rlast", rlast ? 1u : 0u);
    return f;
}

// The idle-ID bypass exempts the first transaction of an idle id. A test that wants
// the transaction under test to take the RoB path must first put one transaction in
// flight for that id. The primer allocates no slot, so every ordering_tag expectation
// in the migrated tests below is unchanged.
//
// Primer dest is tile 15 (kPrimerAddr), deliberately far from the small
// (tile-0) addresses the tests below push -- so the same-destination bypass
// never fires between the primer and the transaction under test, matching the
// idle-ID-bypass-only ordering_tag/free_space expectations those tests were written against.
constexpr uint64_t kPrimerAddr = 0x100000000ull * 15 + 0x8000;

void prime_write_id(Rob& rob, ChannelModel& noc, uint8_t id) {
    ASSERT_TRUE(rob.push_aw(make_aw(id, kPrimerAddr)));
    noc.req_in().pop_flit();  // discard the primer's AW flit
}

void prime_read_id(Rob& rob, ReqCapture& ar_cap, uint8_t id) {
    ASSERT_TRUE(rob.push_ar(make_ar(id, kPrimerAddr)));
    ar_cap.pop();  // discard the primer's AR flit
}

// Overload for the backpressure tests, which wire all three Packetize outputs to
// noc.req_out() and construct no ar_cap.
void prime_read_id(Rob& rob, ChannelModel& noc, uint8_t id) {
    ASSERT_TRUE(rob.push_ar(make_ar(id, kPrimerAddr)));
    noc.req_in().pop_flit();  // discard the primer's AR flit
}

// Retire a primer by feeding its bypassed response and draining it. Call AFTER
// pushing the transaction under test, so that transaction becomes the list head.
void retire_write_primer(Rob& rob, ChannelModel& noc, Depacketize& depkt, uint8_t id) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_B);
    f.set_header_field("dst_id", kSrcId);
    f.set_header_field("flit_tail", 1);
    f.set_header_field("ordering_req", 0);
    f.set_header_field("ordering_tag", 0);
    f.set_payload_field("B", "bid", id);
    f.set_payload_field("B", "bresp", 0);
    ASSERT_TRUE(noc.rsp_out().push_flit(f));
    depkt.tick();
    ASSERT_TRUE(rob.pop_b().has_value());
}

void retire_read_primer(Rob& rob, ChannelModel& noc, Depacketize& depkt, uint8_t id) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_R);
    f.set_header_field("dst_id", kSrcId);
    f.set_header_field("flit_tail", 1);
    f.set_header_field("ordering_req", 0);
    f.set_header_field("ordering_tag", 0);
    f.set_payload_field("R", "rid", id);
    f.set_payload_field("R", "rresp", 0);
    f.set_payload_field("R", "rlast", 1u);
    std::array<uint8_t, 32> d{};
    f.set_payload_bytes("R", "rdata", d.data(), 256);
    ASSERT_TRUE(noc.rsp_out().push_flit(f));
    depkt.tick();
    ASSERT_TRUE(rob.pop_r().has_value());
}

// Testbench: Rob wraps Packetize + Depacketize over ChannelModel.
// PerChannelCapture is used for w/ar outputs; aw_out is ChannelModel.req_out()
// so the rsp side (noc.rsp_out) still works for injecting B/R flits.
struct RobTestbench {
    ChannelModel noc{16, 16};
    ReqCapture w_cap, ar_cap;
    // Rob always calls sam_.translate itself (its own SamTable member) and
    // drives push_*_with_meta, never Packetize::push_aw/push_ar directly, so
    // Packetize's sam is never touched here.
    Packetize pkt{noc.req_out(), w_cap, ar_cap, kSrcId, {}};
    Depacketize depkt{noc.rsp_in(), 16, 16};
    Rob rob{pkt, depkt, RobMode::Disabled, legacy_sam()};
};
}  // namespace

// === ROB-specific core behavior (2 tests) ===
// The B single-outstanding-stall tests were removed: the B RoB is now
// unconditional, so a second same-id same-dest AW rides the same-destination bypass
// rather than stalling. B RoB behavior is covered by the RobSameDestBypass / Enabled
// tests below.

TEST(NmuRob, Disabled_StallReleaseOnRlast) {
    SCENARIO("Rob Disabled: AR stall on second same-id released when matching R(rlast=1) arrives");
    RobTestbench r;
    ASSERT_TRUE(r.rob.push_ar(make_ar(0x05, 0x100)));
    EXPECT_FALSE(r.rob.push_ar(make_ar(0x05, 0x200)));
    // Inject R(last=true)
    ASSERT_TRUE(r.noc.rsp_out().push_flit(make_r_flit(0x05, /*rlast=*/true)));
    r.depkt.tick();
    auto rb = r.rob.pop_r();
    ASSERT_TRUE(rb.has_value());
    EXPECT_TRUE(rb->last);
    EXPECT_TRUE(r.rob.push_ar(make_ar(0x05, 0x200)));
}

TEST(NmuRob, Disabled_WCreditBlocksWBeforeAw) {
    SCENARIO("Rob Disabled: W-burst credit starts at 0; push_w before any push_aw returns false");
    RobTestbench r;
    // No push_aw yet -> credit=0 -> push_w must return false
    EXPECT_FALSE(r.rob.push_w(make_w(/*last=*/true)));
}

// === ROB invariants (3 tests) ===

TEST(NmuRob, Disabled_BackpressureAtomicityPushAw) {
    SCENARIO("Rob Disabled: push_aw failing on downstream backpressure leaves ROB state unchanged");
    // Force downstream NoC full via small req_depth.
    // All 3 Packetize outputs share the same ChannelModel req_out.
    ChannelModel noc(/*req*/ 1, /*rsp*/ 16);
    Packetize pkt(noc.req_out(), noc.req_out(), noc.req_out(), kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Disabled, legacy_sam());

    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));  // 1st fills req queue
    // 2nd push: should also stall on downstream backpressure, but ROB state must NOT mutate
    EXPECT_FALSE(rob.push_aw(make_aw(0x06, 0x200)));  // different id, no ROB stall; downstream full
    // Drain
    noc.req_in().pop_flit();
    // Retry now succeeds (state was atomic - second push didn't leave dangling outstanding)
    EXPECT_TRUE(rob.push_aw(make_aw(0x06, 0x200)));
}

TEST(NmuRob, Disabled_WCreditMultiOutstandingCorrectDecrement) {
    SCENARIO(
        "Rob Disabled: W credit increments per AW, decrements per wlast=1; 3rd wlast fails at 0");
    RobTestbench r;
    // Two AWs for different ids: each adds 1 to the global W credit.
    ASSERT_TRUE(r.rob.push_aw(make_aw(0x05, 0x100)));
    ASSERT_TRUE(r.rob.push_aw(make_aw(0x06, 0x200)));
    // credit=2; push wlast twice
    ASSERT_TRUE(r.rob.push_w(make_w(/*last=*/true)));  // credit-- to 1
    ASSERT_TRUE(r.rob.push_w(make_w(/*last=*/true)));  // credit-- to 0
    // Now credit=0 -> next push_w must fail
    EXPECT_FALSE(r.rob.push_w(make_w(/*last=*/true)));
}

// === Edge cases (2 tests) ===

TEST(NmuRob, Disabled_WBackpressureDoesNotConsumeCredit) {
    SCENARIO(
        "Rob Disabled: push_w failing on downstream backpressure preserves W credit (no "
        "decrement)");
    // Trigger backpressure: small req_depth fills after AW + W beats.
    // All 3 Packetize outputs share the same ChannelModel req_out so depth
    // limits apply regardless of which channel (AW or W) is being pushed.
    ChannelModel noc(/*req*/ 2, /*rsp*/ 16);
    Packetize pkt(noc.req_out(), noc.req_out(), noc.req_out(), kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Disabled, legacy_sam());

    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));   // AW flit (req queue 1/2)
    ASSERT_TRUE(rob.push_w(make_w(/*last=*/false)));  // W beat 1 (req queue 2/2 full)
    // credit was 1, still 1 (wlast=false didn't decrement)
    EXPECT_FALSE(rob.push_w(make_w(/*last=*/false)));  // downstream full -> false; credit unchanged
    // Drain + retry - verify credit still 1, succeeds without becoming negative
    noc.req_in().pop_flit();                         // drain AW
    EXPECT_TRUE(rob.push_w(make_w(/*last=*/true)));  // now succeeds, credit-- to 0
}

// NmuRobDeath/Disabled_AbortPaths was a runtime wrong_side_() test.
// Rob inherits only RequestPacketizer + ResponseDepacketizer;
// the wrong-direction methods don't exist — compile-time protection. Removed.

// === ROB Enabled mode: push-side tests ===

TEST(NmuRob, Enabled_PushAw_AllocatesSlotAndStampsOrderingTag) {
    SCENARIO(
        "Rob Enabled: push_aw allocates ROB slot, stamps ordering_req=1 + ordering_tag on AW "
        "header");
    ChannelModel noc(/*req=*/16, /*rsp=*/16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    prime_write_id(rob, noc, 0x05);
    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));
    auto f = *noc.req_in().pop_flit();
    EXPECT_EQ(f.get_header_field("axi_ch"), ni::AXI_CH_AW);
    EXPECT_EQ(f.get_header_field("ordering_req"), 1u);
    EXPECT_EQ(f.get_header_field("ordering_tag"), 0u);  // first allocated slot
}

TEST(NmuRob, Enabled_PushAr_AllocatesConsecutiveSlotsForBurst) {
    SCENARIO(
        "Rob Enabled: AR len=3 (4 beats) -> 4 consecutive ROB slots, base ordering_tag stamped to "
        "AR "
        "header");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    prime_read_id(rob, ar_cap, 0x05);
    prime_read_id(rob, ar_cap, 0x06);
    // AR len=3 → 4 beats → 4 consecutive slots
    axi::ArBeat ar = make_ar(0x05, 0x100);
    ar.len = 3;
    ASSERT_TRUE(rob.push_ar(ar));
    auto f = *ar_cap.pop();
    EXPECT_EQ(f.get_header_field("axi_ch"), ni::AXI_CH_AR);
    EXPECT_EQ(f.get_header_field("ordering_req"), 1u);
    EXPECT_EQ(f.get_header_field("ordering_tag"), 0u);  // base = 0
    // Next AR should allocate slot 4 (slots 0-3 occupied)
    axi::ArBeat ar2 = make_ar(0x06, 0x200);
    ar2.len = 1;
    ASSERT_TRUE(rob.push_ar(ar2));
    auto f2 = *ar_cap.pop();
    EXPECT_EQ(f2.get_header_field("ordering_tag"), 4u);
}

TEST(NmuRob, Enabled_ConstructorMarksOnlyDepthSlotsFree) {
    SCENARIO("Rob Enabled: only [0, depth) is free at construction; slots above it never are");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam(), 4, 8);
    EXPECT_EQ(rob.b_rob_depth(), 4u);
    EXPECT_EQ(rob.r_rob_depth(), 8u);
    EXPECT_EQ(rob.write_free_space(), 4u);
    EXPECT_EQ(rob.read_free_space(), 8u);
}

TEST(NmuRob, Enabled_DefaultDepthIsThirtyTwo) {
    SCENARIO("Rob Enabled: the default pool is 32 entries, not the 256-entry index space");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());
    EXPECT_EQ(rob.b_rob_depth(), 32u);
    EXPECT_EQ(rob.r_rob_depth(), 32u);
    EXPECT_EQ(Rob::ORDERING_TAG_SPACE, 256u);
}

TEST(NmuRob, Enabled_MaxBurst_AllocatesEveryEntry) {
    SCENARIO("Rob Enabled: a 256-beat AR into a 256-deep read pool allocates 256 slots");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam(), Rob::ORDERING_TAG_SPACE,
            Rob::ORDERING_TAG_SPACE);

    prime_read_id(rob, ar_cap, 0x05);
    axi::ArBeat ar = make_ar(0x05, 0x100);
    ar.len = 255;  // 256 beats: len_plus_1 does not fit in uint8_t
    ASSERT_TRUE(rob.push_ar(ar));
    EXPECT_EQ(rob.read_free_space(), 0u) << "all 256 slots consumed";
    auto f = *ar_cap.pop();
    EXPECT_EQ(f.get_header_field("ordering_tag"), 0u);
    EXPECT_EQ(f.get_header_field("ordering_req"), 1u);
}

TEST(NmuRob, Enabled_MaxBurst_AllBeatsLandInOrder) {
    SCENARIO("Rob Enabled: all 256 beats of a max-length burst land and release in order");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam(), Rob::ORDERING_TAG_SPACE,
            Rob::ORDERING_TAG_SPACE);

    prime_read_id(rob, ar_cap, 0x05);
    axi::ArBeat ar = make_ar(0x05, 0x100);
    ar.len = 255;  // 256 beats; len_plus_1 = 256 does not fit in uint8_t
    ASSERT_TRUE(rob.push_ar(ar));
    ar_cap.pop();
    retire_read_primer(rob, noc, depkt, 0x05);  // the AR under test is now the list head

    auto push_r = [&](bool rlast, uint8_t marker) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_R);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("flit_tail", 1);
        f.set_header_field("ordering_req", 1);
        f.set_header_field("ordering_tag", 0);  // the NSU stamps the burst base on every beat
        f.set_payload_field("R", "rid", 0x05);
        f.set_payload_field("R", "rresp", 0);
        f.set_payload_field("R", "rlast", rlast ? 1u : 0u);
        std::array<uint8_t, 32> d{};
        d[0] = marker;
        f.set_payload_bytes("R", "rdata", d.data(), 256);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
        depkt.tick();
    };

    // One beat per iteration, draining as we go: the rsp queue is only 16 deep.
    std::vector<uint8_t> got;
    for (int i = 0; i < 256; ++i) {
        push_r(/*rlast=*/i == 255, static_cast<uint8_t>(i));
        if (auto r = rob.pop_r()) got.push_back(r->data[0]);
    }
    // Per-beat release: each beat commits as it lands. This drains any tail.
    for (int i = 0; i < 512 && got.size() < 256; ++i) {
        if (auto r = rob.pop_r()) got.push_back(r->data[0]);
    }
    ASSERT_EQ(got.size(), 256u) << "len_plus_1 truncated, or the drain loop never terminated";
    for (int i = 0; i < 256; ++i) {
        EXPECT_EQ(got[i], static_cast<uint8_t>(i)) << "beat " << i << " out of order";
    }
}

TEST(NmuRob, Enabled_LzcAllocator_IsAStack) {
    SCENARIO(
        "Rob Enabled: freeing a low range does not return its space while a higher range lives");
    ChannelModel noc(64, 64);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 64, 64);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam(), 8, 8);

    // Prime every id so its burst takes the RoB path; c (0x07) must attempt an
    // allocation to hit the pool-full refusal instead of bypassing.
    prime_read_id(rob, ar_cap, 0x05);
    prime_read_id(rob, ar_cap, 0x06);
    prime_read_id(rob, ar_cap, 0x07);

    axi::ArBeat a = make_ar(0x05, 0x100);
    a.len = 3;  // A occupies [0..3]; only index 3 is marked
    ASSERT_TRUE(rob.push_ar(a));
    axi::ArBeat b = make_ar(0x06, 0x200);
    b.len = 1;  // B occupies [4..5]; only index 5 is marked
    ASSERT_TRUE(rob.push_ar(b));
    retire_read_primer(rob, noc, depkt, 0x05);  // A is now the head of id 0x05
    EXPECT_EQ(rob.read_free_space(), 2u);

    auto push_r = [&](uint8_t base, bool rlast, uint8_t rid) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_R);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("flit_tail", 1);
        f.set_header_field("ordering_req", 1);
        f.set_header_field("ordering_tag", base);
        f.set_payload_field("R", "rid", rid);
        f.set_payload_field("R", "rresp", 0);
        f.set_payload_field("R", "rlast", rlast ? 1u : 0u);
        std::array<uint8_t, 32> d{};
        f.set_payload_bytes("R", "rdata", d.data(), 256);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
        depkt.tick();
    };

    // Drain A completely. pop_r() pulls ONE depacketized flit per call and only
    // commits once every beat of the burst is ready -- so four beats take more
    // than four calls. Poll.
    push_r(0, false, 0x05);
    push_r(0, false, 0x05);
    push_r(0, false, 0x05);
    push_r(0, true, 0x05);
    std::size_t got = 0;
    for (int i = 0; i < 32 && got < 4; ++i) {
        if (rob.pop_r().has_value()) ++got;
    }
    ASSERT_EQ(got, 4u) << "all four beats of A must be released";

    EXPECT_EQ(rob.read_free_space(), 2u) << "high-water is still B's top at index 5";
    axi::ArBeat c = make_ar(0x07, 0x300);
    c.len = 3;  // four beats: four slots are notionally free but unreachable
    EXPECT_FALSE(rob.push_ar(c));
}

TEST(NmuRob, Enabled_LzcAllocator_NonTopReleaseDoesNotGrowFreeSpace) {
    SCENARIO("Rob Enabled: clearing a non-highest range top leaves free_space unchanged");
    ChannelModel noc(64, 64);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 64, 64);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam(), 8, 8);

    prime_write_id(rob, noc, 0x05);
    prime_write_id(rob, noc, 0x06);
    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));  // slot 0
    ASSERT_TRUE(rob.push_aw(make_aw(0x06, 0x200)));  // slot 1
    retire_write_primer(rob, noc, depkt, 0x05);      // the robbed AWs are now the heads
    retire_write_primer(rob, noc, depkt, 0x06);
    EXPECT_EQ(rob.write_free_space(), 6u);

    auto push_b = [&](uint8_t ordering_tag, uint8_t bid) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_B);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("flit_tail", 1);
        f.set_header_field("ordering_req", 1);
        f.set_header_field("ordering_tag", ordering_tag);
        f.set_payload_field("B", "bid", bid);
        f.set_payload_field("B", "bresp", 0);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
        depkt.tick();
    };

    push_b(0, 0x05);
    ASSERT_TRUE(rob.pop_b().has_value());
    EXPECT_EQ(rob.write_free_space(), 6u) << "slot 0 is below the high-water mark";
    push_b(1, 0x06);
    ASSERT_TRUE(rob.pop_b().has_value());
    EXPECT_EQ(rob.write_free_space(), 8u) << "the mark cleared; everything is free again";
}

// Depth is a parameter now. The stack allocator must respect it at every legal value.
class RobDepthParam : public ::testing::TestWithParam<std::size_t> {};

TEST_P(RobDepthParam, Enabled_AllocationNeverExceedsDepth) {
    SCENARIO("Rob Enabled: free_space starts at depth and no base ever reaches it");
    const std::size_t depth = GetParam();
    ChannelModel noc(512, 512);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 512, 512);
    // The idle-ID bypass exempts the first AW of an id, so drive one primed id. Raise the
    // per-id cap above depth so the pool, not the cap, is the binding constraint.
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam(), depth, depth, depth + 2);

    EXPECT_EQ(rob.write_free_space(), depth);
    EXPECT_EQ(rob.read_free_space(), depth);

    prime_write_id(rob, noc, 0x40);
    for (std::size_t i = 0; i < depth; ++i) {
        ASSERT_TRUE(rob.push_aw(make_aw(0x40, 0x100))) << "AW " << i;
        auto f = *noc.req_in().pop_flit();
        EXPECT_LT(f.get_header_field("ordering_tag"), depth) << "base " << i << " escaped the pool";
    }
    EXPECT_EQ(rob.write_free_space(), 0u);
    EXPECT_FALSE(rob.push_aw(make_aw(0x40, 0x200)));
}

INSTANTIATE_TEST_SUITE_P(Depths, RobDepthParam,
                         ::testing::Values(1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u, 256u));

TEST(NmuRob, Enabled_LzcAllocator_ReusesFromTheTop) {
    SCENARIO("Rob Enabled: once the bitmap is empty the next base is 0");
    ChannelModel noc(64, 64);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 64, 64);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam(), 8, 8);

    prime_read_id(rob, ar_cap, 0x05);
    prime_read_id(rob, ar_cap, 0x06);
    axi::ArBeat a = make_ar(0x05, 0x100);
    a.len = 3;
    ASSERT_TRUE(rob.push_ar(a));
    ar_cap.pop();
    retire_read_primer(rob, noc, depkt, 0x05);  // A is now the head of id 0x05

    auto push_r = [&](bool rlast) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_R);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("flit_tail", 1);
        f.set_header_field("ordering_req", 1);
        f.set_header_field("ordering_tag", 0);
        f.set_payload_field("R", "rid", 0x05);
        f.set_payload_field("R", "rresp", 0);
        f.set_payload_field("R", "rlast", rlast ? 1u : 0u);
        std::array<uint8_t, 32> d{};
        f.set_payload_bytes("R", "rdata", d.data(), 256);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
        depkt.tick();
    };
    push_r(false);
    push_r(false);
    push_r(false);
    push_r(true);
    std::size_t got = 0;  // see the polling note in Enabled_LzcAllocator_IsAStack
    for (int i = 0; i < 32 && got < 4; ++i) {
        if (rob.pop_r().has_value()) ++got;
    }
    ASSERT_EQ(got, 4u);
    EXPECT_EQ(rob.read_free_space(), 8u);

    axi::ArBeat b = make_ar(0x06, 0x200);
    b.len = 1;
    ASSERT_TRUE(rob.push_ar(b));
    auto f = *ar_cap.pop();
    EXPECT_EQ(f.get_header_field("ordering_tag"), 0u);
}

TEST(NmuRob, Enabled_PushAr_DownstreamBackpressure_AtomicRollback) {
    SCENARIO(
        "Rob Enabled: push_ar rolled back on downstream backpressure; slots stay free for retry");
    // req queue depth = 1: pkt.push_ar_with_meta will fail after 1st push.
    // All 3 Packetize outputs share the same ChannelModel req_out so the
    // depth limit applies to AR pushes as well as AW.
    ChannelModel noc(/*req=*/1, /*rsp=*/16);
    Packetize pkt(noc.req_out(), noc.req_out(), noc.req_out(), kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    // ar3 (id 0x07) is the request that gets rolled back, so prime 0x07 to force
    // it onto the RoB path -- otherwise it bypasses and there is no allocation to
    // roll back (the ChannelModel overload: all three outputs share noc.req_out).
    prime_read_id(rob, noc, 0x07);

    axi::ArBeat ar = make_ar(0x05, 0x100);
    ar.len = 3;                    // 4 beats
    ASSERT_TRUE(rob.push_ar(ar));  // fills req queue 1/1
    // Drain to allow next push to find consecutive free + downstream available
    noc.req_in().pop_flit();
    // Refill: push to fill queue again, then push another AR — downstream rejects
    axi::ArBeat ar2 = make_ar(0x06, 0x200);
    ar2.len = 3;
    ASSERT_TRUE(rob.push_ar(ar2));
    // Now downstream full. Next push_ar must return false WITHOUT touching the read pool.
    axi::ArBeat ar3 = make_ar(0x07, 0x300);
    ar3.len = 1;  // 2 beats
    EXPECT_FALSE(rob.push_ar(ar3));
    // Drain, then ar3 retry must succeed (proving state was atomic — slots 8-9 still available)
    noc.req_in().pop_flit();
    EXPECT_TRUE(rob.push_ar(ar3));
}

TEST(NmuRob, Enabled_PushAw_PoolFull_ReturnFalseAtomic) {
    SCENARIO("Rob Enabled: b_rob_depth AWs fill the write pool; the next push_aw returns false");
    ChannelModel noc(64, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});  // aw uses noc; w/ar use captures
    Depacketize depkt(noc.rsp_in(), 16, 16);
    // One id, primed so the idle-ID bypass does not exempt the transactions under test.
    // Distinct ids would each bypass and never fill the pool. The per-id cap must
    // exceed the pool depth for the pool to be the binding constraint (spec D4).
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam(), 32, 32, 64);

    prime_write_id(rob, noc, 0x40);
    for (std::size_t i = 0; i < rob.b_rob_depth(); ++i) {
        ASSERT_TRUE(rob.push_aw(make_aw(0x40, 0x100))) << "AW " << i;
    }
    EXPECT_EQ(rob.write_free_space(), 0u);
    EXPECT_FALSE(rob.push_aw(make_aw(0x40, 0x200)));
}

TEST(NmuRob, Enabled_MaxTxnsPerIdGate_RefusesWithFreeSlotsAvailable) {
    SCENARIO("Rob Enabled: the (max_txns_per_id+1)-th same-id AW is refused while slots remain");
    ChannelModel noc(256, 256);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 256, 256);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam(), 32, 32, 3);

    // The idle-ID bypass exempts the 1st (idle id). The 2nd changes dest, so the
    // same-destination bypass does not apply -- it allocates a RoB slot and sets
    // the sticky fallback flag. The 3rd reverts to the 1st's dest but stays
    // allocated (sticky), so the cap admits a bypass plus two allocated. The cap
    // counts list entries, not slots.
    const std::size_t free_before = rob.write_free_space();
    ASSERT_TRUE(rob.push_aw(make_aw(0x09, 0x100)));                   // dst 0: bypass
    ASSERT_TRUE(rob.push_aw(make_aw(0x09, 0x100000000ull + 0x140)));  // dst 1: allocates, sticky
    ASSERT_TRUE(rob.push_aw(make_aw(0x09, 0x180)));                   // dst 0: allocates (sticky)
    EXPECT_FALSE(rob.push_aw(make_aw(0x09, 0x400))) << "per-id cap bites before the pool does";
    EXPECT_EQ(rob.write_free_space(), free_before - 2) << "one bypass plus two allocated slots";
    EXPECT_TRUE(rob.push_aw(make_aw(0x0A, 0x500))) << "the gate is per-id, not global";
}

TEST(NmuRob, Enabled_MaxTxnsPerIdGate_AppliesToReadsIndependently) {
    SCENARIO("Rob Enabled: the per-id cap gates AR independently of AW");
    ChannelModel noc(256, 256);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 256, 256);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam(), 32, 32, 2);

    ASSERT_TRUE(rob.push_ar(make_ar(0x0B, 0x100)));
    ASSERT_TRUE(rob.push_ar(make_ar(0x0B, 0x200)));
    EXPECT_FALSE(rob.push_ar(make_ar(0x0B, 0x300)));
    EXPECT_GT(rob.read_free_space(), 0u) << "the cap refused, not the pool";
    EXPECT_TRUE(rob.push_aw(make_aw(0x0B, 0x400)))
        << "the write list of the same id is independent";
}

TEST(NmuRob, Enabled_MaxTxnsPerIdDefaultIsThirtyTwo) {
    SCENARIO("Rob Enabled: the default per-id cap is FlooNoC's MaxRoTxnsPerId = 32");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());
    EXPECT_EQ(rob.max_txns_per_id(), 32u);
}

TEST(NmuRob, Enabled_PushAw_DownstreamBackpressure_AtomicRollback) {
    SCENARIO(
        "Rob Enabled: push_aw rolled back on downstream backpressure; slot stays free for retry");
    ChannelModel noc(/*req=*/1, /*rsp=*/16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});  // aw uses noc for backpressure
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    // The 0x06 AW is the one rolled back, so prime 0x06 to force it onto the RoB
    // path -- otherwise it bypasses and there is no slot allocation to roll back.
    prime_write_id(rob, noc, 0x06);

    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));   // fills req queue 1/1
    EXPECT_FALSE(rob.push_aw(make_aw(0x06, 0x200)));  // downstream full, state unchanged
    noc.req_in().pop_flit();                          // drain
    EXPECT_TRUE(rob.push_aw(make_aw(0x06, 0x200)));   // retry succeeds with slot still available
}

// === ROB Enabled mode: pop-side tests ===

TEST(NmuRob, Enabled_PopB_InOrder_ImmediateCommit) {
    SCENARIO("Rob Enabled: B for ordering_tag=0 (per-id head) commits immediately on pop_b");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    prime_write_id(rob, noc, 0x05);
    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));  // allocates slot 0
    retire_write_primer(rob, noc, depkt, 0x05);      // the robbed AW is now the head
    // Inject B with ordering_tag=0, matching the head of id=5's sequence
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_B);
    f.set_header_field("dst_id", kSrcId);
    f.set_header_field("flit_tail", 1);
    f.set_header_field("ordering_req", 1);
    f.set_header_field("ordering_tag", 0);
    f.set_payload_field("B", "bid", 0x05);
    f.set_payload_field("B", "bresp", 0);
    ASSERT_TRUE(noc.rsp_out().push_flit(f));
    depkt.tick();
    auto b = rob.pop_b();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->id, 0x05u);
}

TEST(NmuRob, Enabled_PopB_OutOfOrder_HeldUntilHeadReady) {
    SCENARIO(
        "Rob Enabled: out-of-order B (slot 1 before 0) held; chain-flushes when head (0) arrives");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    // id=5: two AWs in flight, slots 0 + 1
    prime_write_id(rob, noc, 0x05);
    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));
    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100000100)));
    retire_write_primer(rob, noc, depkt, 0x05);  // the robbed AWs (slots 0,1) are now the head
    auto push_b = [&](uint8_t ordering_tag, uint8_t bresp) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_B);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("flit_tail", 1);
        f.set_header_field("ordering_req", 1);
        f.set_header_field("ordering_tag", ordering_tag);
        f.set_payload_field("B", "bid", 0x05);
        f.set_payload_field("B", "bresp", bresp);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
    };
    push_b(/*ordering_tag=*/1, /*bresp=*/0);  // B for AW2 arrives first
    depkt.tick();
    EXPECT_FALSE(rob.pop_b().has_value());    // not head, held
    push_b(/*ordering_tag=*/0, /*bresp=*/0);  // B for AW1 arrives second
    depkt.tick();
    auto b1 = rob.pop_b();
    ASSERT_TRUE(b1.has_value());  // chain-flush: AW1's B
    auto b2 = rob.pop_b();
    ASSERT_TRUE(b2.has_value());            // then AW2's B
    EXPECT_FALSE(rob.pop_b().has_value());  // empty
}

TEST(NmuRob, Enabled_PopR_MultiBeatBurstCommitInOrder) {
    SCENARIO(
        "Rob Enabled: AR1 4-beat then AR2 2-beat R flits arrive reversed; commit in submission "
        "order");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    // id=5: AR1 len=3 -> slots 0..3; AR2 len=1 -> slots 4..5
    prime_read_id(rob, ar_cap, 0x05);
    axi::ArBeat ar1 = make_ar(0x05, 0x100);
    ar1.len = 3;
    axi::ArBeat ar2 = make_ar(0x05, 0x200);
    ar2.len = 1;
    ASSERT_TRUE(rob.push_ar(ar1));
    ASSERT_TRUE(rob.push_ar(ar2));
    retire_read_primer(rob, noc, depkt, 0x05);  // AR1 is now the head of id 0x05
    auto push_r = [&](uint8_t ordering_tag, bool rlast, uint8_t marker) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_R);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("flit_tail", 1);
        f.set_header_field("ordering_req", 1);
        f.set_header_field("ordering_tag", ordering_tag);
        f.set_payload_field("R", "rid", 0x05);
        f.set_payload_field("R", "rresp", 0);
        f.set_payload_field("R", "rlast", rlast ? 1u : 0u);
        std::array<uint8_t, 32> d{};
        d[0] = marker;
        f.set_payload_bytes("R", "rdata", d.data(), 256);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
    };
    // Arrive in order: AR2 beats (base=4) then AR1 beats (base=0).
    // Real NSU stamps every beat with the same base ordering_tag.
    push_r(4, false, 0xB0);  // AR2 beat 0 (base=4)
    push_r(4, true, 0xB1);   // AR2 beat 1 (base=4)
    push_r(0, false, 0xA0);  // AR1 beat 0 (base=0)
    push_r(0, false, 0xA1);  // AR1 beat 1 (base=0)
    push_r(0, false, 0xA2);  // AR1 beat 2 (base=0)
    push_r(0, true, 0xA3);   // AR1 beat 3 (base=0)
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

TEST(NmuRob, Enabled_PerBeatRelease_HeadBurstStreams) {
    SCENARIO("Rob Enabled: beat 0 of the head burst leaves before beats 1-3 have arrived");
    ChannelModel noc(64, 64);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 64, 64);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam(), 8, 8);

    prime_read_id(rob, ar_cap, 0x05);
    axi::ArBeat ar = make_ar(0x05, 0x100);
    ar.len = 3;  // 4 beats, base 0
    ASSERT_TRUE(rob.push_ar(ar));
    retire_read_primer(rob, noc, depkt, 0x05);  // the burst under test is now the head

    auto push_r = [&](bool rlast, uint8_t marker) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_R);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("flit_tail", 1);
        f.set_header_field("ordering_req", 1);
        f.set_header_field("ordering_tag", 0);  // the NSU stamps the burst base on every beat
        f.set_payload_field("R", "rid", 0x05);
        f.set_payload_field("R", "rresp", 0);
        f.set_payload_field("R", "rlast", rlast ? 1u : 0u);
        std::array<uint8_t, 32> d{};
        d[0] = marker;
        f.set_payload_bytes("R", "rdata", d.data(), 256);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
        depkt.tick();
    };

    // The range is [0..3]; only index 3 is marked, so free_space is 8 - 1 - 3 = 4.
    EXPECT_EQ(rob.read_free_space(), 4u);

    // After this task, one injected beat yields one pop_r(). Before it, this line
    // is exactly what fails: the burst gate holds beat 0 until beat 3 lands.
    push_r(false, 0xA0);  // only beat 0 has arrived
    ASSERT_TRUE(rob.pop_r().has_value()) << "beat 0 must not wait for beats 1-3";
    EXPECT_FALSE(rob.pop_r().has_value()) << "beat 1 has not arrived";
    EXPECT_EQ(rob.read_free_space(), 4u) << "beat 0 is not the range top; no marker cleared";

    push_r(false, 0xA1);
    ASSERT_TRUE(rob.pop_r().has_value());
    push_r(false, 0xA2);
    ASSERT_TRUE(rob.pop_r().has_value());
    EXPECT_EQ(rob.read_free_space(), 4u) << "still no marker cleared";

    push_r(true, 0xA3);
    ASSERT_TRUE(rob.pop_r().has_value());
    EXPECT_EQ(rob.read_free_space(), 8u) << "the range top cleared, so the pool is empty";
}

TEST(NmuRob, Enabled_DifferentIdsInterleaveAtTransactionBoundary) {
    SCENARIO(
        "Rob Enabled: different-id Rs may commit interleaved (per-id order preserved within each "
        "id)");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    // id=5 AR slot 0; id=6 AR slot 1
    prime_read_id(rob, ar_cap, 0x05);
    prime_read_id(rob, ar_cap, 0x06);
    ASSERT_TRUE(rob.push_ar(make_ar(0x05, 0x100)));  // len=0 -> 1 beat
    ASSERT_TRUE(rob.push_ar(make_ar(0x06, 0x100)));  // slot 1
    retire_read_primer(rob, noc, depkt, 0x05);       // each burst under test is now its list head
    retire_read_primer(rob, noc, depkt, 0x06);
    auto push_r = [&](uint8_t ordering_tag, uint8_t rid) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_R);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("flit_tail", 1);
        f.set_header_field("ordering_req", 1);
        f.set_header_field("ordering_tag", ordering_tag);
        f.set_payload_field("R", "rid", rid);
        f.set_payload_field("R", "rresp", 0);
        f.set_payload_field("R", "rlast", 1u);
        std::array<uint8_t, 32> d{};
        d[0] = rid;
        f.set_payload_bytes("R", "rdata", d.data(), 256);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
    };
    push_r(1, 0x06);  // id=6 R arrives first
    push_r(0, 0x05);  // id=5 R arrives second
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

TEST(NmuRobDeath, Enabled_PopBWithUnallocatedOrderingTag_Abort) {
    SCENARIO(
        "Rob Enabled: pop_b on B flit with unallocated ordering_tag aborts (defensive assert)");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    // Inject B with ordering_tag=7, but no AW allocated that slot -> assert fires
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_B);
    f.set_header_field("dst_id", kSrcId);
    f.set_header_field("flit_tail", 1);
    f.set_header_field("ordering_req", 1);
    f.set_header_field("ordering_tag", 7);
    f.set_payload_field("B", "bid", 0x05);
    f.set_payload_field("B", "bresp", 0);
    ASSERT_TRUE(noc.rsp_out().push_flit(f));
    depkt.tick();
    EXPECT_DEATH(rob.pop_b(), ".*");
}

TEST(NmuRobDeath, Enabled_DepthZeroAborts) {
    SCENARIO("Rob: b_rob_depth = 0 is rejected at construction");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    EXPECT_DEATH({ Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam(), 0, 32); }, ".*");
}

TEST(NmuRobDeath, Enabled_DepthAboveIdxSpaceAborts) {
    SCENARIO("Rob: r_rob_depth > ORDERING_TAG_SPACE is rejected at construction");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    EXPECT_DEATH(
        { Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam(), 32, Rob::ORDERING_TAG_SPACE + 1); },
        ".*");
}

TEST(NmuRobDeath, Enabled_MaxTxnsPerIdZeroAborts) {
    SCENARIO("Rob: max_txns_per_id = 0 is rejected at construction");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    EXPECT_DEATH({ Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam(), 32, 32, 0); }, ".*");
}

TEST(NmuRob, ReadFillSameBaseOrderingTagLandsInOrder) {
    SCENARIO(
        "Enabled read ROB: two R beats stamped with the same base ordering_tag (real NSU "
        "stamping) land at base+0 and base+1 via the per-base arrival counter, in order.");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    prime_read_id(rob, ar_cap, 0x05);
    axi::ArBeat ar = make_ar(0x05, 0x100);
    ar.len = 1;  // 2 beats -> slots 0..1, base=0
    ASSERT_TRUE(rob.push_ar(ar));
    retire_read_primer(rob, noc, depkt, 0x05);  // the burst under test is now the head

    auto push_r = [&](uint8_t ordering_tag, bool rlast, uint8_t marker) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_R);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("flit_tail", 1);
        f.set_header_field("ordering_req", 1);
        f.set_header_field("ordering_tag", ordering_tag);
        f.set_payload_field("R", "rid", 0x05);
        f.set_payload_field("R", "rresp", 0);
        f.set_payload_field("R", "rlast", rlast ? 1u : 0u);
        std::array<uint8_t, 32> d{};
        d[0] = marker;
        f.set_payload_bytes("R", "rdata", d.data(), 256);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
    };
    push_r(/*ordering_tag=*/0, /*rlast=*/false, 0xA0);
    push_r(/*ordering_tag=*/0, /*rlast=*/true, 0xA1);
    depkt.tick();
    // Multi-beat reads legitimately return nullopt until the last beat arrives
    // and the range commits; pop_r is one-flit-per-call. Drain by polling.
    std::vector<uint8_t> got;
    for (int i = 0; i < 16 && got.size() < 2; ++i) {
        auto r = rob.pop_r();
        if (r.has_value()) got.push_back(r->data[0]);
    }
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0], 0xA0);  // base+0
    EXPECT_EQ(got[1], 0xA1);  // base+1
}

// === Defensive boundary tests (lock the computed-slot guard) ===

TEST(NmuRobDeath, ReadExtraBeatPastBurstLengthAborts) {
    SCENARIO(
        "Enabled read ROB: a 3rd R beat for a 2-beat burst (offset == len) aborts "
        "rather than writing into an adjacent burst's slot.");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    prime_read_id(rob, ar_cap, 0x05);  // the AR under test allocates, so read_range_len_ is set
    axi::ArBeat ar = make_ar(0x05, 0x100);
    ar.len = 1;  // 2 beats
    ASSERT_TRUE(rob.push_ar(ar));

    auto push_r = [&](bool rlast) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_R);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("flit_tail", 1);
        f.set_header_field("ordering_req", 1);
        f.set_header_field("ordering_tag", 0);
        f.set_payload_field("R", "rid", 0x05);
        f.set_payload_field("R", "rresp", 0);
        f.set_payload_field("R", "rlast", rlast ? 1u : 0u);
        std::array<uint8_t, 32> d{};
        f.set_payload_bytes("R", "rdata", d.data(), 256);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
    };
    push_r(false);  // slot 0
    push_r(false);  // slot 1 (no rlast yet)
    push_r(false);  // extra 3rd beat for a 2-beat burst -> must abort when reached
    depkt.tick();
    EXPECT_DEATH(
        {
            for (int i = 0; i < 8; ++i) (void)rob.pop_r();
        },
        ".*");
}

TEST(NmuRob, ReadSameBaseReuseStartsAtZero) {
    SCENARIO(
        "Enabled read ROB: after a burst fully commits and frees base 0, a new "
        "burst that reuses base 0 starts its arrival counter at 0.");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    prime_read_id(rob, ar_cap, 0x05);
    prime_read_id(rob, ar_cap, 0x06);

    auto push_r = [&](uint8_t id, bool rlast, uint8_t marker) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_R);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("flit_tail", 1);
        f.set_header_field("ordering_req", 1);
        f.set_header_field("ordering_tag", 0);
        f.set_payload_field("R", "rid", id);
        f.set_payload_field("R", "rresp", 0);
        f.set_payload_field("R", "rlast", rlast ? 1u : 0u);
        std::array<uint8_t, 32> d{};
        d[0] = marker;
        f.set_payload_bytes("R", "rdata", d.data(), 256);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
    };

    // Burst 1: id=5, len=1 -> base 0, 2 beats. Fill, drain, free.
    axi::ArBeat ar1 = make_ar(0x05, 0x100);
    ar1.len = 1;
    ASSERT_TRUE(rob.push_ar(ar1));
    retire_read_primer(rob, noc, depkt, 0x05);  // burst 1 is now the head of id 0x05
    push_r(0x05, false, 0xB0);
    push_r(0x05, true, 0xB1);
    depkt.tick();
    std::vector<uint8_t> got1;
    for (int i = 0; i < 16 && got1.size() < 2; ++i) {
        auto r = rob.pop_r();
        if (r.has_value()) got1.push_back(r->data[0]);
    }
    ASSERT_EQ(got1.size(), 2u);

    // Burst 2 reuses base 0 (slots freed). Counter must start at 0 again.
    axi::ArBeat ar2 = make_ar(0x06, 0x200);
    ar2.len = 1;
    ASSERT_TRUE(rob.push_ar(ar2));
    retire_read_primer(rob, noc, depkt, 0x06);  // burst 2 is now the head of id 0x06
    push_r(0x06, false, 0xC0);
    push_r(0x06, true, 0xC1);
    depkt.tick();
    std::vector<uint8_t> got2;
    for (int i = 0; i < 16 && got2.size() < 2; ++i) {
        auto r = rob.pop_r();
        if (r.has_value()) got2.push_back(r->data[0]);
    }
    ASSERT_EQ(got2.size(), 2u);
    EXPECT_EQ(got2[0], 0xC0);  // proves base-0 counter restarted at offset 0
    EXPECT_EQ(got2[1], 0xC1);
}

TEST(NmuRob, ReadSameIdDifferentDstInterleavedFilesPerBase) {
    SCENARIO(
        "Enabled read ROB: two same-id bursts to different dst get distinct bases; "
        "interleaved R beats fill per base, not per id. Egress holds AR order.");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    // Burst A: id=5, len=1 -> base 0 (slots 0..1). Burst B: id=5, len=1 -> base 2 (slots 2..3).
    prime_read_id(rob, ar_cap, 0x05);
    axi::ArBeat ar_a = make_ar(0x05, 0x100);
    ar_a.len = 1;
    axi::ArBeat ar_b = make_ar(0x05, 0x200);
    ar_b.len = 1;
    ASSERT_TRUE(rob.push_ar(ar_a));
    ASSERT_TRUE(rob.push_ar(ar_b));
    retire_read_primer(rob, noc, depkt, 0x05);  // burst A is now the head of id 0x05

    auto push_r = [&](uint8_t base, bool rlast, uint8_t marker) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_R);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("flit_tail", 1);
        f.set_header_field("ordering_req", 1);
        f.set_header_field("ordering_tag", base);
        f.set_payload_field("R", "rid", 0x05);
        f.set_payload_field("R", "rresp", 0);
        f.set_payload_field("R", "rlast", rlast ? 1u : 0u);
        std::array<uint8_t, 32> d{};
        d[0] = marker;
        f.set_payload_bytes("R", "rdata", d.data(), 256);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
    };

    // Interleave: A0, B0, A1(last), B1(last). Each carries its own base.
    push_r(/*base=*/0, false, 0xA0);
    push_r(/*base=*/2, false, 0xB0);
    push_r(/*base=*/0, true, 0xA1);
    push_r(/*base=*/2, true, 0xB1);
    depkt.tick();

    // Egress order = AR order: burst A (0xA0,0xA1) fully, then burst B (0xB0,0xB1).
    std::vector<uint8_t> got;
    for (int i = 0; i < 8 && got.size() < 4; ++i) {
        auto r = rob.pop_r();
        if (r.has_value()) got.push_back(r->data[0]);
    }
    ASSERT_EQ(got.size(), 4u);
    EXPECT_EQ(got[0], 0xA0);
    EXPECT_EQ(got[1], 0xA1);
    EXPECT_EQ(got[2], 0xB0);
    EXPECT_EQ(got[3], 0xB1);
}

// === Idle-ID bypass ===

TEST(NmuRobDeath, Enabled_PopBBypassFlitOnRobbedHead_Abort) {
    SCENARIO(
        "Rob Enabled: an ordering_req=0 B whose id's list head owns a slot is malformed, aborts");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    prime_write_id(rob, noc, 0x05);                  // bypassed head
    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));  // robbed, slot 0
    retire_write_primer(rob, noc, depkt, 0x05);      // head is now the robbed entry

    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_B);
    f.set_header_field("dst_id", kSrcId);
    f.set_header_field("flit_tail", 1);
    f.set_header_field("ordering_req", 0);
    f.set_header_field("ordering_tag", 0);
    f.set_payload_field("B", "bid", 0x05);
    f.set_payload_field("B", "bresp", 0);
    ASSERT_TRUE(noc.rsp_out().push_flit(f));
    depkt.tick();
    EXPECT_DEATH(rob.pop_b(), ".*");
}

TEST(NmuRob, Enabled_PushAr_OversizedBurst_AdmittedViaBypass) {
    SCENARIO(
        "Rob Enabled: a 256-beat AR on an idle id is admitted with ordering_req=0, no slots taken");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());  // depth 32

    const std::size_t free_before = rob.read_free_space();
    axi::ArBeat ar = make_ar(0x05, 0x100);
    ar.len = 255;  // 256 beats, eight times the 32-slot read pool
    EXPECT_TRUE(rob.push_ar(ar));
    auto f = *ar_cap.pop();
    EXPECT_EQ(f.get_header_field("axi_ch"), ni::AXI_CH_AR);
    EXPECT_EQ(f.get_header_field("ordering_req"), 0u) << "bypassed AR carries ordering_req=0";
    EXPECT_EQ(rob.read_free_space(), free_before) << "bypass allocates nothing";
}

TEST(NmuRob, Enabled_PushAr_OversizedBurst_SecondSameIdRefusedNotWedged) {
    SCENARIO("Rob Enabled: a second oversized AR on the same id is refused while the first flies");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    axi::ArBeat first = make_ar(0x05, 0x100);  // dst 0
    first.len = 255;
    ASSERT_TRUE(rob.push_ar(first));
    // Different dest than `first`: the same-destination bypass does not apply, so this
    // allocates a slot (needs 256 slots) and is refused by the pool, not bypassed.
    axi::ArBeat second = make_ar(0x05, 0x100000000ull + 0x200);  // dst 1
    second.len = 255;
    EXPECT_FALSE(rob.push_ar(second)) << "list non-empty -> ordering_req=1 -> 256 slots -> refuse";

    axi::ArBeat other = make_ar(0x06, 0x300);
    other.len = 255;
    EXPECT_TRUE(rob.push_ar(other)) << "the refusal is per-id, not a channel wedge";
}

TEST(NmuRob, Enabled_IdleIdBypass_FirstTxnPerIdAllocatesNoSlot) {
    SCENARIO(
        "Rob Enabled: the first AW of an id takes no slot, stamps ordering_req=0; the second does");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    const std::size_t free_before = rob.write_free_space();
    ASSERT_TRUE(rob.push_aw(make_aw(0x11, 0x100)));  // dst 0
    auto f0 = *noc.req_in().pop_flit();
    EXPECT_EQ(f0.get_header_field("ordering_req"), 0u);
    EXPECT_EQ(rob.write_free_space(), free_before);

    // Different dest than the first: the same-destination bypass does not apply, so this
    // allocates a slot.
    ASSERT_TRUE(rob.push_aw(make_aw(0x11, 0x100000000ull + 0x200)));  // dst 1
    auto f1 = *noc.req_in().pop_flit();
    EXPECT_EQ(f1.get_header_field("ordering_req"), 1u);
    EXPECT_EQ(f1.get_header_field("ordering_tag"), 0u) << "first allocated slot";
    EXPECT_EQ(rob.write_free_space(), free_before - 1);
}

TEST(NmuRob, Enabled_BypassedBeat_ReleasesNoSlot) {
    SCENARIO("Rob Enabled: a bypassed B forwards without touching the allocation bitmap");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    const std::size_t free_before = rob.write_free_space();
    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));  // bypassed: id 5 was idle
    noc.req_in().pop_flit();

    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_B);
    f.set_header_field("dst_id", kSrcId);
    f.set_header_field("flit_tail", 1);
    f.set_header_field("ordering_req", 0);
    f.set_header_field("ordering_tag", 0);
    f.set_payload_field("B", "bid", 0x05);
    f.set_payload_field("B", "bresp", 0);
    ASSERT_TRUE(noc.rsp_out().push_flit(f));
    depkt.tick();

    auto b = rob.pop_b();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->id, 0x05u);
    EXPECT_EQ(rob.write_free_space(), free_before) << "no slot taken, none may be released";

    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x200)));  // the id is idle again
    auto f2 = *noc.req_in().pop_flit();
    EXPECT_EQ(f2.get_header_field("ordering_req"), 0u);
}

TEST(NmuRob, Enabled_MixedList_OrderPreserved) {
    SCENARIO("Rob Enabled: id 5 holds [bypassed, robbed]; the robbed B arrives first and waits");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));  // dst 0, bypassed, ordering_req=0
    // Different dest than the first: the same-destination bypass does not apply, so this
    // allocates a slot, slot 0.
    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100000000ull + 0x200)));  // dst 1

    auto push_b = [&](unsigned ordering_req, unsigned ordering_tag, unsigned bresp) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_B);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("flit_tail", 1);
        f.set_header_field("ordering_req", ordering_req);
        f.set_header_field("ordering_tag", ordering_tag);
        f.set_payload_field("B", "bid", 0x05);
        f.set_payload_field("B", "bresp", bresp);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
        depkt.tick();
    };

    // Both responses carry bid=5, so bresp is the only way to tell them apart.
    // axi::Resp is an enum class: EXOKAY = 1, SLVERR = 2.
    push_b(/*ordering_req=*/1, /*ordering_tag=*/0,
           /*bresp=*/2);  // the LATER transaction returns first
    EXPECT_FALSE(rob.pop_b().has_value()) << "must wait behind the bypassed head";

    push_b(/*ordering_req=*/0, /*ordering_tag=*/0, /*bresp=*/1);  // the bypassed head returns
    auto first = rob.pop_b();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->resp, axi::Resp::EXOKAY) << "bypassed head released first";
    auto second = rob.pop_b();
    ASSERT_TRUE(second.has_value()) << "a missing re-drain strands this response forever";
    EXPECT_EQ(second->resp, axi::Resp::SLVERR) << "the robbed entry released after it";
    EXPECT_EQ(rob.write_free_space(), rob.b_rob_depth()) << "slot 0 returned to the pool";
}

TEST(NmuRob, Enabled_MaxTxnsPerId1_MatchesDisabled) {
    SCENARIO("Rob Enabled with max_txns_per_id=1 emits the same AR flits as Rob Disabled");
    ChannelModel noc_e(64, 64);
    ReqCapture w_e, ar_e;
    Packetize pkt_e(noc_e.req_out(), w_e, ar_e, kSrcId, {});
    Depacketize depkt_e(noc_e.rsp_in(), 64, 64);
    Rob rob_e(pkt_e, depkt_e, RobMode::Enabled, legacy_sam(), 32, 32, 1);

    ChannelModel noc_d(64, 64);
    ReqCapture w_d, ar_d;
    Packetize pkt_d(noc_d.req_out(), w_d, ar_d, kSrcId, {});
    Depacketize depkt_d(noc_d.rsp_in(), 64, 64);
    Rob rob_d(pkt_d, depkt_d, RobMode::Disabled, legacy_sam());

    axi::ArBeat a0 = make_ar(0x21, 0x100);
    axi::ArBeat a1 = make_ar(0x21, 0x200);  // same id: both must refuse
    axi::ArBeat a2 = make_ar(0x22, 0x300);  // different id: both must accept

    EXPECT_EQ(rob_e.push_ar(a0), rob_d.push_ar(a0));
    EXPECT_EQ(rob_e.push_ar(a1), rob_d.push_ar(a1));
    EXPECT_EQ(rob_e.push_ar(a2), rob_d.push_ar(a2));

    ASSERT_EQ(ar_e.size(), ar_d.size());
    while (ar_e.size() > 0) {
        auto fe = *ar_e.pop();
        auto fd = *ar_d.pop();
        EXPECT_EQ(fe.get_header_field("ordering_req"), fd.get_header_field("ordering_req"));
        EXPECT_EQ(fe.get_header_field("ordering_tag"), fd.get_header_field("ordering_tag"));
        EXPECT_EQ(fe.get_header_field("dst_id"), fd.get_header_field("dst_id"));
    }
    EXPECT_EQ(rob_e.read_free_space(), rob_e.r_rob_depth()) << "Enabled took no slot either";
}

// === Same-destination bypass (same-dest, sticky prev_dest) ===

TEST(RobSameDestBypass, SameDestStreakBypassesAll) {
    SCENARIO(
        "Rob Enabled: a same-id same-dest AR streak bypasses in full (idle-ID bypass the 1st, "
        "same-destination bypass the rest); read_slot_hwm stays 0");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam(),
            /*b_rob_depth=*/32, /*r_rob_depth=*/32, /*max_txns_per_id=*/32);

    // 5 single-beat reads, same id, same dest (addr in one tile), none drained.
    const uint8_t id = 3;
    const uint64_t addr = 0x100000000ull * 4;  // dst tile 4, len 0
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(rob.push_ar(make_ar(id, addr)));
        auto f = *ar_cap.pop();
        EXPECT_EQ(f.get_header_field("ordering_req"), 0u) << "beat " << i << " must bypass";
    }
    EXPECT_EQ(rob.read_slot_hwm(), 0u) << "the same-destination bypass took no slot for the streak";
}

TEST(RobSameDestBypass, DestChangeTriggersStickyFallback) {
    SCENARIO(
        "Rob Enabled: a dest change mid-streak allocates a RoB slot and stays sticky-allocated "
        "even after the dest "
        "reverts, until the id's list fully drains, at which point the idle-ID bypass re-opens");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    const uint8_t id = 3;
    const uint64_t dst_a = 0x100000000ull * 4;  // tile 4
    const uint64_t dst_b = 0x100000000ull * 5;  // tile 5

    ASSERT_TRUE(rob.push_ar(make_ar(id, dst_a)));  // idle-ID bypass
    EXPECT_EQ(ar_cap.pop()->get_header_field("ordering_req"), 0u);
    ASSERT_TRUE(rob.push_ar(make_ar(id, dst_a)));  // same-destination bypass: same dest
    EXPECT_EQ(ar_cap.pop()->get_header_field("ordering_req"), 0u);

    ASSERT_TRUE(rob.push_ar(make_ar(id, dst_b)));  // dest change: allocates a slot, sticky set
    auto f_dst_b = *ar_cap.pop();
    EXPECT_EQ(f_dst_b.get_header_field("ordering_req"), 1u);

    ASSERT_TRUE(rob.push_ar(make_ar(id, dst_a)));  // dest reverts to A: still allocates (sticky)
    auto f_dst_a2 = *ar_cap.pop();
    EXPECT_EQ(f_dst_a2.get_header_field("ordering_req"), 1u)
        << "sticky flag outlives the dest reverting to a prior value";

    // Drain the id's order deque in push order, exercising the sticky-clear sites:
    // the two bypassed entries via retire_read_primer's bypass-response pattern
    // (pop_r_staged's bypassed arm), then the two robbed entries via
    // Enabled_PopR_MultiBeatBurstCommitInOrder's ordering_tag-addressed response
    // pattern (drain_ready_read_heads_).
    retire_read_primer(rob, noc, depkt, id);  // drains bypassed entry 1 (dst_a)
    retire_read_primer(rob, noc, depkt, id);  // drains bypassed entry 2 (dst_a)

    const auto ordering_tag_dst_b = static_cast<uint8_t>(f_dst_b.get_header_field("ordering_tag"));
    const auto ordering_tag_dst_a2 =
        static_cast<uint8_t>(f_dst_a2.get_header_field("ordering_tag"));
    auto push_r = [&](uint8_t ordering_tag) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_R);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("flit_tail", 1);
        f.set_header_field("ordering_req", 1);
        f.set_header_field("ordering_tag", ordering_tag);
        f.set_payload_field("R", "rid", id);
        f.set_payload_field("R", "rresp", 0);
        f.set_payload_field("R", "rlast", 1u);
        std::array<uint8_t, 32> d{};
        f.set_payload_bytes("R", "rdata", d.data(), 256);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
    };
    push_r(ordering_tag_dst_b);
    depkt.tick();
    EXPECT_TRUE(rob.pop_r().has_value());  // drains robbed entry 3 (dst_b)

    push_r(ordering_tag_dst_a2);
    depkt.tick();
    EXPECT_TRUE(rob.pop_r().has_value());  // drains robbed entry 4 (dst_a revert)
    EXPECT_FALSE(rob.pop_r().has_value());

    // The order deque is now empty: fallen_back_read_ was reset by the sticky-clear
    // site in drain_ready_read_heads_, so a same-id push takes the idle-ID bypass (empty
    // list) and bypasses again -- proving the sticky flag actually cleared, not just that
    // it survived a dest revert.
    ASSERT_TRUE(rob.push_ar(make_ar(id, dst_a)));
    EXPECT_EQ(ar_cap.pop()->get_header_field("ordering_req"), 0u)
        << "the idle-ID bypass re-enables bypass once the id's order list fully drains";
}

TEST(RobSameDestBypass, MaxTxnsPerIdStillBoundsBypassedEntries) {
    SCENARIO("Rob Enabled: max_txns_per_id gates a same-id same-dest bypass streak too");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam(), 32, 32,
            /*max_txns_per_id=*/3);

    const uint8_t id = 3;
    const uint64_t addr = 0x100000000ull * 4;
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(rob.push_ar(make_ar(id, addr))) << "same-dest AR " << i;
        ar_cap.pop();
    }
    EXPECT_FALSE(rob.push_ar(make_ar(id, addr)))
        << "the order-list gate still bounds an all-bypassed streak";
}
