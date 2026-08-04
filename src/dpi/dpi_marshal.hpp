// DPI wire-format marshal helpers — svBitVecVal[] <-> c_model byte/word types.
//
// Word counts are derived from ni::FLIT_WIDTH / axi::DATA_WIDTH, not pinned to
// today's values (341-bit flit, 256-bit data bus): S2 T2d widens both constants
// and this header keeps producing the right word count / tail mask without
// editing a single formula here. Extracted out of cmodel_dpi.cpp (rather than
// left as an anonymous-namespace block) so ctest can round-trip pack/unpack
// directly; the svdpi.h dependency confines this header to src/dpi/, not
// src/c_model/include/wrap/ (whose other headers stay DPI-agnostic).
#pragma once
#include "svdpi.h"
#include "axi/types.hpp"        // ni::cmodel::axi::DATA_BYTES / DATA_WIDTH
#include "wrap/flit_bytes.hpp"  // FlitBytes, FLIT_BYTES, FLIT_VEC_WORDS
#include "ni_flit_constants.h"  // ni::FLIT_WIDTH, ni::width::AXI_ADDR_WIDTH
#include <array>
#include <cstdint>

namespace ni::cmodel::wrap {

// --- word counts, derived (no pinned literals) ------------------------------

// svBitVecVal words needed for the AXI data bus (little-endian, byte-packed).
constexpr int DATA_VEC_WORDS = (axi::DATA_WIDTH + 31) / 32;
// svBitVecVal words needed for WSTRB (one bit per data-bus byte == DATA_BYTES bits).
constexpr int WSTRB_VEC_WORDS = (axi::DATA_BYTES + 31) / 32;

// --- flit tail masking --------------------------------------------------
//
// FLIT_VEC_WORDS * 32 bits are reserved on the wire, but only FLIT_WIDTH of
// them are real flit content (341 of 352 today; 21 valid bits in the last
// word). pack_flit masks the tail word explicitly instead of relying on the
// caller's FlitBytes padding bits already being zero.

constexpr int FLIT_TAIL_VALID_BITS = ni::FLIT_WIDTH - (FLIT_VEC_WORDS - 1) * 32;
static_assert(FLIT_TAIL_VALID_BITS > 0 && FLIT_TAIL_VALID_BITS <= 32,
              "FLIT_TAIL_VALID_BITS must be a real, non-overflowing bit count; "
              "FLIT_VEC_WORDS = ceil(FLIT_WIDTH / 32) should guarantee this");
constexpr uint32_t FLIT_TAIL_MASK =
    (FLIT_TAIL_VALID_BITS >= 32) ? 0xFFFFFFFFu : ((1u << FLIT_TAIL_VALID_BITS) - 1u);

// Unpack svBitVecVal[FLIT_VEC_WORDS] -> FlitBytes (little-endian within each word).
inline FlitBytes unpack_flit(const svBitVecVal* vec) {
    FlitBytes b{};
    for (int w = 0; w < FLIT_VEC_WORDS; ++w) {
        for (int byte = 0; byte < 4; ++byte) {
            int idx = w * 4 + byte;
            if (idx < FLIT_BYTES) {
                b[idx] = static_cast<uint8_t>((vec[w] >> (byte * 8)) & 0xFF);
            }
        }
    }
    return b;
}

// Pack FlitBytes -> svBitVecVal[FLIT_VEC_WORDS] (little-endian within each word).
// The tail word is explicitly masked to FLIT_TAIL_VALID_BITS so padding bits
// never leak onto the wire regardless of what FlitBytes carries past FLIT_WIDTH.
inline void pack_flit(const FlitBytes& b, svBitVecVal* vec) {
    for (int w = 0; w < FLIT_VEC_WORDS; ++w) {
        vec[w] = 0;
        for (int byte = 0; byte < 4; ++byte) {
            int idx = w * 4 + byte;
            if (idx < FLIT_BYTES) {
                vec[w] |= static_cast<uint32_t>(b[idx]) << (byte * 8);
            }
        }
    }
    vec[FLIT_VEC_WORDS - 1] &= FLIT_TAIL_MASK;
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
