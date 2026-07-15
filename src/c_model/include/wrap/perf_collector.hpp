#ifndef NI_CMODEL_COSIM_PERF_COLLECTOR_HPP
#define NI_CMODEL_COSIM_PERF_COLLECTOR_HPP

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace ni::cmodel::wrap {

// Single-run NoC perf readout. C-side sampling pushes per-router fifo occupancy
// (max-tracked) and per-link flit/stall counters; dump() writes the noc section
// of the perf.json schema. (The AXI per-transaction slots/latency section was
// dropped: its DPI hook was never wired, so it only ever emitted empty output.)
class PerfCollector {
  public:
    void set_scenario(std::string scenario) { scenario_ = std::move(scenario); }
    void set_window(uint64_t start_cyc, uint64_t end_cyc) {
        win_start_ = start_cyc;
        win_end_ = end_cyc;
    }

    void sample_router(const std::string& name, uint64_t in_occ, uint64_t out_occ) {
        Router& r = routers_[name];
        if (in_occ > r.in_max) r.in_max = in_occ;
        if (out_occ > r.out_max) r.out_max = out_occ;
    }

    void set_link(const std::string& name, uint64_t flit_count, uint64_t stall_cyc) {
        links_[name] = Link{flit_count, stall_cyc};
    }

    std::string to_json() const {
        std::ostringstream os;
        os << "{\"schema_version\":1,\"scenario\":\"" << scenario_ << "\","
           << "\"window\":{\"start_cyc\":" << win_start_ << ",\"end_cyc\":" << win_end_ << "},";
        emit_noc(os);
        os << '}';
        return os.str();
    }

    void dump(const std::string& path) const {
        std::ofstream f(path);
        if (!f.is_open()) {
            std::fprintf(stderr, "[perf] WARNING: failed to open '%s' for writing\n", path.c_str());
            return;
        }
        f << to_json() << '\n';
    }

  private:
    struct Router {
        uint64_t in_max = 0;
        uint64_t out_max = 0;
    };
    struct Link {
        uint64_t flit_count = 0;
        uint64_t stall_cyc = 0;
    };

    void emit_noc(std::ostringstream& os) const {
        os << "\"noc\":{\"routers\":[";
        bool first = true;
        for (const auto& [name, r] : routers_) {
            if (!first) os << ',';
            first = false;
            os << "{\"name\":\"" << name << "\",\"in_fifo_occ_max\":" << r.in_max
               << ",\"out_fifo_occ_max\":" << r.out_max << '}';
        }
        os << "],\"links\":[";
        first = true;
        for (const auto& [name, l] : links_) {
            if (!first) os << ',';
            first = false;
            os << "{\"name\":\"" << name << "\",\"flit_count\":" << l.flit_count
               << ",\"stall_cyc\":" << l.stall_cyc << '}';
        }
        os << "]}";
    }

    std::string scenario_;
    uint64_t win_start_ = 0, win_end_ = 0;
    std::map<std::string, Router> routers_;
    std::map<std::string, Link> links_;
};

}  // namespace ni::cmodel::wrap

#endif  // NI_CMODEL_COSIM_PERF_COLLECTOR_HPP
