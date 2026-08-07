#include "nmu/addr_trans.hpp"
#include "axi/types.hpp"
#include <gtest/gtest.h>
#include <vector>

using ni::cmodel::nmu::addr_trans::SamEntry;
using ni::cmodel::nmu::addr_trans::SamTable;
namespace axi = ni::cmodel::axi;

TEST(SamTable, PackedAccumulatesBasesGapFree) {
    // base(0)=0, base(1)=size(0), base(2)=size(0)+size(1) -- heterogeneous sizes.
    auto sam = SamTable::packed({
        {0, 0, 0x1000},
        {1, 0, 0x2000},
        {2, 0, 0x1000},
    });
    ASSERT_EQ(sam.entries().size(), 3u);
    EXPECT_EQ(sam.entries()[0].base, 0x0ull);
    EXPECT_EQ(sam.entries()[1].base, 0x1000ull);
    EXPECT_EQ(sam.entries()[2].base, 0x3000ull);
}

TEST(SamTable, PackedDstIdFromXY) {
    auto sam = SamTable::packed({{2, 1, 0x1000}});  // dst = (1<<X_WIDTH)|2 = 0x12
    EXPECT_EQ(sam.entries()[0].dst_id, 0x12u);
}

TEST(SamTable, PackedTranslateRebasesFromAccumulatedBase) {
    auto sam = SamTable::packed({
        {0, 0, 0x100000000ull},
        {1, 0, 0x100000000ull},
    });
    auto t = sam.translate(0x100000040ull);  // tile 1, offset 0x40
    EXPECT_EQ(t.dst_id, 0x01u);
    EXPECT_EQ(t.local_addr, 0x40ull);  // rebased: addr - base
}

// Tile-local layering: the memory space's slot is DERIVED from the largest
// memory entry, not fixed. Same numbers as the Python twin's mixed-size case
// (test_address_map_tile_layout_derives_span_from_entries in
// sim/tools/test_gen_test_patterns_filemaster.py) -- if the two rules diverge,
// one of the pair fails.
TEST(SamTable, SpaceBaseDerivedFromLargestEntryOfThatSpace) {
    auto sam = SamTable::packed({
        {0, 0, 0x200000, axi::AxiClass::Data},  // largest memory tile
        {1, 0, 0x100000, axi::AxiClass::Data},
        {0, 1, 0x100000, axi::AxiClass::Data},
        {1, 1, 0x100000, axi::AxiClass::Data},
        {0, 0, 0x1000, axi::AxiClass::Narrow},
        {1, 0, 0x1000, axi::AxiClass::Narrow},
        {0, 1, 0x1000, axi::AxiClass::Narrow},
        {1, 1, 0x1000, axi::AxiClass::Narrow},
    });
    // config span 0x1000 at 0x0; memory span 0x200000 aligned up from 0x1000.
    EXPECT_EQ(sam.entries()[0].space_base, 0x200000ull);
    EXPECT_EQ(sam.entries()[4].space_base, 0x0ull);
    // A smaller memory tile shares the space's slot -- the span is per space,
    // not per entry, so multicast replicas keep the same term (design §1.3).
    EXPECT_EQ(sam.entries()[1].space_base, 0x200000ull);
}

TEST(SamTable, SpaceBaseIsZeroWhenOnlyOneSpacePresent) {
    auto sam = SamTable::packed({{0, 0, 0x100000}, {1, 0, 0x100000}});
    EXPECT_EQ(sam.entries()[0].space_base, 0x0ull);  // memory-only tile: no config slot to skip
}

TEST(SamTable, LookupMissReturnsNull) {
    auto sam = SamTable::packed({{0, 0, 0x1000}});
    EXPECT_EQ(sam.lookup(0x2000ull), nullptr);
}

TEST(SamValidator, RejectsOverlap) {
    // 2-node mesh (2x1) so entry count matches x_dim*y_dim and the overlap
    // pass (which runs after exactly-once coverage) is actually reached.
    SamTable bad(std::vector<SamEntry>{
        {0x0, 0x2000, 0x00}, {0x1000, 0x2000, 0x01},  // overlaps first
    });
    EXPECT_DEATH(bad.validate(2, 1), "overlap");
}

TEST(SamValidator, RejectsNon4KBSize) {
    SamTable bad(std::vector<SamEntry>{{0x0, 0x1800, 0x00}});  // 6 KB, not a 4 KB multiple
    // "|" alternation is unsupported by gtest's simple regex engine (used
    // when GTEST_USES_POSIX_RE=0, e.g. MSVC/MinGW) -- keep this a plain literal.
    EXPECT_DEATH(bad.validate(4, 4), "aligned");
}

TEST(SamValidator, RejectsZeroSize) {
    SamTable bad(std::vector<SamEntry>{{0x0, 0x0, 0x00}});
    EXPECT_DEATH(bad.validate(4, 4), "zero-size");
}

TEST(SamValidator, RejectsDstOutsideMesh) {
    SamTable bad(std::vector<SamEntry>{{0x0, 0x1000, 0x33}});  // x=3,y=3 outside 2x2
    EXPECT_DEATH(bad.validate(2, 2), "mesh");
}

TEST(SamValidator, RejectsMissingNode) {
    // 2x2 mesh needs 4 nodes; only 3 given -- (1,1) missing.
    SamTable bad(
        std::vector<SamEntry>{{0x0, 0x1000, 0x00}, {0x1000, 0x1000, 0x01}, {0x2000, 0x1000, 0x10}});
    EXPECT_DEATH(bad.validate(2, 2), "exactly once");
}

TEST(SamValidator, RejectsDuplicateNode) {
    // 2x2 mesh: dst 0x00 listed twice, (1,1) missing.
    SamTable bad(std::vector<SamEntry>{{0x0, 0x1000, 0x00},
                                       {0x1000, 0x1000, 0x00},
                                       {0x2000, 0x1000, 0x01},
                                       {0x3000, 0x1000, 0x10}});
    EXPECT_DEATH(bad.validate(2, 2), "duplicate");
}

TEST(SamFootprint, RejectsBurstCrossingTile) {
    auto sam = SamTable::packed({{0, 0, 0x100000000ull}, {1, 0, 0x100000000ull}});
    // burst inside tile 0: [0x40, 0x80] ok
    EXPECT_TRUE(sam.burst_footprint_ok(0x40, 0x80));
    // burst spilling past tile 0 end into tile 1: not ok
    EXPECT_FALSE(sam.burst_footprint_ok(0xFFFFFFF0, 0x100000010ull));
}

using ni::cmodel::nmu::addr_trans::burst_last_byte;

TEST(BurstLastByte, IncrSpansTotal) {
    // len=3, size=3 (8 B/beat) -> 32 B span
    EXPECT_EQ(burst_last_byte(0x1000, 3, 3, axi::Burst::INCR), 0x1000 + 32 - 1);
}
TEST(BurstLastByte, WrapWindowAligned) {
    // len=3,size=3 -> total 32; addr 0x1018 -> window [0x1000,0x1020)
    EXPECT_EQ(burst_last_byte(0x1018, 3, 3, axi::Burst::WRAP), 0x1020 - 1);
}
TEST(BurstLastByte, FixedMatchesSlaveTotal) {
    // FIXED uses total to match the slave OOB math
    EXPECT_EQ(burst_last_byte(0x1000, 3, 3, axi::Burst::FIXED), 0x1000 + 32 - 1);
}
