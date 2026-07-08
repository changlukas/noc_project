#include "nmu/addr_trans.hpp"
#include <gtest/gtest.h>

using ni::cmodel::nmu::addr_trans::SamEntry;
using ni::cmodel::nmu::addr_trans::SamTable;

// tile_size 4 GB reproduces the legacy coord_id<<32 layout. X_WIDTH=4 -> dst (2,1)=0x12.
TEST(SamTable, UniformRebase_DstFromTableLocalRebased) {
    auto sam =
        SamTable::uniform(/*x_dim=*/4, /*y_dim=*/4, /*tile_size=*/0x100000000ull, /*rebase=*/true);
    auto t = sam.translate(0x1200000040ull);  // tile (2,1) coord_id 0x12, offset 0x40
    EXPECT_EQ(t.dst_id, 0x12u);
    EXPECT_EQ(t.local_addr, 0x40ull);  // rebased: addr - base
}

TEST(SamTable, UniformNoRebase_LocalIsGlobal) {
    auto sam = SamTable::uniform(4, 4, 0x100000000ull, /*rebase=*/false);
    auto t = sam.translate(0x1200000040ull);
    EXPECT_EQ(t.dst_id, 0x12u);
    EXPECT_EQ(t.local_addr, 0x1200000040ull);  // remove_offset = 0
}

TEST(SamTable, LookupMissReturnsNull) {
    auto sam = SamTable::uniform(2, 2, 0x100000000ull, true);  // tiles 0,1,16,17 (coord_id)
    EXPECT_EQ(sam.lookup(0x0300000000ull),
              nullptr);  // coord_id 3 = (0,0..)+? x=3>=x_dim -> unmapped
}

#include <vector>
using ni::cmodel::nmu::addr_trans::SamEntry;

TEST(SamValidator, RejectsOverlap) {
    SamTable bad(std::vector<SamEntry>{
        {0x0, 0x2000, 0x00, 0x0}, {0x1000, 0x2000, 0x01, 0x1000},  // overlaps first
    });
    EXPECT_DEATH(bad.validate(4, 4), "overlap");
}

TEST(SamValidator, RejectsNon4KBSize) {
    SamTable bad(std::vector<SamEntry>{{0x0, 0x1800, 0x00, 0x0}});  // 6 KB, not a 4 KB multiple
    EXPECT_DEATH(bad.validate(4, 4), "4 ?KB|aligned");
}

TEST(SamValidator, RejectsRemoveOffsetGtBase) {
    SamTable bad(std::vector<SamEntry>{{0x1000, 0x1000, 0x00, 0x2000}});  // remove_offset > base
    EXPECT_DEATH(bad.validate(4, 4), "remove_offset");
}

TEST(SamValidator, RejectsDstOutsideMesh) {
    SamTable bad(std::vector<SamEntry>{{0x0, 0x1000, 0x33, 0x0}});  // x=3,y=3 outside 2x2
    EXPECT_DEATH(bad.validate(2, 2), "mesh");
}

TEST(SamFootprint, RejectsBurstCrossingTile) {
    auto sam = SamTable::uniform(4, 4, 0x100000000ull, true);
    // burst inside tile 0x12: [base+0x40, base+0x80] ok
    EXPECT_TRUE(sam.burst_footprint_ok(0x1200000040ull, 0x1200000080ull));
    // burst spilling past tile end into the next tile: not ok
    EXPECT_FALSE(sam.burst_footprint_ok(0x12FFFFFFF0ull, 0x1300000010ull));
}
