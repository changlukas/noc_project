#pragma once
#include "ni_flit_constants.h"  // ni::width::X_WIDTH
#include "router/router.hpp"    // RouterPort
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ni::cmodel::testing {

// A node's mesh coordinate. Node id encodes (x,y) as x | (y << X_WIDTH),
// the same scheme nmu::addr_trans uses for dst_id (X_WIDTH=4).
struct NodeCoord {
    uint8_t x;
    uint8_t y;
    bool operator==(const NodeCoord& o) const { return x == o.x && y == o.y; }
};

inline NodeCoord node_coord(uint8_t id) {
    constexpr uint8_t x_mask = static_cast<uint8_t>((1u << ni::width::X_WIDTH) - 1);
    return NodeCoord{static_cast<uint8_t>(id & x_mask),
                     static_cast<uint8_t>(id >> ni::width::X_WIDTH)};
}

inline uint8_t node_id(NodeCoord c) {
    return static_cast<uint8_t>(c.x | (c.y << ni::width::X_WIDTH));
}

// XY route: step X to the destination column, then Y to the destination row.
// Returns the inclusive coordinate sequence: k+1 coords for a k-hop path.
inline std::vector<NodeCoord> router_path(uint8_t src_id, uint8_t dst_id, uint8_t /*mesh_x*/,
                                          uint8_t /*mesh_y*/) {
    const NodeCoord src = node_coord(src_id);
    const NodeCoord dst = node_coord(dst_id);
    std::vector<NodeCoord> path;
    NodeCoord cur = src;
    path.push_back(cur);
    while (cur.x != dst.x) {
        cur.x = static_cast<uint8_t>(cur.x + (dst.x > cur.x ? 1 : -1));
        path.push_back(cur);
    }
    while (cur.y != dst.y) {
        cur.y = static_cast<uint8_t>(cur.y + (dst.y > cur.y ? 1 : -1));
        path.push_back(cur);
    }
    return path;
}

// Output port for one single-axis hop from -> to. XY routing changes X before
// Y, so each hop is one unambiguous axis step.
inline std::size_t direction(NodeCoord from, NodeCoord to) {
    if (to.x > from.x) return static_cast<std::size_t>(router::RouterPort::EAST);
    if (to.x < from.x) return static_cast<std::size_t>(router::RouterPort::WEST);
    if (to.y > from.y) return static_cast<std::size_t>(router::RouterPort::NORTH);
    return static_cast<std::size_t>(router::RouterPort::SOUTH);
}

inline std::size_t opposite(std::size_t port) {
    switch (static_cast<router::RouterPort>(port)) {
        case router::RouterPort::EAST:
            return static_cast<std::size_t>(router::RouterPort::WEST);
        case router::RouterPort::WEST:
            return static_cast<std::size_t>(router::RouterPort::EAST);
        case router::RouterPort::NORTH:
            return static_cast<std::size_t>(router::RouterPort::SOUTH);
        case router::RouterPort::SOUTH:
            return static_cast<std::size_t>(router::RouterPort::NORTH);
        default:
            return static_cast<std::size_t>(router::RouterPort::LOCAL);
    }
}

}  // namespace ni::cmodel::testing
