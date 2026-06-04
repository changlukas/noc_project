// Algorithms ported from cocotbext-axi (MIT) — see axi/ATTRIBUTION.md
#pragma once
#include "ni_flit_constants.h"
#include <array>
#include <cstdint>

namespace ni::cmodel::axi {

constexpr int DATA_BYTES = ni::WSTRB_WIDTH;
constexpr int DATA_WIDTH = DATA_BYTES * 8;

static_assert(DATA_BYTES * 8 == ni::width::NOC_DATA_WIDTH,
              "DATA_BYTES (= WSTRB_WIDTH) * 8 must equal NOC_DATA_WIDTH "
              "for byte-level WSTRB semantics");

enum class Burst : uint8_t { FIXED = 0, INCR = 1, WRAP = 2 };
enum class Resp : uint8_t { OKAY = 0, EXOKAY = 1, SLVERR = 2, DECERR = 3 };

// AXI4 IHI 0022 §A7.2: AxLOCK is 1-bit in AXI4 (0=Normal, 1=Exclusive).
// AXI3 deprecated LOCKED bit is not modeled. AwBeat/ArBeat::lock keeps
// uint8_t wire fidelity; LockType is the typed scenario-level abstraction.
enum class LockType : uint8_t { Normal = 0, Exclusive = 1 };

// AXI4 per-beat address (IHI 0022, B1.4.3 Address structure of bursts).
// Single source of truth used by AxiSlave, AxiMaster (W push + R accumulator),
// and Scoreboard so the FIXED/INCR/WRAP switch is not duplicated.
//   FIXED: every beat at base_addr.
//   INCR : base_addr + beat_idx * (1<<size).
//   WRAP : INCR within [wrap_lower, wrap_upper); wraps to wrap_lower at upper.
//          wrap_lower = base_addr & ~(total_burst_bytes - 1).
//          total_burst_bytes = (len+1) * (1<<size). WRAP requires
//          len ∈ {1,3,7,15} (enforced in parser) → total is a power of 2, so
//          the mask is well-defined.
inline uint64_t beat_addr(uint64_t base_addr, uint8_t len, uint8_t size, Burst burst,
                          std::size_t beat_idx) {
    const std::size_t bpb = 1ull << size;
    switch (burst) {
        case Burst::FIXED:
            return base_addr;
        case Burst::INCR:
            return base_addr + beat_idx * bpb;
        case Burst::WRAP: {
            const std::size_t total_burst_bytes = (static_cast<std::size_t>(len) + 1u) * bpb;
            const uint64_t wrap_lower =
                base_addr & ~(static_cast<uint64_t>(total_burst_bytes) - 1u);
            const uint64_t wrap_upper = wrap_lower + total_burst_bytes;
            const uint64_t naive = base_addr + beat_idx * bpb;
            return (naive < wrap_upper) ? naive : wrap_lower + (naive - wrap_upper);
        }
    }
    return base_addr;  // unreachable
}

struct AwBeat {
    uint8_t id;
    uint64_t addr;
    uint8_t len, size;
    Burst burst;
    uint8_t cache, lock, prot, region, user, qos;
};

struct WBeat {
    std::array<uint8_t, DATA_BYTES> data;
    uint32_t strb;
    bool last;
    uint8_t user;
};

struct ArBeat {
    uint8_t id;
    uint64_t addr;
    uint8_t len, size;
    Burst burst;
    uint8_t cache, lock, prot, region, user, qos;
};

struct BBeat {
    uint8_t id;
    Resp resp;
    uint8_t user;
};

struct RBeat {
    uint8_t id;
    std::array<uint8_t, DATA_BYTES> data;
    Resp resp;
    bool last;
    uint8_t user;
};

}  // namespace ni::cmodel::axi
