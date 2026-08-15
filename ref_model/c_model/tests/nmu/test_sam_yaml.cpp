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

// Guards the real topology configs. TOPOLOGY_DIR is sim/topologies/ itself
// (CMakeLists.txt), globbed rather than listed, so a new topology YAML cannot
// fall out of coverage.
//
// The claim is the packing formula, base = ((y << clog2(x_span)) | x) *
// block_size + offset[space], spelled out here from the YAML keys instead of
// read back from SamTable::packed(). This is one half of the bit-identity with
// sim/tools/address_map.py that the model and the stimulus generator both
// depend on: this test holds SamTable::packed() to the formula, and the Python
// twin (test_address_map_pack_real_topologies_at_the_coordinate_formula in
// sim/tools/test_gen_test_patterns_filemaster.py) holds pack() to it over the
// same files. Both passing is what makes the two sides identical. List-order
// accumulation (base += size) agrees with the formula only where the span is a
// power of two and every entry in a space is one slot, which is true of every
// topology shipped today and not of a span with a border coordinate.
TEST(SamYaml, RealTopologiesPackedAtTheCoordinateFormula) {
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
        YAML::Node root = YAML::LoadFile(file);
        YAML::Node topo = root["topology"];
        const unsigned x_span =
            topo["x_span"] ? topo["x_span"].as<unsigned>() : topo["x_dim"].as<unsigned>();
        const unsigned y_span =
            topo["y_span"] ? topo["y_span"].as<unsigned>() : topo["y_dim"].as<unsigned>();
        // Slot per space: the largest size declared in it, bounding the
        // aperture. block_size is the declared node stride.
        uint64_t memory_slot = 0;
        uint64_t config_slot = 0;
        for (const auto& tile : root["address_map"]["tiles"]) {
            const bool is_config = tile["space"] && tile["space"].as<std::string>() == "config";
            uint64_t& slot = is_config ? config_slot : memory_slot;
            slot = std::max(slot, tile["size"].as<uint64_t>());
        }
        const unsigned x_bits = ni::cmodel::address_map::clog2(x_span);
        const uint64_t block = root["address_map"]["block_size"].as<uint64_t>();
        // Spaces sit inside a node's block, memory first at 0.
        const uint64_t config_offset =
            ((memory_slot + config_slot - 1) / config_slot) * config_slot;

        auto sam = load_sam_table(file);
        ASSERT_FALSE(sam.entries().empty());
        for (const auto& e : sam.entries()) {
            const unsigned x = e.dst_id & ((1u << ni::width::X_WIDTH) - 1);
            const unsigned y = e.dst_id >> ni::width::X_WIDTH;
            const bool is_config = e.cls == axi::AxiClass::Narrow;
            const uint64_t expected =
                ((uint64_t{(y << x_bits) | x}) * block) + (is_config ? config_offset : 0);
            EXPECT_EQ(e.base, expected) << "dst_id " << std::hex << unsigned{e.dst_id};
        }
    }
}

// A route span wider than the router array, with the tile region naming which
// coordinates are tiles: the design's worked example, x = 0 a peripheral column
// on both rows. x_span = 3 gives x_bits = clog2(3) = 2, so a row strides four
// slots and row 1 starts at 0x400000 -- 3 * 0x100000, which a 2-wide span would
// give, is what this guards against.
TEST(SamYaml, StatedSpanPacksOverTheSpanAndKeepsTheTileRegion) {
    auto path = ni::cmodel::testing::unique_temp_path("sam_span_region.yaml");
    std::ofstream(path) << "topology:\n"
                           "  name: t\n"
                           "  x_dim: 2\n"
                           "  y_dim: 2\n"
                           "  num_vc: 1\n"
                           "  x_span: 3\n"
                           "  tile_x_first: 1\n"
                           "  tile_x_last: 2\n"
                           "address_map:\n"
                           "  tiles:\n"
                           "    - { x: 0, y: 0, size: 0x100000 }\n"
                           "    - { x: 1, y: 0, size: 0x100000 }\n"
                           "    - { x: 2, y: 0, size: 0x100000 }\n"
                           "    - { x: 0, y: 1, size: 0x100000 }\n"
                           "    - { x: 1, y: 1, size: 0x100000 }\n"
                           "    - { x: 2, y: 1, size: 0x100000 }\n";
    auto sam = load_sam_table(path);
    ASSERT_EQ(sam.entries().size(), 6u);
    const uint64_t expected[] = {0x000000ull, 0x100000ull, 0x200000ull,
                                 0x400000ull, 0x500000ull, 0x600000ull};
    for (std::size_t i = 0; i < sam.entries().size(); ++i) {
        EXPECT_EQ(sam.entries()[i].base, expected[i]) << "entry " << i;
    }
    // The peripheral column is inside the span but outside the tile region, so
    // it is an addressable node and not a collective member.
    const auto* memory = sam.collective_coords(axi::AxiClass::Data);
    ASSERT_NE(memory, nullptr);
    EXPECT_EQ(memory->x_count, 3u);
    EXPECT_EQ(memory->x_first, 1u);
    EXPECT_EQ(memory->x_last, 2u);
    EXPECT_EQ(memory->y_first, 0u);
    EXPECT_EQ(memory->y_last, 1u);
}

// A wider span with no stated tile region defaults the region to the whole
// span (x = 0..2), which must still have extent == x_dim -- otherwise the
// region silently covers the peripheral column and disarms
// check_dst_reachable's cross-row guard by omission instead of by a rejected
// declaration. Symmetric on y.
TEST(SamYamlDeath, TileRegionExtentMustEqualTheRouterArray) {
    auto path_x = ni::cmodel::testing::unique_temp_path("sam_region_extent_x.yaml");
    std::ofstream(path_x) << "topology:\n"
                             "  name: t\n"
                             "  x_dim: 2\n"
                             "  y_dim: 2\n"
                             "  num_vc: 1\n"
                             "  x_span: 3\n"
                             "address_map:\n"
                             "  tiles:\n"
                             "    - { x: 0, y: 0, size: 0x100000 }\n"
                             "    - { x: 1, y: 0, size: 0x100000 }\n"
                             "    - { x: 2, y: 0, size: 0x100000 }\n"
                             "    - { x: 0, y: 1, size: 0x100000 }\n"
                             "    - { x: 1, y: 1, size: 0x100000 }\n"
                             "    - { x: 2, y: 1, size: 0x100000 }\n";
    EXPECT_DEATH(load_sam_table(path_x), "tile x region extent");

    auto path_y = ni::cmodel::testing::unique_temp_path("sam_region_extent_y.yaml");
    std::ofstream(path_y) << "topology:\n"
                             "  name: t\n"
                             "  x_dim: 2\n"
                             "  y_dim: 2\n"
                             "  num_vc: 1\n"
                             "  y_span: 3\n"
                             "address_map:\n"
                             "  tiles:\n"
                             "    - { x: 0, y: 0, size: 0x100000 }\n"
                             "    - { x: 1, y: 0, size: 0x100000 }\n"
                             "    - { x: 0, y: 1, size: 0x100000 }\n"
                             "    - { x: 1, y: 1, size: 0x100000 }\n"
                             "    - { x: 0, y: 2, size: 0x100000 }\n"
                             "    - { x: 1, y: 2, size: 0x100000 }\n";
    EXPECT_DEATH(load_sam_table(path_y), "tile y region extent");
}

// A stated tile region is what arms check_dst_reachable, and the guard reads
// the same declaration collective eligibility does -- so a declaration the
// entries reject would take the guard down with it, silently.
//
// Tile-major gives every space one power-of-two stride (block_size, enforced
// by SamTable::packed()), so a non-power-of-two SIZE can no longer defeat the
// declaration the way it used to. What still can: declare_space_coords
// (sam_yaml.hpp) derives the stride from the first two SAME-CLASS entries in
// table.entries() ORDER, not from their coordinates -- so tiles listed out of
// raster order still give a non-power-of-two stride between them. Here the
// list's second memory tile is (1,1), five slots from the first (0,0)'s, and
// 5 is not a power of two.
TEST(SamYamlDeath, AStatedTileRegionWhoseDeclarationIsRejected) {
    auto path = ni::cmodel::testing::unique_temp_path("sam_region_undeclarable.yaml");
    std::ofstream(path) << "topology:\n"
                           "  name: t\n"
                           "  x_dim: 2\n"
                           "  y_dim: 2\n"
                           "  num_vc: 1\n"
                           "  x_span: 3\n"
                           "  tile_x_first: 1\n"
                           "  tile_x_last: 2\n"
                           "address_map:\n"
                           "  tiles:\n"
                           "    - { x: 0, y: 0, size: 0x1000 }\n"
                           "    - { x: 1, y: 1, size: 0x1000 }\n"
                           "    - { x: 1, y: 0, size: 0x1000 }\n"
                           "    - { x: 2, y: 0, size: 0x1000 }\n"
                           "    - { x: 0, y: 1, size: 0x1000 }\n"
                           "    - { x: 2, y: 1, size: 0x1000 }\n";
    EXPECT_DEATH(load_sam_table(path), "a stated tile region needs every address space");
}

TEST(SamYaml, TileMajorPacksEachNodeIntoOneBlock) {
    const SamTable t = load_sam_table(TOPOLOGY_DIR "/mesh_2x2_vc1.yaml");
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

// SAM class selection from the topology YAML's tile.space attribute
// (docs/noc-target-spec.md §5). mesh_2x2_vc1.yaml gives every node both a
// memory tile (default space) and a config tile.
TEST(SamYaml, SpaceAttributeSelectsClass) {
    auto sam = load_sam_table(TOPOLOGY_DIR "/mesh_2x2_vc1.yaml");
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

// The ranges the loader derives from the shipped YAMLs, spelled out.
// Tile-major packing gives every space the same node stride (block_size,
// 4 GiB in every shipped topology), so both spaces' offsets are 32; the
// lengths are clog2 of the mesh dimension.
//
// declare_space_coords (sam_yaml.hpp) RETURNS FALSE rather than aborting when
// a space stops being collective-eligible, and the shipped topologies below
// state no tile region, so sam_yaml.hpp's own abort guard (region_stated)
// never arms for them -- a regression here would surface only as a
// multicast refused at the source, nothing at build time. The ASSERT_NE
// below is what stands in for that missing abort. mesh_4x4_vc4 adds the
// third shipped topology to the walk; the y_range cross-space EXPECT_EQ is
// the field-identity offset decode requires (spec §5.1) and that offset == 32
// alone does not pin, since both spaces could each be internally offset-32
// with a mismatched y term.
TEST(SamYaml, CoordRangesDerivedFromTheBlockStride) {
    struct Row {
        const char* file;
        unsigned dim_bits;
    } rows[] = {{"/mesh_2x2_vc1.yaml", 1}, {"/mesh_4x4_vc1.yaml", 2}, {"/mesh_4x4_vc4.yaml", 2}};
    for (const auto& row : rows) {
        SCOPED_TRACE(row.file);
        auto sam = load_sam_table(std::string(TOPOLOGY_DIR) + row.file);
        const auto* memory = sam.collective_coords(axi::AxiClass::Data);
        ASSERT_NE(memory, nullptr) << "memory space is not a collective target";
        EXPECT_EQ(memory->x_range.offset, 32u);  // log2(block_size, 4 GiB)
        EXPECT_EQ(memory->x_range.len, row.dim_bits);
        EXPECT_EQ(memory->y_range.offset, 32u + row.dim_bits);
        EXPECT_EQ(memory->y_range.len, row.dim_bits);
        const auto* config = sam.collective_coords(axi::AxiClass::Narrow);
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

// How many (address, mask) pairs on ONE axis have their wildcard box inside
// [first, last], which is what the walk below filters on. Counted from the BOX
// side rather than by re-testing each (address, mask): one box per (mask, fixed
// bits) pair, holding the 2^popcount(mask) addresses that differ only in masked
// bits. A second expression of the same set, so an edit to the walk's own
// filter moves one side and not the other.
//
// `count` is the span, which need not be 2^len: a peripheral topology's span is
// as wide as its coordinates reach, so the addresses above it are not walked.
unsigned axis_pairs_inside_region(unsigned count, unsigned len, unsigned first, unsigned last) {
    unsigned n = 0;
    for (unsigned m = 0; m < (1u << len); ++m) {
        for (unsigned f = 0; f < (1u << len); ++f) {
            if (f & m) continue;                        // f holds the box's FIXED bits
            if (f < first || (f | m) > last) continue;  // box leaves the region
            for (unsigned s = m;; s = (s - 1) & m) {    // the box's own members
                if ((f | s) < count) ++n;
                if (s == 0) break;
            }
        }
    }
    return n;
}

// B1/B2 differential: the node mask collective_translate reads off the declared
// ranges must equal the one the enumeration above walks out of the SAM.
// Exhaustive over every shipped topology, both spaces, every addressed node and
// every legal mask shape -- at most 2^(x_bits + y_bits) masks per space. This
// is the equivalence evidence for B2 replacing the enumeration in the datapath.
//
// Walks only the (address, mask) pairs whose raw wildcard closure already lies
// inside the tile region, which is where clipping is a no-op. The subject here
// is the slice against the enumeration, not the clip: on a span carrying a
// peripheral, collective_translate deliberately refuses a mask whose clipped
// bound is not a member (addr_trans.hpp:391-398), so asserting every pair over
// the whole span is legal would assert against that guard. On every topology
// whose tile region IS the span nothing is filtered and the coverage is the
// same set of pairs as before.
TEST(SamYaml, SlicedNodeMaskMatchesTheEnumeratedOne) {
    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(TOPOLOGY_DIR)) {
        if (entry.path().extension() == ".yaml") files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());
    ASSERT_FALSE(files.empty()) << "expected the real topology YAMLs in " TOPOLOGY_DIR;

    unsigned compared = 0;
    unsigned expected = 0;
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
            unsigned compared_here = 0;
            for (unsigned ay = 0; ay < c->y_count; ++ay) {
                for (unsigned ax = 0; ax < c->x_count; ++ax) {
                    const uint64_t addr = origin | (uint64_t{ax} << c->x_range.offset) |
                                          (uint64_t{ay} << c->y_range.offset);
                    for (unsigned my = 0; my < (1u << c->y_range.len); ++my) {
                        for (unsigned mx = 0; mx < (1u << c->x_range.len); ++mx) {
                            if (mx == 0 && my == 0) continue;  // empty set: rejected, not compared
                            // Closure inside the tile region, so the clip below
                            // is a no-op and the mask is legal by construction.
                            // The closure is a wildcard box, so its per-axis
                            // extremes bound every member.
                            if ((ax & ~mx) < c->x_first || (ax | mx) > c->x_last) continue;
                            if ((ay & ~my) < c->y_first || (ay | my) > c->y_last) continue;
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
                            // The issuer must be a tile, and every address
                            // that survives the closure filter above names
                            // one: a closure containing the address cannot lie
                            // inside the tile region unless the address does.
                            const uint8_t issuer =
                                static_cast<uint8_t>((ay << ni::width::X_WIDTH) | ax);
                            EXPECT_EQ(collective_translate(sam, b, issuer), enumerated)
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
            // reading as a regression. The two axes filter independently, so
            // the surviving pairs are their product, less the mask-0 corner the
            // walk skips: that is one pair per (addr_x, addr_y) inside the
            // region. Where the region IS the span this reduces to the old
            // constant, x_count * y_count * (2^(xlen+ylen) - 1) -- 240 for a
            // 4x4, 12 for a 2x2. The 3-wide peripheral span gives 4: no x mask
            // keeps its closure inside a 2-wide tile region, so only the y mask
            // survives, over 4 addresses.
            // Catches a coverage collapse, which is what the per-pair
            // ASSERT_EQ above cannot see -- it only guards the pairs walked.
            const unsigned expected_here =
                axis_pairs_inside_region(c->x_count, c->x_range.len, c->x_first, c->x_last) *
                    axis_pairs_inside_region(c->y_count, c->y_range.len, c->y_first, c->y_last) -
                (c->x_last - c->x_first + 1) * (c->y_last - c->y_first + 1);
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
    const auto* mem = sam.collective_coords(axi::AxiClass::Data);
    const auto* cfg = sam.collective_coords(axi::AxiClass::Narrow);
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
    const auto* mem = sam.collective_coords(axi::AxiClass::Data);
    const auto* cfg = sam.collective_coords(axi::AxiClass::Narrow);
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
