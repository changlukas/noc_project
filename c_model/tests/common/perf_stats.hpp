#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace ni::cmodel::testing {

struct StatsConfig {
    std::vector<uint64_t> bin_thresholds;  // ascending; empty = scalars only
};

// One metric's accumulator: count/sum/min/max + optional threshold-bin histogram.
// Histogram is maintained only when bin_thresholds is non-empty (NOC_PERF gate).
class Stats {
  public:
    explicit Stats(StatsConfig cfg) : thresholds_(std::move(cfg.bin_thresholds)) {
        if (!thresholds_.empty()) bins_.assign(thresholds_.size() + 1, 0);
    }
    void add(uint64_t v) {
        ++count_;
        sum_ += v;
        sumsq_ += v * v;
        if (v < min_) min_ = v;
        if (v > max_) max_ = v;
        if (!thresholds_.empty()) ++bins_[bin_index_(v)];
    }
    uint64_t count() const { return count_; }
    uint64_t sum() const { return sum_; }
    uint64_t min() const { return count_ ? min_ : 0; }
    uint64_t max() const { return count_ ? max_ : 0; }
    double mean() const {
        return count_ ? static_cast<double>(sum_) / static_cast<double>(count_) : 0.0;
    }
    double variance() const {
        if (!count_) return 0.0;
        const double m = mean();
        return static_cast<double>(sumsq_) / static_cast<double>(count_) - m * m;
    }
    const std::vector<uint64_t>& thresholds() const { return thresholds_; }
    const std::vector<uint64_t>& histogram() const { return bins_; }

  private:
    std::size_t bin_index_(uint64_t v) const {
        for (std::size_t i = 0; i < thresholds_.size(); ++i)
            if (v < thresholds_[i]) return i;
        return thresholds_.size();
    }
    std::vector<uint64_t> thresholds_;
    std::vector<uint64_t> bins_;
    uint64_t count_ = 0;
    uint64_t sum_ = 0;
    uint64_t sumsq_ = 0;
    uint64_t min_ = std::numeric_limits<uint64_t>::max();
    uint64_t max_ = 0;
};

}  // namespace ni::cmodel::testing
