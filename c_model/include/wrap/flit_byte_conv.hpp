// Flit <-> FlitBytes byte-array conversions shared by all wrap components.
//
// Extracted to dedupe identical helpers previously living in
// nmu_wrap.hpp, nsu_wrap.hpp, and channel_model_wrap.hpp.
// FlitBytes is a std::array<uint8_t, FLIT_BYTES> defined in flit_bytes.hpp;
// Flit::raw() returns std::array<uint8_t, Flit::WIDTH_BYTES>. The static_assert below
// ensures FLIT_BYTES (DPI marshalling size) equals Flit::WIDTH_BYTES (c_model
// flit container size), so the per-byte copy is well-defined.
#pragma once
#include "wrap/flit_bytes.hpp"             // FlitBytes, FLIT_BYTES
#include "flit.hpp"                        // Flit
#include <array>
#include <cstdint>

namespace ni::cmodel::wrap {

inline Flit flit_from_bytes(const FlitBytes& b) {
    static_assert(FLIT_BYTES == Flit::WIDTH_BYTES, "FlitBytes size must equal Flit::WIDTH_BYTES");
    std::array<uint8_t, Flit::WIDTH_BYTES> raw{};
    for (int i = 0; i < Flit::WIDTH_BYTES; ++i) raw[i] = b[i];
    return Flit(raw);
}

inline FlitBytes flit_to_bytes(const Flit& f) {
    FlitBytes b{};
    for (int i = 0; i < Flit::WIDTH_BYTES; ++i) b[i] = f.raw()[i];
    return b;
}

}  // namespace ni::cmodel::wrap
