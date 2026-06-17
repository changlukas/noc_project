#pragma once
#include "common/ni_perf_observer.hpp"
#include <ostream>
#include <vector>

namespace ni::cmodel::testing {

// Minimal latency-only report. Rewritten to the spec Section 8 JSON in Task 7.
class PerfReport {
  public:
    void add_ni(const NIPerfObserver* ni) { ni_.push_back(ni); }

    void write_summary(std::ostream& os) const {
        for (const auto* ni : ni_) {
            os << "[perf:" << ni->label() << "] "
               << "wr_lat(min/mean/max)=" << ni->write_latency().min() << '/'
               << ni->write_latency().mean() << '/' << ni->write_latency().max() << ' '
               << "rd_lat(min/mean/max)=" << ni->read_latency().min() << '/'
               << ni->read_latency().mean() << '/' << ni->read_latency().max() << '\n';
        }
    }

  private:
    std::vector<const NIPerfObserver*> ni_;
};

}  // namespace ni::cmodel::testing
