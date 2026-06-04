// Per-channel AXI4 pin snapshot POD structs used to pass C-model boundary
// state across the DPI to SV. Fields mirror c_model/include/axi/types.hpp
// (AwBeat, WBeat, ArBeat, BBeat, RBeat) one-for-one and add `valid`/`ready`
// handshake bits which the channel beats do not carry.
//
// No methods, no constructors. Plain copyable PODs so the DPI getter can
// fill in fields directly with no marshalling overhead.
//
// NOTE: region and user fields are intentionally omitted — they are not part
// of the AXI4 protocol checker's interface (wb2axip does not check them).
#pragma once
#include "axi/types.hpp"
#include <array>
#include <cstdint>

namespace ni::cmodel::cosim {

using ni::cmodel::axi::DATA_BYTES;

struct AwPins {
    bool valid;
    bool ready;
    uint8_t id;
    uint64_t addr;
    uint8_t len, size;
    uint8_t burst;  // Burst encoded as raw 2-bit (0=FIXED, 1=INCR, 2=WRAP)
    uint8_t lock, cache, prot, qos;
};

struct WPins {
    bool valid;
    bool ready;
    std::array<uint8_t, DATA_BYTES> data;
    uint32_t strb;
    bool last;
};

struct ArPins {
    bool valid;
    bool ready;
    uint8_t id;
    uint64_t addr;
    uint8_t len, size;
    uint8_t burst;
    uint8_t lock, cache, prot, qos;
};

struct BPins {
    bool valid;
    bool ready;
    uint8_t id;
    uint8_t resp;  // Resp encoded as raw 2-bit
};

struct RPins {
    bool valid;
    bool ready;
    uint8_t id;
    std::array<uint8_t, DATA_BYTES> data;
    uint8_t resp;
    bool last;
};

}  // namespace ni::cmodel::cosim
