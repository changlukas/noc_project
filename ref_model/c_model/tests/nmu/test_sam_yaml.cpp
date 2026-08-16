#include "nmu/sam_yaml.hpp"
#include "axi/types.hpp"
#include "common/mesh_config.hpp"
#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

using ni::cmodel::nmu::addr_trans::collective_translate;
using ni::cmodel::nmu::addr_trans::load_sam_table;
using ni::cmodel::nmu::addr_trans::SamEntry;
using ni::cmodel::nmu::addr_trans::SamTable;
using ni::cmodel::testing::mesh_config_yaml;
using ni::cmodel::testing::write_config;
using ni::cmodel::testing::write_mesh_config;
namespace axi = ni::cmodel::axi;

// SamTable::validate() is what rejects a map that misses a node, duplicates
// one, or declares a misaligned region, and tests/nmu/test_sam_table.cpp holds
// it to each of those directly. What is only checkable here is that the config
// reader actually RUNS it -- an expansion that skipped validate() would load
// this file happily and hand the model a three-node map for a four-node mesh.
TEST(SamYaml, AnExpandedConfigIsValidated) {
    // num 3 with an explicit member list: the tile endpoint covers three of the
    // four routers, which no array: form can express.
    const std::string text =
        "name: t\n"
        "endpoints:\n"
        "  - name: \"tile\"\n"
        "    num: 3\n"
        "    sbr_port_protocol: [\"axi\"]\n"
        "    addr_range:\n"
        "      - { base: 0x0, size: 0x1000, stride: 0x1000, space: memory }\n"
        "routers:\n"
        "  - { name: \"router\", array: [2, 2] }\n"
        "connections:\n"
        "  - { src: \"tile\", dst: \"router\", src_idx: [0, 1, 2], dst_idx: [0, 1, 2], "
        "dst_dir: 4 }\n";
    EXPECT_DEATH(load_sam_table(write_config("sam_short_mesh.yml", text)), "exactly once");
}

TEST(SamYaml, MeshDimBelowMinimumRejected) {
    const std::string ranges = "      - { base: 0x0, size: 0x1000, stride: 0x1000 }\n";
    EXPECT_DEATH(load_sam_table(write_config("sam_mesh_x1.yml", mesh_config_yaml(1, 4, ranges))),
                 "mesh dimensions must be >= 2");
    EXPECT_DEATH(load_sam_table(write_config("sam_mesh_y1.yml", mesh_config_yaml(2, 1, ranges))),
                 "mesh dimensions must be >= 2");
}

// Guards the real shipped configs. CONFIG_DIR is sim/configs/ itself
// (CMakeLists.txt), globbed rather than listed, so a new config cannot fall out
// of coverage.
//
// The claim is the packing formula, base = range.base + range.stride *
// ((y << clog2(x_dim)) | x), spelled out here from the config keys instead of
// read back from the expansion. This is one half of the bit-identity with
// sim/tools/address_map.py that the model and the stimulus generator both
// depend on: this test holds load_config_table() to the formula, and the Python
// twin (test_address_map_pack_real_configs_at_the_coordinate_formula in
// sim/tools/test_gen_test_patterns_filemaster.py) holds pack_config() to it
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
    const std::string ranges = "      - { base: 0x0, size: 0x1000, stride: 0x1000 }\n";
    EXPECT_DEATH(load_sam_table(write_config("sam_dim_x3.yml", mesh_config_yaml(3, 2, ranges))),
                 "powers of two");
    // Both axes, because a check written against x alone would still pass here.
    EXPECT_DEATH(load_sam_table(write_config("sam_dim_y3.yml", mesh_config_yaml(2, 3, ranges))),
                 "powers of two");
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

// A mesh plus `num` peripherals, whose connections the caller spells out. The
// endpoint belongs under endpoints: and its connection under connections:, so
// the two halves are spliced in at their own keys -- mesh_config_yaml ends on
// the connections list, which is why the second half appends.
static std::string write_peripheral_config(const char* name, unsigned x_dim, unsigned y_dim,
                                           unsigned num, const std::string& connections) {
    std::string text = mesh_config_yaml(
        x_dim, y_dim, "      - { base: 0x0, size: 0x1000, stride: 0x100000000, space: memory }\n");
    text.insert(text.find("routers:\n"),
                "  - name: \"peripheral\"\n"
                "    num: " + std::to_string(num) + "\n" +
                    "    sbr_port_protocol: [\"axi\"]\n"
                    "    addr_range:\n"
                    "      - { base: 0x1000000000, size: 0x1000, stride: 0x1000, "
                    "space: peripheral }\n");
    return write_config(name, text + connections);
}

// The placement asserts are the deadlock-freedom precondition, not input
// tidiness: a boundary port on an edge router is terminal, while the same port
// on an interior router carries a live inter-router link and the peripheral's
// Y-to-X ejection turn closes a channel dependency cycle. No shipped config can
// make them fire -- on a 2x2 every coordinate is on both edges, and the
// four-peripheral config puts every one on an edge deliberately -- so this is
// the only place the guard is exercised at all.
//
// gen_tb_top.py's _peripherals() refuses the same three shapes with the same
// reasons; both readers of one config must reject it, and the generator's copy
// is tested in sim/tools/test_gen_test_patterns_filemaster.py.
//
// A dimension of 4 is the smallest with an interior coordinate: x = 1 on a
// 4-wide mesh is on neither x edge, y = 1 on a 4-tall one is on neither y edge.
// Both axes, because a check written against x alone would still pass the y
// case.
TEST(SamYamlDeath, APeripheralDirectionMustNameAnEdgeItsCoordinateIsOn) {
    // 4x2, router index 1 = (1, 0), WEST: x is 1, not the west edge.
    EXPECT_DEATH(load_sam_table(write_peripheral_config(
                     "sam_face_x.yml", 4, 2, 1,
                     "  - { src: \"peripheral\", dst: \"router\", src_idx: [0], dst_idx: [1], "
                     "dst_dir: 3 }\n")),
                 "names an edge this coordinate is not on");
    // 2x4, router index 2 = (0, 1), NORTH: y is 1, not the north edge.
    EXPECT_DEATH(load_sam_table(write_peripheral_config(
                     "sam_face_y.yml", 2, 4, 1,
                     "  - { src: \"peripheral\", dst: \"router\", src_idx: [0], dst_idx: [2], "
                     "dst_dir: 0 }\n")),
                 "names an edge this coordinate is not on");
}

TEST(SamYamlDeath, APeripheralOutsideTheRouterArrayIsRejected) {
    // A peripheral shares a router's coordinate, so index 8 on a 4x2 array
    // names no router to hang off.
    EXPECT_DEATH(load_sam_table(write_peripheral_config(
                     "sam_periph_off_array.yml", 4, 2, 1,
                     "  - { src: \"peripheral\", dst: \"router\", src_idx: [0], dst_idx: [8], "
                     "dst_dir: 3 }\n")),
                 "outside the array");
}

TEST(SamYamlDeath, TwoPeripheralsOnOneRouterPortAreRejected) {
    EXPECT_DEATH(load_sam_table(write_peripheral_config(
                     "sam_periph_dup_port.yml", 2, 2, 2,
                     "  - { src: \"peripheral\", dst: \"router\", src_idx: [0, 1], "
                     "dst_idx: [0, 0], dst_dir: 3 }\n")),
                 "both claim the same router port");
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
    const std::string ranges =
        "      - { base: 0x0, size: 0x1000, stride: 0x1000, space: bogus }\n";
    EXPECT_DEATH(load_sam_table(write_config("sam_bad_space.yml", mesh_config_yaml(2, 2, ranges))),
                 "space must be");
}

// === Decode mode (spec §5.1) ===
//
// routing.use_id_table false is offset decode, which holds ONE coordinate range
// pair for the whole map and is therefore legal only where every space puts its
// node index at the same address bits. Each addr_range authors its own stride,
// so that is a property of the file: the same map is legal under table decode
// and rejected under offset decode below.
//
// This is also the only place use_id_table: false is exercised at all -- every
// shipped config leaves it at the default.

// One stride for both spaces: memory 4 KB at 0x0, config 4 KB at 0x100000, node
// stride 0x200000 in both, so one range pair reaches both.
static std::string one_stride_config(const char* name, const char* routing) {
    return write_config(
        name, mesh_config_yaml(
                  2, 2,
                  "      - { base: 0x0, size: 0x1000, stride: 0x200000, space: memory }\n"
                  "      - { base: 0x100000, size: 0x1000, stride: 0x200000, space: config }\n",
                  routing));
}

// Two strides: memory strides 0x100000 and config 0x200000, so the node index
// sits at bit 20 in one space and bit 21 in the other. The regions still do not
// overlap and every node is covered exactly once, so nothing but the decode mode
// has anything to say about this map.
static std::string two_stride_config(const char* name, const char* routing) {
    return write_config(
        name, mesh_config_yaml(
                  2, 2,
                  "      - { base: 0x0, size: 0x1000, stride: 0x100000, space: memory }\n"
                  "      - { base: 0x800000, size: 0x1000, stride: 0x200000, space: config }\n",
                  routing));
}

static const char* kOffsetDecode = "routing:\n  use_id_table: false\n";
static const char* kTableDecode = "routing:\n  use_id_table: true\n";

TEST(SamYaml, OffsetDecodeAcceptsOneStrideAcrossSpaces) {
    auto sam = load_sam_table(one_stride_config("sam_offset_ok.yml", kOffsetDecode));
    const auto* mem = sam.collective_coords(axi::Space::Memory);
    const auto* cfg = sam.collective_coords(axi::Space::Config);
    ASSERT_NE(mem, nullptr);
    ASSERT_NE(cfg, nullptr);
    EXPECT_EQ(mem->x_range.offset, cfg->x_range.offset);  // the one global pair
    EXPECT_EQ(mem->y_range.offset, cfg->y_range.offset);
}

TEST(SamYamlDeath, OffsetDecodeRejectsTwoStridesAcrossSpaces) {
    EXPECT_DEATH(load_sam_table(two_stride_config("sam_offset_two.yml", kOffsetDecode)),
                 "same node stride");
}

// The same map under table decode, which holds the ranges per entry and so has
// nothing to say about the two strides disagreeing. Without this the rejection
// above would be indistinguishable from the map simply being malformed.
TEST(SamYaml, TableDecodeAcceptsTwoStridesAcrossSpaces) {
    auto sam = load_sam_table(two_stride_config("sam_table_two.yml", kTableDecode));
    EXPECT_EQ(sam.entries().size(), 8u);
}

// Omitting routing: entirely must decode as table, or a config that never
// mentions the mode would silently take the stricter one.
TEST(SamYaml, TableDecodeIsTheDefault) {
    auto sam = load_sam_table(two_stride_config("sam_default_decode.yml", ""));
    EXPECT_EQ(sam.entries().size(), 8u);
}
