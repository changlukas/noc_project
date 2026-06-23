#pragma once
#include "scenarios_list.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace router::tests {

// Aborts the test binary when called if `id` is not present in
// kAllAxi4Scenarios. Wrap every hand-written scenario reference in scoped
// tests' INSTANTIATE_TEST_SUITE_P with this so stale IDs after a rename
// fire an immediate abort when the test binary starts (which is when the
// INSTANTIATE static initialization runs), not a silent test skip later.
inline std::string_view RequireKnownScenario(std::string_view id) {
    auto const it = std::find(kAllAxi4Scenarios.begin(), kAllAxi4Scenarios.end(), id);
    if (it == kAllAxi4Scenarios.end()) {
        std::fprintf(stderr,
                     "FATAL: unknown scenario id '%.*s' "
                     "(not in sim/test_patterns/AX4-*)\n",
                     int(id.size()), id.data());
        std::abort();
    }
    return id;
}

}  // namespace router::tests
