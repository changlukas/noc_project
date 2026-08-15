#pragma once
#include <cstdint>

// Where an address space keeps its node coordinates.
//
// Both directions need this, which is why it sits at the NI layer rather than
// under nmu/. The NMU reads the field to turn an AWUSER address mask into a
// collective mask; the NSU writes it, so a request that reached this node names
// this node before the tile decodes it.

namespace ni::cmodel::address_map {

// Smallest k with 2^k >= n. clog2(1) = 0, clog2(3) = 2.
inline unsigned clog2(unsigned n) {
    unsigned bits = 0;
    while (bits < 32 && (1u << bits) < n) ++bits;
    return bits;
}

struct BitRange {
    unsigned offset = 0;
    unsigned len = 0;
};

inline uint64_t range_mask(const BitRange& r) {
    if (r.len == 0) return 0;
    return ((uint64_t{1} << r.len) - 1) << r.offset;
}

// Upstream emits the same numbers into every SAM rule at generation time
// (floogen/model/network.py gen_collective_sam, read by floo_id_translation.sv),
// so the NI slices AWUSER instead of walking the map. SamTable does not derive
// them -- construction has no mesh dimensions -- so whoever builds the table
// states them and SamTable checks the statement against the entries.
//
// Deviation from upstream, deliberate: floogen linearizes Y-major so its X
// field sits ABOVE Y. This repo packs raster order, X fastest, so X sits BELOW
// Y -- the same order dst_id = (y << X_WIDTH) | x uses.
//
// No base_id: upstream carries one because a collective array can sit at a
// non-origin sub-mesh. Every space here spans all N nodes from node 0, so the
// field would always be zero.
struct SpaceCoords {
    BitRange x_range;
    BitRange y_range;
    // Mesh dimension, STATED not inferred. Recovering one as 1 << len
    // over-permits every dimension that is not a power of two, and
    // docs/noc-target-spec.md §5 allows 2 to 16 per dimension.
    unsigned x_count = 0;
    unsigned y_count = 0;
    // x_count == 0 marks a space whose coordinate field could not be read off
    // the map (one entry names no stride, a non-power-of-two stride names no
    // field). Such a space is simply not a collective target, spec §5.1.
    bool declared() const { return x_count != 0; }
};

// Overwrite the coordinate field with (x, y), leaving every other bit alone.
// Identity when the address already names (x, y), which is every unicast
// request: the fabric delivered it here because the field says here.
inline uint64_t rebase_node_coords(uint64_t addr, const SpaceCoords& c, unsigned x, unsigned y) {
    if (!c.declared()) return addr;
    const uint64_t field = range_mask(c.x_range) | range_mask(c.y_range);
    return (addr & ~field) | (uint64_t{x} << c.x_range.offset) | (uint64_t{y} << c.y_range.offset);
}

}  // namespace ni::cmodel::address_map
