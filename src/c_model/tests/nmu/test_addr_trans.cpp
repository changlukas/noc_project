#include "nmu/addr_trans.hpp"
#include "common/scenario.hpp"
#include <gtest/gtest.h>

using ni::cmodel::nmu::addr_trans::Translated;
using ni::cmodel::nmu::addr_trans::xy_route;

TEST(AddrTrans, XyRoute_LowBitsAreLocalAddr) {
    SCENARIO("addr_trans: low-addr bits flow through to local_addr unchanged");
    auto t = xy_route(0x1234);
    EXPECT_EQ(t.dst_id, 0x00u);
    EXPECT_EQ(t.local_addr, 0x1234u);
}

TEST(AddrTrans, XyRoute_HighBitsDecodeXY) {
    SCENARIO("addr_trans: addr[39:32] decodes to dst_id (x in [35:32], y in [39:36])");
    // addr[35:32]=0xF (x), addr[39:36]=0xF (y) -> dst_id = (0xF << 4) | 0xF = 0xFF
    auto t = xy_route(0xFF00000000ull);
    EXPECT_EQ(t.dst_id, 0xFFu);
    EXPECT_EQ(t.local_addr, 0xFF00000000ull);
}

TEST(AddrTrans, XyRoute_LocalAddrPassesThroughFullWidth) {
    SCENARIO("addr_trans: full 64-bit addr passes through to local_addr without truncation");
    auto t = xy_route(0xABCDEF1234567890ull);
    EXPECT_EQ(t.local_addr, 0xABCDEF1234567890ull);
    // dst_id = addr[39:32] = 0x12
    EXPECT_EQ(t.dst_id, 0x12u);
}
