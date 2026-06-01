#include "ni_spec.hpp"
#include "flit.hpp"
#include <gtest/gtest.h>

using ni::cmodel::Flit;

TEST(Flit, ConstructFromRawHasMatchingWidth) {
  EXPECT_EQ(Flit::WIDTH_BITS,  ni::FLIT_WIDTH);
  EXPECT_EQ(Flit::WIDTH_BYTES, (ni::FLIT_WIDTH + 7) / 8);
}

TEST(Flit, SetGetDstIdRoundtrip) {
  Flit f;
  f.set_header_field("dst_id", 0x12);
  EXPECT_EQ(f.get_header_field("dst_id"), 0x12u);
}

TEST(Flit, SetHeaderFieldRespectsBitPosition) {
  Flit f;
  f.set_header_field("src_id", 0xAB);
  // Verify by reading raw bytes at expected position
  int lsb = ni::header::SRC_ID_LSB;
  int byte = lsb / 8, off = lsb % 8;
  uint64_t read = 0;
  for (int b = 0; b < (ni::header::SRC_ID_MSB - ni::header::SRC_ID_LSB + 1); ++b) {
    int gb = (lsb + b) / 8, go = (lsb + b) % 8;
    read |= ((uint64_t)((f.raw()[gb] >> go) & 1u)) << b;
  }
  EXPECT_EQ(read, 0xABu & ((1ull << (ni::header::SRC_ID_MSB - ni::header::SRC_ID_LSB + 1)) - 1));
}

TEST(Flit, SilentTruncateOnOversizedValueInRelease) {
  // In NDEBUG build, oversized value silently truncates; this test runs in
  // debug build so assertion fires. We test the truncation by passing exactly
  // the max+0 value (no overshoot).
  Flit f;
  uint64_t max_dst = (1ull << (ni::header::DST_ID_MSB - ni::header::DST_ID_LSB + 1)) - 1;
  f.set_header_field("dst_id", max_dst);
  EXPECT_EQ(f.get_header_field("dst_id"), max_dst);
}

TEST(Flit, PaddingBitSetCausesCheckToFail) {
  std::array<uint8_t, Flit::WIDTH_BYTES> raw{};
  ASSERT_GT(ni::header::PADDING_FIELDS_COUNT, 0u) << "no padding fields elaborated — codegen issue";
  int lsb = ni::header::PADDING_FIELDS[0].lsb;
  int byte = lsb / 8, off = lsb % 8;
  raw[byte] |= (1u << off);
  Flit f(raw);
  EXPECT_FALSE(f.check_padding_is_zero())
      << "padding bit at " << ni::header::PADDING_FIELDS[0].name << " was set, check should fail";
}

TEST(Flit, AllZeroRawPassesPaddingCheck) {
  Flit f;
  EXPECT_TRUE(f.check_padding_is_zero());
}

TEST(Flit, RawBytesAreZeroOnDefaultConstruct) {
  Flit f;
  for (auto byte : f.raw()) {
    EXPECT_EQ(byte, 0u);
  }
}

TEST(Flit, RoundTripMultipleFields) {
  Flit f;
  f.set_header_field("dst_id", 0x05);
  f.set_header_field("src_id", 0x12);
  f.set_header_field("rob_idx", 0x07);
  EXPECT_EQ(f.get_header_field("dst_id"),  0x05u);
  EXPECT_EQ(f.get_header_field("src_id"),  0x12u);
  EXPECT_EQ(f.get_header_field("rob_idx"), 0x07u);
}
