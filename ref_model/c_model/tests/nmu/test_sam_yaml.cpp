#include "nmu/sam_yaml.hpp"
#include "axi/types.hpp"
#include "common/tmp_path.hpp"
#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using ni::cmodel::nmu::addr_trans::collective_translate;
using ni::cmodel::nmu::addr_trans::load_sam_table;
using ni::cmodel::nmu::addr_trans::SamEntry;
using ni::cmodel::nmu::addr_trans::SamTable;
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

// Guards the real shipped configs. CONFIG_DIR is sim/configs/ itself
// (CMakeLists.txt), globbed rather than listed, so a new config cannot fall out
// of coverage.
//
// The claim is the packing formula, base = range.base + range.stride *
// ((y << clog2(x_dim)) | x), spelled out here from the config keys instead of
// read back from SamTable::packed(). This is one half of the bit-identity with
// sim/tools/address_map.py that the model and the stimulus generator both
// depend on: this test holds SamTable::packed() to the formula, and the Python
// twin (test_address_map_pack_real_configs_at_the_coordinate_formula in
// sim/tools/test_gen_test_patterns_filemaster.py) holds pack_document() to it
// over the same files. Both passing is what makes the two sides identical.
// The member index is X-fast, which is this repo's node numbering and not
// FlooNoC's Y-fast one -- on a square mesh the two produce the same SET of
// bases and transpose every coordinate, so a set comparison would say nothing.
TEST(SamYaml, RealConfigsPackedAtTheCoordinateFormula) {
    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(CONFIG_DIR)) {
        if (entry.path().extension() == ".yml") files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());
    // Same floor the Python twin asserts. It guards the scan, not the inventory:
    // an empty or unreachable directory must fail rather than pass vacuously.
    // A count tied to how many configs ship would have to be edited every time
    // one is added or removed, which is exactly what the glob exists to avoid.
    ASSERT_FALSE(files.empty()) << "expected the real configs in " CONFIG_DIR;
    for (const auto& file : files) {
        SCOPED_TRACE(file);
        YAML::Node root = YAML::LoadFile(file);
        const unsigned x_dim = root["routers"][0]["array"][0].as<unsigned>();
        const unsigned x_bits = ni::cmodel::address_map::clog2(x_dim);
        // The tile endpoint's two declared ranges, by space. A peripheral
        // endpoint is skipped: its members are placed by their own connections,
        // so the tile coordinate formula says nothing about them.
        uint64_t base[2] = {0, 0};
        uint64_t stride[2] = {0, 0};
        for (const auto& ep : root["endpoints"]) {
            if (!ep["array"]) continue;  // the tile array is the only array endpoint
            for (const auto& r : ep["addr_range"]) {
                const bool is_config = r["space"] && r["space"].as<std::string>() == "config";
                base[is_config] = r["base"].as<uint64_t>();
                stride[is_config] =
                    r["stride"] ? r["stride"].as<uint64_t>() : r["size"].as<uint64_t>();
            }
        }
        ASSERT_NE(stride[0], 0u) << "no memory range on the tile endpoint";

        auto sam = load_sam_table(file);
        ASSERT_FALSE(sam.entries().empty());
        for (const auto& e : sam.entries()) {
            // The tile spaces only. A peripheral region is placed by its own
            // connection above the tile array, so the coordinate formula says
            // nothing about it -- it shares its host router's coordinate, which
            // the router's own tile already packs at.
            if (e.space == axi::Space::Peripheral) continue;
            const unsigned x = e.dst_id & ((1u << ni::width::X_WIDTH) - 1);
            const unsigned y = e.dst_id >> ni::width::X_WIDTH;
            const bool is_config = e.cls == axi::AxiClass::Narrow;
            const uint64_t expected =
                base[is_config] + stride[is_config] * ((uint64_t{y} << x_bits) | x);
            EXPECT_EQ(e.base, expected) << "dst_id " << std::hex << unsigned{e.dst_id};
        }
    }
}

// The clip deletion in round 3 rests on the coordinate field being exactly as
// wide as the dimension, which holds only for a power of two: a 3-wide mesh
// gives a 2-bit x field spanning four coordinates while only three exist, so a
// collective wildcard would name a node with no router. Refused at load, while
// the topology is still a document and not a mesh.
TEST(SamYamlDeath, ANonPowerOfTwoMeshDimensionIsRejected) {
    auto path_x = ni::cmodel::testing::unique_temp_path("sam_dim_x3.yaml");
    std::ofstream(path_x) << "topology: { name: t, x_dim: 3, y_dim: 2, num_vc: 1 }\n"
                             "address_map:\n"
                             "  tiles:\n"
                             "    - { x: 0, y: 0, size: 0x1000 }\n"
                             "    - { x: 1, y: 0, size: 0x1000 }\n"
                             "    - { x: 2, y: 0, size: 0x1000 }\n"
                             "    - { x: 0, y: 1, size: 0x1000 }\n"
                             "    - { x: 1, y: 1, size: 0x1000 }\n"
                             "    - { x: 2, y: 1, size: 0x1000 }\n";
    EXPECT_DEATH(load_sam_table(path_x), "powers of two");

    // Both axes, because a check written against x alone would still pass here.
    auto path_y = ni::cmodel::testing::unique_temp_path("sam_dim_y3.yaml");
    std::ofstream(path_y) << "topology: { name: t, x_dim: 2, y_dim: 3, num_vc: 1 }\n"
                             "address_map:\n"
                             "  tiles:\n"
                             "    - { x: 0, y: 0, size: 0x1000 }\n"
                             "    - { x: 1, y: 0, size: 0x1000 }\n"
                             "    - { x: 0, y: 1, size: 0x1000 }\n"
                             "    - { x: 1, y: 1, size: 0x1000 }\n"
                             "    - { x: 0, y: 2, size: 0x1000 }\n"
                             "    - { x: 1, y: 2, size: 0x1000 }\n";
    EXPECT_DEATH(load_sam_table(path_y), "powers of two");
}

TEST(SamYaml, TileMajorPacksEachNodeIntoOneBlock) {
    const SamTable t = load_sam_table(CONFIG_DIR "/mesh_2x2.yml");
    constexpr uint64_t kBlock = 0x100000000ull;
    for (unsigned idx = 0; idx < 4; ++idx) {
        const SamEntry* mem = t.lookup(idx * kBlock);
        ASSERT_NE(mem, nullptr);
        EXPECT_EQ(mem->base, idx * kBlock);
        EXPECT_EQ(mem->size, 0x2000000ull);
        EXPECT_EQ(mem->cls, axi::AxiClass::Data);
        const SamEntry* cfg = t.lookup(idx * kBlock + 0x2000000ull);
        ASSERT_NE(cfg, nullptr);
        EXPECT_EQ(cfg->base, idx * kBlock + 0x2000000ull);
        EXPECT_EQ(cfg->size, 0x1000ull);
        EXPECT_EQ(cfg->cls, axi::AxiClass::Narrow);
    }
}

// SAM class selection from the config's addr_range space attribute
// (docs/noc-target-spec.md §5). mesh_2x2.yml gives every node both a
// memory tile (default space) and a config tile.
TEST(SamYaml, SpaceAttributeSelectsClass) {
    auto sam = load_sam_table(CONFIG_DIR "/mesh_2x2.yml");
    // Node (0,0)'s memory tile is [0, 0x2000000), inside its own
    // 0x100000000 block.
    auto memory = sam.translate(0x1000);
    EXPECT_EQ(memory.dst_id, 0x00u);
    EXPECT_EQ(memory.cls, axi::AxiClass::Data);
    EXPECT_EQ(memory.local_addr, 0x1000ull);  // forwarded unchanged

    // The config tile sits inside node (0,0)'s own block, above its memory
    // tile: base = 0x2000000, size 0x1000.
    auto config = sam.translate(0x2000010ull);
    EXPECT_EQ(config.dst_id, 0x00u);
    EXPECT_EQ(config.cls, axi::AxiClass::Narrow);
    EXPECT_EQ(config.local_addr, 0x2000010ull);

    // One node's two spaces stay at distinct addresses, which is what the tile
    // decoder needs. They are distinct because the map itself put them apart,
    // not because anything rebased them -- and they are two different SPACES
    // (Data vs Narrow), not just two offsets inside the same one, which the
    // local_addr compare alone cannot tell apart since translate() forwards
    // the address unchanged.
    EXPECT_NE(memory.local_addr, config.local_addr);
    EXPECT_NE(memory.cls, config.cls);
}

// The ranges the loader derives from the shipped configs, spelled out.
// Tile-major packing gives every space the same node stride (the declared
// range stride, 4 GiB in every shipped config), so both spaces' offsets are 32;
// the lengths are clog2 of the mesh dimension.
//
// declare_space_coords (sam_yaml.hpp) RETURNS FALSE rather than aborting when
// a space stops being collective-eligible, so a regression there would surface
// only as a multicast refused at the source, nothing at build time. The
// ASSERT_NE below is what stands in for that missing abort. All four shipped
// configs are walked; the y_range cross-space EXPECT_EQ is
// the field-identity offset decode requires (spec §5.1) and that offset == 32
// alone does not pin, since both spaces could each be internally offset-32
// with a mismatched y term.
TEST(SamYaml, CoordRangesDerivedFromTheBlockStride) {
    struct Row {
        const char* file;
        unsigned dim_bits;
    } rows[] = {{"/mesh_2x2.yml", 1},
                {"/mesh_2x2_periph.yml", 1},
                {"/mesh_4x4.yml", 2},
                {"/mesh_4x4_periph4.yml", 2}};
    for (const auto& row : rows) {
        SCOPED_TRACE(row.file);
        auto sam = load_sam_table(std::string(CONFIG_DIR) + row.file);
        const auto* memory = sam.collective_coords(axi::Space::Memory);
        ASSERT_NE(memory, nullptr) << "memory space is not a collective target";
        EXPECT_EQ(memory->x_range.offset, 32u);  // log2(block_size, 4 GiB)
        EXPECT_EQ(memory->x_range.len, row.dim_bits);
        EXPECT_EQ(memory->y_range.offset, 32u + row.dim_bits);
        EXPECT_EQ(memory->y_range.len, row.dim_bits);
        const auto* config = sam.collective_coords(axi::Space::Config);
        ASSERT_NE(config, nullptr) << "config space is not a collective target";
        EXPECT_EQ(config->x_range.offset, 32u);  // same block_size stride
        EXPECT_EQ(config->x_range.len, row.dim_bits);
        EXPECT_EQ(config->y_range.offset, 32u + row.dim_bits);
        EXPECT_EQ(config->y_range.len, row.dim_bits);
        // Both spaces put the node index at the same bits, not just the same
        // x offset -- the property offset decode (spec §5.1) needs.
        EXPECT_EQ(memory->y_range.offset, config->y_range.offset);
    }
}

// Every shipped config that declares peripherals, so a four-face map is held
// to the policy the one-peripheral map states.
TEST(SamYaml, PeripheralSpaceIsNotACollectiveTarget) {
    // The loader declares only the tile spaces. A peripheral space's bases are
    // assigned in declaration order at arbitrary sizes, so there is no uniform
    // power-of-two stride to read a coordinate field from -- the declaration is
    // not attempted, rather than attempted and failed.
    const char* files[] = {"/mesh_2x2_periph.yml", "/mesh_4x4_periph4.yml"};
    for (const char* file : files) {
        SCOPED_TRACE(file);
        auto sam = load_sam_table(std::string(CONFIG_DIR) + file);
        EXPECT_NE(sam.collective_coords(axi::Space::Memory), nullptr);
        EXPECT_NE(sam.collective_coords(axi::Space::Config), nullptr);
        EXPECT_EQ(sam.collective_coords(axi::Space::Peripheral), nullptr)
            << "a peripheral must never be a collective target";
    }
}

TEST(SamYaml, APeripheralRegionIsReachableAndCarriesItsPortAndSpace) {
    // The first peripheral member: router (0, 0)'s WEST port, so port 1, sharing
    // that router's coordinate.
    // coordinate. Its region sits above the tile array -- 2x2 tiles at a
    // 0x100000000 block stride put the top of the array at 0x400000000.
    auto sam = load_sam_table(std::string(CONFIG_DIR) + "/mesh_2x2_periph.yml");
    const auto t = sam.translate(0x400000000ull);
    EXPECT_EQ(t.space, axi::Space::Peripheral);
    EXPECT_EQ(t.port, 1u);
    EXPECT_EQ(t.dst_id, 0x00u) << "a peripheral shares its host router's coordinate";
}

// A memory-only x_dim by y_dim mesh plus one peripheral, for the face-legality
// death cases below. Written out rather than listed because the smallest mesh
// with an INTERIOR coordinate is 4 wide, and a hand-listed 4x2 tile map is eight
// lines per fixture that say nothing.
static std::string write_peripheral_map(const char* name, unsigned x_dim, unsigned y_dim,
                                        unsigned px, unsigned py, const char* face) {
    auto path = ni::cmodel::testing::unique_temp_path(name);
    std::ofstream out(path);
    out << "topology: { name: t, x_dim: " << x_dim << ", y_dim: " << y_dim
        << ", num_vc: 1 }\naddress_map:\n  block_size: 0x100000000\n  tiles:\n";
    for (unsigned y = 0; y < y_dim; ++y) {
        for (unsigned x = 0; x < x_dim; ++x) {
            out << "    - { x: " << x << ", y: " << y << ", size: 0x1000 }\n";
        }
    }
    out << "  peripherals:\n    - { x: " << px << ", y: " << py << ", face: " << face
        << ", size: 0x1000 }\n";
    return path;
}

// The face assert is the deadlock-freedom precondition, not input tidiness: a
// boundary port on an edge router is terminal, while the same port on an
// interior router carries a live inter-router link and the peripheral's Y-to-X
// ejection turn closes a channel dependency cycle. No shipped topology can make
// it fire -- on a 2x2 every coordinate is on both edges, and the four-face
// topology puts every peripheral on an edge deliberately -- so this is the only
// place the guard is exercised at all.
//
// A dimension of 4 is the smallest with an interior coordinate: x = 1 on a
// 4-wide mesh is on neither x edge, y = 1 on a 4-tall one is on neither y edge.
// Both axes, because the condition reversed -- on_y_edge for face "x" -- would
// still pass an x-only case.
TEST(SamYamlDeath, APeripheralFaceMustNameAnEdgeItsCoordinateIsOn) {
    EXPECT_DEATH(load_sam_table(write_peripheral_map("sam_face_x.yaml", 4, 2, 1, 0, "x")),
                 "face names an edge");
    EXPECT_DEATH(load_sam_table(write_peripheral_map("sam_face_y.yaml", 2, 4, 0, 1, "y")),
                 "face names an edge");
}

TEST(SamYaml, MemorySpaceStaysACollectiveTargetAlongsideAPeripheral) {
    // The regression this task exists to prevent: keyed on class, a peripheral
    // carrying the Data class joins the memory space's tile walk, the walk's
    // count check fails, and the memory space silently loses eligibility.
    auto sam = load_sam_table(std::string(CONFIG_DIR) + "/mesh_2x2_periph.yml");
    const auto* memory = sam.collective_coords(axi::Space::Memory);
    ASSERT_NE(memory, nullptr);
    EXPECT_EQ(memory->x_range.offset, 32u);  // log2(block_size, 4 GiB)
}

// Reference model for the differential below: the 2^n walk over the SAM that
// collective_translate ran before B2, kept here rather than deleted with it.
// The production side now slices the declared ranges, so without an independent
// second derivation the differential would compare the slice against itself.
// Returns 0xFF -- never a legal node mask on a shipped topology -- if a named
// address reaches no tile.
uint8_t enumerated_node_mask(const ni::cmodel::nmu::addr_trans::SamTable& sam, uint64_t addr,
                             uint64_t addr_mask) {
    std::vector<unsigned> pos;
    for (unsigned i = 0; i < 48; ++i) {
        if ((addr_mask >> i) & 1u) pos.push_back(i);
    }
    const uint64_t base = addr & ~addr_mask;
    const auto* origin = sam.lookup(base);
    if (origin == nullptr) return 0xFF;
    uint8_t node_mask = 0;
    for (uint64_t v = 0; v < (uint64_t{1} << pos.size()); ++v) {
        uint64_t member = base;
        for (std::size_t k = 0; k < pos.size(); ++k) {
            if ((v >> k) & 1u) member |= uint64_t{1} << pos[k];
        }
        const auto* e = sam.lookup(member);
        if (e == nullptr) return 0xFF;
        node_mask |= static_cast<uint8_t>(e->dst_id ^ origin->dst_id);
    }
    return node_mask;
}

// B1/B2 differential: the node mask collective_translate reads off the declared
// ranges must equal the one the enumeration above walks out of the SAM.
// Exhaustive over every shipped topology, both spaces, every addressed node and
// every legal mask shape -- at most 2^(x_bits + y_bits) masks per space. This
// is the equivalence evidence for B2 replacing the enumeration in the datapath.
//
// Every (address, mask) pair the walk builds is legal: a mask confined to the
// coordinate field of a power-of-two dimension expands over exactly the
// coordinates that exist, so nothing is filtered out.
TEST(SamYaml, SlicedNodeMaskMatchesTheEnumeratedOne) {
    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(CONFIG_DIR)) {
        if (entry.path().extension() == ".yml") files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());
    ASSERT_FALSE(files.empty()) << "expected the real configs in " CONFIG_DIR;

    unsigned compared = 0;
    unsigned expected = 0;
    for (const auto& file : files) {
        SCOPED_TRACE(file);
        auto sam = load_sam_table(file);
        for (axi::Space space : {axi::Space::Config, axi::Space::Memory}) {
            const auto* c = sam.collective_coords(space);
            ASSERT_NE(c, nullptr) << "every shipped tile space is collective-eligible";
            uint64_t origin = 0;
            for (const auto& e : sam.entries()) {
                if (e.space == space) {
                    origin = e.base;
                    break;
                }
            }
            unsigned compared_here = 0;
            for (unsigned ay = 0; ay < c->y_count; ++ay) {
                for (unsigned ax = 0; ax < c->x_count; ++ax) {
                    const uint64_t addr = origin | (uint64_t{ax} << c->x_range.offset) |
                                          (uint64_t{ay} << c->y_range.offset);
                    for (unsigned my = 0; my < (1u << c->y_range.len); ++my) {
                        for (unsigned mx = 0; mx < (1u << c->x_range.len); ++mx) {
                            if (mx == 0 && my == 0) continue;  // empty set: rejected, not compared
                            const uint64_t addr_mask = (uint64_t{mx} << c->x_range.offset) |
                                                       (uint64_t{my} << c->y_range.offset);
                            axi::AwBeat b{};
                            b.addr = addr;
                            b.size = 3;  // 8 B, fits the 4 KB config aperture
                            b.burst = axi::Burst::INCR;
                            b.user =
                                (addr_mask << 10) | (uint64_t{axi::COLLECTIVE_OP_MULTICAST} << 8);
                            const uint8_t enumerated = enumerated_node_mask(sam, addr, addr_mask);
                            // The reference is not vacuous: an aligned wildcard
                            // over a raster-packed space IS the (my, mx) pair.
                            ASSERT_EQ(enumerated,
                                      static_cast<uint8_t>((my << ni::width::X_WIDTH) | mx))
                                << "address (" << ax << "," << ay << ") mask 0x" << std::hex
                                << addr_mask;
                            // Issued from a tile: port 0 is the router's LOCAL
                            // port, and only a tile may issue.
                            EXPECT_EQ(collective_translate(sam, b, /*src_port=*/0), enumerated)
                                << "address (" << ax << "," << ay << ") mask 0x" << std::hex
                                << addr_mask;
                            ++compared;
                            ++compared_here;
                        }
                    }
                }
            }
            // Exact count, derived from THIS space's own coordinates rather
            // than hard-coded, so a new topology adds coverage instead of
            // reading as a regression: every node addressed under every mask
            // shape but the empty one -- 240 for a 4x4, 12 for a 2x2. Catches a
            // coverage collapse, which is what the per-pair ASSERT_EQ above
            // cannot see -- it only guards the pairs walked.
            const unsigned expected_here =
                c->x_count * c->y_count * ((1u << (c->x_range.len + c->y_range.len)) - 1);
            EXPECT_EQ(compared_here, expected_here);
            expected += expected_here;
        }
    }
    EXPECT_EQ(compared, expected);
    EXPECT_GT(compared, 0u) << "the topology directory produced no legal collective at all";
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

// Unequal-sized spaces, self-contained (not the shipped tile size, which is
// now 4 GiB): memory 1 MB, config 4 KB. Under tile-major both spaces still
// share one node stride (block_size, not the individual region sizes), so the
// node index sits at the same bit position in both -- legal under table
// decode AND offset decode, unlike the old per-space-sized stride this
// fixture used to demonstrate the rejection of.
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
    const auto* mem = sam.collective_coords(axi::Space::Memory);
    const auto* cfg = sam.collective_coords(axi::Space::Config);
    ASSERT_NE(mem, nullptr);
    ASSERT_NE(cfg, nullptr);
    EXPECT_EQ(mem->x_range.offset, cfg->x_range.offset);  // the one global pair
    EXPECT_EQ(mem->y_range.offset, cfg->y_range.offset);
}

// Tile-major gives every space the same node stride (block_size), so decode
// 'offset' -- one coordinate range pair for the whole map -- is satisfied by
// construction, whatever the spaces' own region sizes are. This fixture used
// to be the offset-decode rejection case; it is not one anymore.
TEST(SamYaml, OffsetDecodeIsSatisfiedByConstructionUnderTileMajor) {
    auto sam = load_sam_table(write_map("sam_offset_unequal.yaml", "offset", kShippedSizedSpaces));
    const auto* mem = sam.collective_coords(axi::Space::Memory);
    const auto* cfg = sam.collective_coords(axi::Space::Config);
    ASSERT_NE(mem, nullptr);
    ASSERT_NE(cfg, nullptr);
    EXPECT_EQ(mem->x_range.offset, cfg->x_range.offset);  // the one global pair
    EXPECT_EQ(mem->y_range.offset, cfg->y_range.offset);
}

TEST(SamYaml, TableDecodeAcceptsUnequalSpaceSizes) {
    auto sam = load_sam_table(write_map("sam_table_unequal.yaml", "table", kShippedSizedSpaces));
    EXPECT_EQ(sam.entries().size(), 8u);
}

TEST(SamYaml, UnknownDecodeModeRejected) {
    auto path = write_map("sam_bad_decode.yaml", "slice", kEqualSizedSpaces);
    EXPECT_DEATH(load_sam_table(path), "table");
}
