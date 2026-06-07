// Flit <-> FlitBytes byte-array conversions shared by all cosim shell adapters.
//
// Extracted to dedupe identical helpers previously living in
// nmu_shell_adapter.hpp, nsu_shell_adapter.hpp, and loopback_noc_shell_adapter.hpp.
// FlitBytes is a std::array<uint8_t, FLIT_BYTES> defined in loopback_noc_shell_io.hpp;
// Flit::raw() returns std::array<uint8_t, Flit::WIDTH_BYTES>. The static_assert below
// ensures FLIT_BYTES (cosim DPI marshalling size) equals Flit::WIDTH_BYTES (c_model
// flit container size), so the per-byte copy is well-defined.
#pragma once
#include "cosim/loopback_noc_shell_io.hpp"  // FlitBytes, FLIT_BYTES
#include "ni/flit.hpp"                       // Flit
#include <array>
#include <cstdint>

namespace ni::cmodel::cosim {

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

}  // namespace ni::cmodel::cosim
