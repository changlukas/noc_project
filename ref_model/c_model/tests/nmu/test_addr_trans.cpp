#include "nmu/addr_trans.hpp"
#include "nmu/sam_yaml.hpp"
#include "axi/types.hpp"
#include <gtest/gtest.h>
#include <string>

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
}  // namespace

TEST(AddrTrans, Tile0_AddressForwardedUnchanged) {
    auto t = sam().translate(0x1234);
    EXPECT_EQ(t.dst_id, 0x00u);
    EXPECT_EQ(t.local_addr, 0x1234u);
}

TEST(AddrTrans, HighBitsDecodeDstId) {
    // addr[35:32]=0xF (x), addr[39:36]=0xF (y) -> dst_id = (0xF << 4) | 0xF = 0xFF
    auto t = sam().translate(0xFF00000000ull);
    EXPECT_EQ(t.dst_id, 0xFFu);
    EXPECT_EQ(t.local_addr, 0xFF00000000ull);  // untouched, base included
}

TEST(AddrTrans, TileBaseStaysInTheForwardedAddress) {
    auto t = sam().translate(0x12ABCDEF01ull);  // tile 0x12, base 0x1200000000
    EXPECT_EQ(t.dst_id, 0x12u);
    EXPECT_EQ(t.local_addr, 0x12ABCDEF01ull);
}

// mesh_2x2_vc1_periph.yaml declares a peripheral on router (0, 0)'s x face, so
// the peripheral and the tile that issue below share the coordinate 0x00 and
// differ only in the port. A 4 GiB block stride puts the memory space's
// coordinate field at [33:32], so 0x100000000 wildcards x.
constexpr uint64_t kWildcardX = 0x100000000ull;

TEST(CollectiveIssuer, ATileMayIssueACollective) {
    auto sam = addr_trans::load_sam_table(std::string(CONFIG_DIR) + "/mesh_2x2_periph.yml");
    axi::AwBeat b{};
    b.addr = 0x0;  // node (0, 0)'s memory tile
    b.burst = axi::Burst::INCR;
    b.user = awuser(axi::COLLECTIVE_OP_MULTICAST, kWildcardX);
    const uint8_t mask = addr_trans::collective_translate(sam, b, /*src_port=*/0);
    EXPECT_EQ(mask & ((1u << ni::width::X_WIDTH) - 1u), 0x1u);
}

TEST(CollectiveIssuerDeath, APeripheralCannotIssueACollective) {
    // Round 2 gave every endpoint a port. A peripheral's is non-zero, and that
    // is now the only thing that distinguishes it -- it shares its router's
    // coordinate, so the old outside-the-tile-region test would pass.
    auto sam = addr_trans::load_sam_table(std::string(CONFIG_DIR) + "/mesh_2x2_periph.yml");
    axi::AwBeat b{};
    b.addr = 0x0;
    b.burst = axi::Burst::INCR;
    b.user = awuser(axi::COLLECTIVE_OP_MULTICAST, kWildcardX);
    EXPECT_DEATH(addr_trans::collective_translate(sam, b, /*src_port=*/1),
                 "issued from a peripheral");
}
