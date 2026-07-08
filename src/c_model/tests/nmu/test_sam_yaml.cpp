#include "nmu/sam_yaml.hpp"
#include "common/tmp_path.hpp"
#include <gtest/gtest.h>
#include <fstream>

using ni::cmodel::nmu::addr_trans::load_sam_table;

TEST(SamYaml, UniformBlockBuildsTable) {
    auto path = ni::cmodel::testing::unique_temp_path("sam_uniform.yaml");
    std::ofstream(path) << "topology: { name: t, x_dim: 4, y_dim: 4, num_vc: 1 }\n"
                           "address_map:\n"
                           "  tile_size: 0x100000000\n"
                           "  rebase: true\n";
    auto sam = load_sam_table(path);
    auto t = sam.translate(0x1200000040ull);
    EXPECT_EQ(t.dst_id, 0x12u);
    EXPECT_EQ(t.local_addr, 0x40ull);
}

TEST(SamYaml, ExplicitTilesOverride) {
    auto path = ni::cmodel::testing::unique_temp_path("sam_tiles.yaml");
    std::ofstream(path) << "topology: { name: t, x_dim: 2, y_dim: 2, num_vc: 1 }\n"
                           "address_map:\n"
                           "  tile_size: 0x100000000\n"
                           "  rebase: true\n"
                           "  tiles:\n"
                           "    - { x: 0, y: 0, base: 0x0, size: 0x10000000 }\n";
    auto sam = load_sam_table(path);
    const auto* e = sam.lookup(0x40ull);  // tile (0,0) overridden to 256 MB at base 0
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->size, 0x10000000ull);
}
