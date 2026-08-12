// DPI wire-format marshal helpers — svBitVecVal[] <-> c_model byte/word types.
//
// Word counts are derived from ni::FLIT_WIDTH / axi::DATA_WIDTH, not pinned to
// today's values (629-bit flit, 512-bit data bus): S2 T2d widened both constants
// and this header kept producing the right word count / tail mask without
// editing a single formula here. Extracted out of cmodel_dpi.cpp (rather than
// left as an anonymous-namespace block) so ctest can round-trip pack/unpack
// directly; the svdpi.h dependency confines this header to ref_model/dpi/, not
// ref_model/c_model/include/wrap/ (whose other headers stay DPI-agnostic).
#pragma once
#include "svdpi.h"
#include "axi/types.hpp"        // ni::cmodel::axi::DATA_BYTES / DATA_WIDTH
#include "wrap/flit_bytes.hpp"  // FlitBytes, FLIT_BYTES, FLIT_VEC_WORDS
#include "ni_flit_constants.h"  // ni::FLIT_WIDTH, ni::width::AXI_ADDR_WIDTH
#include "ni_params.h"          // ni::NOC_{REQ,RSP,DAT}_FLIT_WIDTH
#include <array>
#include <cassert>
#include <cstdint>

namespace ni::cmodel::wrap {

// --- word counts, derived (no pinned literals) ------------------------------

// svBitVecVal words needed for the AXI data bus (little-endian, byte-packed).
constexpr int DATA_VEC_WORDS = (axi::DATA_WIDTH + 31) / 32;
// svBitVecVal words needed for WSTRB (one bit per data-bus byte == DATA_BYTES bits).
constexpr int WSTRB_VEC_WORDS = (axi::DATA_BYTES + 31) / 32;

// --- flit tail masking, templated on per-network width (S3a T5) ------------
//
// Each physical network has its own flit width (REQ 132 b, RSP 122 b, DAT
// 629 b, docs/noc-target-spec.md §6); the C++ Flit/FlitBytes container stays
// fixed at the max (ni::FLIT_WIDTH = DAT's width, S3a stage design §6) --
// only the DPI wire word count narrows per network. FlitMarshalT<WIDTH_BITS>
// VEC_WORDS * 32 bits are reserved on the wire, but only WIDTH_BITS of them
// are real flit content; pack() masks the tail word explicitly instead of
// relying on the caller's FlitBytes padding bits already being zero.
template <int WIDTH_BITS>
struct FlitMarshalT {
    static constexpr int VEC_WORDS = (WIDTH_BITS + 31) / 32;
    static constexpr int TAIL_VALID_BITS = WIDTH_BITS - (VEC_WORDS - 1) * 32;
    static_assert(TAIL_VALID_BITS > 0 && TAIL_VALID_BITS <= 32,
                  "TAIL_VALID_BITS must be a real, non-overflowing bit count; "
                  "VEC_WORDS = ceil(WIDTH_BITS / 32) should guarantee this");
    static constexpr uint32_t TAIL_MASK =
        (TAIL_VALID_BITS >= 32) ? 0xFFFFFFFFu : ((1u << TAIL_VALID_BITS) - 1u);

    // Unpack svBitVecVal[VEC_WORDS] -> FlitBytes (little-endian within each
    // word). Bytes above VEC_WORDS*4 (this network's unused upper flit range)
    // are left at 0 from FlitBytes{} value-init.
    static FlitBytes unpack(const svBitVecVal* vec) {
        FlitBytes b{};
        for (int w = 0; w < VEC_WORDS; ++w) {
            for (int byte = 0; byte < 4; ++byte) {
                int idx = w * 4 + byte;
                if (idx < FLIT_BYTES) {
                    b[idx] = static_cast<uint8_t>((vec[w] >> (byte * 8)) & 0xFF);
                }
            }
        }
        return b;
    }

    // fits — no live flit bit sits at or above this network's wire width.
    // pack() discards everything above WIDTH_BITS; when the discarded bits are
    // real payload the loss is invisible on the wire and shows up far
    // downstream as corrupt AXI data. Same role as SimpleRouter's input-FIFO
    // overflow assert: make a wrong configuration loud, not silent.
    static bool fits(const FlitBytes& b) {
        for (int idx = 0; idx < FLIT_BYTES; ++idx) {
            const int lo_bit = idx * 8;
            if (lo_bit >= WIDTH_BITS) {
                if (b[idx] != 0) return false;
            } else if (lo_bit + 8 > WIDTH_BITS) {
                const uint8_t keep = static_cast<uint8_t>((1u << (WIDTH_BITS - lo_bit)) - 1u);
                if ((b[idx] & static_cast<uint8_t>(~keep)) != 0) return false;
            }
        }
        return true;
    }

    // Pack FlitBytes -> svBitVecVal[VEC_WORDS] (little-endian within each
    // word). The tail word is explicitly masked to TAIL_VALID_BITS so padding
    // bits never leak onto the wire regardless of what FlitBytes carries past
    // WIDTH_BITS.
    static void pack(const FlitBytes& b, svBitVecVal* vec) {
        assert(fits(b) &&
               "flit payload exceeds this network's wire width — the DPI would silently "
               "discard live bits (check the axi_ch -> network steering against the "
               "per-network NOC_<NET>_FLIT_WIDTH)");
        for (int w = 0; w < VEC_WORDS; ++w) {
            vec[w] = 0;
            for (int byte = 0; byte < 4; ++byte) {
                int idx = w * 4 + byte;
                if (idx < FLIT_BYTES) {
                    vec[w] |= static_cast<uint32_t>(b[idx]) << (byte * 8);
                }
            }
        }
        vec[VEC_WORDS - 1] &= TAIL_MASK;
    }
};

// Per-network flit marshallers (S3a T5). DAT's width equals ni::FLIT_WIDTH
// (the max), so DatFlitMarshal and the legacy FLIT_VEC_WORDS/unpack_flit/
// pack_flit names below are the same instantiation.
using ReqFlitMarshal = FlitMarshalT<ni::NOC_REQ_FLIT_WIDTH>;
using RspFlitMarshal = FlitMarshalT<ni::NOC_RSP_FLIT_WIDTH>;
using DatFlitMarshal = FlitMarshalT<ni::NOC_DAT_FLIT_WIDTH>;

// Legacy names, kept for source compatibility with existing callers/tests
// (test_cmodel_dpi.cpp): the DAT-width instantiation, since DAT carries
// ni::FLIT_WIDTH end to end. FLIT_VEC_WORDS itself is already defined in
// flit_bytes.hpp (ni::FLIT_WIDTH-derived) and equals DatFlitMarshal::VEC_WORDS;
// not redefined here.
static_assert(FLIT_VEC_WORDS == DatFlitMarshal::VEC_WORDS,
              "flit_bytes.hpp FLIT_VEC_WORDS must match the DAT-width marshal instantiation");
constexpr int FLIT_TAIL_VALID_BITS = DatFlitMarshal::TAIL_VALID_BITS;
constexpr uint32_t FLIT_TAIL_MASK = DatFlitMarshal::TAIL_MASK;
inline FlitBytes unpack_flit(const svBitVecVal* vec) {
    return DatFlitMarshal::unpack(vec);
}
inline void pack_flit(const FlitBytes& b, svBitVecVal* vec) {
    DatFlitMarshal::pack(b, vec);
}

// Unpack svBitVecVal[DATA_VEC_WORDS] -> AXI data bus bytes (little-endian).
inline std::array<uint8_t, axi::DATA_BYTES> unpack_axi_data(const svBitVecVal* vec) {
    std::array<uint8_t, axi::DATA_BYTES> out{};
    for (int w = 0; w < DATA_VEC_WORDS; ++w) {
        for (int byte = 0; byte < 4; ++byte) {
            int idx = w * 4 + byte;
            if (idx < axi::DATA_BYTES) {
                out[idx] = static_cast<uint8_t>((vec[w] >> (byte * 8)) & 0xFF);
            }
        }
    }
    return out;
}

// Pack AXI data bus bytes -> svBitVecVal[DATA_VEC_WORDS] (little-endian).
// DATA_WIDTH is a multiple of 32 at every allowed value (32..1024 per
// constants.yaml), so no tail word masking is needed here (unlike the flit).
inline void pack_axi_data(const std::array<uint8_t, axi::DATA_BYTES>& src, svBitVecVal* vec) {
    for (int w = 0; w < DATA_VEC_WORDS; ++w) {
        vec[w] = 0;
        for (int byte = 0; byte < 4; ++byte) {
            int idx = w * 4 + byte;
            if (idx < axi::DATA_BYTES) {
                vec[w] |= static_cast<uint32_t>(src[idx]) << (byte * 8);
            }
        }
    }
}

// Unpack svBitVecVal[WSTRB_VEC_WORDS] -> WSTRB (one bit per data-bus byte).
inline uint64_t unpack_wstrb(const svBitVecVal* vec) {
    uint64_t strb = 0;
    for (int w = 0; w < WSTRB_VEC_WORDS; ++w) {
        strb |= static_cast<uint64_t>(vec[w]) << (32 * w);
    }
    return strb;
}

// Pack WSTRB -> svBitVecVal[WSTRB_VEC_WORDS] (one bit per data-bus byte).
inline void pack_wstrb(uint64_t strb, svBitVecVal* vec) {
    for (int w = 0; w < WSTRB_VEC_WORDS; ++w) {
        vec[w] = static_cast<uint32_t>((strb >> (32 * w)) & 0xFFFF'FFFFu);
    }
}

// Pack 64-bit address -> svBitVecVal[2]. ADDR_WIDTH (48 b) is not parameterized
// this stage (S2 T2c widens data/flit only, per specgen/source/constants.yaml);
// the 2-word split stays exact for any ADDR_WIDTH in (32, 64].
inline void pack_addr64(uint64_t addr, svBitVecVal* vec) {
    static_assert(ni::width::AXI_ADDR_WIDTH > 32 && ni::width::AXI_ADDR_WIDTH <= 64,
                  "pack_addr64 hardcodes a 2-word (64-bit) split; widen it if "
                  "ADDR_WIDTH moves outside (32, 64]");
    vec[0] = static_cast<uint32_t>(addr & 0xFFFF'FFFFu);
    vec[1] = static_cast<uint32_t>((addr >> 32) & 0xFFFF'FFFFu);
}

}  // namespace ni::cmodel::wrap
