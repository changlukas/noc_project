#include "ni/vc_pools.hpp"
#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

using ni::cmodel::derive_vc_pools;
using ni::cmodel::VcPools;

TEST(VcPools, NumVc1_BothShareVc0) {
    VcPools p = derive_vc_pools(1);
    EXPECT_EQ(p.write_vcs, (std::vector<uint8_t>{0}));
    EXPECT_EQ(p.read_vcs, (std::vector<uint8_t>{0}));
}

TEST(VcPools, NumVc2_WriteLowReadHigh) {
    VcPools p = derive_vc_pools(2);
    EXPECT_EQ(p.write_vcs, (std::vector<uint8_t>{0}));
    EXPECT_EQ(p.read_vcs, (std::vector<uint8_t>{1}));
}

TEST(VcPools, NumVc4_TwoEach) {
    VcPools p = derive_vc_pools(4);
    EXPECT_EQ(p.write_vcs, (std::vector<uint8_t>{0, 1}));
    EXPECT_EQ(p.read_vcs, (std::vector<uint8_t>{2, 3}));
}

TEST(VcPools, NumVc8_FourEach) {
    VcPools p = derive_vc_pools(8);
    EXPECT_EQ(p.write_vcs, (std::vector<uint8_t>{0, 1, 2, 3}));
    EXPECT_EQ(p.read_vcs, (std::vector<uint8_t>{4, 5, 6, 7}));
}

TEST(VcPoolsDeath, OddNumVcAborts) {
    EXPECT_DEATH({ (void)derive_vc_pools(3); }, "num_vc must be 1 or even");
}
