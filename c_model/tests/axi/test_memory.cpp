// Algorithms ported from cocotbext-axi (MIT) — see axi/ATTRIBUTION.md
#include "axi/memory.hpp"
#include <gtest/gtest.h>

namespace axi = ni::cmodel::axi;

TEST(Memory, InBoundsWriteImmediateResp_ZeroLatency) {
  axi::Memory mem(0x1000, 0x1000, 0, 0);
  axi::MemWriteReq req{};
  req.addr = 0x1000;
  req.data.fill(0xAB);
  req.strb = 0xFFFF'FFFFu;
  req.id   = 0x05;
  req.last = true;
  req.tag  = 42;
  EXPECT_TRUE(mem.submit_write(req));
  mem.tick();
  auto resp = mem.pop_write_resp();
  ASSERT_TRUE(resp.has_value());
  EXPECT_EQ(resp->id,   0x05);
  EXPECT_EQ(resp->resp, axi::Resp::OKAY);
  EXPECT_EQ(resp->tag,  42u);
}

TEST(Memory, WriteLatencyCountdown) {
  axi::Memory mem(0x1000, 0x1000, 5, 0);
  axi::MemWriteReq req{};
  req.addr = 0x1000; req.data.fill(0x55); req.strb = 0xFFFF'FFFFu;
  req.id = 1; req.last = true; req.tag = 100;
  EXPECT_TRUE(mem.submit_write(req));
  for (int t = 1; t <= 5; ++t) {
    mem.tick();
    auto r = mem.pop_write_resp();
    if (t < 5) {
      EXPECT_FALSE(r.has_value()) << "premature response at tick " << t;
    } else {
      ASSERT_TRUE(r.has_value());
      EXPECT_EQ(r->tag, 100u);
    }
  }
}

TEST(Memory, ReadLatencyCountdown) {
  axi::Memory mem(0x1000, 0x1000, 0, 3);
  axi::MemReadReq req{};
  req.addr = 0x1000; req.size = 5; req.id = 2; req.last = true; req.tag = 200;
  EXPECT_TRUE(mem.submit_read(req));
  EXPECT_FALSE((mem.tick(), mem.pop_read_resp()).has_value());
  EXPECT_FALSE((mem.tick(), mem.pop_read_resp()).has_value());
  auto r = (mem.tick(), mem.pop_read_resp());
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->tag, 200u);
}

// Lane-positioned bus: bounds check is per the aligned-down word (the actual
// 32-byte beat the bus carries). To trip DECERR, the request addr must lie in
// (or past) a word that is itself out of bounds — e.g., addr=0x1100 in a
// [0x1000, 0x1100) region.
TEST(Memory, OobWriteReturnsDecerr) {
  axi::Memory mem(0x1000, 0x100, 0, 0);
  axi::MemWriteReq req{};
  req.addr = 0x1100; req.data.fill(0xAA); req.strb = 0xFFFF'FFFFu;
  req.id = 1; req.last = true; req.tag = 300;
  EXPECT_TRUE(mem.submit_write(req));
  mem.tick();
  auto r = mem.pop_write_resp();
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->resp, axi::Resp::DECERR);
}

TEST(Memory, OobReadReturnsDecerr) {
  axi::Memory mem(0x1000, 0x100, 0, 0);
  axi::MemReadReq req{};
  req.addr = 0x1100; req.size = 5; req.id = 2; req.last = true; req.tag = 400;
  EXPECT_TRUE(mem.submit_read(req));
  mem.tick();
  auto r = mem.pop_read_resp();
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->resp, axi::Resp::DECERR);
}

TEST(Memory, OobWriteDoesNotMutateStorage) {
  // Lay down a sentinel byte inside the in-bounds last word (0x10E0..0x10FF),
  // then issue an OOB write at the next word boundary. No storage byte should
  // change (DECERR short-circuits the strb apply loop).
  axi::Memory mem(0x1000, 0x100, 0, 0, 32, 0xFF);
  axi::MemWriteReq req{};
  req.addr = 0x1100; req.data.fill(0x00); req.strb = 0xFFFF'FFFFu;
  req.id = 1; req.last = true;
  mem.submit_write(req); mem.tick(); (void)mem.pop_write_resp();
  EXPECT_EQ(mem.peek(0x10F8), 0xFF);
  EXPECT_EQ(mem.peek(0x10FE), 0xFF);
}

TEST(Memory, WstrbByteMaskMergeWithFill) {
  axi::Memory mem(0x1000, 0x100, 0, 0, 32, 0xFF);
  axi::MemWriteReq req{};
  req.addr = 0x1000;
  req.data.fill(0x00);
  req.strb = 0b1010;
  req.id = 1; req.last = true;
  mem.submit_write(req); mem.tick(); (void)mem.pop_write_resp();
  EXPECT_EQ(mem.peek(0x1000), 0xFF);
  EXPECT_EQ(mem.peek(0x1001), 0x00);
  EXPECT_EQ(mem.peek(0x1002), 0xFF);
  EXPECT_EQ(mem.peek(0x1003), 0x00);
  EXPECT_EQ(mem.peek(0x1004), 0xFF);
}

TEST(Memory, BackpressureSubmitReturnsFalseWhenQueueFull) {
  axi::Memory mem(0x1000, 0x10000, 10, 10, 4);
  axi::MemWriteReq req{};
  req.addr = 0x1000; req.data.fill(0); req.strb = 0xFFFF'FFFFu;
  req.id = 1; req.last = true;
  EXPECT_TRUE(mem.submit_write(req));
  EXPECT_TRUE(mem.submit_write(req));
  EXPECT_TRUE(mem.submit_write(req));
  EXPECT_TRUE(mem.submit_write(req));
  EXPECT_FALSE(mem.submit_write(req));
  for (int i = 0; i < 11; ++i) mem.tick();
  ASSERT_TRUE(mem.pop_write_resp().has_value());
  EXPECT_TRUE(mem.submit_write(req));
}
