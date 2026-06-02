#pragma once
#include "ni_flit_constants.h"  // for ni::width::X_WIDTH / Y_WIDTH (documentation)
#include <cstdint>

namespace ni::cmodel::nmu::addr_trans {

struct Translated {
    uint8_t  dst_id;     // X_WIDTH + Y_WIDTH = 8 bits per ni_packet.json
    uint64_t local_addr; // for c_model = addr (no remap)
};

// XYRouting bit allocation (c_model policy -- will move when SAM table or
// remap added; per spec sec 4.3):
//   addr[15:0]  = local address (64 KB per dst)
//   addr[19:16] = x (low 4 bits of dst_id)
//   addr[23:20] = y (high 4 bits of dst_id)
//   addr[63:24] = unused (zero in current test fixtures)
//
// local_addr is unmodified -- XYRouting only extracts dst_id; address space
// is global for c_model. Future remap (NSU subtracts base address) may set
// local_addr = addr - base.
inline Translated xy_route(uint64_t addr) noexcept {
    constexpr uint64_t LOCAL_ADDR_BITS = 16;
    uint8_t dst = static_cast<uint8_t>((addr >> LOCAL_ADDR_BITS) & 0xFF);
    return { dst, /*local_addr=*/addr };
}

}  // namespace ni::cmodel::nmu::addr_trans
