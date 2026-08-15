#include "flit.hpp"
#include <gtest/gtest.h>

using ni::cmodel::Flit;

TEST(Flit, ConstructFromRawHasMatchingWidth) {
    EXPECT_EQ(Flit::WIDTH_BITS, ni::FLIT_WIDTH);
    EXPECT_EQ(Flit::WIDTH_BYTES, (ni::FLIT_WIDTH + 7) / 8);
}

TEST(Flit, SetGetHeaderRoundtripAllFields) {
    Flit f;
    f.set_header_field("axi_ch", 0x4);  // NarrowR
    f.set_header_field("src_id", 0x12);
    f.set_header_field("dst_id", 0x34);
    f.set_header_field("fixed_vc", 0x1);
    f.set_header_field("vc_id", 0x2);
    f.set_header_field("flit_tail", 0x1);
    f.set_header_field("ordering_req", 0x1);
    f.set_header_field("ordering_tag", 0xFF);
    f.set_header_field("collective_op", 0x0);  // UNICAST
    f.set_header_field("collective_mask", 0x00);
    EXPECT_EQ(f.get_header_field("axi_ch"), 0x4u);
    EXPECT_EQ(f.get_header_field("src_id"), 0x12u);
    EXPECT_EQ(f.get_header_field("dst_id"), 0x34u);
    EXPECT_EQ(f.get_header_field("fixed_vc"), 0x1u);
    EXPECT_EQ(f.get_header_field("vc_id"), 0x2u);
    EXPECT_EQ(f.get_header_field("flit_tail"), 0x1u);
    EXPECT_EQ(f.get_header_field("ordering_req"), 0x1u);
    EXPECT_EQ(f.get_header_field("ordering_tag"), 0xFFu);
    EXPECT_EQ(f.get_header_field("collective_op"), 0x0u);
    EXPECT_EQ(f.get_header_field("collective_mask"), 0x00u);
}

TEST(Flit, SetGetPayloadAwFields) {
    Flit f;
    f.set_payload_field("AW", "awid", 0x05);
    f.set_payload_field("AW", "awaddr", 0xBEEFCAFEBABEull);  // 48 b, max representable
    f.set_payload_field("AW", "awlen", 0xFF);
    f.set_payload_field("AW", "awsize", 0x5);
    EXPECT_EQ(f.get_payload_field("AW", "awid"), 0x05u);
    EXPECT_EQ(f.get_payload_field("AW", "awaddr"), 0xBEEFCAFEBABEull);
    EXPECT_EQ(f.get_payload_field("AW", "awlen"), 0xFFu);
    EXPECT_EQ(f.get_payload_field("AW", "awsize"), 0x5u);
}

TEST(Flit, SetGetPayloadBytesWdata) {
    Flit f;
    constexpr int kBytes = ni::width::NOC_DATA_WIDTH / 8;
    std::array<uint8_t, kBytes> wdata{};
    for (int i = 0; i < kBytes; ++i) wdata[i] = static_cast<uint8_t>(0xA0 + i);
    f.set_payload_bytes("DATA_W", "wdata", wdata.data(), ni::width::NOC_DATA_WIDTH);
    std::array<uint8_t, kBytes> out{};
    f.get_payload_bytes("DATA_W", "wdata", out.data(), ni::width::NOC_DATA_WIDTH);
    EXPECT_EQ(out, wdata);
}

TEST(Flit, SetGetPayloadBytesNarrowWdata) {
    Flit f;
    constexpr int kBytes = ni::width::NOC_NARROW_DATA_WIDTH / 8;
    std::array<uint8_t, kBytes> wdata{};
    for (int i = 0; i < kBytes; ++i) wdata[i] = static_cast<uint8_t>(0xA0 + i);
    f.set_payload_bytes("NARROW_W", "wdata", wdata.data(), ni::width::NOC_NARROW_DATA_WIDTH);
    std::array<uint8_t, kBytes> out{};
    f.get_payload_bytes("NARROW_W", "wdata", out.data(), ni::width::NOC_NARROW_DATA_WIDTH);
    EXPECT_EQ(out, wdata);
}

TEST(Flit, PaddingCheckPassesWhenZero) {
    // The 48 b header has no reserved bits (spec drops the padding/rsvd
    // field entirely), so PADDING_FIELDS_COUNT is 0 and check_padding_is_zero
    // is vacuously true.
    Flit f;
    EXPECT_TRUE(f.check_padding_is_zero());
    EXPECT_EQ(ni::header::PADDING_FIELDS_COUNT, 0u);
}

TEST(FlitDispatch, UnknownHeaderFieldQueryAborts) {
    // header_field_pos falls through to the not-found abort for any name not
    // in codegen-emitted HEADER_FIELDS[]. Regex anchors on the fprintf
    // diagnostic prefix specifically (the assert string alone would also
    // match the assert message path).
    EXPECT_DEATH(
        { ni::cmodel::detail::header_field_pos("nonexistent_field"); },
        "ni::cmodel::detail::header_field_pos: name 'nonexistent_field' not found");
}

TEST(FlitDispatch, EnabledHeaderFieldsStillResolve) {
    // Sanity: enabled fields still resolve via generic loop.
    auto pos = ni::cmodel::detail::header_field_pos("dst_id");
    EXPECT_EQ(pos.lsb, ni::header::DST_ID_LSB);
    EXPECT_EQ(pos.msb, ni::header::DST_ID_MSB);
}

TEST(Flit, SetGetPayloadBFields) {
    Flit f;
    f.set_payload_field("B", "bid", 0x02);
    f.set_payload_field("B", "bresp", 0x2);  // SLVERR
    f.set_payload_field("B", "buser", 0x55);
    EXPECT_EQ(f.get_payload_field("B", "bid"), 0x02u);
    EXPECT_EQ(f.get_payload_field("B", "bresp"), 0x2u);
    EXPECT_EQ(f.get_payload_field("B", "buser"), 0x55u);
}

TEST(Flit, SetGetPayloadRFields) {
    Flit f;
    f.set_payload_field("NARROW_R", "rid", 0x07);
    f.set_payload_field("NARROW_R", "rresp", 0x3);  // DECERR
    f.set_payload_field("NARROW_R", "rlast", 0x1);
    EXPECT_EQ(f.get_payload_field("NARROW_R", "rid"), 0x07u);
    EXPECT_EQ(f.get_payload_field("NARROW_R", "rresp"), 0x3u);
    EXPECT_EQ(f.get_payload_field("NARROW_R", "rlast"), 0x1u);
}
