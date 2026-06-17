#include "common/zero_load_calculator.hpp"
#include <gtest/gtest.h>
#include <vector>

using namespace ni::cmodel::testing;

TEST(ZeroLoadCalc, XyPathCountsHops) {
    // 2x1 mesh: node 0 -> node 1 visits {0,1} (one hop); 0 -> 0 visits {0}.
    EXPECT_EQ(xy_path(0, 1, /*mx=*/2, /*my=*/1), (std::vector<uint8_t>{0, 1}));
    EXPECT_EQ(xy_path(0, 0, 2, 1), (std::vector<uint8_t>{0}));
}

TEST(ZeroLoadCalc, FormulaMatchesManualSum) {
    // router_req=3 is the caller-pre-summed total over all routers on the leg.
    // depths: nmu_req=2 nsu_req=2 router_req=3 ; nsu_rsp=2 nmu_rsp=2 router_rsp=3.
    DepthTable d{/*nmu_req=*/2, /*nmu_rsp=*/2,    /*nsu_req=*/2,
                 /*nsu_rsp=*/2, /*router_req=*/3, /*router_rsp=*/3};
    // request leg: nmu_req(2) + router_req(3) + nsu_req(2) = 7  (added once)
    // response leg: nsu_rsp(2) + router_rsp(3) + nmu_rsp(2) = 7  (added once)
    // serialization: (num_data_flits - 1). For a 1-beat write: 0.
    EXPECT_EQ(zero_load(0, 1, 2, 1, /*num_data_flits=*/1, d), 14u);
    // 3-beat write adds (3-1)=2.
    EXPECT_EQ(zero_load(0, 1, 2, 1, /*num_data_flits=*/3, d), 16u);
}

TEST(ZeroLoadCalc, LocalPathHasNoRouterTerm) {
    DepthTable d{2, 2, 2, 2, /*router_req=*/0, /*router_rsp=*/0};
    // local path: caller passes router_req=router_rsp=0;
    // nmu_req(2)+nsu_req(2)+nsu_rsp(2)+nmu_rsp(2) = 8.
    EXPECT_EQ(zero_load(0, 0, 2, 1, /*num_data_flits=*/1, d), 8u);
}

TEST(ZeroLoadCalc, BurstWithinBufferRangeCheck) {
    EXPECT_TRUE(burst_within_buffer(/*num_data_flits=*/4, /*buffer_depth=*/4));
    EXPECT_FALSE(burst_within_buffer(/*num_data_flits=*/5, /*buffer_depth=*/4));
}
