// Shared flit byte-array type for the DPI wire boundary.
//
// FlitBytes carries the full c_model flit (ni::FLIT_WIDTH bits) as a byte array
// for DPI marshalling. Used by every *_wrap IO header and the DPI bridge; not
// specific to any one component.
#pragma once
#include "ni_flit_constants.h"
#include <array>
#include <cstdint>

namespace ni::cmodel::wrap {

// Full c_model flit stored as a byte array, size = ni::FLIT_WIDTH rounded up to bytes.
static constexpr int FLIT_BYTES = (ni::FLIT_WIDTH + 7) / 8;  // 51
using FlitBytes = std::array<uint8_t, FLIT_BYTES>;

// Number of 32-bit svBitVecVal words needed to carry one flit.
static constexpr int FLIT_VEC_WORDS = (ni::FLIT_WIDTH + 31) / 32;  // 13

}  // namespace ni::cmodel::wrap
