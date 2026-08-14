#include "nmu/rob.hpp"
#include "nmu/packetize.hpp"
#include "nmu/depacketize.hpp"
#include "common/channel_model.hpp"
#include "common/per_channel_capture.hpp"
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

// 16x16 uniform, 4 GB/tile: dst = addr/4GB, address forwarded
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
axi::ArBeat make_ar(uint8_t id, uint64_t addr, uint8_t len = 0) {
    axi::ArBeat b{};
    b.id = id;
    b.addr = addr;
    b.len = len;
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
    f.set_header_field("axi_ch", ni::AXI_CH_DataB);
    f.set_header_field("dst_id", 0x01);
    f.set_header_field("flit_tail", 1);
    f.set_payload_field("B", "bid", bid);
    f.set_payload_field("B", "bresp", 0);
    return f;
}
ni::cmodel::Flit make_r_flit(uint8_t rid, bool rlast) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_DataR);
    f.set_header_field("dst_id", 0x01);
    f.set_header_field("flit_tail", 1);
    f.set_payload_field("DATA_R", "rid", rid);
    f.set_payload_field("DATA_R", "rlast", rlast ? 1u : 0u);
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
    f.set_header_field("axi_ch", ni::AXI_CH_DataB);
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
    f.set_header_field("axi_ch", ni::AXI_CH_DataR);
    f.set_header_field("dst_id", kSrcId);
    f.set_header_field("flit_tail", 1);
    f.set_header_field("ordering_req", 0);
    f.set_header_field("ordering_tag", 0);
    f.set_payload_field("DATA_R", "rid", id);
    f.set_payload_field("DATA_R", "rresp", 0);
    f.set_payload_field("DATA_R", "rlast", 1u);
    std::array<uint8_t, axi::DATA_BYTES> d{};
    f.set_payload_bytes("DATA_R", "rdata", d.data(), ni::width::NOC_DATA_WIDTH);
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
    Packetize pkt{noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {}};
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
    RobTestbench r;
    // No push_aw yet -> credit=0 -> push_w must return false
    EXPECT_FALSE(r.rob.push_w(make_w(/*last=*/true)));
}

// === ROB invariants (3 tests) ===

TEST(NmuRob, Disabled_BackpressureAtomicityPushAw) {
    // Force downstream NoC full via small req_depth.
    // All 3 Packetize outputs share the same ChannelModel req_out.
    ChannelModel noc(/*req*/ 1, /*rsp*/ 16);
    Packetize pkt(noc.req_out(), noc.req_out(), noc.req_out(), noc.req_out(), noc.req_out(), kSrcId,
                  {});
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
    // Trigger backpressure: small req_depth fills after AW + W beats.
    // All 3 Packetize outputs share the same ChannelModel req_out so depth
    // limits apply regardless of which channel (AW or W) is being pushed.
    ChannelModel noc(/*req*/ 2, /*rsp*/ 16);
    Packetize pkt(noc.req_out(), noc.req_out(), noc.req_out(), noc.req_out(), noc.req_out(), kSrcId,
                  {});
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
    ChannelModel noc(/*req=*/16, /*rsp=*/16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    prime_write_id(rob, noc, 0x05);
    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));
    auto f = *noc.req_in().pop_flit();
    EXPECT_EQ(f.get_header_field("axi_ch"), ni::AXI_CH_DataAw);
    EXPECT_EQ(f.get_header_field("ordering_req"), 1u);
    EXPECT_EQ(f.get_header_field("ordering_tag"), 0u);  // first allocated slot
}

TEST(NmuRob, Enabled_PushAr_AllocatesConsecutiveSlotsForBurst) {
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    prime_read_id(rob, ar_cap, 0x05);
    prime_read_id(rob, ar_cap, 0x06);
    // AR len=3 → 4 beats → 4 consecutive slots
    axi::ArBeat ar = make_ar(0x05, 0x100);
    ar.len = 3;
    ASSERT_TRUE(rob.push_ar(ar));
    auto f = *ar_cap.pop();
    EXPECT_EQ(f.get_header_field("axi_ch"), ni::AXI_CH_DataAr);
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
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam(), 4, 8);
    EXPECT_EQ(rob.b_rob_depth(), 4u);
    EXPECT_EQ(rob.r_rob_depth(), 8u);
    EXPECT_EQ(rob.write_free_space(), 4u);
    EXPECT_EQ(rob.read_free_space(), 8u);
}

TEST(NmuRob, Enabled_DefaultDepthIsOneTwentyEight) {
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());
    EXPECT_EQ(rob.b_rob_depth(), 128u);
    EXPECT_EQ(rob.r_rob_depth(), 128u);
    EXPECT_EQ(Rob::ORDERING_TAG_SPACE, 256u);
}

TEST(NmuRob, Enabled_MaxBurst_AllocatesEveryEntry) {
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
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
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
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
        f.set_header_field("axi_ch", ni::AXI_CH_DataR);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("flit_tail", 1);
        f.set_header_field("ordering_req", 1);
        f.set_header_field("ordering_tag", 0);  // the NSU stamps the burst base on every beat
        f.set_payload_field("DATA_R", "rid", 0x05);
        f.set_payload_field("DATA_R", "rresp", 0);
        f.set_payload_field("DATA_R", "rlast", rlast ? 1u : 0u);
        std::array<uint8_t, axi::DATA_BYTES> d{};
        d[0] = marker;
        f.set_payload_bytes("DATA_R", "rdata", d.data(), ni::width::NOC_DATA_WIDTH);
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
    ChannelModel noc(64, 64);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
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
        f.set_header_field("axi_ch", ni::AXI_CH_DataR);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("flit_tail", 1);
        f.set_header_field("ordering_req", 1);
        f.set_header_field("ordering_tag", base);
        f.set_payload_field("DATA_R", "rid", rid);
        f.set_payload_field("DATA_R", "rresp", 0);
        f.set_payload_field("DATA_R", "rlast", rlast ? 1u : 0u);
        std::array<uint8_t, axi::DATA_BYTES> d{};
        f.set_payload_bytes("DATA_R", "rdata", d.data(), ni::width::NOC_DATA_WIDTH);
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
    ChannelModel noc(64, 64);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
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
        f.set_header_field("axi_ch", ni::AXI_CH_DataB);
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
    const std::size_t depth = GetParam();
    ChannelModel noc(512, 512);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 512, 512);
    // The idle-ID bypass exempts the first AW of an id, so drive one primed id. Raise the
    // per-id cap above depth so the RoB depth, not the cap, is the binding constraint:
    // prime_write_id issues one AW that never retires, then the loop below issues depth more.
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam(), depth, depth, depth + 2);

    EXPECT_EQ(rob.write_free_space(), depth);
    EXPECT_EQ(rob.read_free_space(), depth);

    prime_write_id(rob, noc, 0x04);
    for (std::size_t i = 0; i < depth; ++i) {
        ASSERT_TRUE(rob.push_aw(make_aw(0x04, 0x100))) << "AW " << i;
        auto f = *noc.req_in().pop_flit();
        EXPECT_LT(f.get_header_field("ordering_tag"), depth) << "base " << i << " escaped the pool";
    }
    EXPECT_EQ(rob.write_free_space(), 0u);
    EXPECT_FALSE(rob.push_aw(make_aw(0x04, 0x200)));
}

INSTANTIATE_TEST_SUITE_P(Depths, RobDepthParam,
                         ::testing::Values(1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u, 256u));

TEST(NmuRob, Enabled_LzcAllocator_ReusesFromTheTop) {
    ChannelModel noc(64, 64);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
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
        f.set_header_field("axi_ch", ni::AXI_CH_DataR);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("flit_tail", 1);
        f.set_header_field("ordering_req", 1);
        f.set_header_field("ordering_tag", 0);
        f.set_payload_field("DATA_R", "rid", 0x05);
        f.set_payload_field("DATA_R", "rresp", 0);
        f.set_payload_field("DATA_R", "rlast", rlast ? 1u : 0u);
        std::array<uint8_t, axi::DATA_BYTES> d{};
        f.set_payload_bytes("DATA_R", "rdata", d.data(), ni::width::NOC_DATA_WIDTH);
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
    // req queue depth = 1: pkt.push_ar_with_meta will fail after 1st push.
    // All 3 Packetize outputs share the same ChannelModel req_out so the
    // depth limit applies to AR pushes as well as AW.
    ChannelModel noc(/*req=*/1, /*rsp=*/16);
    Packetize pkt(noc.req_out(), noc.req_out(), noc.req_out(), noc.req_out(), noc.req_out(), kSrcId,
                  {});
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
    ChannelModel noc(64, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId,
                  {});  // aw uses noc; w/ar use captures
    Depacketize depkt(noc.rsp_in(), 16, 16);
    // One id, primed so the idle-ID bypass does not exempt the transactions under test.
    // Distinct ids would each bypass and never fill the RoB. The per-id cap must exceed
    // the RoB depth for the RoB to be the binding constraint (spec D4); the primer's AW
    // never retires, so the cap has to clear b_rob_depth + 1.
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam(), 32, 32, 64);

    prime_write_id(rob, noc, 0x04);
    for (std::size_t i = 0; i < rob.b_rob_depth(); ++i) {
        ASSERT_TRUE(rob.push_aw(make_aw(0x04, 0x100))) << "AW " << i;
    }
    EXPECT_EQ(rob.write_free_space(), 0u);
    EXPECT_FALSE(rob.push_aw(make_aw(0x04, 0x200)));
}

TEST(NmuRob, Enabled_MaxTxnsPerIdGate_RefusesWithFreeSlotsAvailable) {
    ChannelModel noc(256, 256);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 256, 256);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam(), 32, 32, 3);

    // The idle-ID bypass exempts the 1st (idle id). The 2nd changes dest, so the
    // same-destination bypass does not apply -- it allocates a RoB slot and sets
    // the sticky fallback flag. The 3rd reverts to the 1st's dest but stays
    // allocated (sticky), so the cap admits a bypass plus two allocated. The cap
    // counts list entries, not slots.
    const std::size_t free_before = rob.write_free_space();
    ASSERT_TRUE(rob.push_aw(make_aw(0x01, 0x100)));                   // dst 0: bypass
    ASSERT_TRUE(rob.push_aw(make_aw(0x01, 0x100000000ull + 0x140)));  // dst 1: allocates, sticky
    ASSERT_TRUE(rob.push_aw(make_aw(0x01, 0x180)));                   // dst 0: allocates (sticky)
    EXPECT_FALSE(rob.push_aw(make_aw(0x01, 0x400))) << "the per-id cap is what refuses here";
    EXPECT_EQ(rob.write_free_space(), free_before - 2) << "one bypass plus two allocated slots";
    EXPECT_TRUE(rob.push_aw(make_aw(0x02, 0x500))) << "the gate is per-id, not global";
}

TEST(NmuRob, Enabled_MaxTxnsPerIdGate_AppliesToReadsIndependently) {
    ChannelModel noc(256, 256);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 256, 256);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam(), 32, 32, 2);

    ASSERT_TRUE(rob.push_ar(make_ar(0x03, 0x100)));
    ASSERT_TRUE(rob.push_ar(make_ar(0x03, 0x200)));
    EXPECT_FALSE(rob.push_ar(make_ar(0x03, 0x300)));
    EXPECT_GT(rob.read_free_space(), 0u) << "the cap refused, not the pool";
    EXPECT_TRUE(rob.push_aw(make_aw(0x03, 0x400)))
        << "the write list of the same id is independent";
}

TEST(NmuRob, Enabled_MaxTxnsPerIdDefaultIsThirtyTwo) {
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());
    EXPECT_EQ(rob.max_txns_per_id(), 32u);
}

TEST(NmuRob, Enabled_PushAw_DownstreamBackpressure_AtomicRollback) {
    ChannelModel noc(/*req=*/1, /*rsp=*/16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId,
                  {});  // aw uses noc for backpressure
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
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    prime_write_id(rob, noc, 0x05);
    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));  // allocates slot 0
    retire_write_primer(rob, noc, depkt, 0x05);      // the robbed AW is now the head
    // Inject B with ordering_tag=0, matching the head of id=5's sequence
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_DataB);
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
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    // id=5: two AWs in flight, slots 0 + 1
    prime_write_id(rob, noc, 0x05);
    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));
    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100000100)));
    retire_write_primer(rob, noc, depkt, 0x05);  // the robbed AWs (slots 0,1) are now the head
    auto push_b = [&](uint8_t ordering_tag, uint8_t bresp) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_DataB);
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
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
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
        f.set_header_field("axi_ch", ni::AXI_CH_DataR);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("flit_tail", 1);
        f.set_header_field("ordering_req", 1);
        f.set_header_field("ordering_tag", ordering_tag);
        f.set_payload_field("DATA_R", "rid", 0x05);
        f.set_payload_field("DATA_R", "rresp", 0);
        f.set_payload_field("DATA_R", "rlast", rlast ? 1u : 0u);
        std::array<uint8_t, axi::DATA_BYTES> d{};
        d[0] = marker;
        f.set_payload_bytes("DATA_R", "rdata", d.data(), ni::width::NOC_DATA_WIDTH);
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
    ChannelModel noc(64, 64);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 64, 64);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam(), 8, 8);

    prime_read_id(rob, ar_cap, 0x05);
    axi::ArBeat ar = make_ar(0x05, 0x100);
    ar.len = 3;  // 4 beats, base 0
    ASSERT_TRUE(rob.push_ar(ar));
    retire_read_primer(rob, noc, depkt, 0x05);  // the burst under test is now the head

    auto push_r = [&](bool rlast, uint8_t marker) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_DataR);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("flit_tail", 1);
        f.set_header_field("ordering_req", 1);
        f.set_header_field("ordering_tag", 0);  // the NSU stamps the burst base on every beat
        f.set_payload_field("DATA_R", "rid", 0x05);
        f.set_payload_field("DATA_R", "rresp", 0);
        f.set_payload_field("DATA_R", "rlast", rlast ? 1u : 0u);
        std::array<uint8_t, axi::DATA_BYTES> d{};
        d[0] = marker;
        f.set_payload_bytes("DATA_R", "rdata", d.data(), ni::width::NOC_DATA_WIDTH);
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
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
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
        f.set_header_field("axi_ch", ni::AXI_CH_DataR);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("flit_tail", 1);
        f.set_header_field("ordering_req", 1);
        f.set_header_field("ordering_tag", ordering_tag);
        f.set_payload_field("DATA_R", "rid", rid);
        f.set_payload_field("DATA_R", "rresp", 0);
        f.set_payload_field("DATA_R", "rlast", 1u);
        std::array<uint8_t, axi::DATA_BYTES> d{};
        d[0] = rid;
        f.set_payload_bytes("DATA_R", "rdata", d.data(), ni::width::NOC_DATA_WIDTH);
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
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    // Inject B with ordering_tag=7, but no AW allocated that slot -> assert fires
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_DataB);
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
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    EXPECT_DEATH({ Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam(), 0, 32); }, ".*");
}

TEST(NmuRobDeath, Enabled_DepthAboveIdxSpaceAborts) {
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    EXPECT_DEATH(
        { Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam(), 32, Rob::ORDERING_TAG_SPACE + 1); },
        ".*");
}

TEST(NmuRobDeath, Enabled_MaxTxnsPerIdZeroAborts) {
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    EXPECT_DEATH({ Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam(), 32, 32, 0); }, ".*");
}

TEST(NmuRob, ReadFillSameBaseOrderingTagLandsInOrder) {
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    prime_read_id(rob, ar_cap, 0x05);
    axi::ArBeat ar = make_ar(0x05, 0x100);
    ar.len = 1;  // 2 beats -> slots 0..1, base=0
    ASSERT_TRUE(rob.push_ar(ar));
    retire_read_primer(rob, noc, depkt, 0x05);  // the burst under test is now the head

    auto push_r = [&](uint8_t ordering_tag, bool rlast, uint8_t marker) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_DataR);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("flit_tail", 1);
        f.set_header_field("ordering_req", 1);
        f.set_header_field("ordering_tag", ordering_tag);
        f.set_payload_field("DATA_R", "rid", 0x05);
        f.set_payload_field("DATA_R", "rresp", 0);
        f.set_payload_field("DATA_R", "rlast", rlast ? 1u : 0u);
        std::array<uint8_t, axi::DATA_BYTES> d{};
        d[0] = marker;
        f.set_payload_bytes("DATA_R", "rdata", d.data(), ni::width::NOC_DATA_WIDTH);
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
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    prime_read_id(rob, ar_cap, 0x05);  // the AR under test allocates, so read_range_len_ is set
    axi::ArBeat ar = make_ar(0x05, 0x100);
    ar.len = 1;  // 2 beats
    ASSERT_TRUE(rob.push_ar(ar));

    auto push_r = [&](bool rlast) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_DataR);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("flit_tail", 1);
        f.set_header_field("ordering_req", 1);
        f.set_header_field("ordering_tag", 0);
        f.set_payload_field("DATA_R", "rid", 0x05);
        f.set_payload_field("DATA_R", "rresp", 0);
        f.set_payload_field("DATA_R", "rlast", rlast ? 1u : 0u);
        std::array<uint8_t, axi::DATA_BYTES> d{};
        f.set_payload_bytes("DATA_R", "rdata", d.data(), ni::width::NOC_DATA_WIDTH);
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
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    prime_read_id(rob, ar_cap, 0x05);
    prime_read_id(rob, ar_cap, 0x06);

    auto push_r = [&](uint8_t id, bool rlast, uint8_t marker) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_DataR);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("flit_tail", 1);
        f.set_header_field("ordering_req", 1);
        f.set_header_field("ordering_tag", 0);
        f.set_payload_field("DATA_R", "rid", id);
        f.set_payload_field("DATA_R", "rresp", 0);
        f.set_payload_field("DATA_R", "rlast", rlast ? 1u : 0u);
        std::array<uint8_t, axi::DATA_BYTES> d{};
        d[0] = marker;
        f.set_payload_bytes("DATA_R", "rdata", d.data(), ni::width::NOC_DATA_WIDTH);
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
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
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
        f.set_header_field("axi_ch", ni::AXI_CH_DataR);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("flit_tail", 1);
        f.set_header_field("ordering_req", 1);
        f.set_header_field("ordering_tag", base);
        f.set_payload_field("DATA_R", "rid", 0x05);
        f.set_payload_field("DATA_R", "rresp", 0);
        f.set_payload_field("DATA_R", "rlast", rlast ? 1u : 0u);
        std::array<uint8_t, axi::DATA_BYTES> d{};
        d[0] = marker;
        f.set_payload_bytes("DATA_R", "rdata", d.data(), ni::width::NOC_DATA_WIDTH);
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
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    prime_write_id(rob, noc, 0x05);                  // bypassed head
    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));  // robbed, slot 0
    retire_write_primer(rob, noc, depkt, 0x05);      // head is now the robbed entry

    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_DataB);
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
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());  // depth 128

    const std::size_t free_before = rob.read_free_space();
    axi::ArBeat ar = make_ar(0x05, 0x100);
    ar.len = 255;  // 256 beats, twice the 128-slot read pool
    EXPECT_TRUE(rob.push_ar(ar));
    auto f = *ar_cap.pop();
    EXPECT_EQ(f.get_header_field("axi_ch"), ni::AXI_CH_DataAr);
    EXPECT_EQ(f.get_header_field("ordering_req"), 0u) << "bypassed AR carries ordering_req=0";
    EXPECT_EQ(rob.read_free_space(), free_before) << "bypass allocates nothing";
}

TEST(NmuRobDeath, Enabled_PushAr_OversizedBurst_SecondSameIdAbortsNotWedged) {
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    axi::ArBeat first = make_ar(0x05, 0x100);  // dst 0
    first.len = 255;
    ASSERT_TRUE(rob.push_ar(first));
    // Different dest than `first`: the same-destination bypass does not apply, so this
    // needs a RoB slot -- 256 beats into a 128-deep pool can never be admitted, which is
    // exactly the n > r_rob_depth_ guard's target, not ordinary backpressure.
    axi::ArBeat second = make_ar(0x05, 0x100000000ull + 0x200);  // dst 1
    second.len = 255;
    // EXPECT_DEATH forks: `second` never actually mutates `rob` in this (parent) process.
    // Matcher pins the new guard's message, not just "something aborted".
    EXPECT_DEATH(rob.push_ar(second), "exceed RoB read depth");

    axi::ArBeat other = make_ar(0x06, 0x300);
    other.len = 255;
    EXPECT_TRUE(rob.push_ar(other)) << "the abort is per-push, not a channel wedge";
}

TEST(NmuRob, Enabled_IdleIdBypass_FirstTxnPerIdAllocatesNoSlot) {
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    const std::size_t free_before = rob.write_free_space();
    ASSERT_TRUE(rob.push_aw(make_aw(0x01, 0x100)));  // dst 0
    auto f0 = *noc.req_in().pop_flit();
    EXPECT_EQ(f0.get_header_field("ordering_req"), 0u);
    EXPECT_EQ(rob.write_free_space(), free_before);

    // Different dest than the first: the same-destination bypass does not apply, so this
    // allocates a slot.
    ASSERT_TRUE(rob.push_aw(make_aw(0x01, 0x100000000ull + 0x200)));  // dst 1
    auto f1 = *noc.req_in().pop_flit();
    EXPECT_EQ(f1.get_header_field("ordering_req"), 1u);
    EXPECT_EQ(f1.get_header_field("ordering_tag"), 0u) << "first allocated slot";
    EXPECT_EQ(rob.write_free_space(), free_before - 1);
}

TEST(NmuRob, Enabled_BypassedBeat_ReleasesNoSlot) {
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    const std::size_t free_before = rob.write_free_space();
    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));  // bypassed: id 5 was idle
    noc.req_in().pop_flit();

    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_DataB);
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
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));  // dst 0, bypassed, ordering_req=0
    // Different dest than the first: the same-destination bypass does not apply, so this
    // allocates a slot, slot 0.
    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100000000ull + 0x200)));  // dst 1

    auto push_b = [&](unsigned ordering_req, unsigned ordering_tag, unsigned bresp) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_DataB);
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
    ChannelModel noc_e(64, 64);
    ReqCapture w_e, ar_e;
    Packetize pkt_e(noc_e.req_out(), w_e, ar_e, noc_e.req_out(), w_e, kSrcId, {});
    Depacketize depkt_e(noc_e.rsp_in(), 64, 64);
    Rob rob_e(pkt_e, depkt_e, RobMode::Enabled, legacy_sam(), 32, 32, 1);

    ChannelModel noc_d(64, 64);
    ReqCapture w_d, ar_d;
    Packetize pkt_d(noc_d.req_out(), w_d, ar_d, noc_d.req_out(), w_d, kSrcId, {});
    Depacketize depkt_d(noc_d.rsp_in(), 64, 64);
    Rob rob_d(pkt_d, depkt_d, RobMode::Disabled, legacy_sam());

    axi::ArBeat a0 = make_ar(0x01, 0x100);
    axi::ArBeat a1 = make_ar(0x01, 0x200);  // same id: both must refuse
    axi::ArBeat a2 = make_ar(0x02, 0x300);  // different id: both must accept

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
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
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
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
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
        f.set_header_field("axi_ch", ni::AXI_CH_DataR);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("flit_tail", 1);
        f.set_header_field("ordering_req", 1);
        f.set_header_field("ordering_tag", ordering_tag);
        f.set_payload_field("DATA_R", "rid", id);
        f.set_payload_field("DATA_R", "rresp", 0);
        f.set_payload_field("DATA_R", "rlast", 1u);
        std::array<uint8_t, axi::DATA_BYTES> d{};
        f.set_payload_bytes("DATA_R", "rdata", d.data(), ni::width::NOC_DATA_WIDTH);
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

// === Admission counters (measurement-only) ===
//
// Pins each counter to the branch it is supposed to count, so a counter wired to
// the wrong arm of the push_aw / push_ar chain fails here rather than silently
// producing a plausible-looking clause split in a co-sim run. The write trace is
// the spec Section 2.5 worked example (SPEC 17) up to AW#4, the one that
// allocates although its destination matches AW#3. AW#5 (list drained, branch 1
// again) is not repeated: RobSameDestBypass.DestChangeTriggersStickyFallback
// already proves the drain reopens the idle-ID bypass, and the counters read the
// same branch variables.
TEST(RobAdmissionStats, ClauseCountsFollowTheSpecTraceBranches) {
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, legacy_sam());

    // Section 2.5: AW#1 dst 2 (branch 1), AW#2 dst 2 (branch 2), AW#3 dst 5
    // (branch 3, sticky), AW#4 dst 5 (branch 3 despite the dest match).
    const uint8_t w_id = 3;
    const uint64_t dst_2 = 0x100000000ull * 2;
    const uint64_t dst_5 = 0x100000000ull * 5;
    for (uint64_t addr : {dst_2, dst_2, dst_5, dst_5}) {
        ASSERT_TRUE(rob.push_aw(make_aw(w_id, addr)));
    }
    EXPECT_EQ(rob.aw_idle_bypass_count(), 1u) << "only AW#1 found the id idle";
    EXPECT_EQ(rob.aw_same_dest_bypass_count(), 1u) << "only AW#2 matched a non-sticky dest";
    EXPECT_EQ(rob.aw_fallback_alloc_count(), 2u) << "AW#3 by dest change, AW#4 by the sticky flag";
    EXPECT_EQ(rob.aw_idle_bypass_count() + rob.aw_same_dest_bypass_count() +
                  rob.aw_fallback_alloc_count(),
              4u)
        << "the three branches partition the accepted AWs, none counted twice or dropped";
    EXPECT_EQ(rob.ar_idle_bypass_count() + rob.ar_same_dest_bypass_count() +
                  rob.ar_fallback_alloc_count(),
              0u)
        << "AW pushes must not touch the AR counters";

    // A same-dest AR streak on its own id: branch 1 once, then branch 2.
    const uint8_t r_id = 7;
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(rob.push_ar(make_ar(r_id, dst_2)));
        ar_cap.pop();
    }
    EXPECT_EQ(rob.ar_idle_bypass_count(), 1u);
    EXPECT_EQ(rob.ar_same_dest_bypass_count(), 2u);
    EXPECT_EQ(rob.ar_fallback_alloc_count(), 0u)
        << "an unbroken same-dest streak allocates nothing";
    EXPECT_EQ(rob.aw_fallback_alloc_count(), 2u) << "AR pushes must not touch the AW counters";
    EXPECT_EQ(rob.read_slot_hwm(), 0u) << "no AR allocated, so the R-RoB mark stays put";

    // The order-list mark is the deepest single list over both directions (4 writes
    // vs 3 reads); the pool marks are per direction. Nothing retired, so each mark
    // equals its current occupancy.
    EXPECT_EQ(rob.order_list_hwm(), 4u);
    EXPECT_EQ(rob.write_txns_hwm(), 4u);
    EXPECT_EQ(rob.read_txns_hwm(), 3u);
}

TEST(RobSameDestBypass, MaxTxnsPerIdStillBoundsBypassedEntries) {
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
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

// === Same-ID cross-class read ordering guard (S3a T3) ===
//
// The same-destination bypass predicate keys on (dst_id, id). Once DataR rides
// DAT while NarrowR stays on RSP (S3a T6), a same-ID config-then-memory (or
// memory-then-config) read pair to the same node would bypass on dst alone and
// return on two independently arbitrated networks with nothing enforcing AR
// order -- an AXI4 IHI 0022 A5.3 violation. The fix adds a class term: a class
// change is treated like a dest change, so the second read falls back to the
// RoB path and retires by ordering_tag in AR order. These tests are provable
// today (RobMode::Enabled, no fabric/network involved) even though the guard
// stays inert until T6 actually splits the networks.
//
// One SAM tile (dst 0) with two address windows of different class -- the
// shape the design's "config read then memory read to the same node" hazard
// requires: same dst_id, different axi::AxiClass.
addr_trans::SamTable cross_class_sam() {
    // Hand-built rather than SamTable::packed(): a single-node table has no
    // coordinate to derive a base from, so the bases are just stated.
    return addr_trans::SamTable(std::vector<addr_trans::SamEntry>{
        {0x0000, 0x1000, 0x00, axi::AxiClass::Narrow},        // [0, 0x1000): config space, dst 0
        {0x1000, 0x100000000ull, 0x00, axi::AxiClass::Data},  // [0x1000, ...): memory space, dst 0
    });
}
constexpr uint64_t kCrossClassNarrowAddr = 0x100;          // dst 0, Narrow
constexpr uint64_t kCrossClassDataAddr = 0x1000 + 0x2000;  // dst 0, Data

axi::ArBeat make_narrow_ar(uint8_t id, uint64_t addr) {
    axi::ArBeat b{};
    b.id = id;
    b.addr = addr;
    b.len = 0;
    b.size = 2;  // 4 B/beat -- legal narrow (<=3), matches the existing narrow-lane test
    b.burst = axi::Burst::INCR;
    return b;
}
axi::AwBeat make_narrow_aw(uint8_t id, uint64_t addr) {
    axi::AwBeat b{};
    b.id = id;
    b.addr = addr;
    b.len = 0;
    b.size = 2;  // 4 B/beat -- legal narrow (<=3), matches make_narrow_ar
    b.burst = axi::Burst::INCR;
    return b;
}

// AXI4 A5.3: a class change (Narrow->Data) forces ordering_req=1, closing the same-dest-bypass
// ordering hole S3a T6 opens across the two response networks.
TEST(RobCrossClassRead, NarrowThenDataSameDestFallsBackToRob) {
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, cross_class_sam());

    const uint8_t id = 5;
    ASSERT_TRUE(rob.push_ar(make_narrow_ar(id, kCrossClassNarrowAddr)));  // idle-ID bypass
    EXPECT_EQ(ar_cap.pop()->get_header_field("ordering_req"), 0u);

    ASSERT_TRUE(rob.push_ar(make_ar(id, kCrossClassDataAddr)));  // same dst, class change
    auto f = *ar_cap.pop();
    EXPECT_EQ(f.get_header_field("ordering_req"), 1u)
        << "same dst but Narrow->Data must fall back to the RoB path, not bypass";
}

TEST(RobCrossClassRead, DataThenNarrowSameDestFallsBackToRob) {
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, cross_class_sam());

    const uint8_t id = 5;
    ASSERT_TRUE(rob.push_ar(make_ar(id, kCrossClassDataAddr)));  // idle-ID bypass
    EXPECT_EQ(ar_cap.pop()->get_header_field("ordering_req"), 0u);

    ASSERT_TRUE(rob.push_ar(make_narrow_ar(id, kCrossClassNarrowAddr)));  // same dst, class change
    auto f = *ar_cap.pop();
    EXPECT_EQ(f.get_header_field("ordering_req"), 1u)
        << "same dst but Data->Narrow must fall back to the RoB path, not bypass";
}

TEST(RobCrossClassRead, SameClassSameDestStillBypasses) {
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, cross_class_sam());

    const uint8_t id = 5;
    ASSERT_TRUE(rob.push_ar(make_narrow_ar(id, kCrossClassNarrowAddr)));  // idle-ID bypass
    EXPECT_EQ(ar_cap.pop()->get_header_field("ordering_req"), 0u);

    ASSERT_TRUE(rob.push_ar(make_narrow_ar(id, kCrossClassNarrowAddr)));  // same dst, same class
    EXPECT_EQ(ar_cap.pop()->get_header_field("ordering_req"), 0u)
        << "same dst, same class must still bypass -- the class term must not over-trigger";
}

// === Same-ID cross-class write ordering guard (final-review fix wave) ===
//
// Mirrors RobCrossClassRead above on the WRITE side. T6 splits the REQUEST
// side by class regardless of AXI channel direction: NarrowAw rides REQ,
// DataAw rides DAT. The same-destination bypass predicate keyed on dst_id
// alone would let a same-ID config-then-memory (or memory-then-config) write
// pair to the same node bypass, sending the two AWs out on two
// independently-arbitrated networks with nothing enforcing AW order -- B
// responses (which always ride RSP, unlike R) would then return out of AW
// order, an AXI4 IHI 0022 §A5.3 violation. aw_cap is wired to both aw_out_
// and dat_aw_out_ so it captures the AW header regardless of which network
// steering sends it to, mirroring how ar_cap captures AR unconditionally
// (AR never splits by class).
// AXI4 A5.3: symmetric to the read case -- a class change on a same-id same-dest write also forces
// ordering_req=1 to close the S3a T6 ordering hole.
TEST(RobCrossClassWrite, NarrowThenDataSameDestFallsBackToRob) {
    ChannelModel noc(16, 16);
    ReqCapture aw_cap, w_cap;
    Packetize pkt(aw_cap, w_cap, w_cap, aw_cap, w_cap, kSrcId, cross_class_sam());
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, cross_class_sam());

    const uint8_t id = 5;
    ASSERT_TRUE(rob.push_aw(make_narrow_aw(id, kCrossClassNarrowAddr)));  // idle-ID bypass
    EXPECT_EQ(aw_cap.pop()->get_header_field("ordering_req"), 0u);

    ASSERT_TRUE(rob.push_aw(make_aw(id, kCrossClassDataAddr)));  // same dst, class change
    auto f = *aw_cap.pop();
    EXPECT_EQ(f.get_header_field("ordering_req"), 1u)
        << "same dst but Narrow->Data must fall back to the RoB path, not bypass";
}

TEST(RobCrossClassWrite, DataThenNarrowSameDestFallsBackToRob) {
    ChannelModel noc(16, 16);
    ReqCapture aw_cap, w_cap;
    Packetize pkt(aw_cap, w_cap, w_cap, aw_cap, w_cap, kSrcId, cross_class_sam());
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, cross_class_sam());

    const uint8_t id = 5;
    ASSERT_TRUE(rob.push_aw(make_aw(id, kCrossClassDataAddr)));  // idle-ID bypass
    EXPECT_EQ(aw_cap.pop()->get_header_field("ordering_req"), 0u);

    ASSERT_TRUE(rob.push_aw(make_narrow_aw(id, kCrossClassNarrowAddr)));  // same dst, class change
    auto f = *aw_cap.pop();
    EXPECT_EQ(f.get_header_field("ordering_req"), 1u)
        << "same dst but Data->Narrow must fall back to the RoB path, not bypass";
}

TEST(RobCrossClassWrite, SameClassSameDestStillBypasses) {
    ChannelModel noc(16, 16);
    ReqCapture aw_cap, w_cap;
    Packetize pkt(aw_cap, w_cap, w_cap, aw_cap, w_cap, kSrcId, cross_class_sam());
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, cross_class_sam());

    const uint8_t id = 5;
    ASSERT_TRUE(rob.push_aw(make_narrow_aw(id, kCrossClassNarrowAddr)));  // idle-ID bypass
    EXPECT_EQ(aw_cap.pop()->get_header_field("ordering_req"), 0u);

    ASSERT_TRUE(rob.push_aw(make_narrow_aw(id, kCrossClassNarrowAddr)));  // same dst, same class
    EXPECT_EQ(aw_cap.pop()->get_header_field("ordering_req"), 0u)
        << "same dst, same class must still bypass -- the class term must not over-trigger";
}

TEST(NmuRob, Enabled_NarrowReadUnalignedAddrReanchorsToCorrectLane) {
    // Narrow-class SAM: every tile is config space, unlike legacy_sam()'s memory-space default.
    std::vector<addr_trans::PackedTile> tiles;
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 16; ++x)
            tiles.push_back({x, y, 0x100000000ull, axi::AxiClass::Narrow});
    auto narrow_sam = addr_trans::SamTable::packed(tiles, /*x_span=*/16, /*y_span=*/16);

    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId,
                  addr_trans::SamTable{});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, narrow_sam);

    auto narrow_ar = [](uint8_t id, uint64_t addr) {
        axi::ArBeat b{};
        b.id = id;
        b.addr = addr;
        b.len = 0;
        b.size = 2;  // 4 B/beat -- legal narrow (<=3)
        b.burst = axi::Burst::INCR;
        return b;
    };
    auto narrow_r_flit = [](uint8_t id, uint8_t ordering_req, uint8_t ordering_tag,
                            const uint8_t* lane_bytes) {
        ni::cmodel::Flit r;
        r.set_header_field("axi_ch", ni::AXI_CH_NarrowR);
        r.set_header_field("dst_id", kSrcId);
        r.set_header_field("flit_tail", 1);
        r.set_header_field("ordering_req", ordering_req);
        r.set_header_field("ordering_tag", ordering_tag);
        r.set_payload_field("NARROW_R", "rid", id);
        r.set_payload_field("NARROW_R", "rlast", 1u);
        r.set_payload_bytes("NARROW_R", "rdata", lane_bytes, ni::width::NOC_NARROW_DATA_WIDTH);
        return r;
    };

    // Primer (idle-ID bypass) at a far destination (tile 15) so the AR under
    // test -- same id, different dest -- takes the robbed path, not same-dest
    // bypass.
    ASSERT_TRUE(rob.push_ar(narrow_ar(0x05, 0x100000000ull * 15 + 0x8)));
    ar_cap.pop();  // discard the primer's AR flit

    // AR under test: genuinely unaligned local_addr (0x1B, not a multiple of
    // the 4 B beat size), tile 0 (different dest -> robbed).
    constexpr uint64_t kUnalignedAddr = 0x1B;
    ASSERT_TRUE(rob.push_ar(narrow_ar(0x05, kUnalignedAddr)));
    auto f = *ar_cap.pop();
    const uint8_t ordering_tag = static_cast<uint8_t>(f.get_header_field("ordering_tag"));

    // Retire the primer (bypassed) so it stops blocking the head of id 0x05's
    // order list.
    std::array<uint8_t, axi::NARROW_DATA_BYTES> primer_bytes{};
    ASSERT_TRUE(noc.rsp_out().push_flit(
        narrow_r_flit(0x05, /*ordering_req=*/0, /*ordering_tag=*/0, primer_bytes.data())));
    depkt.tick();
    ASSERT_TRUE(rob.pop_r().has_value());

    // Inject the R response for the AR under test as nmu::Depacketize::decode_r
    // would produce it: data placed at byte offset 0 (it has no address of its
    // own to re-anchor with -- that is exactly the risk this site flags).
    std::array<uint8_t, axi::NARROW_DATA_BYTES> lane_bytes{};
    for (int i = 0; i < 4; ++i) lane_bytes[i] = static_cast<uint8_t>(0xE0 + i);
    ASSERT_TRUE(noc.rsp_out().push_flit(
        narrow_r_flit(0x05, /*ordering_req=*/1, ordering_tag, lane_bytes.data())));
    depkt.tick();

    auto rb = rob.pop_r();
    ASSERT_TRUE(rb.has_value());
    // narrow_lane(0x1B) = (0x1B >> 3) & 7 = 3 -> byte offset 24: the RoB must
    // move the decoder's lane-0 placement here, not leave it at 0.
    constexpr unsigned kByteOffset = 24;
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(rb->data[kByteOffset + i], static_cast<uint8_t>(0xE0 + i));
    EXPECT_EQ(rb->data[0], 0u) << "must not be left at the decoder's lane-0 placeholder";
}

// === In-flight transaction counts (AW / AR, all AXI ids) ===
//
// write_txns_ / read_txns_ admit and refuse nothing -- the per-id order list is
// the only admission bound. What they still carry is the transaction GRAIN
// (a burst of any length counts one, released on rlast) and the retire-side
// unmatched-response guard, which is what the cases below pin down.
namespace {

struct PoolTestbench {
    ChannelModel noc{1024, 1024};
    ReqCapture w_cap, ar_cap;
    Packetize pkt{noc.req_out(), w_cap, ar_cap, noc.req_out(), w_cap, kSrcId, {}};
    Depacketize depkt{noc.rsp_in(), 1024, 1024};
};

// Bypassed R beat for id, ordering_req = 0 (the idle-ID / same-dest arm).
ni::cmodel::Flit make_bypassed_r_flit(uint8_t rid, bool rlast) {
    ni::cmodel::Flit f = make_r_flit(rid, rlast);
    f.set_header_field("ordering_req", 0);
    f.set_header_field("ordering_tag", 0);
    std::array<uint8_t, axi::DATA_BYTES> d{};
    f.set_payload_bytes("DATA_R", "rdata", d.data(), ni::width::NOC_DATA_WIDTH);
    return f;
}

}  // namespace

// The per-id cap is a free parameter; every case below runs across its legal range.
class OutstandingCountParam : public ::testing::TestWithParam<std::size_t> {};

TEST_P(OutstandingCountParam, BypassStreakCountsTransactionsButTakesNoRobSlots) {
    const std::size_t cap = GetParam();
    constexpr std::size_t kRobDepth = 32;
    PoolTestbench t;
    Rob rob(t.pkt, t.depkt, RobMode::Enabled, legacy_sam(), kRobDepth, kRobDepth,
            /*max_txns_per_id=*/cap);

    // Same id, same destination: the first push is the idle-ID bypass, the rest ride the
    // same-destination bypass. None reserves an ordering_tag.
    for (std::size_t i = 0; i < cap; ++i) {
        ASSERT_TRUE(rob.push_aw(make_aw(7, kPrimerAddr))) << "bypassed AW " << i;
    }
    EXPECT_EQ(rob.write_free_space(), kRobDepth) << "a bypassed streak must not touch the RoB";
    EXPECT_EQ(rob.write_txns(), cap) << "every bypassed push still counts";
}

TEST_P(OutstandingCountParam, MultiBeatReadCountsOneTransactionAndRetiresOnLast) {
    const std::size_t cap = GetParam();
    PoolTestbench t;
    Rob rob(t.pkt, t.depkt, RobMode::Enabled, legacy_sam(), 32, 32, /*max_txns_per_id=*/cap);

    constexpr uint8_t kId = 1;
    constexpr uint8_t kBeats = 8;
    ASSERT_TRUE(rob.push_ar(make_ar(kId, kPrimerAddr, /*len=*/kBeats - 1)));
    t.ar_cap.pop();
    EXPECT_EQ(rob.read_txns(), 1u) << "a burst of any length is one outstanding transaction";

    for (uint8_t beat = 0; beat < kBeats; ++beat) {
        const bool last = (beat == kBeats - 1);
        ASSERT_TRUE(t.noc.rsp_out().push_flit(make_bypassed_r_flit(kId, last)));
        t.depkt.tick();
        ASSERT_TRUE(rob.pop_r().has_value()) << "beat " << int(beat);
        EXPECT_EQ(rob.read_txns(), last ? 0u : 1u) << "beat " << int(beat);
    }
}

TEST_P(OutstandingCountParam, RoblessMultiBeatReadCountsOneTransaction) {
    const std::size_t cap = GetParam();
    PoolTestbench t;
    Rob rob(t.pkt, t.depkt, RobMode::Disabled, legacy_sam(), 32, 32, /*max_txns_per_id=*/cap);

    constexpr uint8_t kId = 1;
    constexpr uint8_t kBeats = 4;
    ASSERT_TRUE(rob.push_ar(make_ar(kId, kPrimerAddr, /*len=*/kBeats - 1)));
    EXPECT_EQ(rob.read_txns(), 1u);

    for (uint8_t beat = 0; beat < kBeats; ++beat) {
        const bool last = (beat == kBeats - 1);
        ASSERT_TRUE(t.noc.rsp_out().push_flit(make_bypassed_r_flit(kId, last)));
        t.depkt.tick();
        ASSERT_TRUE(rob.pop_r().has_value()) << "beat " << int(beat);
        EXPECT_EQ(rob.read_txns(), last ? 0u : 1u) << "beat " << int(beat);
    }
    // The per-id interlock released with the transaction, so the id is usable again.
    EXPECT_TRUE(rob.push_ar(make_ar(kId, kPrimerAddr)));
}

INSTANTIATE_TEST_SUITE_P(Caps, OutstandingCountParam,
                         ::testing::Values(1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u, 256u));

TEST(NmuRobOutstandingCount, BypassedThenRobbedOnOneIdCountOneEach) {
    constexpr std::size_t kRobDepth = 32;
    PoolTestbench t;
    Rob rob(t.pkt, t.depkt, RobMode::Enabled, legacy_sam(), kRobDepth, kRobDepth,
            /*max_txns_per_id=*/8);

    constexpr uint8_t kId = 4;
    ASSERT_TRUE(rob.push_aw(make_aw(kId, kPrimerAddr)));  // idle-ID bypass, no slot
    t.noc.req_in().pop_flit();
    ASSERT_TRUE(rob.push_aw(make_aw(kId, 0x100)));  // new destination -> falls back, takes a slot
    t.noc.req_in().pop_flit();

    EXPECT_EQ(rob.write_txns(), 2u) << "both arms count a transaction";
    EXPECT_EQ(rob.write_free_space(), kRobDepth - 1) << "only the robbed push takes a slot";

    // The bypassed transaction is the list head and retires on its own response.
    retire_write_primer(rob, t.noc, t.depkt, kId);
    EXPECT_EQ(rob.write_txns(), 1u);
}

TEST(NmuRobOutstandingCount, WBeatsOfAdmittedBurstsFlowWhileAwIsRefused) {
    PoolTestbench t;
    // The per-id cap is what refuses here; the subject is w_bursts_owed_, which must
    // count admitted AWs only, whatever refused the rest.
    Rob rob(t.pkt, t.depkt, RobMode::Disabled, legacy_sam(), 256, 256, /*max_txns_per_id=*/2);

    ASSERT_TRUE(rob.push_aw(make_aw(0, kPrimerAddr)));
    ASSERT_TRUE(rob.push_aw(make_aw(0, kPrimerAddr)));
    ASSERT_FALSE(rob.push_aw(make_aw(0, kPrimerAddr))) << "per-id cap full";

    // w_bursts_owed_ counts admitted AWs only, so exactly two W bodies are owed.
    EXPECT_TRUE(rob.push_w(make_w(/*last=*/true)));
    EXPECT_TRUE(rob.push_w(make_w(/*last=*/true)));
    EXPECT_FALSE(rob.push_w(make_w(/*last=*/true)))
        << "the refused burst's W beat has no admitted AW to pair with";
}
