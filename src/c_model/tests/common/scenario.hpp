// SCENARIO("<English desc>") — minimal print wrapper.
//
// Standalone header so any test source can write a one-line, always-emitted
// scenario description before its assertions without pulling in the
// AxiMasterObserver dependencies (axi_master.hpp -> scenario_parser.hpp ->
// yaml-cpp). Tests that need the observer should include
// "common/test_logger.hpp" instead, which re-uses this header.
//
// Output: "[scenario] <desc>"
//
// See docs/superpowers/specs/2026-06-03-test-logger-scenario-observer-design.md
#pragma once

#include <iostream>

namespace ni::cmodel::testing {

inline void print_scenario(const char* desc) {
    std::cout << "[scenario] " << desc << '\n';
}

}  // namespace ni::cmodel::testing

// Wrap to a single statement so SCENARIO("...") can sit on its own line
// inside any TEST() body without surprising the surrounding control flow.
#define SCENARIO(desc) ::ni::cmodel::testing::print_scenario(desc)
