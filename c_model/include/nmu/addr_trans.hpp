#pragma once
#include "ni_flit_constants.h"  // ni::width::X_WIDTH / Y_WIDTH (DST_ID composition)
#include <cstdint>

namespace ni::cmodel::nmu::addr_trans {

// XYRouting bit allocation (c_model policy -- will move when SAM table or
// remap added; per spec sec 4.3):
//   addr[LOCAL_ADDR_BITS-1:0]                = local address (64 KB per dst)
//   addr[LOCAL_ADDR_BITS + X_WIDTH - 1 : LOCAL_ADDR_BITS]               = x
//   addr[LOCAL_ADDR_BITS + DST_ID_BITS - 1 : LOCAL_ADDR_BITS + X_WIDTH] = y
//   addr[63 : LOCAL_ADDR_BITS + DST_ID_BITS] = unused (zero in test fixtures)
//
// LOCAL_ADDR_BITS is namespace-scope so Packetize / Rob can share it without
// duplicating `>> 16`. DST_ID_MASK derives from generated X_WIDTH + Y_WIDTH
// so this file desync-detects if the spec regenerates with different widths.
constexpr uint64_t LOCAL_ADDR_BITS = 16;
constexpr unsigned DST_ID_BITS     = ni::width::X_WIDTH + ni::width::Y_WIDTH;
constexpr uint8_t  DST_ID_MASK     = static_cast<uint8_t>((1u << DST_ID_BITS) - 1);
static_assert(DST_ID_BITS == 8,
              "addr_trans assumes 8-bit dst_id; if X+Y changes, audit LOCAL_ADDR_BITS");

struct Translated {
    uint8_t  dst_id;     // X_WIDTH + Y_WIDTH bits per ni_packet.json
    uint64_t local_addr; // for c_model = addr (no remap)
};

// local_addr is unmodified -- XYRouting only extracts dst_id; address space
// is global for c_model. Future remap (NSU subtracts base address) may set
// local_addr = addr - base.
inline Translated xy_route(uint64_t addr) noexcept {
    uint8_t dst = static_cast<uint8_t>((addr >> LOCAL_ADDR_BITS) & DST_ID_MASK);
    return { dst, /*local_addr=*/addr };
}

}  // namespace ni::cmodel::nmu::addr_trans
