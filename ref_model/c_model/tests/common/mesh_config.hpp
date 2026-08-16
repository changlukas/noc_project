#pragma once
#include "common/tmp_path.hpp"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

namespace ni::cmodel::testing {

// A mesh config in the shipped sim/configs/ shape: one tile array on the router
// array's EJECT port, carrying the addr_range block the caller supplies.
// `extra` is appended verbatim, for the routing: block or a second endpoint.
//
// Fixtures build this instead of a hand-listed map because the reader under
// test only takes this shape -- sim/configs/*.yml is the only format
// load_sam_table parses.
inline std::string mesh_config_yaml(unsigned x_dim, unsigned y_dim, const std::string& ranges,
                                    const std::string& extra = "") {
    std::ostringstream os;
    const std::string rng = "[[0, " + std::to_string(x_dim - 1) + "], [0, " +
                            std::to_string(y_dim - 1) + "]]";
    os << "name: t\n"
       << "network_type: \"axi\"\n"
       << "endpoints:\n"
       << "  - name: \"tile\"\n"
       << "    array: [" << x_dim << ", " << y_dim << "]\n"
       << "    sbr_port_protocol: [\"axi\"]\n"
       << "    addr_range:\n"
       << ranges
       << "routers:\n"
       << "  - { name: \"router\", array: [" << x_dim << ", " << y_dim << "] }\n"
       << "connections:\n"
       << "  - { src: \"tile\", dst: \"router\", src_range: " << rng << ", dst_range: " << rng
       << ", dst_dir: 4 }\n"
       << extra;
    return os.str();
}

// Writes a config document to a unique temp file and returns its path.
inline std::string write_config(const char* name, const std::string& text) {
    auto path = unique_temp_path(name);
    std::ofstream(path) << text;
    return path;
}

// The common fixture: an x_dim by y_dim mesh, one memory aperture and one
// config aperture per node, both at the same node stride.
inline std::string write_mesh_config(const char* name, unsigned x_dim, unsigned y_dim,
                                     uint64_t memory_size, uint64_t config_base,
                                     uint64_t stride, uint64_t config_size = 0x1000) {
    std::ostringstream ranges;
    ranges << std::hex << "      - { base: 0x0, size: 0x" << memory_size << ", stride: 0x" << stride
           << ", space: memory }\n"
           << "      - { base: 0x" << config_base << ", size: 0x" << config_size << ", stride: 0x"
           << stride << ", space: config }\n";
    return write_config(name, mesh_config_yaml(x_dim, y_dim, ranges.str()));
}

}  // namespace ni::cmodel::testing
