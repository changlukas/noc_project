#pragma once
#include "ni_flit_constants.h"  // ni::width::X_WIDTH / Y_WIDTH (DST_ID composition)
#include "axi/types.hpp"        // axi::Burst (used by burst_last_byte, Task 4)
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>  // std::move
#include <vector>

namespace ni::cmodel::nmu::addr_trans {

struct Translated {
    uint8_t dst_id;       // X_WIDTH + Y_WIDTH bits per ni_packet.json
    uint64_t local_addr;  // tile-local (rebased): addr - tile base
};

struct SamEntry {
    uint64_t base;
    uint64_t size;
    uint8_t dst_id;
};

class SamTable {
  public:
    SamTable() = default;
    explicit SamTable(std::vector<SamEntry> entries) : entries_(std::move(entries)) {}

    // Uniform map: dst_id = coord_id = (y<<X_WIDTH)|x, base = coord_id * tile_size.
    static SamTable uniform(unsigned x_dim, unsigned y_dim, uint64_t tile_size) {
        std::vector<SamEntry> es;
        for (unsigned y = 0; y < y_dim; ++y) {
            for (unsigned x = 0; x < x_dim; ++x) {
                uint8_t dst = static_cast<uint8_t>((y << ni::width::X_WIDTH) | x);
                uint64_t base = static_cast<uint64_t>(dst) * tile_size;
                es.push_back({base, tile_size, dst});
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
        return {e->dst_id, addr - e->base};  // rebase: slave sees 0-based local address
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

// Highest byte a burst touches, for the SAM footprint guard (Task 4).
inline uint64_t burst_last_byte(uint64_t addr, uint8_t len, uint8_t size, axi::Burst burst) {
    uint64_t bytes_per_beat = uint64_t{1} << size;
    uint64_t total = bytes_per_beat * (static_cast<uint64_t>(len) + 1);
    if (burst == axi::Burst::WRAP) {
        // Legal WRAP len is 1/3/7/15 (protocol_rules.hpp:65-75) so total is a power of
        // two; the window is [addr - addr%total, +total). Same window axi types.hpp uses.
        uint64_t wrap_lower = addr - (addr % total);
        return wrap_lower + total - 1;
    }
    // INCR and FIXED both use [addr, addr+total): match the slave's OOB math
    // (axi_slave.hpp:316-318,:519-520 treats FIXED like INCR), so the SAM guard and the
    // slave agree at a tile edge (Codex #6). Conservative for FIXED, but consistent.
    return addr + total - 1;
}

}  // namespace ni::cmodel::nmu::addr_trans
