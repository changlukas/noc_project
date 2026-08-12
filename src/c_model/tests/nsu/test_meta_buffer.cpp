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
    EXPECT_EQ(e->ordering_req, 1);
    EXPECT_EQ(e->ordering_tag, 7);

    // peek without commit — entry stays
    auto e2 = mb.peek_write(0x05);
    ASSERT_TRUE(e2.has_value());
    EXPECT_EQ(e2->src_id, 0x10);

    mb.commit_write(0x05);
    EXPECT_FALSE(mb.peek_write(0x05).has_value());
}

TEST(MetaBuffer, MultiOutstandingSameIdFifoOrder) {
    SCENARIO(
        "MetaBuffer: 3 same-id writes returned by peek+commit in FIFO order (ordering_tag 1,2,3)");
    MetaBuffer mb(4);
    mb.allocate_write(0x05, {0x10, 0x05, 0, 1});
    mb.allocate_write(0x05, {0x10, 0x05, 0, 2});
    mb.allocate_write(0x05, {0x10, 0x05, 0, 3});
    EXPECT_EQ(mb.peek_write(0x05)->ordering_tag, 1);
    mb.commit_write(0x05);
    EXPECT_EQ(mb.peek_write(0x05)->ordering_tag, 2);
    mb.commit_write(0x05);
    EXPECT_EQ(mb.peek_write(0x05)->ordering_tag, 3);
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
    EXPECT_EQ(mb.peek_read(0x03)->ordering_tag, 5);
    EXPECT_EQ(mb.peek_read(0x03)->ordering_tag, 5);  // still there
    mb.commit_read(0x03);
    EXPECT_FALSE(mb.peek_read(0x03).has_value());
}

TEST(MetaBuffer, PeekEmptyReturnsNullopt) {
    SCENARIO("MetaBuffer: peek_write/peek_read on unknown id returns nullopt (no spurious entry)");
    MetaBuffer mb(4);
    EXPECT_FALSE(mb.peek_write(0x06).has_value());
    EXPECT_FALSE(mb.peek_read(0x07).has_value());
}

using ni::cmodel::nsu::remap_downstream_id;

TEST(RemapDownstreamId, CollapsesToAllOnesOfTheDrivenIdWidth) {
    SCENARIO(
        "remap_downstream_id: max_unique_ids=1 maps every upstream id to all-ones of "
        "AXI_ID_WIDTH, the width the NSU drives -- matching floo_meta_buffer's '1 of "
        "its OUTPUT id width. The value must be addressable on the port it drives.");
    constexpr uint8_t collapsed = (1u << ni::AXI_ID_WIDTH) - 1u;
    EXPECT_EQ(collapsed, (1u << ni::AXI_ID_WIDTH) - 1u) << "all-ones of the driven width";
    EXPECT_LT(collapsed, ni::cmodel::axi::AXI_ID_SPACE) << "must fit the port it is driven onto";
    EXPECT_EQ(remap_downstream_id(0x00, 1), collapsed);
    EXPECT_EQ(remap_downstream_id(0x05, 1), collapsed);
    EXPECT_EQ(remap_downstream_id(collapsed, 1), collapsed);
}

TEST(RemapDownstreamId, IdentityWhenFullIdSpace) {
    SCENARIO("remap_downstream_id: max_unique_ids=AXI_ID_SPACE passes the id through");
    constexpr uint8_t kMaxId = ni::cmodel::axi::AXI_ID_SPACE - 1u;
    EXPECT_EQ(remap_downstream_id(0x00, ni::cmodel::axi::AXI_ID_SPACE), 0x00);
    EXPECT_EQ(remap_downstream_id(0x05, ni::cmodel::axi::AXI_ID_SPACE), 0x05);
    EXPECT_EQ(remap_downstream_id(kMaxId, ni::cmodel::axi::AXI_ID_SPACE), kMaxId);
}

TEST(MetaBuffer, SharedPoolFullReportsInsteadOfAborting) {
    SCENARIO(
        "MetaBuffer: the write pool is shared across ids, not bounded per-id. Allocations "
        "spread across 3 downstream ids (bucket sizes 2/1/1, none reaching max_outstanding) "
        "still trip write_full() on the global count; read pool is unaffected; FIFO order "
        "within a bucket and commit_write's freed slot both hold.");
    constexpr std::size_t kMaxOutstanding = 4;
    MetaBuffer mb(kMaxOutstanding);
    constexpr uint8_t kDownA = 0x01;  // bucket ends up with 2 entries
    constexpr uint8_t kDownB = 0x02;  // bucket ends up with 1 entry
    constexpr uint8_t kDownC = 0x03;  // bucket ends up with 1 entry

    EXPECT_FALSE(mb.write_full());
    mb.allocate_write(kDownA, {/*src_id=*/0, /*upstream_id=*/0x05, 0, 0});
    EXPECT_FALSE(mb.write_full());
    mb.allocate_write(kDownA, {/*src_id=*/1, /*upstream_id=*/0x05, 0, 1});
    EXPECT_FALSE(mb.write_full());
    mb.allocate_write(kDownB, {/*src_id=*/2, /*upstream_id=*/0x06, 0, 0});
    EXPECT_FALSE(mb.write_full());
    mb.allocate_write(kDownC, {/*src_id=*/3, /*upstream_id=*/0x07, 0, 0});

    // Global count is now 4 == max_outstanding, but every single bucket is well
    // under it (sizes 2, 1, 1) -- the only case where a shared pool and a
    // per-id bound disagree.
    EXPECT_TRUE(mb.write_full());

    // The read pool is a separate pool.
    EXPECT_FALSE(mb.read_full());

    // FIFO order survives within a bucket: the first source in is the first out.
    auto e = mb.peek_write(kDownA);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->src_id, 0);
    EXPECT_EQ(e->upstream_id, 0x05);

    mb.commit_write(kDownA);
    EXPECT_FALSE(mb.write_full());
    EXPECT_EQ(mb.peek_write(kDownA)->src_id, 1);
}
