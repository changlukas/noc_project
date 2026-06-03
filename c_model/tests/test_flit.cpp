#include "ni/flit.hpp"
#include "common/scenario.hpp"
#include <gtest/gtest.h>

using ni::cmodel::Flit;

TEST(Flit, ConstructFromRawHasMatchingWidth) {
  SCENARIO("Flit: WIDTH_BITS/WIDTH_BYTES match codegen FLIT_WIDTH constant");
  EXPECT_EQ(Flit::WIDTH_BITS,  ni::FLIT_WIDTH);
  EXPECT_EQ(Flit::WIDTH_BYTES, (ni::FLIT_WIDTH + 7) / 8);
}

TEST(Flit, SetGetHeaderRoundtripAllFields) {
  SCENARIO("Flit: every header field (noc_qos/axi_ch/src_id/.../flit_ecc) set/get bit-perfect");
  Flit f;
  f.set_header_field("noc_qos",   0xA);
  f.set_header_field("axi_ch",    0x4);  // R
  f.set_header_field("src_id",    0x12);
  f.set_header_field("dst_id",    0x34);
  f.set_header_field("vc_id",     0x2);
  f.set_header_field("route_par", 0x1);
  f.set_header_field("last",      0x1);
  f.set_header_field("rob_req",   0x1);
  f.set_header_field("rob_idx",   0x1F);
  f.set_header_field("commtype",  0x2);
  f.set_header_field("multicast", 0xFF);
  f.set_header_field("flit_ecc",  0x3FF);
  EXPECT_EQ(f.get_header_field("noc_qos"),   0xAu);
  EXPECT_EQ(f.get_header_field("axi_ch"),    0x4u);
  EXPECT_EQ(f.get_header_field("src_id"),    0x12u);
  EXPECT_EQ(f.get_header_field("dst_id"),    0x34u);
  EXPECT_EQ(f.get_header_field("vc_id"),     0x2u);
  EXPECT_EQ(f.get_header_field("route_par"), 0x1u);
  EXPECT_EQ(f.get_header_field("last"),      0x1u);
  EXPECT_EQ(f.get_header_field("rob_req"),   0x1u);
  EXPECT_EQ(f.get_header_field("rob_idx"),   0x1Fu);
  EXPECT_EQ(f.get_header_field("commtype"),  0x2u);
  EXPECT_EQ(f.get_header_field("multicast"), 0xFFu);
  EXPECT_EQ(f.get_header_field("flit_ecc"),  0x3FFu);
}

TEST(Flit, SetGetPayloadAwFields) {
  SCENARIO("Flit: AW payload fields (awid/awaddr/awlen/awsize) set/get bit-perfect");
  Flit f;
  f.set_payload_field("AW", "awid",   0x55);
  f.set_payload_field("AW", "awaddr", 0xDEADBEEFCAFEBABEull);
  f.set_payload_field("AW", "awlen",  0xFF);
  f.set_payload_field("AW", "awsize", 0x5);
  EXPECT_EQ(f.get_payload_field("AW", "awid"),   0x55u);
  EXPECT_EQ(f.get_payload_field("AW", "awaddr"), 0xDEADBEEFCAFEBABEull);
  EXPECT_EQ(f.get_payload_field("AW", "awlen"),  0xFFu);
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

TEST(Flit, RsvdPaddingCheckPassesWhenZero) {
  SCENARIO("Flit: default-constructed flit has all rsvd/padding bits zero");
  Flit f;
  EXPECT_TRUE(f.check_padding_is_zero());
}

TEST(Flit, RsvdPaddingCheckFailsWhenSet) {
  SCENARIO("Flit: setting any rsvd bit makes check_padding_is_zero return false");
  Flit f;
  // Set a bit in rsvd region
  f.set_header_field("rsvd", 0x3);  // 2-bit rsvd
  EXPECT_FALSE(f.check_padding_is_zero());
}

TEST(Flit, SetGetPayloadBFields) {
  SCENARIO("Flit: B payload fields (bid/bresp/buser) set/get bit-perfect");
  Flit f;
  f.set_payload_field("B", "bid",   0x42);
  f.set_payload_field("B", "bresp", 0x2);  // SLVERR
  f.set_payload_field("B", "buser", 0x55);
  EXPECT_EQ(f.get_payload_field("B", "bid"),   0x42u);
  EXPECT_EQ(f.get_payload_field("B", "bresp"), 0x2u);
  EXPECT_EQ(f.get_payload_field("B", "buser"), 0x55u);
}

TEST(Flit, SetGetPayloadRFields) {
  SCENARIO("Flit: R payload fields (rid/rresp/rlast) set/get bit-perfect");
  Flit f;
  f.set_payload_field("R", "rid",   0x37);
  f.set_payload_field("R", "rresp", 0x3);  // DECERR
  f.set_payload_field("R", "rlast", 0x1);
  EXPECT_EQ(f.get_payload_field("R", "rid"),   0x37u);
  EXPECT_EQ(f.get_payload_field("R", "rresp"), 0x3u);
  EXPECT_EQ(f.get_payload_field("R", "rlast"), 0x1u);
}
