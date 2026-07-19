#pragma once
#include "nmu/addr_trans.hpp"
#include <yaml-cpp/yaml.h>

#include <cassert>
#include <string>
#include <vector>

namespace ni::cmodel::nmu::addr_trans {

// address_map.tiles: ordered list of { x, y, size }; base(i) is derived by
// SamTable::packed() as base(i-1) + size(i-1). No tile_size, no base, no default.
inline SamTable load_sam_table(const std::string& yaml_path) {
    YAML::Node root = YAML::LoadFile(yaml_path);
    unsigned x_dim = root["topology"]["x_dim"].as<unsigned>();
    unsigned y_dim = root["topology"]["y_dim"].as<unsigned>();
    YAML::Node am = root["address_map"];
    assert(am && "address_map block missing from topology YAML");
    YAML::Node tiles_node = am["tiles"];
    assert(tiles_node && "address_map: tiles list missing");

    std::vector<PackedTile> tiles;
    for (const auto& t : tiles_node) {
        tiles.push_back({t["x"].as<unsigned>(), t["y"].as<unsigned>(), t["size"].as<uint64_t>()});
    }
    SamTable table = SamTable::packed(tiles);
    table.validate(x_dim, y_dim);
    return table;
}

}  // namespace ni::cmodel::nmu::addr_trans
