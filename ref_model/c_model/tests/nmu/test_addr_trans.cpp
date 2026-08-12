#include "nmu/addr_trans.hpp"
#include "common/scenario.hpp"
#include <gtest/gtest.h>

using ni::cmodel::nmu::addr_trans::SamTable;

namespace {
// 16x16 uniform, 4 GB/tile: dst_id = addr[39:32] (covers all 256 dst_ids, matching
// the retired xy_route decode); the address itself is forwarded unchanged.
SamTable sam() {
    return SamTable::uniform(16, 16, 0x100000000ull);
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
