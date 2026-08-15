#include "nmu/addr_trans.hpp"
#include "axi/types.hpp"
#include <gtest/gtest.h>
#include <vector>

using ni::cmodel::nmu::addr_trans::SamEntry;
using ni::cmodel::nmu::addr_trans::SamTable;
namespace axi = ni::cmodel::axi;

TEST(SamTable, PackedBasesFromCoordinateAndSlot) {
    // base(x, y) = ((y << x_bits) | x) * slot, slot = largest declared size in
    // the space -- not list-order accumulation. x_span = 3 -> x_bits = 2, slot
    // = max(0x1000, 0x2000, 0x1000) = 0x2000.
    auto sam = SamTable::packed(
        {
            {0, 0, 0x1000},
            {1, 0, 0x2000},
            {2, 0, 0x1000},
        },
        /*x_span=*/3, /*y_span=*/1, /*block_size=*/0x2000);
    ASSERT_EQ(sam.entries().size(), 3u);
    EXPECT_EQ(sam.entries()[0].base, 0x0ull);
    EXPECT_EQ(sam.entries()[1].base, 0x2000ull);
    EXPECT_EQ(sam.entries()[2].base, 0x4000ull);
}

TEST(SamTable, PackedDstIdFromXY) {
    // dst = (1<<X_WIDTH)|2 = 0x12, independent of x_span/y_span.
    auto sam =
        SamTable::packed({{2, 1, 0x1000}}, /*x_span=*/3, /*y_span=*/2, /*block_size=*/0x1000);
    EXPECT_EQ(sam.entries()[0].dst_id, 0x12u);
}

TEST(SamTable, PackedTranslateForwardsTheAddressUnchanged) {
    auto sam = SamTable::packed(
        {
            {0, 0, 0x100000000ull},
            {1, 0, 0x100000000ull},
        },
        /*x_span=*/2, /*y_span=*/1, /*block_size=*/0x100000000ull);
    auto t = sam.translate(0x100000040ull);  // tile 1, offset 0x40
    EXPECT_EQ(t.dst_id, 0x01u);
    EXPECT_EQ(t.local_addr, 0x100000040ull);  // the request address, untouched
}

TEST(SamTable, TranslateIsInjectiveAcrossSpacesOfOneNode) {
    auto sam = SamTable::packed(
        {
            {0, 0, 0x100000, axi::AxiClass::Data, axi::Space::Memory},
            {1, 0, 0x100000, axi::AxiClass::Data, axi::Space::Memory},
            {0, 0, 0x1000, axi::AxiClass::Narrow, axi::Space::Config},
            {1, 0, 0x1000, axi::AxiClass::Narrow, axi::Space::Config},
        },
        /*x_span=*/2, /*y_span=*/1, /*block_size=*/0x200000);
    const auto memory = sam.translate(0x40);  // node 0, memory space
    // node 0's config tile sits inside its own block, above its memory tile:
    // offset 0x100000 (memory_slot 0x100000, config_slot 0x1000, aligned).
    const auto config = sam.translate(0x100040);  // node 0, config space
    EXPECT_EQ(memory.dst_id, config.dst_id);      // same node
    EXPECT_NE(memory.cls, config.cls);            // different class
    EXPECT_NE(memory.local_addr, config.local_addr);
}

TEST(SamTable, LookupMissReturnsNull) {
    auto sam =
        SamTable::packed({{0, 0, 0x1000}}, /*x_span=*/1, /*y_span=*/1, /*block_size=*/0x1000);
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
        {0x4000, 0x1000, 0x00, axi::AxiClass::Narrow, /*port=*/0, axi::Space::Config},
        {0x5000, 0x1000, 0x01, axi::AxiClass::Narrow, /*port=*/0, axi::Space::Config},
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

// No peripheral in any fixture below, so the tile region is the full span --
// x_first/y_first = 0, x_last/y_last = count - 1.

TEST(SamCoords, DeclarationMatchingTheEntriesIsEligible) {
    auto sam = SamTable::uniform(2, 2, 0x1000);  // bases 0, 0x1000, 0x2000, 0x3000
    EXPECT_TRUE(sam.declare_space_coords(
        axi::Space::Memory, SpaceCoords{/*x=*/{12, 1}, /*y=*/{13, 1}, 2, 2, 0, 1, 0, 1}));
    const auto* c = sam.collective_coords(axi::Space::Memory);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->x_range.offset, 12u);
    EXPECT_EQ(c->y_range.offset, 13u);
    EXPECT_EQ(c->x_count, 2u);
    // The space that was never declared stays ineligible.
    EXPECT_EQ(sam.collective_coords(axi::Space::Config), nullptr);
}

TEST(SamCoords, NonUniformStrideIsNotEligible) {
    // Node (1,0) sits at 0x3000, so the declared X range at bit 12 names 0x1000
    // and reaches no entry: the entries contradict the declaration.
    SamTable sam(std::vector<SamEntry>{{0x0000, 0x1000, 0x00},
                                       {0x3000, 0x1000, 0x01},
                                       {0x4000, 0x1000, 0x10},
                                       {0x5000, 0x1000, 0x11}});
    EXPECT_FALSE(sam.declare_space_coords(axi::Space::Memory,
                                          SpaceCoords{{12, 1}, {13, 1}, 2, 2, 0, 1, 0, 1}));
    EXPECT_EQ(sam.collective_coords(axi::Space::Memory), nullptr);
}

TEST(SamCoords, NonUniformApertureIsNotEligible) {
    // Uniform stride, non-uniform aperture: the two are different claims, and
    // the per-request burst-footprint check depends on the aperture claim.
    SamTable sam(std::vector<SamEntry>{{0x0000, 0x1000, 0x00},
                                       {0x1000, 0x800, 0x01},
                                       {0x2000, 0x1000, 0x10},
                                       {0x3000, 0x1000, 0x11}});
    EXPECT_FALSE(sam.declare_space_coords(axi::Space::Memory,
                                          SpaceCoords{{12, 1}, {13, 1}, 2, 2, 0, 1, 0, 1}));
}

TEST(SamCoords, NonRasterOrderIsNotEligible) {
    // Uniform stride and aperture, but (1,0) and (0,1) are swapped, so the
    // sliced coordinates would name the wrong node.
    SamTable sam(std::vector<SamEntry>{{0x0000, 0x1000, 0x00},
                                       {0x1000, 0x1000, 0x10},
                                       {0x2000, 0x1000, 0x01},
                                       {0x3000, 0x1000, 0x11}});
    EXPECT_FALSE(sam.declare_space_coords(axi::Space::Memory,
                                          SpaceCoords{{12, 1}, {13, 1}, 2, 2, 0, 1, 0, 1}));
}

TEST(SamCoords, DimensionsAreStatedNotInferred) {
    // A 3-wide dimension needs 2 bits but only 3 of the 4 values are nodes.
    // 1 << len would over-permit the fourth; x_count records the real bound.
    auto sam = SamTable::packed({{0, 0, 0x1000}, {1, 0, 0x1000}, {2, 0, 0x1000}}, /*x_span=*/3,
                                /*y_span=*/1, /*block_size=*/0x1000);
    ASSERT_TRUE(sam.declare_space_coords(axi::Space::Memory,
                                         SpaceCoords{{12, 2}, {14, 0}, 3, 1, 0, 2, 0, 0}));
    EXPECT_EQ(sam.collective_coords(axi::Space::Memory)->x_count, 3u);
    // Claiming the full 4 does not fit: node (3,0) has no entry.
    EXPECT_FALSE(sam.declare_space_coords(axi::Space::Memory,
                                          SpaceCoords{{12, 2}, {14, 0}, 4, 1, 0, 3, 0, 0}));
}

TEST(SamTable, PeripheralEntryDoesNotJoinTheMemorySpacesTileWalk) {
    // Four memory tiles in a 2x2, plus one peripheral entry that also carries
    // the Data class. Keyed on class the walk counts five and rejects; keyed on
    // space it counts four and declares.
    std::vector<SamEntry> es;
    for (unsigned y = 0; y < 2; ++y) {
        for (unsigned x = 0; x < 2; ++x) {
            const uint64_t base = ((y << 1) | x) * 0x100000000ull;
            es.push_back({base, 0x2000000, static_cast<uint8_t>((y << 4) | x), axi::AxiClass::Data,
                          /*port=*/0, axi::Space::Memory});
        }
    }
    es.push_back({0x400000000ull, 0x1000, /*dst_id=*/0x00, axi::AxiClass::Data, /*port=*/1,
                  axi::Space::Peripheral});
    SamTable sam{std::move(es)};
    EXPECT_TRUE(sam.declare_space_coords(
        axi::Space::Memory, SpaceCoords{/*x=*/{32, 1}, /*y=*/{33, 1}, 2, 2, 0, 1, 0, 1}));
    EXPECT_NE(sam.collective_coords(axi::Space::Memory), nullptr);
    EXPECT_EQ(sam.collective_coords(axi::Space::Peripheral), nullptr);
}

TEST(SamFootprint, RejectsBurstCrossingTile) {
    auto sam = SamTable::packed({{0, 0, 0x100000000ull}, {1, 0, 0x100000000ull}}, /*x_span=*/2,
                                /*y_span=*/1, /*block_size=*/0x100000000ull);
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
