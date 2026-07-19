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

// Guards the real topology configs: sim/topologies/ is copied next to the
// test binary at build time (CMakeLists.txt). Cross-checked against the
// Python loader on the same files (test_address_map_pack_real_topologies_gap_free
// in sim/tools/test_gen_test_patterns_filemaster.py) -- if both pass, the C++
// and Python packing agree on every real topology YAML.
TEST(SamYaml, RealTopologiesGapFreePacked) {
    static const char* kFiles[] = {
        "topologies/mesh_1x1_vc1.yaml", "topologies/mesh_2x2_nonuniform_vc1.yaml",
        "topologies/mesh_2x4_vc1.yaml", "topologies/mesh_4x4_vc1.yaml",
        "topologies/mesh_4x4_vc2.yaml", "topologies/mesh_4x4_vc4.yaml",
        "topologies/mesh_4x4_vc8.yaml",
    };
    for (const char* file : kFiles) {
        SCOPED_TRACE(file);
        auto sam = load_sam_table(file);
        ASSERT_FALSE(sam.entries().empty());
        uint64_t expected_base = 0;
        for (const auto& e : sam.entries()) {
            EXPECT_EQ(e.base, expected_base);
            expected_base += e.size;
        }
    }
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
