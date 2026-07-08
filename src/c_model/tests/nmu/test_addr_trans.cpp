#include "nmu/addr_trans.hpp"
#include "common/scenario.hpp"
#include <gtest/gtest.h>

using ni::cmodel::nmu::addr_trans::SamTable;

namespace {
// 16x16 uniform, 4 GB/tile, no rebase: dst = addr/4GB, local_addr = addr
// unchanged -- reproduces the retired addr_trans::xy_route bit-slice mapping
// (addr[39:32] -> dst_id, local_addr = addr) exactly for addr < 2^40. This
// file is the regression anchor pinning that equivalence.
SamTable legacy_sam() {
    return SamTable::uniform(16, 16, 0x100000000ull, /*rebase=*/false);
}
}  // namespace

TEST(AddrTrans, LegacySam_LowBitsAreLocalAddr) {
    SCENARIO("addr_trans: low-addr bits flow through to local_addr unchanged (tile 0)");
    auto t = legacy_sam().translate(0x1234);
    EXPECT_EQ(t.dst_id, 0x00u);
    EXPECT_EQ(t.local_addr, 0x1234u);
}

TEST(AddrTrans, LegacySam_HighBitsDecodeXY) {
    SCENARIO("addr_trans: addr[39:32] decodes to dst_id (x in [35:32], y in [39:36])");
    // addr[35:32]=0xF (x), addr[39:36]=0xF (y) -> dst_id = (0xF << 4) | 0xF = 0xFF
    auto t = legacy_sam().translate(0xFF00000000ull);
    EXPECT_EQ(t.dst_id, 0xFFu);
    EXPECT_EQ(t.local_addr, 0xFF00000000ull);
}

TEST(AddrTrans, LegacySam_LocalAddrPassesThroughWithinSamRange) {
    SCENARIO(
        "addr_trans: full 40-bit addr (this SAM's range) passes through to local_addr without "
        "truncation");
    // Legacy xy_route tolerated any 64-bit address (only masked out dst_id from
    // bits [39:32], upper bits were simply along for the ride in local_addr).
    // A bounded 256-tile SAM has no entry above 2^40, so this pins the same
    // dst-decode + local-passthrough behavior at the SAM's upper bound instead.
    auto t = legacy_sam().translate(0x12ABCDEF01ull);
    EXPECT_EQ(t.local_addr, 0x12ABCDEF01ull);
    // dst_id = addr[39:32] = 0x12
    EXPECT_EQ(t.dst_id, 0x12u);
}
