#pragma once
#include <cstdint>

namespace ni::cmodel::testing {

enum class Phase { Warmup, Measurement, Drain };

struct PhaseConfig {
    uint64_t warmup_cycles = 0;
};

// Three-phase gating bound to the harness cycle counter by const reference.
// Drain is one-way: begin_drain() is called once when all masters report done().
class PhaseController {
  public:
    PhaseController(const uint64_t& now, PhaseConfig cfg) : now_(now), cfg_(cfg) {}
    void begin_drain() { draining_ = true; }
    Phase phase() const {
        if (draining_) return Phase::Drain;
        if (now_ < cfg_.warmup_cycles) return Phase::Warmup;
        return Phase::Measurement;
    }

  private:
    const uint64_t& now_;
    PhaseConfig cfg_;
    bool draining_ = false;
};

}  // namespace ni::cmodel::testing
