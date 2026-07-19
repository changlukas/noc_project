#include "nmu/sam_yaml.hpp"
#include "common/tmp_path.hpp"
#include <gtest/gtest.h>
#include <fstream>

using ni::cmodel::nmu::addr_trans::load_sam_table;

TEST(SamYaml, PackedTilesAccumulateBases) {
    auto path = ni::cmodel::testing::unique_temp_path("sam_packed.yaml");
    std::ofstream(path) << "topology: { name: t, x_dim: 2, y_dim: 1, num_vc: 1 }\n"
                           "address_map:\n"
                           "  tiles:\n"
                           "    - { x: 0, y: 0, size: 0x1000 }\n"
                           "    - { x: 1, y: 0, size: 0x2000 }\n";
    auto sam = load_sam_table(path);
    ASSERT_EQ(sam.entries().size(), 2u);
    EXPECT_EQ(sam.entries()[0].base, 0x0ull);
    EXPECT_EQ(sam.entries()[1].base, 0x1000ull);  // base(1) = base(0) + size(0)
}

TEST(SamYaml, TranslateRebasesAgainstPackedBase) {
    auto path = ni::cmodel::testing::unique_temp_path("sam_packed_translate.yaml");
    std::ofstream(path) << "topology: { name: t, x_dim: 2, y_dim: 2, num_vc: 1 }\n"
                           "address_map:\n"
                           "  tiles:\n"
                           "    - { x: 0, y: 0, size: 0x100000000 }\n"
                           "    - { x: 1, y: 0, size: 0x100000000 }\n"
                           "    - { x: 0, y: 1, size: 0x100000000 }\n"
                           "    - { x: 1, y: 1, size: 0x100000000 }\n";
    auto sam = load_sam_table(path);
    auto t = sam.translate(0x300000040ull);  // 4th tile (x=1,y=1), base 0x300000000
    EXPECT_EQ(t.dst_id, 0x11u);
    EXPECT_EQ(t.local_addr, 0x40ull);  // rebased: addr - base
}

TEST(SamYaml, MissingNodeRejected) {
    auto path = ni::cmodel::testing::unique_temp_path("sam_missing_node.yaml");
    std::ofstream(path) << "topology: { name: t, x_dim: 2, y_dim: 2, num_vc: 1 }\n"
                           "address_map:\n"
                           "  tiles:\n"
                           "    - { x: 0, y: 0, size: 0x1000 }\n"
                           "    - { x: 1, y: 0, size: 0x1000 }\n"
                           "    - { x: 0, y: 1, size: 0x1000 }\n";  // (1,1) missing
    EXPECT_DEATH(load_sam_table(path), "exactly once");
}

TEST(SamYaml, DuplicateNodeRejected) {
    auto path = ni::cmodel::testing::unique_temp_path("sam_dup_node.yaml");
    std::ofstream(path) << "topology: { name: t, x_dim: 1, y_dim: 2, num_vc: 1 }\n"
                           "address_map:\n"
                           "  tiles:\n"
                           "    - { x: 0, y: 0, size: 0x1000 }\n"
                           "    - { x: 0, y: 0, size: 0x1000 }\n";  // dup (0,0), (0,1) missing
    EXPECT_DEATH(load_sam_table(path), "duplicate");
}

TEST(SamYaml, NonAlignedSizeRejected) {
    auto path = ni::cmodel::testing::unique_temp_path("sam_bad_size.yaml");
    std::ofstream(path) << "topology: { name: t, x_dim: 1, y_dim: 1, num_vc: 1 }\n"
                           "address_map:\n"
                           "  tiles:\n"
                           "    - { x: 0, y: 0, size: 0x1800 }\n";  // 6 KB, not 4 KB aligned
    // "|" alternation is unsupported by gtest's simple regex engine (used
    // when GTEST_USES_POSIX_RE=0, e.g. MSVC/MinGW) -- keep this a plain literal.
    EXPECT_DEATH(load_sam_table(path), "aligned");
}
