#pragma once
#include "common/perf_stats.hpp"
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace ni::cmodel::testing {

// NI-edge latency collector. Driven by AxiMaster issue/completion callbacks
// (the harness forwards them). Latency keyed by scenario_line within ONE NMU
// (one instance per NMU). Read-only: records issue cycle on on_issue, emits
// (now - issue) into the matching Stats on on_complete. No phase gating, no
// outstanding/RoB sampling (dropped with the v1 probe, spec sec 3).
class NIPerfObserver {
  public:
    NIPerfObserver(const uint64_t& now, std::string flow_label)
        : now_(now), label_(std::move(flow_label)) {}

    void on_issue(bool is_write, std::size_t line) {
        (is_write ? wr_inflight_ : rd_inflight_)[line] = now_;
    }
    void on_complete(bool is_write, std::size_t line) {
        auto& m = is_write ? wr_inflight_ : rd_inflight_;
        auto it = m.find(line);
        if (it != m.end()) {
            (is_write ? wr_lat_ : rd_lat_).add(now_ - it->second);
            m.erase(it);
        }
    }

    const std::string& label() const { return label_; }
    const Stats& write_latency() const { return wr_lat_; }
    const Stats& read_latency() const { return rd_lat_; }
    std::size_t stuck_count() const { return wr_inflight_.size() + rd_inflight_.size(); }

  private:
    const uint64_t& now_;
    std::string label_;
    Stats wr_lat_{}, rd_lat_{};
    std::map<std::size_t, uint64_t> wr_inflight_, rd_inflight_;
};

}  // namespace ni::cmodel::testing
