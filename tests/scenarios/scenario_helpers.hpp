#pragma once
#include "scenarios_list.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace noc::tests {

// Aborts the test binary at startup if `id` is not present in
// kAllAxi4Scenarios. Wrap every hand-written scenario reference in scoped
// tests with this so stale IDs after a rename surface as immediate abort,
// not a silent skip.
inline std::string_view RequireKnownScenario(std::string_view id) {
    auto const it = std::find(kAllAxi4Scenarios.begin(), kAllAxi4Scenarios.end(), id);
    if (it == kAllAxi4Scenarios.end()) {
        std::fprintf(stderr,
                     "FATAL: unknown scenario id '%.*s' "
                     "(not in tests/scenarios/AX4-*)\n",
                     int(id.size()), id.data());
        std::abort();
    }
    return id;
}

}  // namespace noc::tests
