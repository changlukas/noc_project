#pragma once
#include "ni_flit_constants.h"  // ni::width::X_WIDTH / Y_WIDTH (DST_ID composition)
#include "axi/types.hpp"        // axi::Burst (used by burst_last_byte, Task 4)
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>  // std::move
#include <vector>

namespace ni::cmodel::nmu::addr_trans {

// XYRouting bit allocation (c_model policy -- will move when SAM table or
// remap added; per spec sec 4.3):
//   addr[LOCAL_ADDR_BITS-1:0]                = local address (4 GB per dst)
//   addr[LOCAL_ADDR_BITS + X_WIDTH - 1 : LOCAL_ADDR_BITS]               = x
//   addr[LOCAL_ADDR_BITS + DST_ID_BITS - 1 : LOCAL_ADDR_BITS + X_WIDTH] = y
//   addr[63 : LOCAL_ADDR_BITS + DST_ID_BITS] = unused (zero in test fixtures)
//
// LOCAL_ADDR_BITS is namespace-scope so Packetize / Rob can share it without
// duplicating `>> 32`. DST_ID_MASK derives from generated X_WIDTH + Y_WIDTH
// so this file desync-detects if the spec regenerates with different widths.
constexpr uint64_t LOCAL_ADDR_BITS = 32;
constexpr unsigned DST_ID_BITS = ni::width::X_WIDTH + ni::width::Y_WIDTH;
constexpr uint8_t DST_ID_MASK = static_cast<uint8_t>((1u << DST_ID_BITS) - 1);
static_assert(DST_ID_BITS == 8,
              "addr_trans assumes 8-bit dst_id; if X+Y changes, audit LOCAL_ADDR_BITS");

struct Translated {
    uint8_t dst_id;       // X_WIDTH + Y_WIDTH bits per ni_packet.json
    uint64_t local_addr;  // for c_model = addr (no remap)
};

struct SamEntry {
    uint64_t base;
    uint64_t size;
    uint8_t dst_id;
    uint64_t remove_offset;
};

class SamTable {
  public:
    SamTable() = default;
    explicit SamTable(std::vector<SamEntry> entries) : entries_(std::move(entries)) {}

    // Uniform map: dst_id = coord_id = (y<<X_WIDTH)|x, base = coord_id * tile_size.
    static SamTable uniform(unsigned x_dim, unsigned y_dim, uint64_t tile_size, bool rebase) {
        std::vector<SamEntry> es;
        for (unsigned y = 0; y < y_dim; ++y) {
            for (unsigned x = 0; x < x_dim; ++x) {
                uint8_t dst = static_cast<uint8_t>((y << ni::width::X_WIDTH) | x);
                uint64_t base = static_cast<uint64_t>(dst) * tile_size;
                es.push_back({base, tile_size, dst, rebase ? base : 0});
            }
        }
        return SamTable(std::move(es));
    }

    // First-match by start address (FlooNoC get_entry). Miss -> nullptr.
    const SamEntry* lookup(uint64_t addr) const {
        for (const auto& e : entries_) {
            if (addr >= e.base && addr < e.base + e.size) return &e;
        }
        return nullptr;
    }

    Translated translate(uint64_t addr) const {
        const SamEntry* e = lookup(addr);
        assert(e && "SAM miss: address maps to no tile (config/stimulus bug)");
        return {e->dst_id, addr - e->remove_offset};
    }

    const std::vector<SamEntry>& entries() const { return entries_; }

    // Validate explicit entries; fail-loud. Uniform tables satisfy these by construction.
    // Two passes (Codex): validate every entry's fields FIRST so the overlap pass can
    // trust each `base+size` (no overflow) when it reads it.
    void validate(unsigned x_dim, unsigned y_dim) const {
        constexpr uint64_t k4k = 0x1000;
        for (const auto& e : entries_) {
            assert(e.size != 0 && "SAM: zero-size tile");
            assert((e.base % k4k == 0) && (e.size % k4k == 0) &&
                   "SAM: base and size must be 4 KB aligned");
            assert(e.base + e.size > e.base && "SAM: base+size overflow");
            assert(e.remove_offset <= e.base && "SAM: remove_offset > base (local underflow)");
            unsigned x = e.dst_id & ((1u << ni::width::X_WIDTH) - 1);
            unsigned y = e.dst_id >> ni::width::X_WIDTH;
            assert(x < x_dim && y < y_dim && "SAM: dst outside mesh");
        }
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            for (std::size_t j = i + 1; j < entries_.size(); ++j) {
                const auto& e = entries_[i];
                const auto& f = entries_[j];
                assert(!(e.base < f.base + f.size && f.base < e.base + e.size) &&
                       "SAM: overlapping tile ranges");
            }
        }
    }

    bool burst_footprint_ok(uint64_t addr, uint64_t last_byte) const {
        const SamEntry* a = lookup(addr);
        return a != nullptr && last_byte >= a->base && last_byte < a->base + a->size;
    }

  private:
    std::vector<SamEntry> entries_;
};

// local_addr is unmodified -- XYRouting only extracts dst_id; address space
// is global for c_model. Future remap (NSU subtracts base address) may set
// local_addr = addr - base.
inline Translated xy_route(uint64_t addr) noexcept {
    uint8_t dst = static_cast<uint8_t>((addr >> LOCAL_ADDR_BITS) & DST_ID_MASK);
    return {dst, /*local_addr=*/addr};
}

}  // namespace ni::cmodel::nmu::addr_trans
