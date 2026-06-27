#pragma once
// Split a VC count into disjoint, equal read/write class pools.
//   num_vc == 1       -> write {0}, read {0}  (degenerate, shared lane)
//   num_vc >= 2, even -> write {0..n/2-1}, read {n/2..n-1}
// Odd num_vc (>1) has no equal split and is rejected loudly: message-class
// separation requires two disjoint, equal pools for deadlock avoidance.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ni::cmodel {

struct VcPools {
    std::vector<uint8_t> write_vcs;
    std::vector<uint8_t> read_vcs;
};

inline VcPools derive_vc_pools(std::size_t num_vc) {
    assert((num_vc == 1 || num_vc % 2 == 0) &&
           "derive_vc_pools: num_vc must be 1 or even (no equal read/write split otherwise)");
    VcPools pools;
    if (num_vc == 1) {
        pools.write_vcs = {0};
        pools.read_vcs = {0};
        return pools;
    }
    const std::size_t half = num_vc / 2;
    for (std::size_t i = 0; i < half; ++i) pools.write_vcs.push_back(static_cast<uint8_t>(i));
    for (std::size_t i = half; i < num_vc; ++i) pools.read_vcs.push_back(static_cast<uint8_t>(i));
    return pools;
}

}  // namespace ni::cmodel
