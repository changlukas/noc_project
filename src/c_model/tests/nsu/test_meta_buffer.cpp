#include "nsu/meta_buffer.hpp"
#include "common/scenario.hpp"
#include <gtest/gtest.h>

using ni::cmodel::nsu::MetaBuffer;
using ni::cmodel::nsu::MetaEntry;

TEST(MetaBuffer, WriteSnapshotPeekCommit) {
    SCENARIO("MetaBuffer: allocate_write -> peek_write returns entry; commit_write erases it");
    MetaBuffer mb(/*max_outstanding=*/4);
    mb.allocate_write(0x05, {0x10, 0x05, 1, 7});
    auto e = mb.peek_write(0x05);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->src_id, 0x10);
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
    SCENARIO("MetaBuffer: 3 same-id writes returned by peek+commit in FIFO order (rob_idx 1,2,3)");
    MetaBuffer mb(4);
    mb.allocate_write(0x05, {0x10, 0x05, 0, 1});
    mb.allocate_write(0x05, {0x10, 0x05, 0, 2});
    mb.allocate_write(0x05, {0x10, 0x05, 0, 3});
    EXPECT_EQ(mb.peek_write(0x05)->rob_idx, 1);
    mb.commit_write(0x05);
    EXPECT_EQ(mb.peek_write(0x05)->rob_idx, 2);
    mb.commit_write(0x05);
    EXPECT_EQ(mb.peek_write(0x05)->rob_idx, 3);
    mb.commit_write(0x05);
    EXPECT_FALSE(mb.peek_write(0x05).has_value());
}

TEST(MetaBuffer, DifferentIdsIndependent) {
    SCENARIO("MetaBuffer: id=0x05 ops do not touch id=0x07 state (per-id deques are independent)");
    MetaBuffer mb(4);
    mb.allocate_write(0x05, {0x10, 0x05, 0, 0});
    mb.allocate_write(0x07, {0x20, 0x07, 0, 0});
    EXPECT_EQ(mb.peek_write(0x07)->src_id, 0x20);
    EXPECT_EQ(mb.peek_write(0x05)->src_id, 0x10);  // not affected by 0x07 ops
}

TEST(MetaBuffer, ReadPeekCommitMultiBeat) {
    SCENARIO("MetaBuffer: read entry survives repeated peek_read; commit_read on rlast erases");
    MetaBuffer mb(4);
    mb.allocate_read(0x03, {0x10, 0x03, 0, 5});
    // R burst: peek twice for r0/r1, commit only on r1 (last)
    EXPECT_EQ(mb.peek_read(0x03)->rob_idx, 5);
    EXPECT_EQ(mb.peek_read(0x03)->rob_idx, 5);  // still there
    mb.commit_read(0x03);
    EXPECT_FALSE(mb.peek_read(0x03).has_value());
}

TEST(MetaBuffer, PeekEmptyReturnsNullopt) {
    SCENARIO("MetaBuffer: peek_write/peek_read on unknown id returns nullopt (no spurious entry)");
    MetaBuffer mb(4);
    EXPECT_FALSE(mb.peek_write(0xAA).has_value());
    EXPECT_FALSE(mb.peek_read(0xBB).has_value());
}

using ni::cmodel::nsu::remap_downstream_id;

TEST(RemapDownstreamId, CollapsesToAllOnesWhenSingleUniqueId) {
    SCENARIO("remap_downstream_id: max_unique_ids=1 maps every upstream id to 0xFF");
    EXPECT_EQ(remap_downstream_id(0x00, 1), 0xFF);
    EXPECT_EQ(remap_downstream_id(0x05, 1), 0xFF);
    EXPECT_EQ(remap_downstream_id(0xFF, 1), 0xFF);
}

TEST(RemapDownstreamId, IdentityWhenFullIdSpace) {
    SCENARIO("remap_downstream_id: max_unique_ids=AXI_ID_SPACE passes the id through");
    EXPECT_EQ(remap_downstream_id(0x00, ni::cmodel::axi::AXI_ID_SPACE), 0x00);
    EXPECT_EQ(remap_downstream_id(0x05, ni::cmodel::axi::AXI_ID_SPACE), 0x05);
    EXPECT_EQ(remap_downstream_id(0xFF, ni::cmodel::axi::AXI_ID_SPACE), 0xFF);
}

TEST(MetaBuffer, SharedPoolFullReportsInsteadOfAborting) {
    SCENARIO(
        "MetaBuffer: the write pool is shared across ids. 16 sources on one collapsed "
        "downstream id fill it to max_outstanding, write_full() reports, read pool is "
        "unaffected, and a commit frees one slot.");
    constexpr std::size_t kMaxOutstanding = 4;
    MetaBuffer mb(kMaxOutstanding);
    const uint8_t down = 0xFF;  // remap_downstream_id(any, 1)

    for (uint8_t src = 0; src < kMaxOutstanding; ++src) {
        EXPECT_FALSE(mb.write_full());
        mb.allocate_write(down, {src, /*upstream_id=*/0x05, 0, src});
    }
    EXPECT_TRUE(mb.write_full());

    // The read pool is a separate pool.
    EXPECT_FALSE(mb.read_full());

    // FIFO order survives: the first source out is the first source in.
    auto e = mb.peek_write(down);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->src_id, 0);
    EXPECT_EQ(e->upstream_id, 0x05);

    mb.commit_write(down);
    EXPECT_FALSE(mb.write_full());
    EXPECT_EQ(mb.peek_write(down)->src_id, 1);
}
