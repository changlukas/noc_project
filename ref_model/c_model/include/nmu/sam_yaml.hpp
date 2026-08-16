#pragma once
#include "axi/types.hpp"
#include "nmu/addr_trans.hpp"
#include <yaml-cpp/yaml.h>

#include <cassert>
#include <cstdlib>
#include <string>
#include <vector>

namespace ni::cmodel::nmu::addr_trans {

// tile.space: "config" | "memory", default "memory" (spec §5: config selects
// the narrow class, memory the data class). Fail-loud on anything else --
// same shape as the other config/stimulus-trust-boundary checks in this file.
inline axi::Space parse_tile_space(const YAML::Node& tile) {
    if (!tile["space"]) return axi::Space::Memory;
    const std::string space = tile["space"].as<std::string>();
    if (space == "config") return axi::Space::Config;
    if (space == "memory") return axi::Space::Memory;
    assert(false && "address_map tile: space must be 'config' or 'memory'");
    std::abort();  // belt-and-braces for NDEBUG
}

// Per-space coordinate ranges, mirroring floogen's gen_collective_sam: the
// loader is where x_dim, y_dim and the space's node stride are all in hand.
//
//   range.len      = clog2(dim)
//   x_range.offset = log2(node_stride)
//   y_range.offset = log2(node_stride) + clog2(x_dim)
//
// X sits below Y because this repo packs raster order, X fastest (see
// SpaceCoords). A space the declaration does not fit is simply not a collective
// target (spec §5.1), so the return value is not an error to raise here.
inline void declare_space_coords(SamTable& table, unsigned x_dim, unsigned y_dim) {
    // The tile spaces only. A peripheral region is placed in declaration order
    // at its own size, so it names no stride to read a coordinate field from --
    // not attempting the declaration is what makes "never a collective target"
    // a stated policy instead of a declaration that happened to fail.
    for (axi::Space space : {axi::Space::Config, axi::Space::Memory}) {
        const SamEntry* first = nullptr;
        const SamEntry* second = nullptr;
        for (const auto& e : table.entries()) {
            if (e.space != space) continue;
            if (first == nullptr) {
                first = &e;
            } else {
                second = &e;
                break;
            }
        }
        // One entry names no stride, so no coordinate field can be read off it.
        if (second == nullptr) continue;
        const uint64_t stride = second->base - first->base;
        if (stride == 0 || (stride & (stride - 1)) != 0) continue;  // not a power of two
        unsigned offset = 0;
        while ((uint64_t{1} << offset) != stride) ++offset;
        SpaceCoords c;
        c.x_count = x_dim;
        c.y_count = y_dim;
        c.x_range = {offset, clog2(x_dim)};
        c.y_range = {offset + clog2(x_dim), clog2(y_dim)};
        table.declare_space_coords(space, c);
    }
    assert(table.collective_coords(axi::Space::Peripheral) == nullptr &&
           "sam_yaml: the peripheral space must not be collective-eligible");
}

// Does this address space appear in the map at all? Memory always does; config
// is optional (spec §5.1 covers the spaces a topology declares).
inline bool space_present(const SamTable& table, axi::Space space) {
    for (const auto& e : table.entries()) {
        if (e.space == space) return true;
    }
    return false;
}

// What offset decode requires of a map (spec §5.1). The mode itself is named
// address_map.decode in the topology YAML and routing.use_id_table in the
// FlooNoC-shaped config; both reach this one check.
//
// Table decode holds the coordinate ranges per address-map entry, offset decode
// one pair global to the map (upstream RouteCfg.XYAddrOffsetX/Y, floo_pkg.sv).
// One global pair reaches one field position. Tile-major gives every space
// the same node stride (block_size), so every space's node index already
// sits at the same address bits regardless of its own region size -- a
// YAML-loaded map satisfies offset decode by construction.
//
// On a map meeting §5.1 both modes decode every address to the same node and
// the same node-local offset, so the mode changes which maps are legal rather
// than how an address is read, and the model validates it here instead of
// carrying a second lookup that would return the same answer. The 2N range
// compares a table decoder costs against one slice is a hardware difference.
inline void check_offset_ranges(const SamTable& table) {
    const SpaceCoords* first = nullptr;
    // The tile spaces only. Offset decode is a claim about where a node stride
    // puts the node index, and a peripheral region has no node stride -- it is
    // placed in declaration order at its own size, one region per peripheral.
    // Including it would abort on a legal peripheral topology.
    for (axi::Space space : {axi::Space::Config, axi::Space::Memory}) {
        if (!space_present(table, space)) continue;
        const SpaceCoords* c = table.collective_coords(space);
        assert(c && "address_map: decode 'offset' needs every space to meet spec 5.1");
        if (c == nullptr) std::abort();
        if (first == nullptr) {
            first = c;
            continue;
        }
        assert(c->x_range.offset == first->x_range.offset &&
               c->y_range.offset == first->y_range.offset &&
               "address_map: decode 'offset' holds one range pair for the whole map, so "
               "every space must use the same node stride");
    }
}

// address_map.decode: "table" | "offset", default "table". The FlooNoC-shaped
// config spells the same two modes routing.use_id_table (true = table).
inline void check_decode_mode(const YAML::Node& am, const SamTable& table) {
    if (!am["decode"]) return;
    const std::string mode = am["decode"].as<std::string>();
    if (mode == "table") return;
    if (mode != "offset") {
        assert(false && "address_map: decode must be 'table' or 'offset'");
        std::abort();
    }
    check_offset_ranges(table);
}

// address_map.tiles: ordered list of { x, y, size, space? }; base(x, y) is
// derived by SamTable::packed() from the coordinate and the space's slot
// size. No tile_size, no base, no default base. A node may appear once per
// space (validate()). The map is packed over the router array (x_dim, y_dim);
// a peripheral shares its host router's coordinate and takes no coordinate of
// its own.
//
// The stride a topology gets when it declares none: the next power of two at
// or above what the spaces occupy.
inline uint64_t default_block_size(const std::vector<PackedTile>& tiles) {
    uint64_t memory_slot = 0;
    uint64_t config_slot = 0;
    for (const auto& t : tiles) {
        uint64_t& slot = (t.cls == axi::AxiClass::Narrow) ? config_slot : memory_slot;
        slot = std::max(slot, t.size);
    }
    const uint64_t config_offset =
        config_slot == 0 ? 0 : ((memory_slot + config_slot - 1) / config_slot) * config_slot;
    // No config tile: the block only has to hold memory. With one, config
    // sits after it and dominates.
    const uint64_t extent = config_slot == 0 ? memory_slot : config_offset + config_slot;
    uint64_t p = 1;
    while (p < extent) p <<= 1;
    return p;
}

// --- FlooNoC-shaped config (sim/configs/*.yml) ---------------------------
//
// Twin of sim/tools/address_map.py's pack_config(). The two expansions must
// agree rule for rule: this one runs at simulation time through +sam_config
// while the Python one runs at generate time, so a divergence leaves the
// generated package and the generated stimulus agreeing with each other and
// disagreeing with the runtime translator. Held to that by
// tests/nmu/test_sam_config.cpp and sim/tools/test_sam_config_parity.py, which
// compare both against sim/configs/sam_rules.golden.

// addr_range.space: "config" | "memory" | "peripheral". A range names its own
// space here, where the topology YAML keeps peripherals in a separate list.
inline axi::Space parse_range_space(const YAML::Node& range) {
    if (range["space"] && range["space"].as<std::string>() == "peripheral") {
        return axi::Space::Peripheral;
    }
    return parse_tile_space(range);
}

// XYDirections (upstream routing.py:245-252) -> the router port an endpoint
// hangs off. EJECT is the LOCAL port the tile owns; EAST/WEST are the x face
// and NORTH/SOUTH the y face, which is what address_map.peripherals spells
// "face". dst_dir is read as a port and nothing else: a peripheral shares its
// host router's coordinate, so upstream's coordinate derivation for a
// non-router NI must NOT be applied -- it would place the peripheral one step
// outside the mesh.
inline uint8_t port_of_dst_dir(unsigned dir) {
    switch (dir) {
        case 4:
            return 0;  // EJECT
        case 1:
        case 3:
            return 1;  // EAST / WEST -- the x face
        case 0:
        case 2:
            return 2;  // NORTH / SOUTH -- the y face
        default:
            break;
    }
    assert(false && "connection: dst_dir must be an XYDirections value 0..4");
    std::abort();
}

// Endpoint member count. num is authored in our configs; upstream derives it
// from array (endpoint.py:73-84), so an absent num falls back to that product.
inline std::size_t endpoint_members(const YAML::Node& ep) {
    if (ep["num"]) return ep["num"].as<std::size_t>();
    if (!ep["array"]) return 1;
    std::size_t n = 1;
    for (const auto& d : ep["array"]) n *= d.as<std::size_t>();
    return n;
}

// Where each member of an endpoint attaches: the host router's array index and
// the port. The index is X-fast, idx = (y << clog2(x_dim)) | x, which is this
// repo's node numbering -- and not the array index's own value as a dst_id,
// which packs x into X_WIDTH bits instead.
struct Attachment {
    unsigned idx;
    uint8_t port;
};

inline std::vector<Attachment> endpoint_attachments(const YAML::Node& root, const std::string& name,
                                                    std::size_t num) {
    constexpr unsigned kUnattached = ~0u;
    std::vector<Attachment> out(num, Attachment{kUnattached, 0});
    for (const auto& c : root["connections"]) {
        if (c["src"].as<std::string>() != name) continue;
        const uint8_t port = port_of_dst_dir(c["dst_dir"].as<unsigned>());
        if (c["src_idx"]) {
            const auto src = c["src_idx"].as<std::vector<unsigned>>();
            const auto dst = c["dst_idx"].as<std::vector<unsigned>>();
            assert(src.size() == dst.size() && "connection: src_idx and dst_idx differ in length");
            for (std::size_t i = 0; i < src.size(); ++i) {
                assert(src[i] < num && "connection: src_idx names a member this endpoint has not");
                out[src[i]] = {dst[i], port};
            }
        } else {
            // src_range/dst_range pair the two arrays element for element. This
            // reader accepts that only when the two ranges are identical: every
            // shipped config connects a tile array to the router array it sits
            // on, and a general range remap has no caller to be checked by.
            assert(c["src_range"] && c["dst_range"] &&
                   "connection: needs either src_idx/dst_idx or src_range/dst_range");
            assert(c["src_range"].as<std::vector<std::vector<unsigned>>>() ==
                       c["dst_range"].as<std::vector<std::vector<unsigned>>>() &&
                   "connection: src_range and dst_range differ -- this reader pairs the two "
                   "arrays element for element and has no range remap");
            for (unsigned k = 0; k < num; ++k) out[k] = {k, port};
        }
    }
    for (const auto& a : out) {
        assert(a.idx != kUnattached && "endpoint: a member has no connection");
    }
    return out;
}

inline SamTable load_config_table(const YAML::Node& root) {
    YAML::Node routers = root["routers"];
    assert(routers && routers.size() == 1 && "config.routers: expected exactly one router array");
    const auto array = routers[0]["array"].as<std::vector<unsigned>>();
    assert(array.size() == 2 && "config.routers: array must be [x, y]");
    const unsigned x_dim = array[0];
    const unsigned y_dim = array[1];
    // Same two mesh constraints the topology YAML carries: a mesh communicating
    // through NI + router needs at least 2x2, and the collective coordinate
    // field is clog2(dim) bits wide, so a non-power-of-two dimension leaves a
    // wildcard address naming a coordinate with no router.
    assert(x_dim >= 2 && y_dim >= 2 &&
           "config.routers: mesh dimensions must be >= 2 per dimension (1x1/1xN mesh illegal)");
    assert((x_dim & (x_dim - 1)) == 0 && (y_dim & (y_dim - 1)) == 0 &&
           "config.routers: mesh dimensions must be powers of two");
    const unsigned x_bits = clog2(x_dim);

    std::vector<SamEntry> es;
    for (const auto& ep : root["endpoints"]) {
        // mgr_port_protocol / sbr_port_protocol are presence markers this reader
        // ignores. The protocols: block they would resolve against upstream is
        // omitted by design decision 2 -- the four AXI widths do not vary per
        // configuration and live in specgen/source/constants.yaml -- so only the
        // presence of sbr_port_protocol is read: is_sbr() is what gates an
        // endpoint's participation in the SAM (endpoint.py:87-89).
        if (!ep["sbr_port_protocol"]) continue;
        const std::size_t num = endpoint_members(ep);
        const auto attach = endpoint_attachments(root, ep["name"].as<std::string>(), num);
        // Range-major: every member of a range, then the next range. Same order
        // the topology YAML lists its tiles in, so the two shapes of one map
        // expand to the same list.
        for (const auto& r : ep["addr_range"]) {
            const axi::Space space = parse_range_space(r);
            const uint64_t base = r["base"].as<uint64_t>();
            const uint64_t size = r["size"].as<uint64_t>();
            // Absent stride means stride == size, which is upstream's own
            // expansion (routing.py:449). Ours differs because a node's two
            // apertures sit at different offsets inside one block stride.
            const uint64_t stride = r["stride"] ? r["stride"].as<uint64_t>() : size;
            for (std::size_t k = 0; k < num; ++k) {
                const unsigned x = attach[k].idx & (x_dim - 1);
                const unsigned y = attach[k].idx >> x_bits;
                es.push_back({base + stride * k, size,
                              static_cast<uint8_t>((y << ni::width::X_WIDTH) | x),
                              axi::class_of(space), attach[k].port, space});
            }
        }
    }

    SamTable table(std::move(es));
    table.validate(x_dim, y_dim);
    declare_space_coords(table, x_dim, y_dim);
    // routing.use_id_table names the two decode modes: true (the default) is
    // table decode, false is the address-offset slice, whose validation is ours
    // and not upstream's -- upstream's non-table path is a bit-slice and
    // nothing more.
    YAML::Node routing = root["routing"];
    const bool use_id_table =
        !routing || !routing["use_id_table"] || routing["use_id_table"].as<bool>();
    if (!use_id_table) check_offset_ranges(table);
    return table;
}

inline SamTable load_sam_table(const std::string& yaml_path) {
    YAML::Node root = YAML::LoadFile(yaml_path);
    // Dispatch on the file's shape, not on a flag. endpoints: is the
    // FlooNoC-shaped config; topology: + address_map: is the topology YAML it
    // replaces, still what the build and +sam_config load today.
    if (root["endpoints"]) return load_config_table(root);
    YAML::Node topo = root["topology"];
    unsigned x_dim = topo["x_dim"].as<unsigned>();
    unsigned y_dim = topo["y_dim"].as<unsigned>();
    // Mesh dim minimum is 2 per dimension: a mesh communicating through NI +
    // router needs at least 2x2. 1x1 and 1xN meshes are illegal.
    assert(x_dim >= 2 && y_dim >= 2 &&
           "topology: mesh dimensions must be >= 2 per dimension (1x1/1xN mesh illegal)");
    // A collective names its destination set by wildcarding the coordinate
    // field, and nothing clips the expansion back to the coordinates that
    // exist -- the field being exactly as wide as the dimension is what makes
    // clipping unnecessary (addr_trans.hpp collective_translate, route_mask.hpp).
    assert((x_dim & (x_dim - 1)) == 0 && (y_dim & (y_dim - 1)) == 0 &&
           "topology: mesh dimensions must be powers of two -- the collective coordinate field is "
           "clog2(dim) bits wide, so a non-power-of-two dimension leaves a wildcard address naming "
           "a coordinate with no router");
    YAML::Node am = root["address_map"];
    assert(am && "address_map block missing from topology YAML");
    YAML::Node tiles_node = am["tiles"];
    assert(tiles_node && "address_map: tiles list missing");

    std::vector<PackedTile> tiles;
    for (const auto& t : tiles_node) {
        const axi::Space space = parse_tile_space(t);
        tiles.push_back({t["x"].as<unsigned>(), t["y"].as<unsigned>(), t["size"].as<uint64_t>(),
                         axi::class_of(space), space});
    }
    // address_map.peripherals: ordered list of { x, y, face, size }. A peripheral
    // hangs off a boundary port of the router at (x, y) -- face "x" is port 1,
    // face "y" is port 2 -- and its region is placed above the tile array in
    // declaration order, not derived from the coordinate.
    std::vector<PeripheralRegion> peripherals;
    for (const auto& p : am["peripherals"]) {
        const unsigned x = p["x"].as<unsigned>();
        const unsigned y = p["y"].as<unsigned>();
        const std::string face = p["face"].as<std::string>();
        assert((face == "x" || face == "y") && "address_map peripheral: face must be 'x' or 'y'");
        const bool on_x_edge = (x == 0 || x == x_dim - 1);
        const bool on_y_edge = (y == 0 || y == y_dim - 1);
        // Deadlock freedom, not input tidiness. On an edge router the named face
        // has no neighbour, so the port is terminal: nothing downstream of the
        // peripheral requests a further channel. On an interior router that port
        // carries a live inter-router link, and the Y-to-X ejection turn the
        // peripheral adds there closes a real channel dependency cycle.
        assert((face == "x" ? on_x_edge : on_y_edge) &&
               "address_map peripheral: face names an edge this coordinate is not on -- an "
               "interior router's port carries a live inter-router link, and hanging a "
               "peripheral off it closes a channel dependency cycle");
        for (const auto& q : peripherals) {
            assert(!(q.x == x && q.y == y && q.port == (face == "x" ? 1 : 2)) &&
                   "address_map: two peripherals share the same (x, y, face)");
        }
        peripherals.push_back(
            {x, y, static_cast<uint8_t>(face == "x" ? 1 : 2), p["size"].as<uint64_t>()});
    }
    const uint64_t block_size =
        am["block_size"] ? am["block_size"].as<uint64_t>() : default_block_size(tiles);
    SamTable table = SamTable::packed(tiles, x_dim, y_dim, block_size, peripherals);
    table.validate(x_dim, y_dim);
    declare_space_coords(table, x_dim, y_dim);
    check_decode_mode(am, table);  // after the ranges exist -- offset mode is checked against them
    return table;
}

}  // namespace ni::cmodel::nmu::addr_trans
