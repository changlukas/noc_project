#include "nmu/addr_trans.hpp"
#include "axi/types.hpp"
#include "common/scenario.hpp"
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

TEST(SamTable, PackedTranslateForwardsTheAddressUnchanged) {
    SCENARIO(
        "SamTable: translate() names the destination and leaves the address alone. The tile-local "
        "rebase it used to apply was removed so a tile's own initiator and its NI decode in one "
        "address domain; upstream does the same (floo_id_translation returns a node id only)");
    auto sam = SamTable::packed({
        {0, 0, 0x100000000ull},
        {1, 0, 0x100000000ull},
    });
    auto t = sam.translate(0x100000040ull);  // tile 1, offset 0x40
    EXPECT_EQ(t.dst_id, 0x01u);
    EXPECT_EQ(t.local_addr, 0x100000040ull);  // the request address, untouched
}

TEST(SamTable, TranslateIsInjectiveAcrossSpacesOfOneNode) {
    SCENARIO(
        "SamTable: a node's config and memory addresses stay distinct after translate(), which is "
        "what the tile decoder needs to tell the two apart. Under the old rebase both landed at "
        "their own space's slot; now they keep their own bases");
    auto sam = SamTable::packed({
        {0, 0, 0x100000, axi::AxiClass::Data},
        {1, 0, 0x100000, axi::AxiClass::Data},
        {0, 0, 0x1000, axi::AxiClass::Narrow},
        {1, 0, 0x1000, axi::AxiClass::Narrow},
    });
    const auto memory = sam.translate(0x40);      // node 0, memory space
    const auto config = sam.translate(0x200040);  // node 0, config space
    EXPECT_EQ(memory.dst_id, config.dst_id);      // same node
    EXPECT_NE(memory.cls, config.cls);            // different class
    EXPECT_NE(memory.local_addr, config.local_addr);
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

TEST(SamValidator, RejectsBasePlusSizeOverflow) {
    // Non-zero and 4 KB aligned, so the two earlier field checks pass and this
    // is the one that fires. base+size wraps to 0, which the overlap pass and
    // lookup() would both read as an empty range instead of a top-of-space tile.
    SamTable bad(std::vector<SamEntry>{{0xFFFFFFFFFFFFF000ull, 0x1000, 0x00}});
    EXPECT_DEATH(bad.validate(2, 2), "overflow");
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

TEST(SamValidator, RejectsPartialConfigCoverage) {
    // Spec §5.1: every node owns one region per address space. Memory covers the
    // 2x2 mesh; config reaches two nodes. A memory-only table stays legal -- it
    // is what SamTable::uniform() builds for most fixtures -- so the check is
    // gated on the config space being present, not on it being absent.
    SamTable bad(std::vector<SamEntry>{
        {0x0000, 0x1000, 0x00},
        {0x1000, 0x1000, 0x01},
        {0x2000, 0x1000, 0x10},
        {0x3000, 0x1000, 0x11},
        {0x4000, 0x1000, 0x00, axi::AxiClass::Narrow},
        {0x5000, 0x1000, 0x01, axi::AxiClass::Narrow},
    });
    EXPECT_DEATH(bad.validate(2, 2), "config space");
}

TEST(SamValidator, RejectsDuplicateNode) {
    // 2x2 mesh: dst 0x00 listed twice, (1,1) missing.
    SamTable bad(std::vector<SamEntry>{{0x0, 0x1000, 0x00},
                                       {0x1000, 0x1000, 0x00},
                                       {0x2000, 0x1000, 0x01},
                                       {0x3000, 0x1000, 0x10}});
    EXPECT_DEATH(bad.validate(2, 2), "duplicate");
}

// === Collective coordinate declaration (B1) ===
//
// SamTable does not derive the ranges -- construction has no mesh dimensions --
// so it checks what the builder states against the entries it holds. A
// declaration that does not fit leaves the space not collective-eligible; it is
// not an abort, because such a space is still a legal unicast target
// (docs/noc-target-spec.md §5.1).

using ni::cmodel::nmu::addr_trans::SpaceCoords;

TEST(SamCoords, DeclarationMatchingTheEntriesIsEligible) {
    auto sam = SamTable::uniform(2, 2, 0x1000);  // bases 0, 0x1000, 0x2000, 0x3000
    EXPECT_TRUE(sam.declare_space_coords(axi::AxiClass::Data,
                                         SpaceCoords{/*x=*/{12, 1}, /*y=*/{13, 1}, 2, 2}));
    const auto* c = sam.collective_coords(axi::AxiClass::Data);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->x_range.offset, 12u);
    EXPECT_EQ(c->y_range.offset, 13u);
    EXPECT_EQ(c->x_count, 2u);
    // The space that was never declared stays ineligible.
    EXPECT_EQ(sam.collective_coords(axi::AxiClass::Narrow), nullptr);
}

TEST(SamCoords, NonUniformStrideIsNotEligible) {
    // Node (1,0) sits at 0x3000, so the declared X range at bit 12 names 0x1000
    // and reaches no entry: the entries contradict the declaration.
    SamTable sam(std::vector<SamEntry>{{0x0000, 0x1000, 0x00},
                                       {0x3000, 0x1000, 0x01},
                                       {0x4000, 0x1000, 0x10},
                                       {0x5000, 0x1000, 0x11}});
    EXPECT_FALSE(
        sam.declare_space_coords(axi::AxiClass::Data, SpaceCoords{{12, 1}, {13, 1}, 2, 2}));
    EXPECT_EQ(sam.collective_coords(axi::AxiClass::Data), nullptr);
}

TEST(SamCoords, NonUniformApertureIsNotEligible) {
    // Uniform stride, non-uniform aperture: the two are different claims, and
    // the per-request burst-footprint check depends on the aperture claim.
    SamTable sam(std::vector<SamEntry>{{0x0000, 0x1000, 0x00},
                                       {0x1000, 0x800, 0x01},
                                       {0x2000, 0x1000, 0x10},
                                       {0x3000, 0x1000, 0x11}});
    EXPECT_FALSE(
        sam.declare_space_coords(axi::AxiClass::Data, SpaceCoords{{12, 1}, {13, 1}, 2, 2}));
}

TEST(SamCoords, NonRasterOrderIsNotEligible) {
    // Uniform stride and aperture, but (1,0) and (0,1) are swapped, so the
    // sliced coordinates would name the wrong node.
    SamTable sam(std::vector<SamEntry>{{0x0000, 0x1000, 0x00},
                                       {0x1000, 0x1000, 0x10},
                                       {0x2000, 0x1000, 0x01},
                                       {0x3000, 0x1000, 0x11}});
    EXPECT_FALSE(
        sam.declare_space_coords(axi::AxiClass::Data, SpaceCoords{{12, 1}, {13, 1}, 2, 2}));
}

TEST(SamCoords, DimensionsAreStatedNotInferred) {
    // A 3-wide dimension needs 2 bits but only 3 of the 4 values are nodes.
    // 1 << len would over-permit the fourth; x_count records the real bound.
    auto sam = SamTable::packed({{0, 0, 0x1000}, {1, 0, 0x1000}, {2, 0, 0x1000}});
    ASSERT_TRUE(sam.declare_space_coords(axi::AxiClass::Data, SpaceCoords{{12, 2}, {14, 0}, 3, 1}));
    EXPECT_EQ(sam.collective_coords(axi::AxiClass::Data)->x_count, 3u);
    // Claiming the full 4 does not fit: node (3,0) has no entry.
    EXPECT_FALSE(
        sam.declare_space_coords(axi::AxiClass::Data, SpaceCoords{{12, 2}, {14, 0}, 4, 1}));
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
