#include "flit.hpp"
#include "common/scenario.hpp"
#include <gtest/gtest.h>

using ni::cmodel::Flit;

TEST(Flit, ConstructFromRawHasMatchingWidth) {
    SCENARIO("Flit: WIDTH_BITS/WIDTH_BYTES match codegen FLIT_WIDTH constant");
    EXPECT_EQ(Flit::WIDTH_BITS, ni::FLIT_WIDTH);
    EXPECT_EQ(Flit::WIDTH_BYTES, (ni::FLIT_WIDTH + 7) / 8);
}

TEST(Flit, SetGetHeaderRoundtripAllFields) {
    SCENARIO(
        "Flit: every header field (axi_ch/src_id/dst_id/fixed_vc/vc_id/"
        "flit_tail/ordering_req/ordering_tag/collective_op/collective_mask) "
        "set/get bit-perfect");
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
    SCENARIO("Flit: AW payload fields (awid/awaddr/awlen/awsize) set/get bit-perfect");
    Flit f;
    f.set_payload_field("AW", "awid", 0x55);
    f.set_payload_field("AW", "awaddr", 0xBEEFCAFEBABEull);  // 48 b, max representable
    f.set_payload_field("AW", "awlen", 0xFF);
    f.set_payload_field("AW", "awsize", 0x5);
    EXPECT_EQ(f.get_payload_field("AW", "awid"), 0x55u);
    EXPECT_EQ(f.get_payload_field("AW", "awaddr"), 0xBEEFCAFEBABEull);
    EXPECT_EQ(f.get_payload_field("AW", "awlen"), 0xFFu);
    EXPECT_EQ(f.get_payload_field("AW", "awsize"), 0x5u);
}

TEST(Flit, SetGetPayloadBytesWdata) {
    SCENARIO("Flit: W payload wdata (32B) byte-array set/get round-trips bit-perfect");
    Flit f;
    std::array<uint8_t, 32> wdata{};
    for (int i = 0; i < 32; ++i) wdata[i] = static_cast<uint8_t>(0xA0 + i);
    f.set_payload_bytes("W", "wdata", wdata.data(), 256);
    std::array<uint8_t, 32> out{};
    f.get_payload_bytes("W", "wdata", out.data(), 256);
    EXPECT_EQ(out, wdata);
}

TEST(Flit, PaddingCheckPassesWhenZero) {
    SCENARIO("Flit: default-constructed flit has all padding bits zero");
    Flit f;
    EXPECT_TRUE(f.check_padding_is_zero());
}

TEST(Flit, PaddingCheckAlwaysPassesNoReservedBits) {
    // The 44 b header has no reserved bits (spec drops the padding/rsvd
    // field entirely), so PADDING_FIELDS_COUNT is 0 and check_padding_is_zero
    // is vacuously true regardless of what bits are set elsewhere in the flit.
    SCENARIO("Flit: check_padding_is_zero is vacuously true, 44 b header has no padding");
    Flit f;
    f.raw()[6] |= (1u << 6);  // arbitrary payload bit, not a header field
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
    SCENARIO("Flit: B payload fields (bid/bresp/buser) set/get bit-perfect");
    Flit f;
    f.set_payload_field("B", "bid", 0x42);
    f.set_payload_field("B", "bresp", 0x2);  // SLVERR
    f.set_payload_field("B", "buser", 0x55);
    EXPECT_EQ(f.get_payload_field("B", "bid"), 0x42u);
    EXPECT_EQ(f.get_payload_field("B", "bresp"), 0x2u);
    EXPECT_EQ(f.get_payload_field("B", "buser"), 0x55u);
}

TEST(Flit, SetGetPayloadRFields) {
    SCENARIO("Flit: R payload fields (rid/rresp/rlast) set/get bit-perfect");
    Flit f;
    f.set_payload_field("R", "rid", 0x37);
    f.set_payload_field("R", "rresp", 0x3);  // DECERR
    f.set_payload_field("R", "rlast", 0x1);
    EXPECT_EQ(f.get_payload_field("R", "rid"), 0x37u);
    EXPECT_EQ(f.get_payload_field("R", "rresp"), 0x3u);
    EXPECT_EQ(f.get_payload_field("R", "rlast"), 0x1u);
}
