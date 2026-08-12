#include "nmu/addr_trans.hpp"
#include "nmu/sam_yaml.hpp"
#include "axi/types.hpp"
#include "common/scenario.hpp"
#include <gtest/gtest.h>
#include <vector>

using ni::cmodel::nmu::addr_trans::SamTable;
namespace addr_trans = ni::cmodel::nmu::addr_trans;
namespace axi = ni::cmodel::axi;

namespace {
// 16x16 uniform, 4 GB/tile: dst_id = addr[39:32] (covers all 256 dst_ids, matching
// the retired xy_route decode); the address itself is forwarded unchanged.
SamTable sam() {
    return SamTable::uniform(16, 16, 0x100000000ull);
}

// AWUSER: [7:0] user, [9:8] collective_op, [57:10] collective_mask (address
// mask). Mirrors test_nmu_collective.cpp's local helper of the same name --
// axi::make_awuser_collective does not exist.
uint64_t awuser(uint8_t op, uint64_t addr_mask, uint8_t user = 0) {
    return (addr_mask << 10) | (uint64_t{op} << 8) | user;
}

// The design's worked example (2026-08-12-off-mesh-peripherals-design.md,
// "Worked example"): route span x = 0..2, y = 0..1; a peripheral occupies
// x = 0 on each row, so the tile region is x = 1..2, y = 0..1. Six 1 MB
// entries, memory class, built through the same loader path a topology YAML
// uses (SamTable::packed + addr_trans::declare_space_coords).
SamTable make_sam_with_border_column() {
    std::vector<addr_trans::PackedTile> tiles = {
        {0, 0, 0x100000},  // peripheral
        {1, 0, 0x100000},  // tile
        {2, 0, 0x100000},  // tile
        {0, 1, 0x100000},  // peripheral
        {1, 1, 0x100000},  // tile
        {2, 1, 0x100000},  // tile
    };
    auto table = addr_trans::SamTable::packed(tiles, /*x_span=*/3, /*y_span=*/2);
    addr_trans::declare_space_coords(table, /*x_span=*/3, /*y_span=*/2,
                                     /*tile_x_first=*/1, /*tile_x_last=*/2,
                                     /*tile_y_first=*/0, /*tile_y_last=*/1);
    return table;
}
}  // namespace

TEST(AddrTrans, Tile0_AddressForwardedUnchanged) {
    SCENARIO("addr_trans: tile 0 (base 0) -> the address is forwarded as it arrived");
    auto t = sam().translate(0x1234);
    EXPECT_EQ(t.dst_id, 0x00u);
    EXPECT_EQ(t.local_addr, 0x1234u);
}

TEST(AddrTrans, HighBitsDecodeDstId) {
    SCENARIO("addr_trans: addr[39:32] decodes to dst_id (x in [35:32], y in [39:36])");
    // addr[35:32]=0xF (x), addr[39:36]=0xF (y) -> dst_id = (0xF << 4) | 0xF = 0xFF
    auto t = sam().translate(0xFF00000000ull);
    EXPECT_EQ(t.dst_id, 0xFFu);
    EXPECT_EQ(t.local_addr, 0xFF00000000ull);  // untouched, base included
}

TEST(AddrTrans, TileBaseStaysInTheForwardedAddress) {
    SCENARIO(
        "addr_trans: the tile base is NOT stripped. A destination that decodes to a high tile "
        "keeps the whole address, so every hop and the destination tile read one address domain");
    auto t = sam().translate(0x12ABCDEF01ull);  // tile 0x12, base 0x1200000000
    EXPECT_EQ(t.dst_id, 0x12u);
    EXPECT_EQ(t.local_addr, 0x12ABCDEF01ull);
}

TEST(CollectiveClip, FullTileRowIsAcceptedWhenTilesDoNotStartAtZero) {
    SCENARIO(
        "nmu::addr_trans::collective_translate: off-mesh peripherals move the tile region off the "
        "route span's origin. Tiles at x = 1..2 with a peripheral at x = 0. The wildcard covering "
        "the tile row also names 0 and 3; after clipping to the tile region it names exactly 1 and "
        "2, so the source must accept it rather than refuse it as out of mesh");
    auto sam = make_sam_with_border_column();
    axi::AwBeat b{};
    b.addr = 0x100000;  // anchor is the tile at (1, 0)
    b.burst = axi::Burst::INCR;
    b.user = awuser(axi::COLLECTIVE_OP_MULTICAST, /*addr_mask=*/0x300000);
    const uint8_t mask = addr_trans::collective_translate(sam, b);
    EXPECT_EQ(mask & ((1u << ni::width::X_WIDTH) - 1u), 0x3u);
}
