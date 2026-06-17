#pragma once
#include <cstdint>
#include <limits>

namespace ni::cmodel::testing {

// Empty tag struct, retained so existing `Stats(StatsConfig{})` call sites
// compile unchanged. Histogram thresholds were dropped with the v1 probe.
struct StatsConfig {};

// One metric's accumulator: count/sum/min/max only. variance() and the
// threshold-bin histogram were removed with the v1 perf probe (spec sec 3).
class Stats {
  public:
    explicit Stats(StatsConfig /*cfg*/ = StatsConfig{}) {}
    void add(uint64_t v) {
        ++count_;
        sum_ += v;
        if (v < min_) min_ = v;
        if (v > max_) max_ = v;
    }
    uint64_t count() const { return count_; }
    uint64_t sum() const { return sum_; }
    uint64_t min() const { return count_ ? min_ : 0; }
    uint64_t max() const { return count_ ? max_ : 0; }
    double mean() const {
        return count_ ? static_cast<double>(sum_) / static_cast<double>(count_) : 0.0;
    }

  private:
    uint64_t count_ = 0;
    uint64_t sum_ = 0;
    uint64_t min_ = std::numeric_limits<uint64_t>::max();
    uint64_t max_ = 0;
};

}  // namespace ni::cmodel::testing
