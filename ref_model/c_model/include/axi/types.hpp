#pragma once
#include "ni_flit_constants.h"
#include "ni_params.h"
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

// The external AXI port and packet field are intentionally independent.
// C++/DPI records at the NoC boundary are keyed by the fixed NoC ID; the
// endpoint's RTL remap restores the external AXI ID before returning a response.
static_assert(ni::NOC_ID_WIDTH == ni::width::NOC_ID_WIDTH,
              "constants.yaml axi.NOC_ID_WIDTH and ni_packet.json flit.field_widths.NOC_ID_WIDTH "
              "must agree; regenerate both after changing either");
static_assert(ni::NOC_ID_WIDTH == 3,
              "the c_model/DPI boundary is the approved fixed 3-bit NoC ID instance");

// NOC_ID_SPACE sizes per-NoC-ID containers in the NMU RoB and NSU MetaBuffer.
// It must never be derived from AXI_ID_WIDTH, which is the external port width.
constexpr std::size_t NOC_ID_SPACE = 1u << ni::width::NOC_ID_WIDTH;
static_assert(NOC_ID_SPACE == 8,
              "NOC_ID_SPACE locked to 8 (NOC_ID_WIDTH=3); update per-NoC-ID "
              "container sizes if NOC_ID_WIDTH changes");

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

// SAM address space. Distinct from AxiClass: the two agreed one-to-one while
// there were two spaces and two classes, but a peripheral region is a third
// space carrying the Data class. The SAM keys on this; the flit still carries
// only the class, through axi_ch.
enum class Space : uint8_t { Config = 0, Memory = 1, Peripheral = 2 };

inline constexpr AxiClass class_of(Space space) {
    return space == Space::Config ? AxiClass::Narrow : AxiClass::Data;
}

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
    return static_cast<unsigned>((addr >> 3) & (DATA_BYTES / NARROW_DATA_BYTES - 1));
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
    // collective_mask. Collective bits are consumed by the NMU at admission
    // (nmu::addr_trans::collective_translate, called from nmu::Rob::push_aw)
    // and never reach the flit payload; the direct nmu::Packetize::push_aw
    // interface bypasses that validation and rejects them outright.
    uint64_t user;
    uint8_t qos;
};

// AWUSER[9:8] carries the same encoding as the flit header field of the same
// name, so both sides read one specgen table (ni_packet.json
// header_fields[collective_op].encoding, generated into ni::COLLECTIVE_OP_*
// and ni_flit_pkg::COLLECTIVE_OP_* for the SV side). Narrowed to uint8_t here
// because that is the width every AXI-side field holding one uses. Codes 2-3
// are absent from the table by design: they are reserved and reject.
constexpr uint8_t COLLECTIVE_OP_UNICAST = ni::COLLECTIVE_OP_UNICAST;
constexpr uint8_t COLLECTIVE_OP_MULTICAST = ni::COLLECTIVE_OP_MULTICAST;

// AWUSER[9:8]. The 48 b AWUSER[57:10] is an ADDRESS mask, not the 8 b node mask
// the flit header carries: a set bit marks the matching AWADDR bit don't care.
// nmu::addr_trans::collective_translate does the address-mask -> node-mask step.
constexpr uint8_t awuser_collective_op(uint64_t user) {
    return static_cast<uint8_t>((user >> 8) & 0x3u);
}
constexpr uint64_t awuser_collective_mask(uint64_t user) {
    return (user >> 10) & ((uint64_t{1} << 48) - 1);
}

// The lifted AWUSER width (specgen constants.yaml) and the AWUSER layout the
// accessors above hardcode come from two different specgen domains, so they
// agree by construction only while this holds.
static_assert(ni::AXI_AWUSER_WIDTH == ni::width::AXI_USER_WIDTH + ni::width::COLLECTIVE_OP_WIDTH +
                                          ni::width::AXI_ADDR_WIDTH,
              "AXI_AWUSER_WIDTH disagrees with the spec AWUSER layout "
              "(user + collective_op + address mask)");

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
