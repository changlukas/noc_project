#pragma once
#include "common/flit_link_probe.hpp"
#include "common/perf_stats.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>

namespace ni::cmodel::testing {

// AXI channel count: AW, W, AR, B, R (ni_flit_constants AXI_CH_*).
inline constexpr std::size_t kAxiChCount = 5;

// Per-component segment dwell. pair() matches the entry-boundary and
// exit-boundary crossing logs by FIFO order within one vc_id (links and FIFOs
// preserve order, so the Nth entry on a vc is the Nth exit on that vc). Dwell =
// exit.cycle - entry.cycle, accumulated into a per-axi_ch Stats and an all()
// aggregate.
class SegmentDwell {
  public:
    SegmentDwell() {
        for (auto& s : by_ch_) s = Stats{};
    }

    void pair(const FlitLog& entry, const FlitLog& exit) {
        // Per-vc FIFO of entry crossings awaiting an exit match.
        std::map<uint8_t, std::deque<FlitCrossing>> pending;
        for (const auto& e : entry.crossings()) pending[e.vc_id].push_back(e);
        for (const auto& x : exit.crossings()) {
            auto it = pending.find(x.vc_id);
            if (it == pending.end() || it->second.empty()) continue;
            const FlitCrossing e = it->second.front();
            it->second.pop_front();
            const uint64_t dwell = x.cycle - e.cycle;
            if (e.axi_ch < kAxiChCount) by_ch_[e.axi_ch].add(dwell);
            all_.add(dwell);
        }
    }

    const Stats& by_channel(uint8_t axi_ch) const { return by_ch_[axi_ch]; }
    const Stats& all() const { return all_; }

  private:
    std::array<Stats, kAxiChCount> by_ch_{};
    Stats all_{};
};

// Aggregate occupancy: running peak fill vs a fixed capacity. The harness calls
// sample(fill) once per cycle with the busiest-buffer fill from the component's
// existing introspection getters.
class OccupancyPeak {
  public:
    explicit OccupancyPeak(std::size_t capacity) : capacity_(capacity) {}
    void sample(std::size_t fill) {
        if (fill > peak_) peak_ = fill;
    }
    std::size_t peak() const { return peak_; }
    std::size_t capacity() const { return capacity_; }

  private:
    std::size_t capacity_;
    std::size_t peak_ = 0;
};

}  // namespace ni::cmodel::testing
