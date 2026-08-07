#include "nmu/sam_yaml.hpp"
#include "axi/types.hpp"
#include "common/tmp_path.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using ni::cmodel::nmu::addr_trans::collective_translate;
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

// Guards the real topology configs. TOPOLOGY_DIR is sim/topologies/ itself
// (CMakeLists.txt), globbed rather than listed, so a new topology YAML cannot
// fall out of coverage. Same files and same shape as the Python twin
// (test_address_map_pack_real_topologies_gap_free in
// sim/tools/test_gen_test_patterns_filemaster.py) -- if both pass, the C++ and
// Python packing agree on every real topology YAML.
TEST(SamYaml, RealTopologiesGapFreePacked) {
    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(TOPOLOGY_DIR)) {
        if (entry.path().extension() == ".yaml") files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());
    // Same floor the Python twin asserts. It guards the scan, not the inventory:
    // an empty or unreachable directory must fail rather than pass vacuously.
    // A count tied to how many topologies ship would have to be edited every time
    // one is added or removed, which is exactly what the glob exists to avoid.
    ASSERT_FALSE(files.empty()) << "expected the real topology YAMLs in " TOPOLOGY_DIR;
    for (const auto& file : files) {
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

// SAM class selection from the topology YAML's tile.space attribute
// (docs/noc-target-spec.md §5). mesh_2x2_vc1.yaml gives every node both a
// memory tile (default space) and a config tile.
TEST(SamYaml, SpaceAttributeSelectsClass) {
    auto sam = load_sam_table(TOPOLOGY_DIR "/mesh_2x2_vc1.yaml");
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

// The ranges the loader derives from the shipped YAMLs, spelled out. Memory
// stride is 1 MB and config stride 4 KB in every topology, so the offsets are
// 20 and 12; the lengths are clog2 of the mesh dimension.
TEST(SamYaml, CoordRangesDerivedFromTheSpaceStride) {
    struct Row {
        const char* file;
        unsigned dim_bits;
    } rows[] = {{"/mesh_2x2_vc1.yaml", 1}, {"/mesh_4x4_vc1.yaml", 2}};
    for (const auto& row : rows) {
        SCOPED_TRACE(row.file);
        auto sam = load_sam_table(std::string(TOPOLOGY_DIR) + row.file);
        const auto* memory = sam.collective_coords(axi::AxiClass::Data);
        ASSERT_NE(memory, nullptr);
        EXPECT_EQ(memory->x_range.offset, 20u);  // log2(1 MB)
        EXPECT_EQ(memory->x_range.len, row.dim_bits);
        EXPECT_EQ(memory->y_range.offset, 20u + row.dim_bits);
        EXPECT_EQ(memory->y_range.len, row.dim_bits);
        const auto* config = sam.collective_coords(axi::AxiClass::Narrow);
        ASSERT_NE(config, nullptr);
        EXPECT_EQ(config->x_range.offset, 12u);  // log2(4 KB)
        EXPECT_EQ(config->x_range.len, row.dim_bits);
        EXPECT_EQ(config->y_range.offset, 12u + row.dim_bits);
        EXPECT_EQ(config->y_range.len, row.dim_bits);
    }
}

// B1 differential: the node mask read off the declared ranges must equal the
// one collective_translate derives today by enumerating the 2^n named
// addresses. Exhaustive over every shipped topology, both spaces, every anchor
// node and every legal mask shape -- at most 2^(x_bits + y_bits) masks per
// space. This is the equivalence evidence for B2 replacing the enumeration.
TEST(SamYaml, SlicedNodeMaskMatchesTheEnumeratedOne) {
    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(TOPOLOGY_DIR)) {
        if (entry.path().extension() == ".yaml") files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());
    ASSERT_FALSE(files.empty()) << "expected the real topology YAMLs in " TOPOLOGY_DIR;

    unsigned compared = 0;
    for (const auto& file : files) {
        SCOPED_TRACE(file);
        auto sam = load_sam_table(file);
        for (axi::AxiClass cls : {axi::AxiClass::Narrow, axi::AxiClass::Data}) {
            const auto* c = sam.collective_coords(cls);
            ASSERT_NE(c, nullptr) << "every shipped space is collective-eligible";
            uint64_t origin = 0;
            for (const auto& e : sam.entries()) {
                if (e.cls == cls) {
                    origin = e.base;
                    break;
                }
            }
            for (unsigned ay = 0; ay < c->y_count; ++ay) {
                for (unsigned ax = 0; ax < c->x_count; ++ax) {
                    const uint64_t anchor = origin | (uint64_t{ax} << c->x_range.offset) |
                                            (uint64_t{ay} << c->y_range.offset);
                    for (unsigned my = 0; my < (1u << c->y_range.len); ++my) {
                        for (unsigned mx = 0; mx < (1u << c->x_range.len); ++mx) {
                            if (mx == 0 && my == 0) continue;  // empty set: rejected, not compared
                            const uint64_t addr_mask = (uint64_t{mx} << c->x_range.offset) |
                                                       (uint64_t{my} << c->y_range.offset);
                            axi::AwBeat b{};
                            b.addr = anchor;
                            b.size = 3;  // 8 B, fits the 4 KB config aperture
                            b.burst = axi::Burst::INCR;
                            b.user =
                                (addr_mask << 10) | (uint64_t{axi::COLLECTIVE_OP_MULTICAST} << 8);
                            const uint8_t sliced =
                                static_cast<uint8_t>((my << ni::width::X_WIDTH) | mx);
                            EXPECT_EQ(collective_translate(sam, b), sliced)
                                << "anchor (" << ax << "," << ay << ") mask 0x" << std::hex
                                << addr_mask;
                            ++compared;
                        }
                    }
                }
            }
        }
    }
    // 5 topologies x 2 spaces: 2x2 gives 4 anchors x 3 masks, each 4x4 gives
    // 16 x 15. Guards against the loops silently collapsing to nothing.
    EXPECT_EQ(compared, 2u * (4u * 3u + 4u * 16u * 15u));
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
