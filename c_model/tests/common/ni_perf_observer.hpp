#pragma once
#include "common/perf_common.hpp"
#include "common/perf_stats.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>

namespace ni::cmodel::testing {

struct NIPerfConfig {
    std::string flow_label = "NI";
    StatsConfig latency_stats{};  // thresholds when NOC_PERF=1, else empty
    StatsConfig occupancy_stats{};
};

// NI-edge observer. Driven by AxiMaster issue/completion callbacks (template-free:
// the harness forwards them) plus a per-cycle sample() polling a RoB-occupancy
// probe. Latency keyed by scenario_line within ONE NMU (one instance per NMU).
class NIPerfObserver {
  public:
    NIPerfObserver(const uint64_t& now, PhaseController& phase,
                   std::function<std::size_t()> rob_occupancy_probe, NIPerfConfig cfg)
        : now_(now),
          phase_(phase),
          rob_probe_(std::move(rob_occupancy_probe)),
          cfg_(std::move(cfg)),
          wr_lat_(cfg_.latency_stats),
          rd_lat_(cfg_.latency_stats),
          outstanding_(cfg_.occupancy_stats),
          rob_occ_(cfg_.occupancy_stats) {}

    void on_issue(bool is_write, std::size_t line) {
        auto& m = is_write ? wr_inflight_ : rd_inflight_;
        m[line] = InFlight{now_, phase_.phase() == Phase::Measurement};
        if (is_write)
            ++w_out_;
        else
            ++r_out_;
        const std::size_t cur = w_out_ + r_out_;
        if (cur > out_peak_) out_peak_ = cur;
    }
    void on_complete(bool is_write, std::size_t line) {
        auto& m = is_write ? wr_inflight_ : rd_inflight_;
        auto it = m.find(line);
        if (it != m.end()) {
            if (it->second.eligible)
                (is_write ? wr_lat_ : rd_lat_).add(now_ - it->second.issue_cycle);
            m.erase(it);
        }
        if (is_write) {
            if (w_out_) --w_out_;
        } else {
            if (r_out_) --r_out_;
        }
    }
    // Called once per cycle by the harness, AFTER component ticks.
    void sample() {
        if (phase_.phase() != Phase::Measurement) return;
        outstanding_.add(w_out_ + r_out_);
        rob_occ_.add(rob_probe_ ? rob_probe_() : 0);
    }

    const std::string& label() const { return cfg_.flow_label; }
    const Stats& write_latency() const { return wr_lat_; }
    const Stats& read_latency() const { return rd_lat_; }
    const Stats& outstanding() const { return outstanding_; }
    const Stats& rob_occupancy() const { return rob_occ_; }
    std::size_t outstanding_peak() const { return out_peak_; }
    std::size_t stuck_count() const { return wr_inflight_.size() + rd_inflight_.size(); }

  private:
    struct InFlight {
        uint64_t issue_cycle;
        bool eligible;
    };
    const uint64_t& now_;
    PhaseController& phase_;
    std::function<std::size_t()> rob_probe_;
    NIPerfConfig cfg_;
    Stats wr_lat_, rd_lat_, outstanding_, rob_occ_;
    std::map<std::size_t, InFlight> wr_inflight_, rd_inflight_;
    std::size_t w_out_ = 0, r_out_ = 0, out_peak_ = 0;
};

}  // namespace ni::cmodel::testing
