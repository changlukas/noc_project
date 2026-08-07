#include "nmu/sam_yaml.hpp"
#include "axi/types.hpp"
#include "common/tmp_path.hpp"
#include <gtest/gtest.h>
#include <fstream>

using ni::cmodel::nmu::addr_trans::load_sam_table;
namespace axi = ni::cmodel::axi;

TEST(SamYaml, PackedTilesAccumulateBases) {
    auto path = ni::cmodel::testing::unique_temp_path("sam_packed.yaml");
    std::ofstream(path) << "topology: { name: t, x_dim: 2, y_dim: 2, num_vc: 1 }\n"
                           "address_map:\n"
                           "  tiles:\n"
                           "    - { x: 0, y: 0, size: 0x1000 }\n"
                           "    - { x: 1, y: 0, size: 0x2000 }\n"
                           "    - { x: 0, y: 1, size: 0x1000 }\n"
                           "    - { x: 1, y: 1, size: 0x1000 }\n";
    auto sam = load_sam_table(path);
    ASSERT_EQ(sam.entries().size(), 4u);
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
    std::ofstream(path) << "topology: { name: t, x_dim: 2, y_dim: 2, num_vc: 1 }\n"
                           "address_map:\n"
                           "  tiles:\n"
                           "    - { x: 0, y: 0, size: 0x1000 }\n"
                           "    - { x: 0, y: 0, size: 0x1000 }\n"  // dup (0,0)
                           "    - { x: 1, y: 0, size: 0x1000 }\n"
                           "    - { x: 0, y: 1, size: 0x1000 }\n";  // (1,1) missing
    EXPECT_DEATH(load_sam_table(path), "duplicate");
}

TEST(SamYaml, MeshDimBelowMinimumRejected) {
    auto path_x = ni::cmodel::testing::unique_temp_path("sam_mesh_x1.yaml");
    std::ofstream(path_x) << "topology: { name: t, x_dim: 1, y_dim: 4, num_vc: 1 }\n"
                             "address_map:\n"
                             "  tiles:\n"
                             "    - { x: 0, y: 0, size: 0x1000 }\n"
                             "    - { x: 0, y: 1, size: 0x1000 }\n"
                             "    - { x: 0, y: 2, size: 0x1000 }\n"
                             "    - { x: 0, y: 3, size: 0x1000 }\n";
    EXPECT_DEATH(load_sam_table(path_x), "mesh dimensions must be >= 2");

    auto path_y = ni::cmodel::testing::unique_temp_path("sam_mesh_y1.yaml");
    std::ofstream(path_y) << "topology: { name: t, x_dim: 1, y_dim: 1, num_vc: 1 }\n"
                             "address_map:\n"
                             "  tiles:\n"
                             "    - { x: 0, y: 0, size: 0x1000 }\n";
    EXPECT_DEATH(load_sam_table(path_y), "mesh dimensions must be >= 2");
}

// Guards the real topology configs: sim/topologies/ is copied next to the
// test binary at build time (CMakeLists.txt). Cross-checked against the
// Python loader on the same files (test_address_map_pack_real_topologies_gap_free
// in sim/tools/test_gen_test_patterns_filemaster.py) -- if both pass, the C++
// and Python packing agree on every real topology YAML.
TEST(SamYaml, RealTopologiesGapFreePacked) {
    static const char* kFiles[] = {
        "topologies/mesh_2x4_vc1.yaml",
        "topologies/mesh_4x4_vc1.yaml",
        "topologies/mesh_4x4_vc2.yaml",
        "topologies/mesh_4x4_vc4.yaml",
        "topologies/mesh_4x4_vc8.yaml",
        "topologies/mesh_2x2_config_narrow_vc1.yaml",
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

// S2 T2d: SAM class selection from the topology YAML's tile.space attribute
// (docs/noc-target-spec.md §5). mesh_2x2_config_narrow_vc1.yaml gives node
// (0,0) both a memory tile (default space) and a config tile.
TEST(SamYaml, SpaceAttributeSelectsClass) {
    auto sam = load_sam_table("topologies/mesh_2x2_config_narrow_vc1.yaml");
    // Memory-space tiles pack first (list order): node (0,0)'s memory tile is
    // [0, 0x100000).
    auto memory = sam.translate(0x1000);
    EXPECT_EQ(memory.dst_id, 0x00u);
    EXPECT_EQ(memory.cls, axi::AxiClass::Data);
    // Tile-local layering: config span 0x1000 takes [0x0, 0x1000), memory span
    // 0x100000 starts at 0x100000, so the memory tile's offset 0x1000 lands at
    // 0x101000 -- outside the config window.
    EXPECT_EQ(memory.local_addr, 0x101000ull);

    // The config tile is the 5th entry: base = sum of the 4 memory tiles'
    // sizes = 4 * 0x100000 = 0x400000, size 0x1000.
    auto config = sam.translate(0x400010);
    EXPECT_EQ(config.dst_id, 0x00u);
    EXPECT_EQ(config.cls, axi::AxiClass::Narrow);
    EXPECT_EQ(config.local_addr, 0x10ull);  // config space sits at tile-local 0x0

    // The two spaces of one node no longer collide at tile-local 0 -- this is
    // what the tile crossbar decodes on.
    EXPECT_NE(sam.translate(0x0).local_addr, sam.translate(0x400000).local_addr);
}

TEST(SamYaml, UnknownSpaceRejected) {
    auto path = ni::cmodel::testing::unique_temp_path("sam_bad_space.yaml");
    std::ofstream(path) << "topology: { name: t, x_dim: 2, y_dim: 2, num_vc: 1 }\n"
                           "address_map:\n"
                           "  tiles:\n"
                           "    - { x: 0, y: 0, size: 0x1000, space: bogus }\n"
                           "    - { x: 1, y: 0, size: 0x1000 }\n"
                           "    - { x: 0, y: 1, size: 0x1000 }\n"
                           "    - { x: 1, y: 1, size: 0x1000 }\n";
    EXPECT_DEATH(load_sam_table(path), "space");
}

TEST(SamYaml, ConfigSpaceDuplicateNodeRejected) {
    auto path = ni::cmodel::testing::unique_temp_path("sam_dup_config.yaml");
    std::ofstream(path) << "topology: { name: t, x_dim: 2, y_dim: 2, num_vc: 1 }\n"
                           "address_map:\n"
                           "  tiles:\n"
                           "    - { x: 0, y: 0, size: 0x1000 }\n"
                           "    - { x: 1, y: 0, size: 0x1000 }\n"
                           "    - { x: 0, y: 1, size: 0x1000 }\n"
                           "    - { x: 1, y: 1, size: 0x1000 }\n"
                           "    - { x: 0, y: 0, size: 0x1000, space: config }\n"
                           "    - { x: 0, y: 0, size: 0x1000, space: config }\n";  // dup config
    EXPECT_DEATH(load_sam_table(path), "duplicate");
}

TEST(SamYaml, NonAlignedSizeRejected) {
    auto path = ni::cmodel::testing::unique_temp_path("sam_bad_size.yaml");
    std::ofstream(path) << "topology: { name: t, x_dim: 2, y_dim: 2, num_vc: 1 }\n"
                           "address_map:\n"
                           "  tiles:\n"
                           "    - { x: 0, y: 0, size: 0x1800 }\n";  // 6 KB, not 4 KB aligned
    // "|" alternation is unsupported by gtest's simple regex engine (used
    // when GTEST_USES_POSIX_RE=0, e.g. MSVC/MinGW) -- keep this a plain literal.
    EXPECT_DEATH(load_sam_table(path), "aligned");
}
