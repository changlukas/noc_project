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
    return table;
}

}  // namespace ni::cmodel::nmu::addr_trans
