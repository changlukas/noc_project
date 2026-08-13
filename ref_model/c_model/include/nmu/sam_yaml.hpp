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
// loader is where x_span, y_span and the space's node stride are all in hand.
//
//   range.len      = clog2(span)
//   x_range.offset = log2(node_stride)
//   y_range.offset = log2(node_stride) + clog2(x_span)
//
// X sits below Y because this repo packs raster order, X fastest (see
// SpaceCoords). A space the declaration does not fit is simply not a collective
// target (spec §5.1), so the return value is not an error to raise here.
//
// tile_x_first/tile_x_last/tile_y_first/tile_y_last are the inclusive tile
// region inside the route span (SpaceCoords); a coordinate outside them is a
// peripheral or unpopulated padding and is not walked by declare_space_coords.
inline void declare_space_coords(SamTable& table, unsigned x_span, unsigned y_span,
                                 unsigned tile_x_first, unsigned tile_x_last, unsigned tile_y_first,
                                 unsigned tile_y_last) {
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
        c.x_count = x_span;
        c.y_count = y_span;
        c.x_range = {offset, clog2(x_span)};
        c.y_range = {offset + clog2(x_span), clog2(y_span)};
        c.x_first = tile_x_first;
        c.x_last = tile_x_last;
        c.y_first = tile_y_first;
        c.y_last = tile_y_last;
        table.declare_space_coords(cls, c);
    }
}

// Does this address space appear in the map at all? Memory always does; config
// is optional (spec §5.1 covers the spaces a topology declares).
inline bool space_present(const SamTable& table, axi::AxiClass cls) {
    for (const auto& e : table.entries()) {
        if (e.cls == cls) return true;
    }
    return false;
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
        if (!space_present(table, cls)) continue;
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

// address_map.tiles: ordered list of { x, y, size, space? }; base(x, y) is
// derived by SamTable::packed() from the coordinate and the space's slot
// size. No tile_size, no base, no default base. A node may appear once per
// space (validate()).
//
// x_dim/y_dim are the router array. The map is packed over the route SPAN
// (x_span/y_span), which additionally covers any border coordinate a
// peripheral occupies, and the tile region (tile_x_first..tile_x_last,
// tile_y_first..tile_y_last, inclusive) says which of those coordinates are
// tiles. All six keys are optional and default to a plain mesh, so a topology
// stating none of them means today exactly what it meant before they existed.
// Same six keys, same defaults, as gen_tb_top.py's _route_span.
inline SamTable load_sam_table(const std::string& yaml_path) {
    YAML::Node root = YAML::LoadFile(yaml_path);
    YAML::Node topo = root["topology"];
    unsigned x_dim = topo["x_dim"].as<unsigned>();
    unsigned y_dim = topo["y_dim"].as<unsigned>();
    // Mesh dim minimum is 2 per dimension: a mesh communicating through NI +
    // router needs at least 2x2. 1x1 and 1xN meshes are illegal.
    assert(x_dim >= 2 && y_dim >= 2 &&
           "topology: mesh dimensions must be >= 2 per dimension (1x1/1xN mesh illegal)");
    unsigned x_span = topo["x_span"] ? topo["x_span"].as<unsigned>() : x_dim;
    unsigned y_span = topo["y_span"] ? topo["y_span"].as<unsigned>() : y_dim;
    unsigned tile_x_first = topo["tile_x_first"] ? topo["tile_x_first"].as<unsigned>() : 0;
    unsigned tile_y_first = topo["tile_y_first"] ? topo["tile_y_first"].as<unsigned>() : 0;
    unsigned tile_x_last = topo["tile_x_last"] ? topo["tile_x_last"].as<unsigned>() : x_span - 1;
    unsigned tile_y_last = topo["tile_y_last"] ? topo["tile_y_last"].as<unsigned>() : y_span - 1;
    // This is the one place a tile region enters the C++ model, so it is where
    // the region is checked: SpaceCoords' four bounds default to zero, which
    // is a one-column region, and nothing downstream can tell a stated region
    // from a forgotten one. Same relationships gen_tb_top.py validates.
    assert(x_span >= x_dim && y_span >= y_dim &&
           "topology: route span must cover the router array (x_span >= x_dim)");
    assert(tile_x_first <= tile_x_last && tile_x_last < x_span &&
           "topology: tile x region must sit inside the route span");
    assert(tile_y_first <= tile_y_last && tile_y_last < y_span &&
           "topology: tile y region must sit inside the route span");
    // A span wider than the array with no stated region defaults to the whole
    // span, which is exactly the silent mis-delivery check_dst_reachable's
    // cross-row guard exists to remove -- so the region's extent is required
    // to equal the array's regardless of whether it was stated or defaulted.
    assert(tile_x_last - tile_x_first + 1 == x_dim &&
           "topology: tile x region extent must equal the router array (x_dim)");
    assert(tile_y_last - tile_y_first + 1 == y_dim &&
           "topology: tile y region extent must equal the router array (y_dim)");
    YAML::Node am = root["address_map"];
    assert(am && "address_map block missing from topology YAML");
    YAML::Node tiles_node = am["tiles"];
    assert(tiles_node && "address_map: tiles list missing");

    std::vector<PackedTile> tiles;
    for (const auto& t : tiles_node) {
        tiles.push_back({t["x"].as<unsigned>(), t["y"].as<unsigned>(), t["size"].as<uint64_t>(),
                         parse_tile_space(t)});
    }
    SamTable table = SamTable::packed(tiles, x_span, y_span);
    table.validate(x_span, y_span);
    declare_space_coords(table, x_span, y_span, tile_x_first, tile_x_last, tile_y_first,
                         tile_y_last);
    // A rejected declaration costs a space its collective eligibility, which is
    // intended, and it also costs it check_dst_reachable's cross-row guard,
    // which is not: both read the same declared ranges, and a space that has
    // none is waved through. On a topology whose tile region is the whole span
    // there is nothing to guard, so only a stated region insists here -- a
    // peripheral window of a different size, or a stride that is not a power of
    // two, would otherwise disarm the guard silently.
    const bool region_stated = tile_x_first != 0 || tile_y_first != 0 ||
                               tile_x_last != x_span - 1 || tile_y_last != y_span - 1;
    if (region_stated) {
        for (axi::AxiClass cls : {axi::AxiClass::Narrow, axi::AxiClass::Data}) {
            if (!space_present(table, cls)) continue;
            assert(table.collective_coords(cls) &&
                   "topology: a stated tile region needs every address space to declare its "
                   "coordinate ranges, and this one's entries reject the declaration -- the "
                   "off-region reachability guard reads those ranges");
            if (table.collective_coords(cls) == nullptr) std::abort();
        }
    }
    check_decode_mode(am, table);  // after the ranges exist -- offset mode is checked against them
    return table;
}

}  // namespace ni::cmodel::nmu::addr_trans
