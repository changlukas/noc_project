#pragma once
#include "ni_flit_constants.h"  // ni::width::X_WIDTH / Y_WIDTH (DST_ID composition)
#include "axi/types.hpp"        // axi::Burst (used by burst_last_byte)
#include "ni/address_map.hpp"   // BitRange / SpaceCoords / clog2 / range_mask
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>  // std::abort
#include <utility>  // std::move
#include <vector>

namespace ni::cmodel::nmu::addr_trans {

struct Translated {
    uint8_t dst_id;       // X_WIDTH + Y_WIDTH bits per ni_packet.json
    uint64_t local_addr;  // the request address, forwarded unchanged
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

// The coordinate-field vocabulary lives at the NI layer: the NMU reads the
// field, the NSU writes it (ni/address_map.hpp). Re-exported here so every
// existing addr_trans:: caller keeps its spelling.
using address_map::BitRange;
using address_map::clog2;
using address_map::range_mask;
using address_map::SpaceCoords;

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
    // order. Test fixtures only -- co-sim always loads a topology YAML, and
    // NmuWrap::init rejects a missing one. The table it builds is memory-only.
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

    // Destination and class only; the address is forwarded untouched. Upstream
    // does the same -- floo_id_translation turns an address into a node id and
    // nothing else, and a grep for addr_decode across FlooNoC's hw/ finds it at
    // exactly two sites, the source NI and the router, with no endpoint-local
    // decoder anywhere (cross-review/s5-floonoc-survey-*.md). One address domain
    // end to end is what lets a tile's own initiator and its NI share a decoder.
    Translated translate(uint64_t addr) const {
        const SamEntry* e = lookup(addr);
        assert(e && "SAM miss: address maps to no tile (config/stimulus bug)");
        return {e->dst_id, addr, e->cls};
    }

    const std::vector<SamEntry>& entries() const { return entries_; }

    // Validate explicit entries; fail-loud. Packed tables satisfy these by construction.
    // Passes run in order so each later pass can trust the earlier ones: field
    // checks first (so base+size doesn't overflow), then per-space coverage,
    // then overlap (which also relies on base+size not overflowing).
    //
    // Per-space coverage (spec §5.1 "Every node owns one region per address
    // space"): a present space covers the mesh exactly once. Memory space is
    // always required; config space is gated on being present at all, because
    // SamTable::uniform() builds memory-only tables and is the fixture
    // constructor for most c_model tests. The Python twin
    // (sim/tools/address_map.py pack()) requires both unconditionally -- it
    // only ever sees a shipped topology YAML.
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
        std::size_t config_count = 0;
        for (const auto& e : entries_) {
            unsigned x = e.dst_id & ((1u << ni::width::X_WIDTH) - 1);
            unsigned y = e.dst_id >> ni::width::X_WIDTH;
            std::size_t idx = static_cast<std::size_t>(y) * x_dim + x;
            std::vector<bool>& seen = (e.cls == axi::AxiClass::Data) ? seen_memory : seen_config;
            assert(!seen[idx] && "SAM: duplicate mesh node (same space)");
            seen[idx] = true;
            ((e.cls == axi::AxiClass::Data) ? memory_count : config_count) += 1;
        }
        assert(memory_count == mesh_nodes &&
               "SAM: memory space must cover the mesh exactly once (tile count mismatch)");
        assert((config_count == 0 || config_count == mesh_nodes) &&
               "SAM: config space must cover the mesh exactly once when present");
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            for (std::size_t j = i + 1; j < entries_.size(); ++j) {
                const auto& e = entries_[i];
                const auto& f = entries_[j];
                assert(!(e.base < f.base + f.size && f.base < e.base + e.size) &&
                       "SAM: overlapping tile ranges");
            }
        }
    }

    // Declare where this space keeps its node coordinates, per SpaceCoords.
    // Returns false -- leaving the space NOT collective-eligible -- when the
    // declaration disagrees with the entries the table already holds. Stated is
    // not trusted: the constructor is public and test fixtures build tables by
    // hand. A failing declaration is not an abort; per docs/noc-target-spec.md
    // §5.1 such a space is a legal unicast target and not a legal collective
    // target.
    //
    // This is a weaker, per-space property than validate(): it says nothing
    // about the mesh, only that the declared ranges reach this space's own
    // entries. A space that does not cover the mesh can still be eligible.
    bool declare_space_coords(axi::AxiClass cls, const SpaceCoords& c) {
        const unsigned slot = static_cast<unsigned>(cls);
        eligible_[slot] = false;
        if (c.x_count == 0 || c.y_count == 0) return false;
        if (c.x_range.len != clog2(c.x_count) || c.y_range.len != clog2(c.y_count)) return false;
        if (c.x_range.offset + c.x_range.len > 64 || c.y_range.offset + c.y_range.len > 64) {
            return false;
        }
        // One address bit cannot carry two coordinates.
        if (c.x_range.len != 0 && c.y_range.len != 0 &&
            c.x_range.offset + c.x_range.len > c.y_range.offset &&
            c.y_range.offset + c.y_range.len > c.x_range.offset) {
            return false;
        }
        const SamEntry* origin = nullptr;
        std::size_t space_entries = 0;
        for (const auto& e : entries_) {
            if (e.cls != cls) continue;
            if (origin == nullptr) origin = &e;
            ++space_entries;
        }
        if (origin == nullptr) return false;
        // No entry of this space outside the slice: the declared ranges must
        // account for the whole space, not a prefix of it.
        if (space_entries != static_cast<std::size_t>(c.x_count) * c.y_count) return false;
        const uint64_t field = range_mask(c.x_range) | range_mask(c.y_range);
        // The origin sits at coordinate (0,0) -- no base_id to subtract.
        if ((origin->base & field) != 0) return false;
        // Uniform USABLE APERTURE is a separate claim from uniform stride: an
        // aperture reaching into the coordinate bits would make a wildcard
        // address land inside its own anchor's region.
        if (field != 0 && origin->size > (field & (~field + 1))) return false;
        for (unsigned y = 0; y < c.y_count; ++y) {
            for (unsigned x = 0; x < c.x_count; ++x) {
                const uint64_t addr = origin->base | (uint64_t{x} << c.x_range.offset) |
                                      (uint64_t{y} << c.y_range.offset);
                const SamEntry* e = lookup(addr);
                if (e == nullptr || e->cls != cls) return false;  // reachable, one class
                if (e->base != addr) return false;                // uniform stride
                if (e->size != origin->size) return false;        // uniform aperture
                if (e->dst_id != ((y << ni::width::X_WIDTH) | x)) return false;  // raster order
            }
        }
        coords_[slot] = c;
        eligible_[slot] = true;
        return true;
    }

    // Declared coordinates of a collective-eligible space, else nullptr.
    const SpaceCoords* collective_coords(axi::AxiClass cls) const {
        const unsigned slot = static_cast<unsigned>(cls);
        return eligible_[slot] ? &coords_[slot] : nullptr;
    }

    bool burst_footprint_ok(uint64_t addr, uint64_t last_byte) const {
        const SamEntry* a = lookup(addr);
        return a != nullptr && last_byte >= a->base && last_byte < a->base + a->size;
    }

  private:
    std::vector<SamEntry> entries_;
    // Indexed by axi::AxiClass (Narrow = 0, Data = 1) -- one address space each.
    SpaceCoords coords_[2];
    bool eligible_[2] = {false, false};
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
// The node mask is a bit-select over the anchor space's declared coordinate
// ranges, as upstream does it (floo_axi_chimney.sv:534-546) -- two shifts, no
// walk over the map. Everything a per-replica scan would re-derive is a
// property of the SPACE and was settled when the ranges were declared
// (SamTable::declare_space_coords): one class, one node per coordinate pair, a
// shared node-local offset, one aperture.
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
    // No anchor-class gate: design Q4 revision 2 rules multicast legal on BOTH
    // classes (config-space message replication rides Narrow/REQ). The class
    // stays UNIFORM across the destination set because the mask cannot leave
    // the anchor's own space -- the outside-the-ranges check below.
    const SpaceCoords* coords = sam.collective_coords(anchor->cls);
    if (coords == nullptr) {
        assert(false &&
               "nmu::addr_trans::collective_translate: the anchor's address space declares no "
               "coordinate ranges -- a legal unicast target, not a collective target");
        std::abort();
    }

    // Spec §6: the mask is confined to the node-index field. A bit below it
    // wildcards an address bit inside one region, so the named addresses stop
    // sharing a node-local offset; a bit above it leaves the space, so they
    // stop sharing a class.
    const uint64_t field = range_mask(coords->x_range) | range_mask(coords->y_range);
    if ((addr_mask & ~field) != 0) {
        assert(false &&
               "nmu::addr_trans::collective_translate: AWUSER collective_mask sets a bit outside "
               "the coordinate ranges of the anchor's address space");
        std::abort();
    }
    const unsigned mask_x = static_cast<unsigned>(addr_mask >> coords->x_range.offset) &
                            ((1u << coords->x_range.len) - 1);
    const unsigned mask_y = static_cast<unsigned>(addr_mask >> coords->y_range.offset) &
                            ((1u << coords->y_range.len) - 1);

    // Bound the HIGHEST member of the wildcard set, anchor | mask, PER
    // COORDINATE: the set is [anchor & ~mask, anchor | mask], dst_id is
    // (y << X_WIDTH) | x, and a dimension need not be a power of two. With
    // x_count = 3, anchor x = 1 and mask_x = 0x2 names {1, 3} -- the mask alone
    // is in range and node 3 does not exist.
    constexpr unsigned kXFieldMask = (1u << ni::width::X_WIDTH) - 1;
    const unsigned anchor_x = anchor->dst_id & kXFieldMask;
    const unsigned anchor_y = anchor->dst_id >> ni::width::X_WIDTH;
    if ((anchor_x | mask_x) >= coords->x_count || (anchor_y | mask_y) >= coords->y_count) {
        assert(false &&
               "nmu::addr_trans::collective_translate: collective destination set names a node "
               "outside the mesh");
        std::abort();
    }

    // Stays per-request: AWADDR/AWLEN/AWSIZE/AWBURST are not space properties.
    // The uniform region size the declaration checked is what lets the anchor's
    // footprint stand for every replica's.
    if (!sam.burst_footprint_ok(b.addr, burst_last_byte(b.addr, b.len, b.size, b.burst))) {
        assert(false &&
               "nmu::addr_trans::collective_translate: collective burst footprint crosses a tile "
               "boundary");
        std::abort();
    }

    // The uint8_t return IS the flit's collective_mask, so the node id this
    // function reasons about and the header field it fills must stay the same
    // width. Same guard route_mask.hpp:43-46 puts on the consumer side.
    static_assert(ni::width::X_WIDTH + ni::width::Y_WIDTH == ni::width::COLLECTIVE_MASK_WIDTH,
                  "collective_mask must be one node id wide (X|Y) -- specgen drift");
    static_assert(ni::width::X_WIDTH + ni::width::Y_WIDTH <= 8,
                  "node id / collective_mask no longer fit uint8_t");
    return static_cast<uint8_t>((mask_y << ni::width::X_WIDTH) | mask_x);
}

}  // namespace ni::cmodel::nmu::addr_trans
