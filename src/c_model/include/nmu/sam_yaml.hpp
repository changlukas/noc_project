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
    return table;
}

}  // namespace ni::cmodel::nmu::addr_trans
