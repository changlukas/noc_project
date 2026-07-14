#include "ni/virtual_network.hpp"
#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

using ni::cmodel::make_virtual_networks;
using ni::cmodel::VirtualNetworks;

TEST(VirtualNetworks, NumVc1_BothShareVc0) {
    VirtualNetworks v = make_virtual_networks(1);
    EXPECT_EQ(v.write_vcs, (std::vector<uint8_t>{0}));
    EXPECT_EQ(v.read_vcs, (std::vector<uint8_t>{0}));
}

TEST(VirtualNetworks, NumVc2_WriteLowReadHigh) {
    VirtualNetworks v = make_virtual_networks(2);
    EXPECT_EQ(v.write_vcs, (std::vector<uint8_t>{0}));
    EXPECT_EQ(v.read_vcs, (std::vector<uint8_t>{1}));
}

TEST(VirtualNetworks, NumVc4_TwoEach) {
    VirtualNetworks v = make_virtual_networks(4);
    EXPECT_EQ(v.write_vcs, (std::vector<uint8_t>{0, 1}));
    EXPECT_EQ(v.read_vcs, (std::vector<uint8_t>{2, 3}));
}

TEST(VirtualNetworks, NumVc8_FourEach) {
    VirtualNetworks v = make_virtual_networks(8);
    EXPECT_EQ(v.write_vcs, (std::vector<uint8_t>{0, 1, 2, 3}));
    EXPECT_EQ(v.read_vcs, (std::vector<uint8_t>{4, 5, 6, 7}));
}

TEST(VirtualNetworksDeath, OddNumVcAborts) {
    EXPECT_DEATH({ (void)make_virtual_networks(3); }, "num_vc must be 1 or even");
}
