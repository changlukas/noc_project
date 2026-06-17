#pragma once
#include "ni_flit_constants.h"  // ni::width::X_WIDTH / Y_WIDTH
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ni::cmodel::testing {

// Decompose a node id into mesh (x, y) using the same bit layout as the router
// (X in the low X_WIDTH bits, Y above). dst_id layout matches nmu::addr_trans.
inline void node_xy(uint8_t id, uint8_t& x, uint8_t& y) {
    const uint8_t x_mask = static_cast<uint8_t>((1u << ni::width::X_WIDTH) - 1);
    x = static_cast<uint8_t>(id & x_mask);
    y = static_cast<uint8_t>((id >> ni::width::X_WIDTH) &
                             static_cast<uint8_t>((1u << ni::width::Y_WIDTH) - 1));
}

inline uint8_t make_node_id(uint8_t x, uint8_t y) {
    return static_cast<uint8_t>(x | (y << ni::width::X_WIDTH));
}

// XY dimension-order path from src to dst (inclusive): step X first, then Y.
// Returns the sequence of node ids visited. Hop count == size() - 1.
inline std::vector<uint8_t> xy_path(uint8_t src_id, uint8_t dst_id, uint8_t /*mesh_x_dim*/,
                                    uint8_t /*mesh_y_dim*/) {
    uint8_t sx, sy, dx, dy;
    node_xy(src_id, sx, sy);
    node_xy(dst_id, dx, dy);
    std::vector<uint8_t> path;
    uint8_t cx = sx, cy = sy;
    path.push_back(make_node_id(cx, cy));
    while (cx != dx) {
        cx = static_cast<uint8_t>(cx + (dx > cx ? 1 : -1));
        path.push_back(make_node_id(cx, cy));
    }
    while (cy != dy) {
        cy = static_cast<uint8_t>(cy + (dy > cy ? 1 : -1));
        path.push_back(make_node_id(cx, cy));
    }
    return path;
}

// Per-component, per-leg segment depths (Pass-1 measured). req leg uses AW/W/AR
// segment depths; rsp leg uses B/R. router_* is the per-router-hop depth.
struct DepthTable {
    uint64_t nmu_req;
    uint64_t nmu_rsp;
    uint64_t nsu_req;
    uint64_t nsu_rsp;
    uint64_t router_req;
    uint64_t router_rsp;
};

// Section 6 formula. router hop count is the number of routers traversed on a
// leg; for an N-node path the flit crosses (N-1) inter-router links but passes
// through the routers at each hop. Per spec sec 6 the segment depths tile the
// path with no gap; the request and response legs each traverse the same hop
// count, so router_req / router_rsp are each multiplied by the inter-router hop
// count (path.size() - 1). Serialization (num_data_flits - 1) is applied once.
inline uint64_t zero_load(uint8_t src_id, uint8_t dst_id, uint8_t mesh_x_dim, uint8_t mesh_y_dim,
                          std::size_t num_data_flits, const DepthTable& d) {
    const auto path = xy_path(src_id, dst_id, mesh_x_dim, mesh_y_dim);
    const uint64_t hops = static_cast<uint64_t>(path.empty() ? 0 : path.size() - 1);
    const uint64_t request_leg = d.nmu_req + hops * d.router_req + d.nsu_req;
    const uint64_t response_leg = d.nsu_rsp + hops * d.router_rsp + d.nmu_rsp;
    const uint64_t serialization = num_data_flits > 0 ? num_data_flits - 1 : 0;
    return request_leg + response_leg + serialization;
}

// Section 6 / 7.1 range check: the serialization term is valid only while the
// burst fits within the downstream buffer (no self-credit-stall). Outside this
// range the signature is out of the calculator's analytic range.
inline bool burst_within_buffer(std::size_t num_data_flits, std::size_t buffer_depth) {
    return num_data_flits <= buffer_depth;
}

}  // namespace ni::cmodel::testing
