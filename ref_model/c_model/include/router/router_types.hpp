#pragma once
// Router port and config base types, split out of router.hpp so that
// route_mask.hpp (the T1 collective route-mask functions) and router.hpp can
// share them without an include cycle: the DAT router's stage-2 multicast
// fork calls route_mask_fork(), and route_mask.hpp needs RouterPort /
// RouterConfig. Definitions are verbatim moves from router.hpp.
#include "ni_params.h"

#include <cstddef>
#include <cstdint>

namespace ni::cmodel::router {

enum class RouterPort : uint8_t { LOCAL = 0, NORTH = 1, EAST = 2, SOUTH = 3, WEST = 4 };
inline constexpr std::size_t ROUTER_PORT_COUNT = 5;

struct RouterConfig {
    uint8_t x = 0;
    uint8_t y = 0;
    uint8_t mesh_x_dim = NOC_MESH_X_DIM;
    uint8_t mesh_y_dim = NOC_MESH_Y_DIM;
    // Inclusive tile-region bounds inside the route span. mesh_*_dim is the
    // span and bounds the range check; these bound collectives. Defaults make
    // a plain mesh, where the two coincide.
    uint8_t tile_x_first = 0;
    uint8_t tile_x_last = static_cast<uint8_t>(NOC_MESH_X_DIM - 1);
    uint8_t tile_y_first = 0;
    uint8_t tile_y_last = static_cast<uint8_t>(NOC_MESH_Y_DIM - 1);
    uint8_t num_vc = NOC_DAT_NUM_VC;
    std::size_t vc_depth = NOC_ROUTER_VC_DEPTH;
    std::size_t output_fifo_depth = NOC_ROUTER_OUTPUT_FIFO_DEPTH;
};

}  // namespace ni::cmodel::router
