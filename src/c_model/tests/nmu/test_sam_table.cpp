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
