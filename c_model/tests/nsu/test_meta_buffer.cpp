#include "nsu/meta_buffer.hpp"
#include <gtest/gtest.h>

using ni::cmodel::nsu::MetaBuffer;
using ni::cmodel::nsu::MetaEntry;

TEST(MetaBuffer, WriteSnapshotPeekCommit) {
  MetaBuffer mb(/*per_id_depth=*/4);
  mb.snapshot_write(0x05, {0x10, 1, 7});
  auto e = mb.peek_write(0x05);
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->src_id,  0x10);
  EXPECT_EQ(e->rob_req, 1);
  EXPECT_EQ(e->rob_idx, 7);

  // peek without commit — entry stays
  auto e2 = mb.peek_write(0x05);
  ASSERT_TRUE(e2.has_value());
  EXPECT_EQ(e2->src_id, 0x10);

  mb.commit_write(0x05);
  EXPECT_FALSE(mb.peek_write(0x05).has_value());
}

TEST(MetaBuffer, MultiOutstandingSameIdFifoOrder) {
  MetaBuffer mb(4);
  mb.snapshot_write(0x05, {0x10, 0, 1});
  mb.snapshot_write(0x05, {0x10, 0, 2});
  mb.snapshot_write(0x05, {0x10, 0, 3});
  EXPECT_EQ(mb.peek_write(0x05)->rob_idx, 1);  mb.commit_write(0x05);
  EXPECT_EQ(mb.peek_write(0x05)->rob_idx, 2);  mb.commit_write(0x05);
  EXPECT_EQ(mb.peek_write(0x05)->rob_idx, 3);  mb.commit_write(0x05);
  EXPECT_FALSE(mb.peek_write(0x05).has_value());
}

TEST(MetaBuffer, DifferentIdsIndependent) {
  MetaBuffer mb(4);
  mb.snapshot_write(0x05, {0x10, 0, 0});
  mb.snapshot_write(0x07, {0x20, 0, 0});
  EXPECT_EQ(mb.peek_write(0x07)->src_id, 0x20);
  EXPECT_EQ(mb.peek_write(0x05)->src_id, 0x10);  // not affected by 0x07 ops
}

TEST(MetaBuffer, ReadPeekCommitMultiBeat) {
  MetaBuffer mb(4);
  mb.snapshot_read(0x03, {0x10, 0, 5});
  // R burst: peek twice for r0/r1, commit only on r1 (last)
  EXPECT_EQ(mb.peek_read(0x03)->rob_idx, 5);
  EXPECT_EQ(mb.peek_read(0x03)->rob_idx, 5);  // still there
  mb.commit_read(0x03);
  EXPECT_FALSE(mb.peek_read(0x03).has_value());
}

TEST(MetaBuffer, PeekEmptyReturnsNullopt) {
  MetaBuffer mb(4);
  EXPECT_FALSE(mb.peek_write(0xAA).has_value());
  EXPECT_FALSE(mb.peek_read(0xBB).has_value());
}

TEST(MetaBuffer, SnapshotOverDepthAsserts) {
  // Document depth limit behavior — depth=2, insert 3 should assert (debug build)
  MetaBuffer mb(/*per_id_depth=*/2);
  mb.snapshot_write(0x01, {0, 0, 0});
  mb.snapshot_write(0x01, {0, 0, 0});
  EXPECT_DEATH(mb.snapshot_write(0x01, {0, 0, 0}),
               "MetaBuffer: per-ID depth exceeded");
}
