#pragma once
#include "ni_flit_constants.h"  // ni::width::X_WIDTH / Y_WIDTH (DST_ID composition)
#include "axi/types.hpp"        // axi::Burst (used by burst_last_byte)
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>  // std::abort
#include <utility>  // std::move
#include <vector>

namespace ni::cmodel::nmu::addr_trans {

struct Translated {
    uint8_t dst_id;       // X_WIDTH + Y_WIDTH bits per ni_packet.json
    uint64_t local_addr;  // tile-local (rebased): addr - tile base
    // Default Data: every SamTable built without a "space" annotation (the
    // uniform()/packed() C++ test helpers, and the co-sim no-config_path
    // fallback) is memory space. sam_yaml.hpp's YAML loader picks the real
    // per-tile default explicitly instead of relying on this one.
    axi::AxiClass cls = axi::AxiClass::Data;
};

struct SamEntry {
    uint64_t base;
    uint64_t size;
    uint8_t dst_id;
    axi::AxiClass cls = axi::AxiClass::Data;
};

// One packed-map input tile: mesh coordinate + size. Bases are not given here;
// SamTable::packed() derives them by accumulation in list order.
struct PackedTile {
    unsigned x;
    unsigned y;
    uint64_t size;
    axi::AxiClass cls = axi::AxiClass::Data;
};

class SamTable {
  public:
    SamTable() = default;
    explicit SamTable(std::vector<SamEntry> entries) : entries_(std::move(entries)) {}

    // Packed map: base(0) = 0, base(i) = base(i-1) + size(i-1), in list order.
    // dst_id = (y << X_WIDTH) | x per tile.
    static SamTable packed(const std::vector<PackedTile>& tiles) {
        std::vector<SamEntry> es;
        es.reserve(tiles.size());
        uint64_t base = 0;
        for (const auto& t : tiles) {
            uint8_t dst = static_cast<uint8_t>((t.y << ni::width::X_WIDTH) | t.x);
            es.push_back({base, t.size, dst, t.cls});
            base += t.size;
        }
        return SamTable(std::move(es));
    }

    // Convenience: pack x_dim*y_dim equal-size tiles in row-major (x, then y)
    // order. Used as the co-sim default SAM (no config_path) and by test
    // fixtures that don't need heterogeneous tile sizes.
    static SamTable uniform(unsigned x_dim, unsigned y_dim, uint64_t tile_size) {
        std::vector<PackedTile> tiles;
        tiles.reserve(static_cast<std::size_t>(x_dim) * y_dim);
        for (unsigned y = 0; y < y_dim; ++y) {
            for (unsigned x = 0; x < x_dim; ++x) {
                tiles.push_back({x, y, tile_size});
            }
        }
        return packed(tiles);
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
        return {e->dst_id, addr - e->base, e->cls};  // rebase: slave sees 0-based local address
    }

    const std::vector<SamEntry>& entries() const { return entries_; }

    // Validate explicit entries; fail-loud. Packed tables satisfy these by construction.
    // Passes run in order so each later pass can trust the earlier ones: field
    // checks first (so base+size doesn't overflow), then per-space coverage,
    // then overlap (which also relies on base+size not overflowing).
    //
    // Per-space coverage (spec §5 "A node may appear once per space"): a node
    // may carry one memory-space tile and, optionally, one config-space tile.
    // Memory space must cover the mesh exactly once per node (every node has a
    // data-class home); config space is sparse -- at most one tile per node,
    // no full-mesh requirement.
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
        const std::size_t mesh_nodes = static_cast<std::size_t>(x_dim) * y_dim;
        std::vector<bool> seen_memory(mesh_nodes, false);
        std::vector<bool> seen_config(mesh_nodes, false);
        std::size_t memory_count = 0;
        for (const auto& e : entries_) {
            unsigned x = e.dst_id & ((1u << ni::width::X_WIDTH) - 1);
            unsigned y = e.dst_id >> ni::width::X_WIDTH;
            std::size_t idx = static_cast<std::size_t>(y) * x_dim + x;
            std::vector<bool>& seen = (e.cls == axi::AxiClass::Data) ? seen_memory : seen_config;
            assert(!seen[idx] && "SAM: duplicate mesh node (same space)");
            seen[idx] = true;
            if (e.cls == axi::AxiClass::Data) ++memory_count;
        }
        assert(memory_count == mesh_nodes &&
               "SAM: memory space must cover the mesh exactly once (tile count mismatch)");
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

// Highest byte a burst touches, for the SAM footprint guard.
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
    // slave agree at a tile edge. Conservative for FIXED, but consistent.
    return addr + total - 1;
}

// AWUSER collective validate + translate (S4 design §2.2 / §2.3), called from
// nmu::Rob::push_aw before any admission gate. Returns the 8 b flit
// collective_mask (node mask); 0 for a plain unicast AW.
//
// Upstream translates with a pure bit-select over SAM-provided node-index bit
// offsets (floo_axi_chimney.sv:534-546). Our SAM is a first-match RANGE lookup
// that stores no such offset, so the honest generalization is to ENUMERATE the
// 2^n addresses the mask names and require the resulting node set to be exactly
// a wildcard over dst_id. n is capped at X_WIDTH+Y_WIDTH, so at most 256 SAM
// lookups per AW -- model-only cost, re-run on every backpressure retry of the
// same AW (design K3: memoization is not warranted).
//
// Every reject here is a PERMANENT illegal input: it never clears on retry, so
// returning false would wedge the caller indistinguishably from congestion.
// Fail loud instead -- the convention this file already uses for a SAM miss.
inline uint8_t collective_translate(const SamTable& sam, const axi::AwBeat& b) {
    const uint8_t op = axi::awuser_collective_op(b.user);
    const uint64_t addr_mask = axi::awuser_collective_mask(b.user);

    // op/mask consistency matrix. Upstream cannot mismatch -- it has no explicit
    // op input and derives one from the mask (floo_axi_chimney.sv:580). Ours is
    // explicit, so a mismatch is a stimulus contradiction, not a downgrade.
    if (op == axi::COLLECTIVE_OP_UNICAST) {
        if (addr_mask != 0) {
            assert(false &&
                   "nmu::addr_trans::collective_translate: AWUSER collective_mask nonzero with "
                   "collective_op=UNICAST -- UNICAST requires a zero mask");
            std::abort();
        }
        return 0;
    }
    if (op != axi::COLLECTIVE_OP_MULTICAST) {
        assert(false &&
               "nmu::addr_trans::collective_translate: reserved AWUSER collective_op (2-3)");
        std::abort();
    }
    if (addr_mask == 0) {
        assert(false &&
               "nmu::addr_trans::collective_translate: collective_op=MULTICAST with a zero "
               "AWUSER collective_mask -- empty destination set");
        std::abort();
    }
    // Spec §6.1: AxLOCK is unicast only, a collective is not an exclusive access.
    if (b.lock != 0) {
        assert(false &&
               "nmu::addr_trans::collective_translate: AWLOCK set on a collective AW -- AxLOCK "
               "is unicast only");
        std::abort();
    }

    const SamEntry* anchor = sam.lookup(b.addr);
    if (anchor == nullptr) {
        assert(false &&
               "nmu::addr_trans::collective_translate: collective anchor address maps to no tile");
        std::abort();
    }
    // Design Q4 (ruled): S4 multicasts the Data class only. The DAT router owns
    // the only fork; a Narrow-class collective would additionally need one in
    // the REQ SimpleRouter.
    if (anchor->cls != axi::AxiClass::Data) {
        assert(false &&
               "nmu::addr_trans::collective_translate: narrow-class collective -- S4 supports "
               "Data-class multicast only");
        std::abort();
    }

    // Set-bit positions of the address mask. More set bits than a node id has
    // bits names more wildcard addresses than the mesh has nodes to absorb.
    constexpr unsigned kNodeIdBits = ni::width::X_WIDTH + ni::width::Y_WIDTH;
    // The uint8_t return IS the flit's collective_mask, so the node id this
    // function reasons about and the header field it fills must stay the same
    // width. Same guard route_mask.hpp:43-46 puts on the consumer side.
    static_assert(kNodeIdBits == ni::width::COLLECTIVE_MASK_WIDTH,
                  "collective_mask must be one node id wide (X|Y) -- specgen drift");
    static_assert(kNodeIdBits <= 8, "node id / collective_mask no longer fit uint8_t");
    uint8_t pos[kNodeIdBits];
    unsigned n = 0;
    for (unsigned i = 0; i < 48; ++i) {
        if (((addr_mask >> i) & 1u) == 0) continue;
        if (n == kNodeIdBits) {
            assert(false &&
                   "nmu::addr_trans::collective_translate: AWUSER collective_mask sets more bits "
                   "than a node id has -- destination set larger than the mesh");
            std::abort();
        }
        pos[n++] = static_cast<uint8_t>(i);
    }

    const uint64_t base_addr = b.addr & ~addr_mask;
    const std::size_t replicas = std::size_t{1} << n;
    bool seen[std::size_t{1} << kNodeIdBits] = {};
    uint8_t dst0 = 0;
    uint64_t local0 = 0;
    uint8_t node_mask = 0;
    for (std::size_t v = 0; v < replicas; ++v) {
        uint64_t addr = base_addr;
        for (unsigned k = 0; k < n; ++k) {
            if ((v >> k) & 1u) addr |= uint64_t{1} << pos[k];
        }
        const SamEntry* e = sam.lookup(addr);
        if (e == nullptr) {
            assert(false &&
                   "nmu::addr_trans::collective_translate: collective replica address maps to no "
                   "tile -- destination set leaves the mesh");
            std::abort();
        }
        const uint64_t local = addr - e->base;
        if (v == 0) {
            dst0 = e->dst_id;
            local0 = local;
        }
        // Spec :461-462: every replica carries the same node-local offset, one
        // aligned region per node. A mask bit inside the tile offset breaks this.
        if (local != local0 || e->cls != anchor->cls) {
            assert(false &&
                   "nmu::addr_trans::collective_translate: collective replicas disagree on "
                   "node-local offset or address space");
            std::abort();
        }
        if (seen[e->dst_id]) {
            assert(false &&
                   "nmu::addr_trans::collective_translate: collective mask names one node twice "
                   "-- not a wildcard over dst_id");
            std::abort();
        }
        seen[e->dst_id] = true;
        node_mask |= static_cast<uint8_t>(e->dst_id ^ dst0);
        if (!sam.burst_footprint_ok(addr, burst_last_byte(addr, b.len, b.size, b.burst))) {
            assert(false &&
                   "nmu::addr_trans::collective_translate: collective replica burst footprint "
                   "crosses a tile boundary");
            std::abort();
        }
    }

    // The node set must be exactly the wildcard set over node_mask, so
    // popcount(node_mask) == n. It already holds 2^n DISTINCT ids that all agree
    // outside node_mask, hence all lie inside a span of 2^popcount ids: equal
    // cardinality then forces the set to BE that span. dst0|node_mask is
    // therefore one of the enumerated -- so SAM-real, so in-mesh -- ids, which
    // is design §2.2's step-4 submesh check without needing the mesh dims here.
    // "SAM-real implies in-mesh" is SamTable::validate's dst-outside-mesh check
    // (:105-107), which the YAML loader runs; hand-built test tables bypass it.
    unsigned span_bits = 0;
    for (unsigned i = 0; i < kNodeIdBits; ++i) span_bits += (node_mask >> i) & 1u;
    if (span_bits != n) {
        assert(false &&
               "nmu::addr_trans::collective_translate: collective node set is not an aligned "
               "wildcard over dst_id");
        std::abort();
    }
    return node_mask;
}

}  // namespace ni::cmodel::nmu::addr_trans
