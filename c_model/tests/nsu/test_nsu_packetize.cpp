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

TEST(NsuPacketize, PushBLooksUpMetaAndEmitsFlit) {
    SCENARIO(
        "NSU Packetize: push_b reads MetaBuffer (src/rob_req/rob_idx) and stamps onto B flit "
        "header");
    RspCapture b_cap, r_cap;
    MetaBuffer mb(4);
    mb.snapshot_write(0x05, {/*src=*/0x12, /*rob_req=*/1, /*rob_idx=*/3});
    Packetize pkt(b_cap, r_cap, mb, kNsuSrcId);

    ASSERT_TRUE(pkt.push_b(make_b(0x05)));

    auto f = *b_cap.pop();
    EXPECT_EQ(f.get_header_field("axi_ch"), ni::AXI_CH_B);
    EXPECT_EQ(f.get_header_field("dst_id"), 0x12u);  // = orig src_id
    EXPECT_EQ(f.get_header_field("src_id"), kNsuSrcId);
    EXPECT_EQ(f.get_header_field("rob_req"), 1u);
    EXPECT_EQ(f.get_header_field("rob_idx"), 3u);
    EXPECT_EQ(f.get_payload_field("B", "bid"), 0x05u);
    EXPECT_EQ(f.get_payload_field("B", "bresp"), static_cast<uint64_t>(axi::Resp::OKAY));
    // metadata consumed
    EXPECT_FALSE(mb.peek_write(0x05).has_value());
}

TEST(NsuPacketize, PushBAssertsWithoutMatchingMeta) {
    SCENARIO("NSU Packetize: push_b without matching MetaBuffer entry aborts (B for unknown AW)");
    RspCapture b_cap, r_cap;
    MetaBuffer mb(4);
    Packetize pkt(b_cap, r_cap, mb, kNsuSrcId);
    EXPECT_DEATH(pkt.push_b(make_b(0x05)), ".*");
}

TEST(NsuPacketize, PushBNoCommitOnNocFull) {
    SCENARIO(
        "NSU Packetize: push_b fail on NoC full keeps MetaBuffer entry (peek-then-commit, no "
        "desync)");
    ChannelModel noc(/*req*/ 16, /*rsp*/ 1);
    RspCapture r_cap;
    MetaBuffer mb(4);
    mb.snapshot_write(0x05, {0x12, 0, 0});
    Packetize pkt(noc.rsp_out(), r_cap, mb, kNsuSrcId);
    // first B fills rsp_q (cap=1)
    ASSERT_TRUE(pkt.push_b(make_b(0x05)));
    EXPECT_FALSE(mb.peek_write(0x05).has_value());
    // Drain + add new entry + retry — peek+commit pattern means a SECOND push
    // attempt with rsp_q already full should NOT desync.
    mb.snapshot_write(0x06, {0x20, 0, 0});
    EXPECT_FALSE(pkt.push_b(make_b(0x06)));        // rsp_q still full
    EXPECT_TRUE(mb.peek_write(0x06).has_value());  // metadata still there
    noc.rsp_in().pop_flit();                       // drain
    EXPECT_TRUE(pkt.push_b(make_b(0x06)));
    EXPECT_FALSE(mb.peek_write(0x06).has_value());
}

TEST(NsuPacketize, PushRMultiBeatPeekUntilRLast) {
    SCENARIO(
        "NSU Packetize: push_r leaves MetaBuffer entry until rlast=1, then commits (multi-beat "
        "read)");
    RspCapture b_cap, r_cap;
    MetaBuffer mb(4);
    mb.snapshot_read(0x03, {0x12, 0, 5});
    Packetize pkt(b_cap, r_cap, mb, kNsuSrcId);

    ASSERT_TRUE(pkt.push_r(make_r(0x03, /*last*/ false)));
    EXPECT_TRUE(mb.peek_read(0x03).has_value());  // not committed
    ASSERT_TRUE(pkt.push_r(make_r(0x03, /*last*/ false)));
    EXPECT_TRUE(mb.peek_read(0x03).has_value());
    ASSERT_TRUE(pkt.push_r(make_r(0x03, /*last*/ true)));
    EXPECT_FALSE(mb.peek_read(0x03).has_value());  // committed on rlast
}

TEST(NsuPacketize, RPayloadBitPerfect) {
    SCENARIO(
        "NSU Packetize: R payload (rid/rresp/rlast/32B rdata) round-trips bit-perfect through "
        "flit");
    RspCapture b_cap, r_cap;
    MetaBuffer mb(4);
    mb.snapshot_read(0x03, {0x12, 0, 0});
    Packetize pkt(b_cap, r_cap, mb, kNsuSrcId);
    ASSERT_TRUE(pkt.push_r(make_r(0x03, /*last*/ true, axi::Resp::SLVERR)));
    auto f = *r_cap.pop();
    EXPECT_EQ(f.get_payload_field("R", "rid"), 0x03u);
    EXPECT_EQ(f.get_payload_field("R", "rresp"), static_cast<uint64_t>(axi::Resp::SLVERR));
    EXPECT_EQ(f.get_payload_field("R", "rlast"), 1u);
    std::array<uint8_t, 32> out{};
    f.get_payload_bytes("R", "rdata", out.data(), 256);
    for (int i = 0; i < 32; ++i) EXPECT_EQ(out[i], static_cast<uint8_t>(0xC0 + i));
}

// NsuPacketize::PushAwAssertFalse was a runtime wrong_side_() test.
// After T4 the method no longer exists on nsu::Packetize; wrong-side
// calls are now caught at compile time. Test removed.
