#pragma once
#include "nmu/addr_trans.hpp"
#include <yaml-cpp/yaml.h>

#include <cassert>
#include <string>

namespace ni::cmodel::nmu::addr_trans {

inline SamTable load_sam_table(const std::string& yaml_path) {
    YAML::Node root = YAML::LoadFile(yaml_path);
    unsigned x_dim = root["topology"]["x_dim"].as<unsigned>();
    unsigned y_dim = root["topology"]["y_dim"].as<unsigned>();
    YAML::Node am = root["address_map"];
    assert(am && "address_map block missing from topology YAML");

    uint64_t tile_size = am["tile_size"].as<uint64_t>();

    // Start from the uniform default, then apply explicit per-tile overrides.
    SamTable base = SamTable::uniform(x_dim, y_dim, tile_size);
    std::vector<SamEntry> es = base.entries();

    if (am["tiles"]) {
        for (const auto& t : am["tiles"]) {
            unsigned x = t["x"].as<unsigned>();
            unsigned y = t["y"].as<unsigned>();
            uint8_t dst = static_cast<uint8_t>((y << ni::width::X_WIDTH) | x);
            uint64_t b = t["base"].as<uint64_t>();
            uint64_t s = t["size"].as<uint64_t>();
            SamEntry repl{b, s, dst};
            for (auto& e : es) {
                if (e.dst_id == dst) {
                    e = repl;
                    break;
                }
            }
        }
    }
    SamTable table(std::move(es));
    table.validate(x_dim, y_dim);
    return table;
}

}  // namespace ni::cmodel::nmu::addr_trans
