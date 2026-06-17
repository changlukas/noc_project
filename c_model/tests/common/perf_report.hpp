#pragma once
#include "common/ni_perf_observer.hpp"
#include "common/router_perf_observer.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <ostream>
#include <string>
#include <vector>

namespace ni::cmodel::testing {

// Aggregates NI + Router observers. Always prints a scalar stdout summary;
// writes a JSON file only under NOC_PERF=1.
class PerfReport {
  public:
    void add_ni(const NIPerfObserver* ni) { ni_.push_back(ni); }
    void add_router(const RouterPerfObserver* r) { router_ = r; }

    void write_summary(std::ostream& os) const {
        for (const auto* ni : ni_) {
            os << "[perf:" << ni->label() << "] "
               << "wr_lat(min/mean/max)=" << ni->write_latency().min() << '/'
               << ni->write_latency().mean() << '/' << ni->write_latency().max() << ' '
               << "rd_lat(min/mean/max)=" << ni->read_latency().min() << '/'
               << ni->read_latency().mean() << '/' << ni->read_latency().max() << ' '
               << "outstanding_peak=" << ni->outstanding_peak() << ' '
               << "rob_occ_peak=" << ni->rob_occupancy().max() << ' '
               << "stuck=" << ni->stuck_count() << '\n';
        }
        if (router_) {
            os << "[perf:ROUTER] credit_stall_cycles=" << router_->credit_stall_cycles() << '\n';
        }
    }

    // Always summary; JSON only under NOC_PERF=1.
    void emit(const std::string& run_label) const {
        write_summary(std::cout);
        const char* g = std::getenv("NOC_PERF");
        if (!g || std::string(g) != "1") return;
        const char* f = std::getenv("NOC_PERF_FILE");
        const std::string path = f ? std::string(f) : ("perf_" + run_label + ".json");
        std::ofstream js(path);
        if (js) write_json(js);
    }

    void write_json(std::ostream& os) const {
        os << "{\"ni\":[";
        for (std::size_t i = 0; i < ni_.size(); ++i) {
            const auto* ni = ni_[i];
            if (i) os << ',';
            os << "{\"label\":\"" << ni->label() << "\","
               << "\"write_latency\":" << stat_json(ni->write_latency()) << ','
               << "\"read_latency\":" << stat_json(ni->read_latency()) << ','
               << "\"outstanding_peak\":" << ni->outstanding_peak() << ','
               << "\"outstanding\":" << stat_json(ni->outstanding()) << ','
               << "\"rob_occupancy\":" << stat_json(ni->rob_occupancy()) << '}';
        }
        os << "],\"router\":{";
        if (router_) {
            os << "\"credit_stall_cycles\":" << router_->credit_stall_cycles()
               << ",\"per_router\":[";
            for (std::size_t i = 0; i < router_->router_count(); ++i) {
                if (i) os << ',';
                os << "{\"label\":\"" << router_->label(i) << "\","
                   << "\"stall\":" << router_->stall(i) << ','
                   << "\"in_fifo\":" << stat_json(router_->in_fifo(i)) << ','
                   << "\"out_fifo\":" << stat_json(router_->out_fifo(i)) << '}';
            }
            os << ']';
        } else {
            os << "\"credit_stall_cycles\":0";
        }
        os << "}}";
    }

  private:
    static std::string stat_json(const Stats& s) {
        std::string o =
            "{\"count\":" + std::to_string(s.count()) + ",\"min\":" + std::to_string(s.min()) +
            ",\"max\":" + std::to_string(s.max()) + ",\"mean\":" + std::to_string(s.mean()) +
            ",\"variance\":" + std::to_string(s.variance());
        if (!s.histogram().empty()) {
            o += ",\"histogram\":{\"thresholds\":[";
            for (std::size_t i = 0; i < s.thresholds().size(); ++i)
                o += (i ? "," : "") + std::to_string(s.thresholds()[i]);
            o += "],\"bins\":[";
            for (std::size_t i = 0; i < s.histogram().size(); ++i)
                o += (i ? "," : "") + std::to_string(s.histogram()[i]);
            o += "]}";
        }
        return o + "}";
    }
    std::vector<const NIPerfObserver*> ni_;
    const RouterPerfObserver* router_ = nullptr;
};

}  // namespace ni::cmodel::testing
