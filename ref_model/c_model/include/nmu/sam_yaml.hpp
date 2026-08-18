#pragma once
#include "axi/types.hpp"
#include "nmu/addr_trans.hpp"
#include <yaml-cpp/yaml.h>

#include <cassert>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace ni::cmodel::nmu::addr_trans {

// range.space: "config" | "memory", default "memory" (spec §5: config selects
// the narrow class, memory the data class). Fail-loud on anything else --
// same shape as the other config/stimulus-trust-boundary checks in this file.
inline axi::Space parse_tile_space(const YAML::Node& range) {
    if (!range["space"]) return axi::Space::Memory;
    const std::string space = range["space"].as<std::string>();
    if (space == "config") return axi::Space::Config;
    if (space == "memory") return axi::Space::Memory;
    assert(false && "addr_range: space must be 'config', 'memory' or 'peripheral'");
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
// space.
inline axi::Space parse_range_space(const YAML::Node& range) {
    if (range["space"] && range["space"].as<std::string>() == "peripheral") {
        return axi::Space::Peripheral;
    }
    return parse_tile_space(range);
}

// XYDirections (upstream routing.py:245-252) -> the router port an endpoint
// hangs off. EJECT is the LOCAL port the tile owns; EAST/WEST are the x face
// and NORTH/SOUTH the y face. dst_dir is read as a port and nothing else: a
// peripheral shares its host router's coordinate, so upstream's coordinate
// derivation for a non-router NI must NOT be applied -- it would place the
// peripheral one step outside the mesh.
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

// Where each member of an endpoint attaches: the host router's array index, the
// port and the direction that named it. The index is X-fast,
// idx = (y << clog2(x_dim)) | x, which is this repo's node numbering -- and not
// the array index's own value as a dst_id, which packs x into X_WIDTH bits
// instead. dir is kept alongside port because EAST and WEST share port 1 while
// naming opposite edges, which is what check_boundary_attachments compares.
struct Attachment {
    unsigned idx;
    uint8_t port;
    unsigned dir;
};

inline std::vector<Attachment> endpoint_attachments(const YAML::Node& root, const std::string& name,
                                                    std::size_t num) {
    constexpr unsigned kUnattached = ~0u;
    std::vector<Attachment> out(num, Attachment{kUnattached, 0, 4});
    for (const auto& c : root["connections"]) {
        if (c["src"].as<std::string>() != name) continue;
        const unsigned dir = c["dst_dir"].as<unsigned>();
        const uint8_t port = port_of_dst_dir(dir);
        if (c["src_idx"]) {
            const auto src = c["src_idx"].as<std::vector<unsigned>>();
            const auto dst = c["dst_idx"].as<std::vector<unsigned>>();
            assert(src.size() == dst.size() && "connection: src_idx and dst_idx differ in length");
            for (std::size_t i = 0; i < src.size(); ++i) {
                assert(src[i] < num && "connection: src_idx names a member this endpoint has not");
                out[src[i]] = {dst[i], port, dir};
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
            for (unsigned k = 0; k < num; ++k) out[k] = {k, port, dir};
        }
    }
    for (const auto& a : out) {
        assert(a.idx != kUnattached && "endpoint: a member has no connection");
    }
    return out;
}

// Deadlock freedom, not input tidiness. An endpoint on a boundary port shares
// its host router's coordinate, and on an edge router the named port has no
// neighbour and is terminal: nothing downstream of it requests a further
// channel. On an interior router that same port carries a live inter-router
// link, and the Y-to-X ejection turn the endpoint adds there closes a real
// channel dependency cycle. EJECT is the tile's own port and is exempt.
//
// One direction, one endpoint: two on the same (coordinate, direction) would
// both drive it. Twin of gen_tb_top.py's _peripherals(), which refuses the same
// two shapes -- the generator is a second, independent reader of the same file
// and both must reject it.
inline void check_boundary_attachments(const std::vector<Attachment>& attach, unsigned x_dim,
                                       unsigned y_dim, unsigned x_bits,
                                       std::vector<std::pair<unsigned, unsigned>>& claimed) {
    for (const auto& a : attach) {
        if (a.dir == 4) continue;  // EJECT -- the tile's own port
        const unsigned x = a.idx & (x_dim - 1);
        const unsigned y = a.idx >> x_bits;
        // An endpoint on a boundary port takes no coordinate of its own, so an
        // index past the array names no router to hang off. Checked before the
        // edge test, which would otherwise report the wrong reason.
        assert(x < x_dim && y < y_dim && "connection: dst_idx names a router outside the array");
        const bool on_edge = a.dir == 3   ? x == 0           // WEST
                             : a.dir == 1 ? x == x_dim - 1   // EAST
                             : a.dir == 2 ? y == 0           // SOUTH
                                          : y == y_dim - 1;  // NORTH
        assert(on_edge &&
               "connection: dst_dir names an edge this coordinate is not on -- an interior "
               "router's port carries a live inter-router link, and hanging an endpoint off it "
               "closes a channel dependency cycle");
        for (const auto& c : claimed) {
            assert(!(c.first == a.idx && c.second == a.dir) &&
                   "connection: two endpoints both claim the same router port");
        }
        claimed.push_back({a.idx, a.dir});
    }
}

inline SamTable load_config_table(const YAML::Node& root) {
    YAML::Node routers = root["routers"];
    assert(routers && routers.size() == 1 && "config.routers: expected exactly one router array");
    const auto array = routers[0]["array"].as<std::vector<unsigned>>();
    assert(array.size() == 2 && "config.routers: array must be [x, y]");
    const unsigned x_dim = array[0];
    const unsigned y_dim = array[1];
    // A mesh communicating through NI + router needs at least 2x2, and the
    // collective coordinate field is clog2(dim) bits wide, so a non-power-of-two
    // dimension leaves a wildcard address naming a coordinate with no router.
    assert(x_dim >= 2 && y_dim >= 2 &&
           "config.routers: mesh dimensions must be >= 2 per dimension (1x1/1xN mesh illegal)");
    assert((x_dim & (x_dim - 1)) == 0 && (y_dim & (y_dim - 1)) == 0 &&
           "config.routers: mesh dimensions must be powers of two");
    const unsigned x_bits = clog2(x_dim);

    std::vector<SamEntry> es;
    std::vector<std::pair<unsigned, unsigned>> claimed;  // (router index, dst_dir)
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
        check_boundary_attachments(attach, x_dim, y_dim, x_bits, claimed);
        // Range-major: every member of a range, then the next range -- the same
        // order sim/tools/address_map.py pack_config() walks them in, which is
        // what makes the two expansions comparable rule for rule.
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
    YAML::Node routing = root["routing"];
    const bool use_id_table =
        !routing || !routing["use_id_table"] || routing["use_id_table"].as<bool>();
    assert(use_id_table && "routing: use_id_table must be true; offset decode is deferred");
    if (!use_id_table) std::abort();
    return table;
}

// The path form the co-sim reaches through +sam_config and the c_model tests
// through a file name.
inline SamTable load_sam_table(const std::string& yaml_path) {
    YAML::Node root = YAML::LoadFile(yaml_path);
    assert(root["endpoints"] && "config: endpoints block missing");
    return load_config_table(root);
}

}  // namespace ni::cmodel::nmu::addr_trans
