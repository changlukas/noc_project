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

TEST(SamYaml, PackedTilesBaseFromCoordinateAndSlot) {
    // x_dim = 2 -> x_bits = 1, slot = largest declared size = 0x2000.
    // base(1,0) = ((0<<1)|1) * 0x2000 -- the coordinate times the slot, not
    // list-order accumulation.
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
    EXPECT_EQ(sam.entries()[1].base, 0x2000ull);
}

TEST(SamYaml, TranslateForwardsTheAddressFromAPackedMap) {
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
    EXPECT_EQ(t.local_addr, 0x300000040ull);  // forwarded unchanged
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
    EXPECT_EQ(memory.local_addr, 0x1000ull);  // forwarded unchanged

    // The config tile is the 5th entry: base = sum of the 4 memory tiles'
    // sizes = 4 * 0x100000 = 0x400000, size 0x1000.
    auto config = sam.translate(0x400010);
    EXPECT_EQ(config.dst_id, 0x00u);
    EXPECT_EQ(config.cls, axi::AxiClass::Narrow);
    EXPECT_EQ(config.local_addr, 0x400010ull);

    // One node's two spaces stay at distinct addresses, which is what the tile
    // decoder needs. They are distinct because the map itself put them apart,
    // not because anything rebased them.
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

// Reference model for the differential below: the 2^n walk over the SAM that
// collective_translate ran before B2, kept here rather than deleted with it.
// The production side now slices the declared ranges, so without an independent
// second derivation the differential would compare the slice against itself.
// Returns 0xFF -- never a legal node mask on a shipped topology -- if a named
// address reaches no tile.
uint8_t enumerated_node_mask(const ni::cmodel::nmu::addr_trans::SamTable& sam, uint64_t anchor,
                             uint64_t addr_mask) {
    std::vector<unsigned> pos;
    for (unsigned i = 0; i < 48; ++i) {
        if ((addr_mask >> i) & 1u) pos.push_back(i);
    }
    const uint64_t base = anchor & ~addr_mask;
    const auto* origin = sam.lookup(base);
    if (origin == nullptr) return 0xFF;
    uint8_t node_mask = 0;
    for (uint64_t v = 0; v < (uint64_t{1} << pos.size()); ++v) {
        uint64_t addr = base;
        for (std::size_t k = 0; k < pos.size(); ++k) {
            if ((v >> k) & 1u) addr |= uint64_t{1} << pos[k];
        }
        const auto* e = sam.lookup(addr);
        if (e == nullptr) return 0xFF;
        node_mask |= static_cast<uint8_t>(e->dst_id ^ origin->dst_id);
    }
    return node_mask;
}

// B1/B2 differential: the node mask collective_translate reads off the declared
// ranges must equal the one the enumeration above walks out of the SAM.
// Exhaustive over every shipped topology, both spaces, every anchor node and
// every legal mask shape -- at most 2^(x_bits + y_bits) masks per space. This
// is the equivalence evidence for B2 replacing the enumeration in the datapath.
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
                            const uint8_t enumerated = enumerated_node_mask(sam, anchor, addr_mask);
                            // The reference is not vacuous: an aligned wildcard
                            // over a raster-packed space IS the (my, mx) pair.
                            ASSERT_EQ(enumerated,
                                      static_cast<uint8_t>((my << ni::width::X_WIDTH) | mx))
                                << "anchor (" << ax << "," << ay << ") mask 0x" << std::hex
                                << addr_mask;
                            EXPECT_EQ(collective_translate(sam, b), enumerated)
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

// === Decode mode (spec §5.1) ===
//
// Offset decode holds one coordinate range pair for the whole map, so it is
// legal only where every space puts its node index at the same address bits.
// Nothing else about the map changes, which is why the same tile list is legal
// under table decode and rejected under offset decode below.

// Equal region size in both spaces: memory 4 KB at 0x0, config 4 KB at 0x4000,
// so both node-index fields land at [13:12] and one pair reaches both.
static const char* kEqualSizedSpaces =
    "    - { x: 0, y: 0, size: 0x1000 }\n"
    "    - { x: 1, y: 0, size: 0x1000 }\n"
    "    - { x: 0, y: 1, size: 0x1000 }\n"
    "    - { x: 1, y: 1, size: 0x1000 }\n"
    "    - { x: 0, y: 0, size: 0x1000, space: config }\n"
    "    - { x: 1, y: 0, size: 0x1000, space: config }\n"
    "    - { x: 0, y: 1, size: 0x1000, space: config }\n"
    "    - { x: 1, y: 1, size: 0x1000, space: config }\n";

// The shipped shape: memory 1 MB, config 4 KB. Node index at [21:20] and
// [13:12] -- legal under table decode, unreachable by one global pair.
static const char* kShippedSizedSpaces =
    "    - { x: 0, y: 0, size: 0x100000 }\n"
    "    - { x: 1, y: 0, size: 0x100000 }\n"
    "    - { x: 0, y: 1, size: 0x100000 }\n"
    "    - { x: 1, y: 1, size: 0x100000 }\n"
    "    - { x: 0, y: 0, size: 0x1000, space: config }\n"
    "    - { x: 1, y: 0, size: 0x1000, space: config }\n"
    "    - { x: 0, y: 1, size: 0x1000, space: config }\n"
    "    - { x: 1, y: 1, size: 0x1000, space: config }\n";

// Returns what unique_temp_path already gives, a std::string. Declaring
// filesystem::path here converted it on the way out and left load_sam_table's
// const std::string& with no way back: string -> path is implicit, path ->
// string is not.
static std::string write_map(const char* name, const char* decode, const char* tiles) {
    auto path = ni::cmodel::testing::unique_temp_path(name);
    std::ofstream(path) << "topology: { name: t, x_dim: 2, y_dim: 2, num_vc: 1 }\n"
                           "address_map:\n"
                        << "  decode: " << decode << "\n"
                        << "  tiles:\n"
                        << tiles;
    return path;
}

TEST(SamYaml, OffsetDecodeAcceptsEqualSizedSpaces) {
    auto sam = load_sam_table(write_map("sam_offset_ok.yaml", "offset", kEqualSizedSpaces));
    const auto* mem = sam.collective_coords(axi::AxiClass::Data);
    const auto* cfg = sam.collective_coords(axi::AxiClass::Narrow);
    ASSERT_NE(mem, nullptr);
    ASSERT_NE(cfg, nullptr);
    EXPECT_EQ(mem->x_range.offset, cfg->x_range.offset);  // the one global pair
    EXPECT_EQ(mem->y_range.offset, cfg->y_range.offset);
}

TEST(SamYaml, OffsetDecodeRejectsUnequalSpaceSizes) {
    auto path = write_map("sam_offset_unequal.yaml", "offset", kShippedSizedSpaces);
    EXPECT_DEATH(load_sam_table(path), "same region size");
}

TEST(SamYaml, TableDecodeAcceptsWhatOffsetRejects) {
    auto sam = load_sam_table(write_map("sam_table_unequal.yaml", "table", kShippedSizedSpaces));
    EXPECT_EQ(sam.entries().size(), 8u);
}

TEST(SamYaml, UnknownDecodeModeRejected) {
    auto path = write_map("sam_bad_decode.yaml", "slice", kEqualSizedSpaces);
    EXPECT_DEATH(load_sam_table(path), "table");
}
