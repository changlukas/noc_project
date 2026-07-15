#pragma once
// Split a VC count into disjoint, equal read/write virtual networks (vnet).
//   num_vc == 1       -> write {0}, read {0}  (degenerate, shared lane)
//   num_vc >= 2, even -> write {0..n/2-1}, read {n/2..n-1}
// Odd num_vc (>1) has no equal split and is rejected loudly: message-class
// separation requires two disjoint, equal vnets for deadlock avoidance.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace ni::cmodel {

struct VirtualNetworks {
    std::vector<uint8_t> write_vcs;
    std::vector<uint8_t> read_vcs;
};

inline VirtualNetworks make_virtual_networks(std::size_t num_vc) {
    if (!(num_vc == 1 || num_vc % 2 == 0)) {
        assert(false &&
               "make_virtual_networks: num_vc must be 1 or even (no equal read/write vnet split "
               "otherwise)");
        std::abort();  // belt-and-braces for NDEBUG
    }
    VirtualNetworks vnets;
    if (num_vc == 1) {
        vnets.write_vcs = {0};
        vnets.read_vcs = {0};
        return vnets;
    }
    const std::size_t half = num_vc / 2;
    for (std::size_t i = 0; i < half; ++i) vnets.write_vcs.push_back(static_cast<uint8_t>(i));
    for (std::size_t i = half; i < num_vc; ++i) vnets.read_vcs.push_back(static_cast<uint8_t>(i));
    return vnets;
}

}  // namespace ni::cmodel
