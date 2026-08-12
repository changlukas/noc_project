#pragma once
// Collective route-mask dual function — line-translate of FlooNoC
// hw/floo_route_xymask.sv, the block that turns a masked (wildcard) node id
// into a multi-hot port set. Both directions live here, selected upstream by
// the FwdMode parameter (:24-27):
//   route_mask_fork()  = FwdMode=1, gen_output_mask (:126-164) — the output
//       ports a multicast flit forks to at this router.
//   route_mask_join()  = FwdMode=0, gen_expected_input_mask (:200-237) — the
//       input ports a CollectB join waits on at this router.
// Only RouteAlgo=XYRouting is translated; the YX branches (:165-195, :238-267)
// have no counterpart here because the fabric's route_compute (router.hpp:70)
// is XY dimension-order. Upstream's Eject is our RouterPort::LOCAL — ports
// translate by name, the same convention simple_router.hpp:46-50 uses.
//
// Mask semantics (:43-45, :102-114): a set collective_mask bit is a don't-care
// on that coordinate bit, so a mask with n set bits names 2^n nodes spanning
// [id & ~mask, id | mask]. The mask reuses the dst_id composition — X in the
// low X_WIDTH bits, Y next (router.hpp:71-73) — and every width comes from
// specgen.
//
// The two directions are NOT mirror images and must not be used as such. The
// fork tree spreads X along the source's row, then Y (:143-164). The join
// collects along each member's XY unicast return path — X within the member's
// own row toward the collector's column, then Y in that column (:216-237).
// Same member set and same hop count both ways; the interior edges differ for
// any mask with both X and Y bits set. A join set may also be a single port,
// which is a plain forward of an already-merged flit, not a join of one.
//
// Pure functions of (dst_id, src_id, collective_mask, this router's config).
// No router state — the fork/join state machines live in their consumers.
#include "ni_flit_constants.h"
#include "router/router_types.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>

namespace ni::cmodel::router {

// The uint8_t node-id/mask API below assumes a mask is exactly one node id
// wide and that an id fits a byte. Both are specgen facts today; catch drift
// at compile time rather than in silently truncated coordinates.
static_assert(ni::width::COLLECTIVE_MASK_WIDTH == ni::width::X_WIDTH + ni::width::Y_WIDTH,
              "collective_mask must be one node id wide (X|Y) — specgen drift");
static_assert(ni::width::COLLECTIVE_MASK_WIDTH <= 8,
              "node id / collective_mask no longer fit uint8_t — widen the route_mask API");

// Multi-hot port set: bit p set means RouterPort p is a member.
// (route_sel_o, floo_route_xymask.sv:40.)
using PortMask = uint8_t;

inline constexpr PortMask port_bit(RouterPort p) {
    return static_cast<PortMask>(1u << static_cast<uint8_t>(p));
}

inline constexpr bool port_in_mask(PortMask set, RouterPort p) {
    return (set & port_bit(p)) != 0;
}

namespace detail {

struct NodeCoord {
    uint8_t x;
    uint8_t y;
};

// dst_id, src_id and collective_mask share one composition (router.hpp:71-73).
inline NodeCoord split_node_id(uint8_t id) {
    return {static_cast<uint8_t>(id & ((1u << ni::width::X_WIDTH) - 1)),
            static_cast<uint8_t>((id >> ni::width::X_WIDTH) & ((1u << ni::width::Y_WIDTH) - 1))};
}

// Reduction-AND of (mask | ~(a ^ b)) — floo_route_xymask.sv:117-118, :121-122.
// True when every bit where a and b differ is a don't-care.
inline bool coord_matched(uint8_t a, uint8_t b, uint8_t mask) {
    return ((a ^ b) & static_cast<uint8_t>(~mask)) == 0;
}

inline bool in_mesh(NodeCoord c, const RouterConfig& cfg) {
    return c.x < cfg.mesh_x_dim && c.y < cfg.mesh_y_dim;
}

}  // namespace detail

// FwdMode=1: output ports a multicast flit with (dst_id, src_id, mask) forks
// to at the router described by cfg. Empty for a router the multicast tree
// never reaches.
inline PortMask route_mask_fork(uint8_t dst_id, uint8_t src_id, uint8_t collective_mask,
                                const RouterConfig& cfg) {
    const detail::NodeCoord dst = detail::split_node_id(dst_id);
    const detail::NodeCoord src = detail::split_node_id(src_id);
    const detail::NodeCoord mask = detail::split_node_id(collective_mask);

    // :104-107 — span of the destination set.
    detail::NodeCoord dst_max{static_cast<uint8_t>(dst.x | mask.x),
                              static_cast<uint8_t>(dst.y | mask.y)};
    detail::NodeCoord dst_min{static_cast<uint8_t>(dst.x & ~mask.x),
                              static_cast<uint8_t>(dst.y & ~mask.y)};

    // Collectives name tiles only. The wildcard block may reach a border
    // coordinate or a coordinate that does not exist; both are clipped here, in
    // the same way on the fork and the join, so every router computes the same
    // member set (spec, "Collectives are clipped to the tile region").
    dst_min.x = std::max<uint8_t>(dst_min.x, cfg.tile_x_first);
    dst_min.y = std::max<uint8_t>(dst_min.y, cfg.tile_y_first);
    dst_max.x = std::min<uint8_t>(dst_max.x, cfg.tile_x_last);
    dst_max.y = std::min<uint8_t>(dst_max.y, cfg.tile_y_last);
    if (dst_min.x > dst_max.x || dst_min.y > dst_max.y) {
        assert(false &&
               "route_mask_fork: collective member set is empty after clipping "
               "to the tile region -- the source should have refused it");
        std::abort();
    }

    // dst_max is now <= tile_max, inside the mesh span by construction, so
    // only the (unclamped) source still needs the range check.
    if (!detail::in_mesh(src, cfg)) {
        assert(false && "route_mask_fork: source outside mesh range");
        std::abort();
    }

    // :117-118 — is this router a receiver of the multicast.
    const bool x_matched = detail::coord_matched(cfg.x, dst.x, mask.x);
    const bool y_matched = detail::coord_matched(cfg.y, dst.y, mask.y);

    PortMask route = 0;
    if (x_matched && y_matched) route |= port_bit(RouterPort::LOCAL);  // :131-133

    // :143-150 — X spread, on the source's row only.
    if (cfg.y == src.y) {
        if (cfg.x >= src.x && cfg.x < dst_max.x) route |= port_bit(RouterPort::EAST);
        if (cfg.x <= src.x && cfg.x > dst_min.x) route |= port_bit(RouterPort::WEST);
    }
    // :157-164 — Y turn, in every column the destination set covers.
    if (x_matched) {
        if (cfg.y >= src.y && cfg.y < dst_max.y) route |= port_bit(RouterPort::NORTH);
        if (cfg.y <= src.y && cfg.y > dst_min.y) route |= port_bit(RouterPort::SOUTH);
    }
    return route;
}

// FwdMode=0: input ports the collector expects replicas from, for a response
// whose src_id is the masked (wildcard) side and whose dst_id is the single
// collecting node. Empty for a router no replica passes through.
inline PortMask route_mask_join(uint8_t dst_id, uint8_t src_id, uint8_t collective_mask,
                                const RouterConfig& cfg) {
    const detail::NodeCoord dst = detail::split_node_id(dst_id);
    const detail::NodeCoord src = detail::split_node_id(src_id);
    const detail::NodeCoord mask = detail::split_node_id(collective_mask);

    // :111-114 — span of the source set.
    detail::NodeCoord src_max{static_cast<uint8_t>(src.x | mask.x),
                              static_cast<uint8_t>(src.y | mask.y)};
    detail::NodeCoord src_min{static_cast<uint8_t>(src.x & ~mask.x),
                              static_cast<uint8_t>(src.y & ~mask.y)};

    // Collectives name tiles only. The wildcard block may reach a border
    // coordinate or a coordinate that does not exist; both are clipped here, in
    // the same way on the fork and the join, so every router computes the same
    // member set (spec, "Collectives are clipped to the tile region").
    src_min.x = std::max<uint8_t>(src_min.x, cfg.tile_x_first);
    src_min.y = std::max<uint8_t>(src_min.y, cfg.tile_y_first);
    src_max.x = std::min<uint8_t>(src_max.x, cfg.tile_x_last);
    src_max.y = std::min<uint8_t>(src_max.y, cfg.tile_y_last);
    if (src_min.x > src_max.x || src_min.y > src_max.y) {
        assert(false &&
               "route_mask_join: collective member set is empty after clipping "
               "to the tile region -- the source should have refused it");
        std::abort();
    }

    // src_max is now <= tile_max, inside the mesh span by construction, so
    // only the (unclamped) collector still needs the range check.
    if (!detail::in_mesh(dst, cfg)) {
        assert(false && "route_mask_join: destination outside mesh range");
        std::abort();
    }

    // :121-122 — does this router contribute an element to the collect.
    const bool x_matched = detail::coord_matched(cfg.x, src.x, mask.x);
    const bool y_matched = detail::coord_matched(cfg.y, src.y, mask.y);

    PortMask route = 0;
    if (x_matched && y_matched) route |= port_bit(RouterPort::LOCAL);  // :205-207

    // :216-223 — Y collect, in the collector's column only.
    if (cfg.x == dst.x) {
        if (cfg.y >= dst.y && cfg.y < src_max.y) route |= port_bit(RouterPort::NORTH);
        if (cfg.y <= dst.y && cfg.y > src_min.y) route |= port_bit(RouterPort::SOUTH);
    }
    // :230-237 — X collect, in every row the source set covers.
    if (y_matched) {
        if (cfg.x >= dst.x && cfg.x < src_max.x) route |= port_bit(RouterPort::EAST);
        if (cfg.x <= dst.x && cfg.x > src_min.x) route |= port_bit(RouterPort::WEST);
    }
    return route;
}

}  // namespace ni::cmodel::router
