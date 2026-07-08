#include "nmu/addr_trans.hpp"
#include "common/scenario.hpp"
#include <gtest/gtest.h>

using ni::cmodel::nmu::addr_trans::SamTable;

namespace {
// 16x16 uniform, 4 GB/tile: dst_id = addr[39:32] (covers all 256 dst_ids, matching
// the retired xy_route decode); local_addr = addr - tile base (rebased).
SamTable sam() {
    return SamTable::uniform(16, 16, 0x100000000ull);
}
}  // namespace

TEST(AddrTrans, Tile0_LocalEqualsAddr) {
    SCENARIO("addr_trans: tile 0 (base 0) -> local_addr == addr (rebase is a no-op there)");
    auto t = sam().translate(0x1234);
    EXPECT_EQ(t.dst_id, 0x00u);
    EXPECT_EQ(t.local_addr, 0x1234u);
}

TEST(AddrTrans, HighBitsDecodeDstId) {
    SCENARIO("addr_trans: addr[39:32] decodes to dst_id (x in [35:32], y in [39:36])");
    // addr[35:32]=0xF (x), addr[39:36]=0xF (y) -> dst_id = (0xF << 4) | 0xF = 0xFF
    auto t = sam().translate(0xFF00000000ull);
    EXPECT_EQ(t.dst_id, 0xFFu);
    EXPECT_EQ(t.local_addr, 0x0ull);  // rebased: 0xFF00000000 - base 0xFF00000000
}

TEST(AddrTrans, RebasedLocalIsTileOffset) {
    SCENARIO("addr_trans: local_addr is the within-tile offset (addr - tile base)");
    auto t = sam().translate(0x12ABCDEF01ull);  // tile 0x12, base 0x1200000000
    EXPECT_EQ(t.dst_id, 0x12u);
    EXPECT_EQ(t.local_addr, 0xABCDEF01ull);  // rebased offset
}
