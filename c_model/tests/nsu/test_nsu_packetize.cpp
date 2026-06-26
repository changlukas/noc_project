// NSU Packetize unit tests — updated for T4 staged pipeline.
//
// Packetize is now a two-step component:
//   push_b/r() accepts ≤1 beat into the S1 stage register (returns false when
//              S1 is occupied).
//   tick()     reads the S1 register, builds the Flit, pushes to b_out_/r_out_
//              (WormholeArbiter input = S2→S3 boundary); commits MetaBuffer only
//              on successful push to the downstream (spec §5.1).
//
// Each test reflects the new two-step contract.
#include "nsu/packetize.hpp"
#include "nsu/meta_buffer.hpp"
#include "common/channel_model.hpp"
#include "common/per_channel_capture.hpp"
#include "common/scenario.hpp"
#include "axi/types.hpp"
#include <gtest/gtest.h>

using ni::cmodel::nsu::MetaBuffer;
using ni::cmodel::nsu::MetaEntry;
using ni::cmodel::nsu::Packetize;
using ni::cmodel::testing::ChannelModel;
using ni::cmodel::testing::RspCapture;
namespace axi = ni::cmodel::axi;

namespace {
constexpr uint8_t kNsuSrcId = 0x02;

axi::BBeat make_b(uint8_t id, axi::Resp resp = axi::Resp::OKAY) {
    axi::BBeat b{};
    b.id = id;
    b.resp = resp;
    b.user = 0;
    return b;
}
axi::RBeat make_r(uint8_t id, bool last, axi::Resp resp = axi::Resp::OKAY) {
    axi::RBeat r{};
    r.id = id;
    for (int i = 0; i < 32; ++i) r.data[i] = static_cast<uint8_t>(0xC0 + i);
    r.resp = resp;
    r.last = last;
    r.user = 0;
    return r;
}
}  // namespace

// push_b() accepts beat into S1 register; tick() peeks meta, builds flit,
// commits MetaBuffer on successful push to b_out_.
TEST(NsuPacketize, PushBLooksUpMetaAndEmitsFlit) {
    SCENARIO(
        "NSU Packetize: push_b accepts beat into S1; tick() reads MetaBuffer "
        "(src/rob_req/rob_idx), stamps onto B flit header, commits meta");
    RspCapture b_cap, r_cap;
    MetaBuffer mb(4);
    mb.snapshot_write(0x05, {/*src=*/0x12, /*rob_req=*/1, /*rob_idx=*/3});
    Packetize pkt(b_cap, r_cap, mb, kNsuSrcId);

    ASSERT_TRUE(pkt.push_b(make_b(0x05)));
    // S1 occupied; no flit emitted yet.
    EXPECT_TRUE(mb.peek_write(0x05).has_value()) << "meta must not be consumed before tick()";

    pkt.tick();  // S2 transform: build flit, push to b_cap, commit meta

    auto f = b_cap.pop();
    ASSERT_TRUE(f.has_value()) << "tick() must emit B flit to b_out_";
    EXPECT_EQ(f->get_header_field("axi_ch"), ni::AXI_CH_B);
    EXPECT_EQ(f->get_header_field("dst_id"), 0x12u);  // = orig src_id
    EXPECT_EQ(f->get_header_field("src_id"), kNsuSrcId);
    EXPECT_EQ(f->get_header_field("rob_req"), 1u);
    EXPECT_EQ(f->get_header_field("rob_idx"), 3u);
    EXPECT_EQ(f->get_payload_field("B", "bid"), 0x05u);
    EXPECT_EQ(f->get_payload_field("B", "bresp"), static_cast<uint64_t>(axi::Resp::OKAY));
    // metadata consumed on successful push
    EXPECT_FALSE(mb.peek_write(0x05).has_value());
}

// tick() with S1 occupied and no matching MetaBuffer entry triggers assert+abort.
TEST(NsuPacketize, TickAssertsOnBWithoutMatchingMeta) {
    SCENARIO(
        "NSU Packetize: tick() with B in S1 but no matching MetaBuffer entry aborts "
        "(pipeline protocol violation — B without prior AW snapshot)");
    RspCapture b_cap, r_cap;
    MetaBuffer mb(4);
    Packetize pkt(b_cap, r_cap, mb, kNsuSrcId);
    ASSERT_TRUE(pkt.push_b(make_b(0x05)));  // no meta for id=0x05
    EXPECT_DEATH(pkt.tick(), ".*");
}

// S1 backpressure: second push_b() returns false when S1 is occupied.
TEST(NsuPacketize, PushBBackpressureWhenS1Full) {
    SCENARIO("NSU Packetize: push_b returns false when S1 register is occupied");
    RspCapture b_cap, r_cap;
    MetaBuffer mb(4);
    mb.snapshot_write(0x05, {0x12, 0, 0});
    mb.snapshot_write(0x06, {0x20, 0, 0});
    Packetize pkt(b_cap, r_cap, mb, kNsuSrcId);

    ASSERT_TRUE(pkt.push_b(make_b(0x05)));   // S1 now occupied
    EXPECT_FALSE(pkt.push_b(make_b(0x06)));  // S1 full → backpressure
    pkt.tick();                              // drain S1
    EXPECT_TRUE(pkt.push_b(make_b(0x06)));   // S1 free again
}

// Peek+commit: if b_out_ is full, tick() does NOT consume S1 or commit meta.
// The beat stays in S1 until the downstream has space (no desync).
TEST(NsuPacketize, PushBNoCommitOnNocFull) {
    SCENARIO(
        "NSU Packetize: tick() with b_out_ full keeps S1 beat and MetaBuffer entry; "
        "peek+commit never desyncs meta from beat");
    ChannelModel noc(/*req*/ 16, /*rsp*/ 1);
    RspCapture r_cap;
    MetaBuffer mb(4);
    mb.snapshot_write(0x05, {0x12, 0, 0});
    Packetize pkt(noc.rsp_out(), r_cap, mb, kNsuSrcId);

    // B(0x05): push to S1, tick() → goes to noc (cap=1, now full), meta consumed.
    ASSERT_TRUE(pkt.push_b(make_b(0x05)));
    pkt.tick();
    EXPECT_FALSE(mb.peek_write(0x05).has_value()) << "meta consumed when noc accepted";
    EXPECT_EQ(pkt.s1_b_occupancy(), 0u);

    // B(0x06): push to S1. tick() → noc still full → push fails, S1 stays, meta kept.
    mb.snapshot_write(0x06, {0x20, 0, 0});
    ASSERT_TRUE(pkt.push_b(make_b(0x06)));
    pkt.tick();
    EXPECT_TRUE(mb.peek_write(0x06).has_value()) << "meta must NOT be consumed when noc full";
    EXPECT_EQ(pkt.s1_b_occupancy(), 1u) << "beat must remain in S1 when noc full";

    // Drain noc; next tick() succeeds and commits meta.
    noc.rsp_in().pop_flit();
    pkt.tick();
    EXPECT_FALSE(mb.peek_write(0x06).has_value()) << "meta consumed after noc drain";
    EXPECT_EQ(pkt.s1_b_occupancy(), 0u);
}

// Multi-beat R burst: MetaBuffer entry kept until rlast=1.
// Each R beat uses push_r() + tick() (S1 only holds one beat at a time).
TEST(NsuPacketize, PushRMultiBeatPeekUntilRLast) {
    SCENARIO(
        "NSU Packetize: tick() with R rlast=0 keeps MetaBuffer entry; "
        "tick() with R rlast=1 commits (multi-beat read burst)");
    RspCapture b_cap, r_cap;
    MetaBuffer mb(4);
    mb.snapshot_read(0x03, {0x12, 0, 5});
    Packetize pkt(b_cap, r_cap, mb, kNsuSrcId);

    ASSERT_TRUE(pkt.push_r(make_r(0x03, /*last*/ false)));
    pkt.tick();
    EXPECT_TRUE(mb.peek_read(0x03).has_value()) << "meta must not commit on non-last beat";

    ASSERT_TRUE(pkt.push_r(make_r(0x03, /*last*/ false)));
    pkt.tick();
    EXPECT_TRUE(mb.peek_read(0x03).has_value()) << "meta must not commit on non-last beat";

    ASSERT_TRUE(pkt.push_r(make_r(0x03, /*last*/ true)));
    pkt.tick();
    EXPECT_FALSE(mb.peek_read(0x03).has_value()) << "meta must commit on rlast=1";
}

// R payload bit-perfect: all fields (rid/rresp/rlast/rdata) survive push_r+tick.
TEST(NsuPacketize, RPayloadBitPerfect) {
    SCENARIO(
        "NSU Packetize: R payload (rid/rresp/rlast/32B rdata) round-trips bit-perfect through "
        "flit after push_r()+tick()");
    RspCapture b_cap, r_cap;
    MetaBuffer mb(4);
    mb.snapshot_read(0x03, {0x12, 0, 0});
    Packetize pkt(b_cap, r_cap, mb, kNsuSrcId);
    ASSERT_TRUE(pkt.push_r(make_r(0x03, /*last*/ true, axi::Resp::SLVERR)));
    pkt.tick();
    auto f = r_cap.pop();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->get_payload_field("R", "rid"), 0x03u);
    EXPECT_EQ(f->get_payload_field("R", "rresp"), static_cast<uint64_t>(axi::Resp::SLVERR));
    EXPECT_EQ(f->get_payload_field("R", "rlast"), 1u);
    std::array<uint8_t, 32> out{};
    f->get_payload_bytes("R", "rdata", out.data(), 256);
    for (int i = 0; i < 32; ++i) EXPECT_EQ(out[i], static_cast<uint8_t>(0xC0 + i));
}

// NsuPacketize::PushAwAssertFalse was a runtime wrong_side_() test.
// After T4 the method no longer exists on nsu::Packetize; wrong-side
// calls are now caught at compile time. Test removed.

// Multi-beat R burst: NSU stamps every R flit with the same rob_idx.
// All R beats of a burst peek the same MetaBuffer entry (committed only on
// rlast=1), so rob_idx is identical for all beats. This documents the
// source of the Family C hazard caught by the NMU ROB guard.
TEST(NsuPacketize, MultiBeatR_AllFlitsCarrySameRobIdx) {
    SCENARIO(
        "NSU Packetize: three R beats for the same AR share one MetaBuffer entry "
        "(peeked, not committed, until rlast=1). Every emitted R flit carries the "
        "same rob_idx, documenting the NSU same-base stamping that the NMU ROB "
        "Family C guard catches.");
    RspCapture b_cap, r_cap;
    MetaBuffer mb(4);
    constexpr uint8_t kRobIdx = 7;
    mb.snapshot_read(0x03, {/*src=*/0x12, /*rob_req=*/1, /*rob_idx=*/kRobIdx});
    Packetize pkt(b_cap, r_cap, mb, kNsuSrcId);

    // Three-beat burst: push_r + tick emits one flit per step.
    ASSERT_TRUE(pkt.push_r(make_r(0x03, /*last*/ false)));
    pkt.tick();
    auto f0 = r_cap.pop();
    ASSERT_TRUE(f0.has_value());
    EXPECT_EQ(f0->get_header_field("rob_idx"), kRobIdx);

    ASSERT_TRUE(pkt.push_r(make_r(0x03, /*last*/ false)));
    pkt.tick();
    auto f1 = r_cap.pop();
    ASSERT_TRUE(f1.has_value());
    EXPECT_EQ(f1->get_header_field("rob_idx"), kRobIdx);

    ASSERT_TRUE(pkt.push_r(make_r(0x03, /*last*/ true)));
    pkt.tick();
    auto f2 = r_cap.pop();
    ASSERT_TRUE(f2.has_value());
    EXPECT_EQ(f2->get_header_field("rob_idx"), kRobIdx);

    // Meta committed on rlast=1.
    EXPECT_FALSE(mb.peek_read(0x03).has_value());
}
