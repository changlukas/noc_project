#pragma once
#include "ni_flit_constants.h"  // ni::width::X_WIDTH / Y_WIDTH (DST_ID composition)
#include "axi/types.hpp"        // axi::Burst (used by burst_last_byte)
#include "ni/address_map.hpp"   // BitRange / SpaceCoords / clog2 / range_mask
#include <algorithm>            // std::max / std::min
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>   // std::fprintf (fatal-message style, flit.hpp)
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
// SamTable::packed() derives them from the coordinate and the space's slot
// size, not from list order.
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

    // Base from the coordinate and block_size, not accumulation. Spaces sit
    // inside a node's block, memory first at 0, each aligned to its own slot.
    // Twin of sim/tools/address_map.py's pack(): the two must agree bit for
    // bit, because the stimulus generator and this model address the same
    // map. x_span/y_span are the route span (docs/noc-target-spec.md), not
    // necessarily the tile count -- a peripheral coordinate outside the tile
    // region still counts. Every topology with no peripheral has the two
    // equal. dst_id = (y << X_WIDTH) | x per tile.
    static SamTable packed(const std::vector<PackedTile>& tiles, unsigned x_span, unsigned y_span,
                           uint64_t block_size) {
        (void)y_span;  // the caller still needs it for validate()
        const unsigned x_bits = clog2(x_span);
        uint64_t memory_slot = 0;
        uint64_t config_slot = 0;
        for (const auto& t : tiles) {
            uint64_t& slot = (t.cls == axi::AxiClass::Narrow) ? config_slot : memory_slot;
            slot = std::max(slot, t.size);
        }
        // Spaces sit inside a node's block, memory first at 0, each aligned to
        // its own slot. block_size is the stride and is declared.
        const uint64_t config_offset =
            config_slot == 0 ? 0 : ((memory_slot + config_slot - 1) / config_slot) * config_slot;
        assert((block_size & (block_size - 1)) == 0 && "SAM: block_size must be a power of two");
        assert(block_size >= config_offset + config_slot &&
               "SAM: block_size smaller than the spaces it must hold");
        std::vector<SamEntry> es;
        es.reserve(tiles.size());
        for (const auto& t : tiles) {
            const bool is_config = t.cls == axi::AxiClass::Narrow;
            const uint64_t base =
                (uint64_t{(t.y << x_bits) | t.x}) * block_size + (is_config ? config_offset : 0);
            es.push_back(
                {base, t.size, static_cast<uint8_t>((t.y << ni::width::X_WIDTH) | t.x), t.cls});
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
        return packed(tiles, x_dim, y_dim, tile_size);
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
    void validate(unsigned x_span, unsigned y_span) const {
        constexpr uint64_t k4k = 0x1000;
        for (const auto& e : entries_) {
            assert(e.size != 0 && "SAM: zero-size tile");
            assert((e.base % k4k == 0) && (e.size % k4k == 0) &&
                   "SAM: base and size must be 4 KB aligned");
            assert(e.base + e.size > e.base && "SAM: base+size overflow");
            unsigned x = e.dst_id & ((1u << ni::width::X_WIDTH) - 1);
            unsigned y = e.dst_id >> ni::width::X_WIDTH;
            assert(x < x_span && y < y_span && "SAM: dst outside mesh");
        }
        const std::size_t mesh_nodes = static_cast<std::size_t>(x_span) * y_span;
        std::vector<bool> seen_memory(mesh_nodes, false);
        std::vector<bool> seen_config(mesh_nodes, false);
        std::size_t memory_count = 0;
        std::size_t config_count = 0;
        for (const auto& e : entries_) {
            unsigned x = e.dst_id & ((1u << ni::width::X_WIDTH) - 1);
            unsigned y = e.dst_id >> ni::width::X_WIDTH;
            std::size_t idx = static_cast<std::size_t>(y) * x_span + x;
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
        const unsigned tiles_x = c.x_last - c.x_first + 1;
        const unsigned tiles_y = c.y_last - c.y_first + 1;
        const SamEntry* origin = nullptr;
        std::size_t tile_entries = 0;
        for (const auto& e : entries_) {
            if (e.cls != cls) continue;
            const unsigned x = e.dst_id & ((1u << ni::width::X_WIDTH) - 1);
            const unsigned y = e.dst_id >> ni::width::X_WIDTH;
            if (x == c.x_first && y == c.y_first) origin = &e;
            if (x < c.x_first || x > c.x_last || y < c.y_first || y > c.y_last) continue;
            ++tile_entries;
        }
        if (origin == nullptr) return false;
        // No tile-region entry outside the slice: the declared ranges must
        // account for the whole tile region, not a prefix of it. A peripheral
        // outside the tile region is not walked and does not have to fit.
        if (tile_entries != static_cast<std::size_t>(tiles_x) * tiles_y) return false;
        const uint64_t field = range_mask(c.x_range) | range_mask(c.y_range);
        // The origin sits at the tile region's own corner (c.x_first,
        // c.y_first), not necessarily (0,0) -- a peripheral may occupy a
        // lower coordinate.
        const uint64_t origin_field =
            (uint64_t{c.x_first} << c.x_range.offset) | (uint64_t{c.y_first} << c.y_range.offset);
        if ((origin->base & field) != origin_field) return false;
        // Uniform USABLE APERTURE is a separate claim from uniform stride: an
        // aperture reaching into the coordinate bits would make a wildcard
        // address land inside the region of the address it wildcards.
        if (field != 0 && origin->size > (field & (~field + 1))) return false;
        const uint64_t base_zero = origin->base & ~field;
        for (unsigned y = c.y_first; y <= c.y_last; ++y) {
            for (unsigned x = c.x_first; x <= c.x_last; ++x) {
                const uint64_t addr = base_zero | (uint64_t{x} << c.x_range.offset) |
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
// The node mask is a bit-select over the declared coordinate ranges of the
// space the request address falls in, as upstream does it
// (floo_axi_chimney.sv:534-546) -- two shifts, no walk over the map.
// Everything a per-replica scan would re-derive is a
// property of the SPACE and was settled when the ranges were declared
// (SamTable::declare_space_coords): one class, one node per coordinate pair, a
// shared node-local offset, one aperture.
//
// Every reject here is a PERMANENT illegal input: it never clears on retry, so
// returning false would wedge the caller indistinguishably from congestion.
// Fail loud instead -- the convention this file already uses for a SAM miss.
inline uint8_t collective_translate(const SamTable& sam, const axi::AwBeat& b, uint8_t src_id) {
    constexpr unsigned kXFieldMask = (1u << ni::width::X_WIDTH) - 1;
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

    const SamEntry* entry = sam.lookup(b.addr);
    if (entry == nullptr) {
        assert(false &&
               "nmu::addr_trans::collective_translate: collective request address maps to no "
               "tile");
        std::abort();
    }
    // No class gate: design Q4 revision 2 rules multicast legal on BOTH
    // classes (config-space message replication rides Narrow/REQ). The class
    // stays UNIFORM across the destination set because the mask cannot leave
    // the space the request address falls in -- the outside-the-ranges check
    // below.
    const SpaceCoords* coords = sam.collective_coords(entry->cls);
    if (coords == nullptr) {
        assert(false &&
               "nmu::addr_trans::collective_translate: the request address's space declares no "
               "coordinate ranges -- a legal unicast target, not a collective target");
        std::abort();
    }
    // Only a tile may ISSUE a collective. Both trees are built out of the
    // issuer's own coordinates -- the fork spreads X along its row and the join
    // collects Y in its column (route_mask.hpp, the cfg.y == src.y and
    // cfg.x == dst.x gates) -- and routers exist only at tile coordinates, so
    // an off-region issuer has no router that recognises its row or its column.
    // An x-border issuer forks, then no router lists a cross-row replica as an
    // expected input and the CollectB never completes; a y-border issuer never
    // forks at all and the replicas are dropped. Refuse rather than hang.
    const unsigned src_x = src_id & kXFieldMask;
    const unsigned src_y = src_id >> ni::width::X_WIDTH;
    if (src_x < coords->x_first || src_x > coords->x_last || src_y < coords->y_first ||
        src_y > coords->y_last) {
        assert(false &&
               "nmu::addr_trans::collective_translate: a collective issued from outside the tile "
               "region -- the fork spreads along the issuer's row and the join collects in its "
               "column, and no router sits outside the region to do either");
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
               "the coordinate ranges of the request address's space");
        std::abort();
    }
    const unsigned mask_x = static_cast<unsigned>(addr_mask >> coords->x_range.offset) &
                            ((1u << coords->x_range.len) - 1);
    const unsigned mask_y = static_cast<unsigned>(addr_mask >> coords->y_range.offset) &
                            ((1u << coords->y_range.len) - 1);

    // The raw wildcard set is [addr & ~mask, addr | mask] PER COORDINATE:
    // dst_id is (y << X_WIDTH) | x, and a dimension need not be a power of
    // two, so the set may reach a coordinate that names no tile (a peripheral,
    // or padding above a non-power-of-two dimension).
    const unsigned addr_x = entry->dst_id & kXFieldMask;
    const unsigned addr_y = entry->dst_id >> ni::width::X_WIDTH;
    // Clip to the tile region exactly as route_mask.hpp does, so the source and
    // every router agree on the member set. A set that is empty after clipping
    // names no tile at all and is a stimulus error.
    //
    // Hand-kept twins of this arithmetic: route_mask_fork and route_mask_join
    // (router/route_mask.hpp, deliberately two copies because a stateless join
    // hangs on a one-node disagreement) and collective_addr_mask in
    // sim/tools/gen_test_patterns.py. Not shared: the router pair sits the far
    // side of the nmu/router layer boundary and the generator's is Python.
    // Change one, change all four.
    const unsigned clip_min_x = std::max(addr_x & ~mask_x, coords->x_first);
    const unsigned clip_max_x = std::min(addr_x | mask_x, coords->x_last);
    const unsigned clip_min_y = std::max(addr_y & ~mask_y, coords->y_first);
    const unsigned clip_max_y = std::min(addr_y | mask_y, coords->y_last);
    if (clip_min_x > clip_max_x || clip_min_y > clip_max_y) {
        assert(false &&
               "nmu::addr_trans::collective_translate: collective destination set is empty "
               "after clipping to the tile region");
        std::abort();
    }
    // Clamping a bound is not the same as clipping the set: the range
    // [clip_min, clip_max] can include a coordinate the wildcard never named
    // (e.g. addr_x=1, mask_x=0x2 names {1, 3}; clamping x_last=2 down from 3
    // gives clip_max_x=2, which is not a member). A terminal router's fork set
    // would then be empty at that coordinate and abort (route_mask.hpp uses
    // coord_matched, not a range test), so this must reject here instead.
    // Checking only the two clipped bounds is sufficient: any non-member
    // strictly between them still has a member neighbour to forward from/to,
    // so only an endpoint that got clamped away from the raw min/max can ever
    // be a non-member.
    if (((clip_min_x ^ addr_x) & ~mask_x) || ((clip_max_x ^ addr_x) & ~mask_x) ||
        ((clip_min_y ^ addr_y) & ~mask_y) || ((clip_max_y ^ addr_y) & ~mask_y)) {
        assert(false &&
               "nmu::addr_trans::collective_translate: a clipped tile-region bound is not a "
               "member of the wildcard set -- the source and a terminal router would disagree "
               "on the collective member set");
        std::abort();
    }

    // Stays per-request: AWADDR/AWLEN/AWSIZE/AWBURST are not space properties.
    // The uniform region size the declaration checked is what lets the request
    // address's footprint stand for every replica's.
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

// Reject a request the fabric cannot deliver, called from Packetize for every
// AW and AR (collective or not) once the destination is known.
//
// XY routing resolves X before Y (router::route_compute), so a node outside the
// tile region on the x axis -- a peripheral on a border router's WEST or EAST
// port -- is reached by running out of x hops, which happens on whichever row
// the flit started from. That constrains BOTH directions of a transaction, one
// rule read twice:
//   request  -- an off-region destination is reached from its own row only.
//   response -- a response's destination is the request's source (nsu's
//               Packetize::build_b_flit stamps dst_id from the buffered
//               src_id) and it takes the same route_compute, so an off-region
//               SOURCE is answered on its own row only.
// The flit that misses leaves the region one row early and lands in whichever
// peripheral borders that row; that peripheral's NSU rebases the address to its
// own tile and answers it, so nothing downstream can tell.
//
// The y axis is not row-sensitive either way: a node outside the region on y is
// reached after x has already resolved, from any column.
//
// Same permanent-illegal-input shape as collective_translate above: a rejected
// request never becomes legal on retry, so it fails loud instead of returning.
inline void check_dst_reachable(const SpaceCoords* coords, uint8_t src_id, uint8_t dst_id) {
    // A space that declares no coordinate ranges states no tile region. The
    // tables that skip the declaration are the uniform fixtures, whose every
    // coordinate is a tile, so neither endpoint can be outside one.
    if (coords == nullptr) return;
    constexpr unsigned kXFieldMask = (1u << ni::width::X_WIDTH) - 1;
    const unsigned dst_x = dst_id & kXFieldMask;
    const unsigned dst_y = dst_id >> ni::width::X_WIDTH;
    const unsigned src_x = src_id & kXFieldMask;
    const unsigned src_y = src_id >> ni::width::X_WIDTH;
    if (dst_y == src_y) return;
    const bool dst_off = dst_x < coords->x_first || dst_x > coords->x_last;
    const bool src_off = src_x < coords->x_first || src_x > coords->x_last;
    if (!dst_off && !src_off) return;
    const unsigned off_x = dst_off ? dst_x : src_x;
    const unsigned off_y = dst_off ? dst_y : src_y;
    std::fprintf(stderr,
                 "nmu::addr_trans::check_dst_reachable: node (%u,%u) addressed (%u,%u); (%u,%u) "
                 "sits outside the tile region [%u,%u] on x, so it exchanges requests and "
                 "responses with row %u only -- both directions run out of x hops on the row "
                 "they started from. Put both endpoints on row %u.\n",
                 src_x, src_y, dst_x, dst_y, off_x, off_y, coords->x_first, coords->x_last, off_y,
                 off_y);
    assert(false && "check_dst_reachable: off-mesh node paired with a node on another row");
    std::abort();  // belt-and-braces for NDEBUG
}

}  // namespace ni::cmodel::nmu::addr_trans
