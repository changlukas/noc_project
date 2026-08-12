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
inline axi::AxiClass parse_tile_space(const YAML::Node& tile) {
    if (!tile["space"]) return axi::AxiClass::Data;
    const std::string space = tile["space"].as<std::string>();
    if (space == "config") return axi::AxiClass::Narrow;
    if (space == "memory") return axi::AxiClass::Data;
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
    for (axi::AxiClass cls : {axi::AxiClass::Narrow, axi::AxiClass::Data}) {
        const SamEntry* first = nullptr;
        const SamEntry* second = nullptr;
        for (const auto& e : table.entries()) {
            if (e.cls != cls) continue;
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
        table.declare_space_coords(cls, c);
    }
}

// address_map.decode: "table" | "offset", default "table" (spec §5.1).
//
// Table decode holds the coordinate ranges per address-map entry, offset decode
// one pair global to the map (upstream RouteCfg.XYAddrOffsetX/Y, floo_pkg.sv).
// One global pair reaches one field position, so offset decode additionally
// requires every space to place its node index at the same address bits -- that
// is, equal region size across spaces.
//
// On a map meeting §5.1 both modes decode every address to the same node and
// the same node-local offset, so the mode changes which maps are legal rather
// than how an address is read, and the model validates it here instead of
// carrying a second lookup that would return the same answer. The 2N range
// compares a table decoder costs against one slice is a hardware difference.
inline void check_decode_mode(const YAML::Node& am, const SamTable& table) {
    if (!am["decode"]) return;
    const std::string mode = am["decode"].as<std::string>();
    if (mode == "table") return;
    if (mode != "offset") {
        assert(false && "address_map: decode must be 'table' or 'offset'");
        std::abort();
    }
    const SpaceCoords* first = nullptr;
    for (axi::AxiClass cls : {axi::AxiClass::Narrow, axi::AxiClass::Data}) {
        bool present = false;
        for (const auto& e : table.entries()) {
            if (e.cls == cls) {
                present = true;
                break;
            }
        }
        if (!present) continue;
        const SpaceCoords* c = table.collective_coords(cls);
        assert(c && "address_map: decode 'offset' needs every space to meet spec 5.1");
        if (c == nullptr) std::abort();
        if (first == nullptr) {
            first = c;
            continue;
        }
        assert(c->x_range.offset == first->x_range.offset &&
               c->y_range.offset == first->y_range.offset &&
               "address_map: decode 'offset' holds one range pair for the whole map, so "
               "every space must use the same region size");
    }
}

// address_map.tiles: ordered list of { x, y, size, space? }; base(i) is derived
// by SamTable::packed() as base(i-1) + size(i-1). No tile_size, no base, no
// default base. A node may appear once per space (validate()).
inline SamTable load_sam_table(const std::string& yaml_path) {
    YAML::Node root = YAML::LoadFile(yaml_path);
    unsigned x_dim = root["topology"]["x_dim"].as<unsigned>();
    unsigned y_dim = root["topology"]["y_dim"].as<unsigned>();
    // Mesh dim minimum is 2 per dimension: a mesh communicating through NI +
    // router needs at least 2x2. 1x1 and 1xN meshes are illegal.
    assert(x_dim >= 2 && y_dim >= 2 &&
           "topology: mesh dimensions must be >= 2 per dimension (1x1/1xN mesh illegal)");
    YAML::Node am = root["address_map"];
    assert(am && "address_map block missing from topology YAML");
    YAML::Node tiles_node = am["tiles"];
    assert(tiles_node && "address_map: tiles list missing");

    std::vector<PackedTile> tiles;
    for (const auto& t : tiles_node) {
        tiles.push_back({t["x"].as<unsigned>(), t["y"].as<unsigned>(), t["size"].as<uint64_t>(),
                         parse_tile_space(t)});
    }
    SamTable table = SamTable::packed(tiles);
    table.validate(x_dim, y_dim);
    declare_space_coords(table, x_dim, y_dim);
    check_decode_mode(am, table);  // after the ranges exist -- offset mode is checked against them
    return table;
}

}  // namespace ni::cmodel::nmu::addr_trans
