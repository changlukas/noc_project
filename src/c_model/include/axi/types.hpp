#pragma once
#include "ni_flit_constants.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace ni::cmodel::axi {

constexpr int DATA_BYTES = ni::WSTRB_WIDTH;
constexpr int DATA_WIDTH = DATA_BYTES * 8;

// Full (all-lanes) WSTRB mask for the current DATA_BYTES. Ternary avoids UB
// from shifting a 64-bit value by 64 when the bus reaches its uint64_t ceiling.
constexpr uint64_t kFullStrbMask = (DATA_BYTES >= 64) ? ~0ull : ((1ull << DATA_BYTES) - 1ull);

// AXI data field width in bits used by NoC payload bulk-bytes accessors
// (set_payload_bytes / get_payload_bytes for wdata / rdata).
constexpr int NOC_DATA_WIDTH_BITS = DATA_BYTES * 8;

// AXI ID space (1 << AXI_ID_WIDTH). Used to size per-id container arrays in
// the NMU Rob and NSU MetaBuffer. Locked to the codegen'd AXI_ID_WIDTH so any
// future widening of the ID field is caught at static_assert below.
constexpr std::size_t AXI_ID_SPACE = 1u << ni::width::AXI_ID_WIDTH;
static_assert(AXI_ID_SPACE == 256,
              "AXI_ID_SPACE locked to 256 (AXI_ID_WIDTH=8); update per-id "
              "container sizes if AXI_ID_WIDTH changes");

static_assert(DATA_BYTES * 8 == ni::width::NOC_DATA_WIDTH,
              "DATA_BYTES (= WSTRB_WIDTH) * 8 must equal NOC_DATA_WIDTH "
              "for byte-level WSTRB semantics");

// constants.yaml allows DATA_WIDTH ∈ {32..1024} per spec; the c_model and DPI
// marshalling lock to the S2 final shape, the shared 512 b data-class endpoint
// (DATA_BYTES = 64). Widen DATA_BYTES + WBeat/RBeat data array + WSTRB type if
// the spec changes.
static_assert(DATA_BYTES == 64,
              "c_model assumes the S2 data-class AXI DATA_WIDTH = 512 bits; widen "
              "DATA_BYTES + WBeat/RBeat data array + WSTRB type if the spec changes");

// WBeat::strb is uint64_t. If DATA_BYTES ever exceeds 64, WSTRB no longer
// fits in a single uint64_t — widen the struct field (e.g. std::bitset)
// before relaxing this.
static_assert(DATA_BYTES <= 64, "WBeat::strb is uint64_t; widen the strb field if DATA_BYTES > 64");

// Narrow-class data width: the fixed 8 B lane the narrow class occupies on the
// shared DATA_BYTES-wide port (docs/noc-target-spec.md §5). Independent of
// DATA_BYTES / DATA_WIDTH, which describe only the data class.
constexpr int NARROW_DATA_BYTES = ni::width::NOC_NARROW_DATA_WIDTH / 8;
static_assert(NARROW_DATA_BYTES == 8, "narrow class data width is fixed at 64 b (8 B) per spec §5");

enum class Burst : uint8_t { FIXED = 0, INCR = 1, WRAP = 2 };
enum class Resp : uint8_t { OKAY = 0, EXOKAY = 1, SLVERR = 2, DECERR = 3 };

// SAM address-space class (spec §5 "SAM address spaces: config, memory"):
// config selects the narrow class, memory the data class. Resolved once per
// transaction at NMU packetize from the matched SAM entry; carried end to end
// through the flit's axi_ch encoding (Narrow*/Data*) so the NSU response path
// and the NMU response path (RoB / RoBless AR-meta) can recover it without a
// second address decode.
enum class AxiClass : uint8_t { Narrow = 0, Data = 1 };

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

// Narrow-class byte lane a beat occupies on the shared DATA_BYTES-wide port:
// bus lane (beat_addr >> 3) & 7, one of DATA_BYTES/NARROW_DATA_BYTES = 8 lanes.
inline unsigned narrow_lane(uint64_t addr) {
    return static_cast<unsigned>((addr >> 3) & 0x7u);
}

// Re-anchor a narrow-class beat's data from lane 0 (where a class-agnostic
// flit decode places it -- the decode step has no address, only the AR/AW
// basis does) into its real byte lane. No-op at lane 0. Used only where the
// address arrives asynchronously relative to the decode (nmu::Rob's R path,
// docs/noc-target-spec.md / S2 design doc §2 lane re-anchor table); every
// other site knows the lane at decode time and addresses the lane directly.
inline void reanchor_narrow_lane(std::array<uint8_t, DATA_BYTES>& data, unsigned lane) {
    if (lane == 0) return;
    std::array<uint8_t, NARROW_DATA_BYTES> tmp{};
    for (int i = 0; i < NARROW_DATA_BYTES; ++i) tmp[i] = data[i];
    for (int i = 0; i < NARROW_DATA_BYTES; ++i) data[i] = 0;
    for (int i = 0; i < NARROW_DATA_BYTES; ++i) data[lane * NARROW_DATA_BYTES + i] = tmp[i];
}

struct AwBeat {
    uint8_t id;
    uint64_t addr;
    uint8_t len, size;
    Burst burst;
    uint8_t cache, lock, prot, region;
    // AWUSER, 58 b (docs/noc-target-spec.md AWUSER layout): [7:0] user-defined
    // (rides into the AW payload `user` field), [9:8] collective_op, [57:10]
    // collective_mask. Collective bits are consumed by the NMU at packetize
    // time and never reach the flit payload; nonzero collective_op or
    // collective_mask is rejected until S4 (nmu::Packetize::push_aw_with_meta).
    uint64_t user;
    uint8_t qos;
};

struct WBeat {
    std::array<uint8_t, DATA_BYTES> data;
    uint64_t strb;
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
