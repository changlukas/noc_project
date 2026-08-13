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
    b.addr = 0x100000;  // the address names the tile at (1, 0)
    b.burst = axi::Burst::INCR;
    b.user = awuser(axi::COLLECTIVE_OP_MULTICAST, /*addr_mask=*/0x300000);
    const uint8_t mask = addr_trans::collective_translate(sam, b, /*src_id=*/0x01);
    EXPECT_EQ(mask & ((1u << ni::width::X_WIDTH) - 1u), 0x3u);
}

TEST(CollectiveClipDeath, EmptyAfterClippingAborts) {
    SCENARIO(
        "nmu::addr_trans::collective_translate: an address naming a peripheral, wildcarding a "
        "coordinate the tile region doesn't reach, clips the set to empty. Address P(0,0), "
        "wildcarding y only: clip_min_x = max(0, tile_x_first=1) = 1, clip_max_x = min(0, "
        "tile_x_last=2) = 0 -- rejected rather than silently forwarding an empty set");
    auto sam = make_sam_with_border_column();
    axi::AwBeat b{};
    b.addr = 0x0;  // the address names the peripheral at (0, 0), outside the tile region
    b.burst = axi::Burst::INCR;
    b.user = awuser(axi::COLLECTIVE_OP_MULTICAST, /*addr_mask=*/0x400000);  // wildcards y only
    EXPECT_DEATH(addr_trans::collective_translate(sam, b, /*src_id=*/0x01),
                 "after clipping to the tile region");
}

TEST(CollectiveClipDeath, ClippedBoundNotAMemberAborts) {
    SCENARIO(
        "nmu::addr_trans::collective_translate: clamping a bound down to the tile region can land "
        "on a coordinate the raw wildcard set never named. Address T(1,0), mask_x = 0x2 -> raw set "
        "{1, 3}; clamping x_last from 3 down to 2 gives a non-empty clip [1,2], but 2 is not a "
        "member of {1, 3} (bit 0 differs from the address and bit 0 is not a don't-care in "
        "mask_x = 0x2). A terminal router's fork set at x=2 would be empty and abort -- must "
        "reject here "
        "instead of forwarding a set the source and a router would disagree on");
    auto sam = make_sam_with_border_column();
    axi::AwBeat b{};
    b.addr = 0x100000;  // the address names the tile at (1, 0)
    b.burst = axi::Burst::INCR;
    b.user = awuser(axi::COLLECTIVE_OP_MULTICAST, /*addr_mask=*/0x200000);  // mask_x = 0x2
    EXPECT_DEATH(addr_trans::collective_translate(sam, b, /*src_id=*/0x01),
                 "member of the wildcard set");
}

TEST(CollectiveIssuer, ATileMayIssueACollectiveAddressedAtAPeripheral) {
    SCENARIO(
        "nmu::addr_trans::collective_translate: the ISSUER is what has to be a tile, not the node "
        "the address names. Tile (1,0) addresses the peripheral (0,0) and wildcards x over the "
        "whole row; the closure {0,1,2,3} clips to the two tiles and both bounds are members, so "
        "the issuer "
        "check leaves it alone -- a memory controller multicasting to every tile is the case the "
        "clip exists for");
    auto sam = make_sam_with_border_column();
    axi::AwBeat b{};
    b.addr = 0x0;  // the address names the peripheral at (0, 0)
    b.burst = axi::Burst::INCR;
    b.user = awuser(axi::COLLECTIVE_OP_MULTICAST, /*addr_mask=*/0x300000);  // mask_x = 0x3
    const uint8_t mask = addr_trans::collective_translate(sam, b, /*src_id=*/0x01);
    EXPECT_EQ(mask & ((1u << ni::width::X_WIDTH) - 1u), 0x3u);
}

TEST(CollectiveIssuerDeath, APeripheralMayNotIssueACollective) {
    SCENARIO(
        "nmu::addr_trans::collective_translate: the same legal mask issued BY the peripheral at "
        "(0,0) is refused. Both trees are built from the issuer's own coordinates -- route_mask "
        "spreads the fork along cfg.y == src.y and collects the join in cfg.x == dst.x -- and no "
        "router sits at x = 0, so an x-border issuer forks but never collects the cross-row "
        "replicas (the CollectB hangs) and a y-border one never forks at all (the replicas are "
        "dropped). Refuse, rather than hang or drop silently");
    auto sam = make_sam_with_border_column();
    axi::AwBeat b{};
    b.addr = 0x0;
    b.burst = axi::Burst::INCR;
    b.user = awuser(axi::COLLECTIVE_OP_MULTICAST, /*addr_mask=*/0x300000);
    EXPECT_DEATH(addr_trans::collective_translate(sam, b, /*src_id=*/0x00),
                 "a collective issued from outside the tile");
}

TEST(OffMeshDst, SameRowPeripheralIsReachable) {
    SCENARIO(
        "nmu::addr_trans::check_dst_reachable: a peripheral sits outside the tile region on x and "
        "is reached by running out of x hops, which happens on the source's own row. Both tiles of "
        "row 0 address the peripheral at (0,0), and the tile at (1,1) addresses the one at (0,1) "
        "-- every one of them is on its destination's row, so none is refused");
    auto sam = make_sam_with_border_column();
    const auto* coords = sam.collective_coords(axi::AxiClass::Data);
    ASSERT_NE(coords, nullptr);
    addr_trans::check_dst_reachable(coords, /*src_id=*/0x01, /*dst_id=*/0x00);  // (1,0) -> (0,0)
    addr_trans::check_dst_reachable(coords, /*src_id=*/0x02, /*dst_id=*/0x00);  // (2,0) -> (0,0)
    addr_trans::check_dst_reachable(coords, /*src_id=*/0x11, /*dst_id=*/0x10);  // (1,1) -> (0,1)
    // A tile destination is inside the region and is reachable from any row --
    // as long as the source is a tile too. Row 0 to row 1, both in region.
    addr_trans::check_dst_reachable(coords, /*src_id=*/0x01, /*dst_id=*/0x12);  // (1,0) -> (2,1)
}

TEST(OffMeshDstDeath, CrossRowPeripheralIsRefused) {
    SCENARIO(
        "nmu::addr_trans::check_dst_reachable: the tile at (2,1) addressing the peripheral at "
        "(0,0) exhausts its x hops while still on row 1 (router.hpp resolves X before Y), leaves "
        "the region WEST there and lands in the peripheral at (0,1), whose NSU rebases the address "
        "to its own tile and answers it. Nothing downstream can tell, so the source refuses it");
    auto sam = make_sam_with_border_column();
    const auto* coords = sam.collective_coords(axi::AxiClass::Data);
    ASSERT_NE(coords, nullptr);
    EXPECT_DEATH(addr_trans::check_dst_reachable(coords, /*src_id=*/0x12, /*dst_id=*/0x00),
                 "outside the tile region");
}

TEST(OffMeshSrc, APeripheralIsAnInitiatorOnItsOwnRow) {
    SCENARIO(
        "nmu::addr_trans::check_dst_reachable: the peripheral at (0,1) is an initiator, and the "
        "response to its request is routed back the same way the request went out -- the same "
        "rule read from the other end. Both tiles of row 1 are on its row, so neither request is "
        "refused");
    auto sam = make_sam_with_border_column();
    const auto* coords = sam.collective_coords(axi::AxiClass::Data);
    ASSERT_NE(coords, nullptr);
    addr_trans::check_dst_reachable(coords, /*src_id=*/0x10, /*dst_id=*/0x11);  // (0,1) -> (1,1)
    addr_trans::check_dst_reachable(coords, /*src_id=*/0x10, /*dst_id=*/0x12);  // (0,1) -> (2,1)
}

TEST(OffMeshSrcDeath, APeripheralAddressingAnotherRowIsRefused) {
    SCENARIO(
        "nmu::addr_trans::check_dst_reachable: the peripheral at (0,1) writing the tile at (2,0) "
        "is delivered, and its B is not. The response carries dst_id = (0,1) (nsu's "
        "build_b_flit), leaves (2,0) WEST, runs out of x hops on row 0 and lands in the "
        "peripheral at (0,0), which takes a B it never issued -- invisible to both scoreboards. "
        "The source's own row is required in this direction too");
    auto sam = make_sam_with_border_column();
    const auto* coords = sam.collective_coords(axi::AxiClass::Data);
    ASSERT_NE(coords, nullptr);
    EXPECT_DEATH(addr_trans::check_dst_reachable(coords, /*src_id=*/0x10, /*dst_id=*/0x02),
                 "sits outside the tile region");
}
