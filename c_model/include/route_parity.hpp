#pragma once
// route_par generation/check helper. Coverage set is fixed by
// ni_packet.json `route_par_coverage`: ["dst_id", "last"]. Even parity:
// XOR(dst_id bits, last, route_par) == 0. LAST_WIDTH is 1, so folding `last`
// into bit 0 before reduction is exact.
#include <cstdint>

namespace ni::cmodel {

inline uint8_t route_parity(uint64_t dst_id, uint64_t last) noexcept {
    uint64_t x = dst_id ^ last;
    x ^= x >> 32;
    x ^= x >> 16;
    x ^= x >> 8;
    x ^= x >> 4;
    x ^= x >> 2;
    x ^= x >> 1;
    return static_cast<uint8_t>(x & 1u);
}

}  // namespace ni::cmodel
