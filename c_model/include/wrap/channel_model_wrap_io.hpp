// ChannelModel shell IO POD structs — Stage 5b spec §5.1 / §6.2.
//
// FlitBytes carries the full c_model flit (ni::FLIT_WIDTH = 408 bits = 51 bytes).
// DPI side packs/unpacks into svBitVecVal[13] words (each 32-bit, little-endian
// within word; the 13th word carries only bits [407:384] in its low 24 bits).
//
// credit_return: one bit per direction for NUM_VC=1 PoC. Multi-VC extends to
// credit_return[NUM_VC-1:0] — shell adapter signals the per-VC credit_avail
// state from ChannelModel as a single aggregated bit (any VC available).
#pragma once
#include "ni_flit_constants.h"
#include <array>
#include <cstdint>

namespace ni::cmodel::wrap {

// Full c_model flit stored as byte array for DPI marshalling.
// Size matches ni::FLIT_WIDTH bits rounded up to bytes.
static constexpr int FLIT_BYTES = (ni::FLIT_WIDTH + 7) / 8;  // 51
using FlitBytes = std::array<uint8_t, FLIT_BYTES>;

// Number of 32-bit svBitVecVal words needed to carry one flit.
static constexpr int FLIT_VEC_WORDS = (ni::FLIT_WIDTH + 31) / 32;  // 13

struct ChannelModelInputs {
    // NMU side: request flit entering the channel (push toward NSU side)
    bool req_in_valid;
    FlitBytes req_in_flit;
    bool req_in_credit_return;  // NUM_VC=1 PoC: single aggregated bit

    // NSU side: response flit entering the channel (push toward NMU side)
    bool rsp_in_valid;
    FlitBytes rsp_in_flit;
    bool rsp_in_credit_return;
};

struct ChannelModelOutputs {
    // NSU side: request flit exiting the channel
    bool req_out_valid;
    FlitBytes req_out_flit;
    bool req_out_credit_return;

    // NMU side: response flit exiting the channel
    bool rsp_out_valid;
    FlitBytes rsp_out_flit;
    bool rsp_out_credit_return;
};

}  // namespace ni::cmodel::wrap
