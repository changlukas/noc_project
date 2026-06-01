#include "axi/scoreboard.hpp"
#include <gtest/gtest.h>

namespace axi = ni::cmodel::axi;

// Under lane-positioned bus semantics, WriteResult/ReadResult carry
// size/len/burst so the scoreboard can re-derive per-beat byte_lane. Tests
// pass size=5, len=0, burst=INCR (1-beat, full-bus) unless the case requires
// otherwise — that combination matches Phase A's only-supported geometry.
TEST(Scoreboard, NoUpdateOnDecerr) {
  axi::Scoreboard sb;
  std::vector<uint32_t> strb1(1, 0xFFFF'FFFFu);
  sb.handle_write_completed(
      axi::WriteResult{0x100, /*size*/5, /*len*/0, axi::Burst::INCR,
                       axi::LockType::Normal,
                       {0xAB, 0xCD, 0xEF, 0x12}, strb1, axi::Resp::DECERR, 1, 1},
      std::vector<uint8_t>{0xAB, 0xCD, 0xEF, 0x12},
      strb1);
  sb.handle_read_observed(
      axi::ReadResult{0x100, /*size*/5, /*len*/0, axi::Burst::INCR,
                      {0x00, 0x00}, axi::Resp::OKAY, 1, 2});
  EXPECT_EQ(sb.mismatch_count(), 0u);
}

TEST(Scoreboard, MismatchDetected) {
  axi::Scoreboard sb;
  std::vector<uint32_t> strb1(1, 0xFFFF'FFFFu);
  std::vector<uint8_t> wdata(axi::DATA_BYTES, 0x00u);
  wdata[0] = 0xAB; wdata[1] = 0xCD; wdata[2] = 0xEF; wdata[3] = 0x12;
  sb.handle_write_completed(
      axi::WriteResult{0x200, /*size*/5, /*len*/0, axi::Burst::INCR,
                       axi::LockType::Normal,
                       wdata, strb1, axi::Resp::OKAY, 1, 1},
      wdata,
      strb1);
  sb.handle_read_observed(
      axi::ReadResult{0x200, /*size*/5, /*len*/0, axi::Burst::INCR,
                      {0xAB, 0xCD, 0xEE, 0x12}, axi::Resp::OKAY, 1, 2});
  EXPECT_EQ(sb.mismatch_count(), 1u);
  EXPECT_FALSE(sb.mismatch_report().empty());
}

TEST(Scoreboard, MatchPassesSilent) {
  axi::Scoreboard sb;
  std::vector<uint32_t> strb1(1, 0xFFFF'FFFFu);
  std::vector<uint8_t> wdata(axi::DATA_BYTES, 0x00u);
  wdata[0] = 0xDE; wdata[1] = 0xAD; wdata[2] = 0xBE; wdata[3] = 0xEF;
  sb.handle_write_completed(
      axi::WriteResult{0x300, /*size*/5, /*len*/0, axi::Burst::INCR,
                       axi::LockType::Normal,
                       wdata, strb1, axi::Resp::OKAY, 1, 1},
      wdata,
      strb1);
  sb.handle_read_observed(
      axi::ReadResult{0x300, /*size*/5, /*len*/0, axi::Burst::INCR,
                      {0xDE, 0xAD, 0xBE, 0xEF}, axi::Resp::OKAY, 1, 2});
  EXPECT_EQ(sb.mismatch_count(), 0u);
  EXPECT_EQ(sb.reads_checked(), 1u);
}

TEST(Scoreboard, ReadFromUnwrittenAddrReturnsFillDefault) {
  axi::Scoreboard sb;
  sb.handle_read_observed(
      axi::ReadResult{0x400, /*size*/5, /*len*/0, axi::Burst::INCR,
                      {0x00, 0x00, 0x00, 0x00}, axi::Resp::OKAY, 1, 1});
  EXPECT_EQ(sb.mismatch_count(), 0u);
}

TEST(Scoreboard, SparseWstrbByteMerge) {
  // 1-beat write with strb=0x0F: only byte lanes 0-3 land in expected_;
  // the remaining bytes stay at the default-fill (0x00). A subsequent read
  // observing 0xAA in lanes 0-3 and 0x00 elsewhere must produce zero mismatches.
  // Under lane-positioned bus, addr 0x100 has byte_lane=0, so strb bits 0..3
  // map to memory addrs 0x100..0x103.
  axi::Scoreboard sb;
  std::vector<uint8_t> data(axi::DATA_BYTES, 0xAAu);
  std::vector<uint32_t> strb{0x0000000Fu};
  axi::WriteResult wr{0x100, /*size*/5, /*len*/0, axi::Burst::INCR,
                      axi::LockType::Normal,
                      data, strb, axi::Resp::OKAY, 1, 1};
  sb.handle_write_completed(wr, data, strb);

  std::vector<uint8_t> read_data(axi::DATA_BYTES, 0x00u);
  for (int i = 0; i < 4; ++i) read_data[i] = 0xAAu;
  axi::ReadResult rr{0x100, /*size*/5, /*len*/0, axi::Burst::INCR,
                     read_data, axi::Resp::OKAY, 1, 2};
  sb.handle_read_observed(rr);
  EXPECT_EQ(sb.mismatch_count(), 0u);
}
