#include "nmu/addr_trans.hpp"
#include "common/scenario.hpp"
#include <gtest/gtest.h>

using ni::cmodel::nmu::addr_trans::xy_route;
using ni::cmodel::nmu::addr_trans::Translated;

TEST(AddrTrans, XyRoute_LowBitsAreLocalAddr) {
    SCENARIO("addr_trans: low-addr bits flow through to local_addr unchanged");
    auto t = xy_route(0x1234);
    EXPECT_EQ(t.dst_id, 0x00u);
    EXPECT_EQ(t.local_addr, 0x1234u);
}

TEST(AddrTrans, XyRoute_HighBitsDecodeXY) {
    SCENARIO("addr_trans: addr[23:16] decodes to dst_id (x in [19:16], y in [23:20])");
    // addr[19:16]=0xF (x), addr[23:20]=0xF (y) -> dst_id = (0xF << 4) | 0xF = 0xFF
    auto t = xy_route(0x00FF0000);
    EXPECT_EQ(t.dst_id, 0xFFu);
    EXPECT_EQ(t.local_addr, 0x00FF0000ull);
}

TEST(AddrTrans, XyRoute_LocalAddrPassesThroughFullWidth) {
    SCENARIO("addr_trans: full 64-bit addr passes through to local_addr without truncation");
    auto t = xy_route(0xABCDEF1234567890ull);
    EXPECT_EQ(t.local_addr, 0xABCDEF1234567890ull);
    // dst_id = addr[23:16] = 0x56
    EXPECT_EQ(t.dst_id, 0x56u);
}
