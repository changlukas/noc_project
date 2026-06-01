#include "axi/types.hpp"
#include "axi/memory_port.hpp"
#include <gtest/gtest.h>

namespace axi = ni::cmodel::axi;

TEST(AxiScaffold, ConstantsFromCodegen) {
  EXPECT_EQ(axi::DATA_BYTES, ni::WSTRB_WIDTH);
  EXPECT_EQ(axi::DATA_WIDTH, ni::width::NOC_DATA_WIDTH);
  EXPECT_EQ(axi::DATA_BYTES * 8, axi::DATA_WIDTH);
}

TEST(AxiScaffold, BurstEnumValues) {
  EXPECT_EQ(static_cast<int>(axi::Burst::FIXED), 0);
  EXPECT_EQ(static_cast<int>(axi::Burst::INCR),  1);
  EXPECT_EQ(static_cast<int>(axi::Burst::WRAP),  2);
}

TEST(AxiScaffold, RespEnumValues) {
  EXPECT_EQ(static_cast<int>(axi::Resp::OKAY),   0);
  EXPECT_EQ(static_cast<int>(axi::Resp::EXOKAY), 1);
  EXPECT_EQ(static_cast<int>(axi::Resp::SLVERR), 2);
  EXPECT_EQ(static_cast<int>(axi::Resp::DECERR), 3);
}

TEST(AxiScaffold, BeatStructsAreConstructible) {
  axi::AwBeat aw{};
  axi::WBeat  w{};
  axi::ArBeat ar{};
  axi::BBeat  b{};
  axi::RBeat  r{};
  (void)aw; (void)w; (void)ar; (void)b; (void)r;
  SUCCEED();
}

TEST(AxiScaffold, WBeatDataArrayMatchesDataBytes) {
  axi::WBeat w{};
  EXPECT_EQ(w.data.size(), static_cast<std::size_t>(axi::DATA_BYTES));
}

TEST(AxiScaffold, MemoryPortStructsAreConstructible) {
  axi::MemWriteReq  wr{};
  axi::MemWriteResp wresp{};
  axi::MemReadReq   rr{};
  axi::MemReadResp  rresp{};
  (void)wr; (void)wresp; (void)rr; (void)rresp;
  SUCCEED();
}
