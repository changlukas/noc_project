// DPI-C bridge: wraps AxiDpiAdapter in C-linkage functions callable from SV.
//
// Address marshalling (ADDR_WIDTH = 64 bits):
//   AwPins/ArPins::addr is uint64_t. Verilator passes svBitVecVal[2] for a
//   bit [63:0] port: [0] = bits[31:0], [1] = bits[63:32].
//   pack_addr64() splits the uint64_t into the two words.
//
// Data marshalling (DATA_WIDTH = 256 bits, DATA_BYTES = 32):
//   WPins/RPins::data is std::array<uint8_t, 32>. Verilator passes
//   svBitVecVal[8] for a bit [255:0] port: [word_i] covers bytes
//   [4*word_i+3 : 4*word_i] (little-endian word order, little-endian byte
//   within word). pack_data256() reassembles 8 words from the byte array.
//
// WSTRB (32 bits): fits in a single svBitVecVal; copied directly.
//
// This file is C++ (not C) so it can include the C++ adapter header.
#include "axi_dpi.h"
#include "cosim/axi_dpi_adapter.hpp"
#include "cosim/pin_snapshot.hpp"
#include <cstring>
#include <memory>

using ni::cmodel::cosim::ArPins;
using ni::cmodel::cosim::AwPins;
using ni::cmodel::cosim::AxiDpiAdapter;
using ni::cmodel::cosim::BPins;
using ni::cmodel::cosim::RPins;
using ni::cmodel::cosim::WPins;

namespace {

std::unique_ptr<AxiDpiAdapter> g_adapter;

// Split uint64_t addr into two svBitVecVal words (little-endian word order).
inline void pack_addr64(uint64_t addr, svBitVecVal* out) {
    out[0] = static_cast<svBitVecVal>(addr & 0xFFFFFFFFu);
    out[1] = static_cast<svBitVecVal>((addr >> 32u) & 0xFFFFFFFFu);
}

// Pack std::array<uint8_t, 32> into eight svBitVecVal words.
// Verilator word order: out[0] = bits[31:0]  = bytes[3:0],
//                       out[1] = bits[63:32] = bytes[7:4], etc.
// Within each word bytes are little-endian: byte[4*i] → bits[7:0] of out[i].
template <std::size_t N>
inline void pack_data(const std::array<uint8_t, N>& src, svBitVecVal* out) {
    static_assert(N % 4 == 0, "data byte count must be a multiple of 4");
    constexpr std::size_t num_words = N / 4;
    for (std::size_t i = 0; i < num_words; ++i) {
        out[i] = (static_cast<svBitVecVal>(src[4 * i + 0])) |
                 (static_cast<svBitVecVal>(src[4 * i + 1]) << 8u) |
                 (static_cast<svBitVecVal>(src[4 * i + 2]) << 16u) |
                 (static_cast<svBitVecVal>(src[4 * i + 3]) << 24u);
    }
}

// ---------------------------------------------------------------------------
// DRY pack helpers — one per channel
// ---------------------------------------------------------------------------

void pack_aw(const AwPins& p, svBit* valid, svBit* ready, svBitVecVal* id, svBitVecVal* addr,
             svBitVecVal* len, svBitVecVal* size, svBitVecVal* burst, svBitVecVal* lock,
             svBitVecVal* cache, svBitVecVal* prot, svBitVecVal* qos) {
    *valid = p.valid ? 1 : 0;
    *ready = p.ready ? 1 : 0;
    *id = p.id;
    pack_addr64(p.addr, addr);
    *len = p.len;
    *size = p.size;
    *burst = p.burst;
    *lock = p.lock;
    *cache = p.cache;
    *prot = p.prot;
    *qos = p.qos;
}

void pack_w(const WPins& p, svBit* valid, svBit* ready, svBitVecVal* data, svBitVecVal* strb,
            svBit* last) {
    *valid = p.valid ? 1 : 0;
    *ready = p.ready ? 1 : 0;
    pack_data(p.data, data);
    *strb = static_cast<svBitVecVal>(p.strb);
    *last = p.last ? 1 : 0;
}

void pack_ar(const ArPins& p, svBit* valid, svBit* ready, svBitVecVal* id, svBitVecVal* addr,
             svBitVecVal* len, svBitVecVal* size, svBitVecVal* burst, svBitVecVal* lock,
             svBitVecVal* cache, svBitVecVal* prot, svBitVecVal* qos) {
    *valid = p.valid ? 1 : 0;
    *ready = p.ready ? 1 : 0;
    *id = p.id;
    pack_addr64(p.addr, addr);
    *len = p.len;
    *size = p.size;
    *burst = p.burst;
    *lock = p.lock;
    *cache = p.cache;
    *prot = p.prot;
    *qos = p.qos;
}

void pack_b(const BPins& p, svBit* valid, svBit* ready, svBitVecVal* id, svBitVecVal* resp) {
    *valid = p.valid ? 1 : 0;
    *ready = p.ready ? 1 : 0;
    *id = p.id;
    *resp = p.resp;
}

void pack_r(const RPins& p, svBit* valid, svBit* ready, svBitVecVal* id, svBitVecVal* data,
            svBitVecVal* resp, svBit* last) {
    *valid = p.valid ? 1 : 0;
    *ready = p.ready ? 1 : 0;
    *id = p.id;
    pack_data(p.data, data);
    *resp = p.resp;
    *last = p.last ? 1 : 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Exported DPI-C functions
// ---------------------------------------------------------------------------

extern "C" {

// --- Lifecycle --------------------------------------------------------------

void cmodel_init(const char* scenario_yaml_path) {
    g_adapter = std::make_unique<AxiDpiAdapter>();
    g_adapter->init(scenario_yaml_path);
}

void cmodel_finalize(void) {
    g_adapter.reset();
}

void cmodel_tick(void) {
    if (g_adapter) g_adapter->tick();
}

int cmodel_done(void) {
    return (g_adapter && g_adapter->done()) ? 1 : 0;
}

int cmodel_scoreboard_clean(void) {
    return (g_adapter && g_adapter->scoreboard_clean()) ? 1 : 0;
}

// --- NMU channel getters ----------------------------------------------------

void cmodel_nmu_get_aw(svBit* valid, svBit* ready, svBitVecVal* id, svBitVecVal* addr,
                       svBitVecVal* len, svBitVecVal* size, svBitVecVal* burst, svBitVecVal* lock,
                       svBitVecVal* cache, svBitVecVal* prot, svBitVecVal* qos) {
    AwPins p{};
    if (g_adapter) g_adapter->get_nmu_aw(p);
    pack_aw(p, valid, ready, id, addr, len, size, burst, lock, cache, prot, qos);
}

void cmodel_nmu_get_w(svBit* valid, svBit* ready, svBitVecVal* data, svBitVecVal* strb,
                      svBit* last) {
    WPins p{};
    if (g_adapter) g_adapter->get_nmu_w(p);
    pack_w(p, valid, ready, data, strb, last);
}

void cmodel_nmu_get_ar(svBit* valid, svBit* ready, svBitVecVal* id, svBitVecVal* addr,
                       svBitVecVal* len, svBitVecVal* size, svBitVecVal* burst, svBitVecVal* lock,
                       svBitVecVal* cache, svBitVecVal* prot, svBitVecVal* qos) {
    ArPins p{};
    if (g_adapter) g_adapter->get_nmu_ar(p);
    pack_ar(p, valid, ready, id, addr, len, size, burst, lock, cache, prot, qos);
}

void cmodel_nmu_get_b(svBit* valid, svBit* ready, svBitVecVal* id, svBitVecVal* resp) {
    BPins p{};
    if (g_adapter) g_adapter->get_nmu_b(p);
    pack_b(p, valid, ready, id, resp);
}

void cmodel_nmu_get_r(svBit* valid, svBit* ready, svBitVecVal* id, svBitVecVal* data,
                      svBitVecVal* resp, svBit* last) {
    RPins p{};
    if (g_adapter) g_adapter->get_nmu_r(p);
    pack_r(p, valid, ready, id, data, resp, last);
}

// --- NSU channel getters ----------------------------------------------------

void cmodel_nsu_get_aw(svBit* valid, svBit* ready, svBitVecVal* id, svBitVecVal* addr,
                       svBitVecVal* len, svBitVecVal* size, svBitVecVal* burst, svBitVecVal* lock,
                       svBitVecVal* cache, svBitVecVal* prot, svBitVecVal* qos) {
    AwPins p{};
    if (g_adapter) g_adapter->get_nsu_aw(p);
    pack_aw(p, valid, ready, id, addr, len, size, burst, lock, cache, prot, qos);
}

void cmodel_nsu_get_w(svBit* valid, svBit* ready, svBitVecVal* data, svBitVecVal* strb,
                      svBit* last) {
    WPins p{};
    if (g_adapter) g_adapter->get_nsu_w(p);
    pack_w(p, valid, ready, data, strb, last);
}

void cmodel_nsu_get_ar(svBit* valid, svBit* ready, svBitVecVal* id, svBitVecVal* addr,
                       svBitVecVal* len, svBitVecVal* size, svBitVecVal* burst, svBitVecVal* lock,
                       svBitVecVal* cache, svBitVecVal* prot, svBitVecVal* qos) {
    ArPins p{};
    if (g_adapter) g_adapter->get_nsu_ar(p);
    pack_ar(p, valid, ready, id, addr, len, size, burst, lock, cache, prot, qos);
}

void cmodel_nsu_get_b(svBit* valid, svBit* ready, svBitVecVal* id, svBitVecVal* resp) {
    BPins p{};
    if (g_adapter) g_adapter->get_nsu_b(p);
    pack_b(p, valid, ready, id, resp);
}

void cmodel_nsu_get_r(svBit* valid, svBit* ready, svBitVecVal* id, svBitVecVal* data,
                      svBitVecVal* resp, svBit* last) {
    RPins p{};
    if (g_adapter) g_adapter->get_nsu_r(p);
    pack_r(p, valid, ready, id, data, resp, last);
}

}  // extern "C"
