#include "nmu/depacketize.hpp"
#include "common/channel_model.hpp"
#include "axi/types.hpp"
#include <gtest/gtest.h>

using ni::cmodel::nmu::Depacketize;
using ni::cmodel::testing::ChannelModel;
namespace axi = ni::cmodel::axi;

namespace {
ni::cmodel::Flit make_b_flit(uint8_t bid, axi::Resp resp = axi::Resp::OKAY) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_NarrowB);
    f.set_header_field("dst_id", 0x10);
    f.set_header_field("flit_tail", 1);
    f.set_payload_field("B", "bid", bid);
    f.set_payload_field("B", "bresp", static_cast<uint64_t>(resp));
    return f;
}
ni::cmodel::Flit make_r_flit(uint8_t rid, bool rlast, uint8_t axi_ch = ni::AXI_CH_NarrowR) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", axi_ch);
    f.set_header_field("dst_id", 0x10);
    f.set_header_field("flit_tail", 1);
    const char* ch = (axi_ch == ni::AXI_CH_DataR) ? "DATA_R" : "NARROW_R";
    f.set_payload_field(ch, "rid", rid);
    f.set_payload_field(ch, "rlast", rlast ? 1u : 0u);
    return f;
}
}  // namespace

TEST(NmuDepacketize, PopRDecodesDataRFromDataRChannel) {
    ChannelModel noc(16, 16);
    Depacketize depkt(noc.rsp_in(), /*b*/ 16, /*r*/ 16);
    ASSERT_TRUE(noc.rsp_out().push_flit(make_r_flit(0x01, /*rlast*/ true, ni::AXI_CH_DataR)));
    depkt.tick();
    auto r = depkt.pop_r();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->id, 0x01);
    EXPECT_TRUE(r->last);
}

TEST(NmuDepacketize, PopBDecodesFromFlit) {
    ChannelModel noc(16, 16);
    Depacketize depkt(noc.rsp_in(), /*b*/ 16, /*r*/ 16);
    ASSERT_TRUE(noc.rsp_out().push_flit(make_b_flit(0x05, axi::Resp::SLVERR)));
    depkt.tick();
    auto b = depkt.pop_b();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->id, 0x05);
    EXPECT_EQ(b->resp, axi::Resp::SLVERR);
}

TEST(NmuDepacketize, DemuxMixedFlitsByAxiCh) {
    ChannelModel noc(16, 16);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    ASSERT_TRUE(noc.rsp_out().push_flit(make_b_flit(0x01)));
    ASSERT_TRUE(noc.rsp_out().push_flit(make_r_flit(0x02, true)));
    ASSERT_TRUE(noc.rsp_out().push_flit(make_b_flit(0x03)));
    depkt.tick();
    EXPECT_EQ(depkt.pop_b()->id, 0x01);
    EXPECT_EQ(depkt.pop_r()->id, 0x02);
    EXPECT_EQ(depkt.pop_b()->id, 0x03);
}

TEST(NmuDepacketize, PendingFlitHolBlockingBFullStallsR) {
    ChannelModel noc(16, 16);
    Depacketize depkt(noc.rsp_in(), /*b cap=*/1, /*r cap=*/16);
    // Queue order: B, B, R
    ASSERT_TRUE(noc.rsp_out().push_flit(make_b_flit(0x01)));
    ASSERT_TRUE(noc.rsp_out().push_flit(make_b_flit(0x02)));
    ASSERT_TRUE(noc.rsp_out().push_flit(make_r_flit(0x03, true)));
    depkt.tick();
    // First B fits; second B holds pending; R behind cannot progress
    EXPECT_TRUE(depkt.pop_b().has_value());   // 0x01
    EXPECT_FALSE(depkt.pop_r().has_value());  // R blocked behind pending B
    depkt.tick();                             // pending B (0x02) now placed
    EXPECT_TRUE(depkt.pop_b().has_value());   // 0x02
    depkt.tick();                             // R (0x03) now placed
    EXPECT_TRUE(depkt.pop_r().has_value());
}

TEST(NmuDepacketize, PopBEmptyReturnsNullopt) {
    ChannelModel noc(16, 16);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    EXPECT_FALSE(depkt.pop_b().has_value());
    EXPECT_FALSE(depkt.pop_r().has_value());
}

// NmuDepacketize::PopAwAssertFalse was a runtime wrong_side_() test.
// The method no longer exists on nmu::Depacketize; wrong-side
// calls are now caught at compile time. Test removed.

TEST(NmuDepacketize, BFifoOrderPreserved) {
    ChannelModel noc(16, 16);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    for (uint8_t i = 0; i < 5; ++i) ASSERT_TRUE(noc.rsp_out().push_flit(make_b_flit(i)));
    depkt.tick();
    for (uint8_t i = 0; i < 5; ++i) EXPECT_EQ(depkt.pop_b()->id, i);
}

TEST(NmuDepacketize, RPayloadBytesDecoded) {
    ChannelModel noc(16, 16);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_NarrowR);
    f.set_header_field("dst_id", 0x10);
    f.set_payload_field("NARROW_R", "rid", 0x07);
    f.set_payload_field("NARROW_R", "rlast", 1);
    std::array<uint8_t, axi::NARROW_DATA_BYTES> lane_data;
    for (int i = 0; i < axi::NARROW_DATA_BYTES; ++i) lane_data[i] = static_cast<uint8_t>(0xE0 + i);
    f.set_payload_bytes("NARROW_R", "rdata", lane_data.data(), ni::width::NOC_NARROW_DATA_WIDTH);
    ASSERT_TRUE(noc.rsp_out().push_flit(f));
    depkt.tick();
    auto r = depkt.pop_r();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->id, 0x07);
    EXPECT_EQ(r->last, true);
    std::array<uint8_t, axi::DATA_BYTES> expected{};
    for (int i = 0; i < axi::NARROW_DATA_BYTES; ++i) expected[i] = lane_data[i];
    EXPECT_EQ(r->data, expected);
}

TEST(NmuDepacketize, PopBWithMeta_ExtractsOrderingTagAndOrderingReq) {
    using namespace ni::cmodel;
    ChannelModel channel(/*req_depth=*/16, /*rsp_depth=*/16);
    nmu::Depacketize depkt(channel.rsp_in(), /*b_q_depth=*/16, /*r_q_depth=*/16);

    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_NarrowB);
    f.set_header_field("src_id", 0x10);
    f.set_header_field("dst_id", 0x01);
    f.set_header_field("vc_id", 0);
    f.set_header_field("flit_tail", 1);
    f.set_header_field("ordering_req", 1);
    f.set_header_field("ordering_tag", 5);
    f.set_payload_field("B", "bid", 0x02);
    f.set_payload_field("B", "bresp", 0);
    f.set_payload_field("B", "buser", 0);

    ASSERT_TRUE(channel.rsp_out().push_flit(f));
    depkt.tick();

    auto opt = depkt.pop_b_with_meta();
    ASSERT_TRUE(opt.has_value());
    auto [b, meta] = *opt;
    EXPECT_EQ(b.id, 0x02u);
    EXPECT_EQ(meta.ordering_tag, 5u);
    EXPECT_EQ(meta.ordering_req, 1u);
}

TEST(NmuDepacketize, PopRWithMeta_ExtractsPerBeatOrderingTag) {
    using namespace ni::cmodel;
    ChannelModel channel(/*req_depth=*/16, /*rsp_depth=*/16);
    nmu::Depacketize depkt(channel.rsp_in(), /*b_q_depth=*/16, /*r_q_depth=*/16);

    for (uint8_t i = 0; i < 4; ++i) {
        Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_NarrowR);
        f.set_header_field("src_id", 0x10);
        f.set_header_field("dst_id", 0x01);
        f.set_header_field("vc_id", 0);
        f.set_header_field("flit_tail", 1);
        f.set_header_field("ordering_req", 1);
        f.set_header_field("ordering_tag", 5 + i);
        f.set_payload_field("NARROW_R", "rid", 0x02);
        f.set_payload_field("NARROW_R", "rresp", 0);
        f.set_payload_field("NARROW_R", "ruser", 0);
        f.set_payload_field("NARROW_R", "rlast", (i == 3) ? 1u : 0u);
        std::array<uint8_t, axi::NARROW_DATA_BYTES> data{};
        data[0] = static_cast<uint8_t>(0xA0 + i);
        f.set_payload_bytes("NARROW_R", "rdata", data.data(), ni::width::NOC_NARROW_DATA_WIDTH);
        ASSERT_TRUE(channel.rsp_out().push_flit(f));
    }
    depkt.tick();

    for (uint8_t i = 0; i < 4; ++i) {
        auto opt = depkt.pop_r_with_meta();
        ASSERT_TRUE(opt.has_value()) << "beat " << static_cast<int>(i);
        auto [r, meta] = *opt;
        EXPECT_EQ(meta.ordering_tag, 5u + i);
        EXPECT_EQ(meta.ordering_req, 1u);
        EXPECT_EQ(r.last, i == 3);
        EXPECT_EQ(r.data[0], 0xA0u + i);
    }
}
