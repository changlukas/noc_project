#ifndef NI_CMODEL_COSIM_PERF_COLLECTOR_HPP
#define NI_CMODEL_COSIM_PERF_COLLECTOR_HPP

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>

namespace ni::cmodel::wrap {

// Single-run NoC perf readout. C-side sampling pushes per-router fifo occupancy
// (max-tracked) and per-link flit/stall counters; dump() writes the noc section
// of the perf.json schema. (The AXI per-transaction slots/latency section was
// dropped: its DPI hook was never wired, so it only ever emitted empty output.)
class PerfCollector {
  public:
    void set_scenario(std::string scenario) { scenario_ = std::move(scenario); }
    void begin(uint64_t start_cyc) {
        win_start_ = start_cyc;
        win_end_ = 0;
        routers_.clear();
        router_dat_input_vcs_.clear();
        router_dat_output_vcs_.clear();
        router_dat_switches_.clear();
        router_dat_allocations_.clear();
        nmu_requests_.clear();
        links_.clear();
        active_ = true;
    }

    void end(uint64_t end_cyc) {
        if (end_cyc <= win_start_)
            throw std::invalid_argument("end cycle must follow start cycle");
        win_end_ = end_cyc;
        active_ = false;
    }

    bool active() const { return active_; }

    void sample_router(const std::string& name, uint64_t in_occ, uint64_t out_occ) {
        if (!active_) return;
        Router& r = routers_[name];
        if (in_occ > r.in_max) r.in_max = in_occ;
        if (out_occ > r.out_max) r.out_max = out_occ;
    }

    void set_link(const std::string& name, uint64_t flit_count, uint64_t stall_cyc) {
        if (!active_) return;
        links_[name] = Link{flit_count, stall_cyc};
    }

    void sample_router_dat_input_vc(const std::string& router, const std::string& port,
                                    uint64_t vc, uint64_t occupancy, uint64_t capacity) {
        if (!active_) return;
        if (capacity == 0 || occupancy > capacity)
            throw std::invalid_argument("invalid DAT input VC occupancy/capacity");
        DatInputVc& q = router_dat_input_vcs_[{router, port, vc}];
        if (q.samples && q.capacity_flits != capacity)
            throw std::invalid_argument("DAT input VC capacity changed inside window");
        if (occupancy > q.hwm_flits) q.hwm_flits = occupancy;
        q.capacity_flits = capacity;
        q.occupancy_sum_flits += occupancy;
        q.full_cycles += occupancy == capacity;
        ++q.samples;
    }

    void sample_router_dat_output_vc(const std::string& router, const std::string& port,
                                     uint64_t vc, bool credit_blocked) {
        if (!active_) return;
        router_dat_output_vcs_[{router, port, vc}].credit_block_cycles += credit_blocked;
    }

    void sample_router_dat_switch(const std::string& router, const std::string& port,
                                  uint64_t eligible_vcs, uint64_t grants) {
        if (!active_) return;
        if (grants > 1 || grants > eligible_vcs)
            throw std::invalid_argument("invalid DAT switch activity");
        auto& activity = router_dat_switches_[{router, port}];
        activity.eligible_vc_cycles += eligible_vcs;
        activity.grant_cycles += grants;
        ++activity.samples;
    }

    void sample_router_dat_allocation(const std::string& router, const std::string& input,
                                      uint64_t vc, const std::string& output,
                                      bool occupied, bool input_full) {
        if (!active_) return;
        auto& wait = router_dat_allocations_[{router, input, vc, output}];
        ++wait.cycles;
        wait.occupied += occupied;
        wait.full += input_full;
        wait.occupied_full += occupied && input_full;
    }

    void sample_nmu_request(const std::string& node, const std::string& channel,
                            bool ordering, bool order_list, bool storage, bool downstream) {
        if (!active_) return;
        auto& request = nmu_requests_[{node, channel}];
        request.ordering += ordering;
        request.order_list += order_list;
        request.storage += storage;
        request.downstream += downstream;
        ++request.samples;
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
    struct DatInputVc {
        uint64_t hwm_flits = 0;
        uint64_t capacity_flits = 0;
        uint64_t occupancy_sum_flits = 0;
        uint64_t full_cycles = 0;
        uint64_t samples = 0;
    };
    struct DatOutputVc {
        uint64_t credit_block_cycles = 0;
    };
    struct DatSwitch {
        uint64_t eligible_vc_cycles = 0;
        uint64_t grant_cycles = 0;
        uint64_t samples = 0;
    };
    struct NmuRequest {
        uint64_t ordering = 0, order_list = 0, storage = 0, downstream = 0, samples = 0;
    };
    struct AllocationWait {
        uint64_t cycles = 0, occupied = 0, full = 0, occupied_full = 0;
    };
    using DatVcKey = std::tuple<std::string, std::string, uint64_t>;

    void emit_noc(std::ostringstream& os) const {
        os << "\"noc\":{\"routers\":[";
        bool first = true;
        for (const auto& [name, r] : routers_) {
            if (!first) os << ',';
            first = false;
            os << "{\"name\":\"" << name << "\",\"in_fifo_occ_max\":" << r.in_max
               << ",\"out_fifo_occ_max\":" << r.out_max << '}';
        }
        os << "],\"router_dat_input_vcs\":[";
        first = true;
        for (const auto& [key, q] : router_dat_input_vcs_) {
            if (!first) os << ',';
            first = false;
            const auto& [router, port, vc] = key;
            os << "{\"router\":\"" << router << "\",\"port\":\"" << port << "\",\"vc\":" << vc
               << ",\"hwm_flits\":" << q.hwm_flits
               << ",\"capacity_flits\":" << q.capacity_flits
               << ",\"occupancy_sum_flits\":" << q.occupancy_sum_flits
               << ",\"full_cycles\":" << q.full_cycles
               << ",\"samples\":" << q.samples << '}';
        }
        os << "],\"router_dat_allocations\":[";
        first = true;
        for (const auto& [key, wait] : router_dat_allocations_) {
            if (!first) os << ',';
            first = false;
            const auto& [router, input, vc, output] = key;
            os << "{\"router\":\"" << router << "\",\"input\":\"" << input
               << "\",\"vc\":" << vc << ",\"output\":\"" << output
               << "\",\"waiting_cycles\":" << wait.cycles
               << ",\"occupied_cycles\":" << wait.occupied
               << ",\"input_full_cycles\":" << wait.full
               << ",\"occupied_input_full_cycles\":" << wait.occupied_full << '}';
        }
        os << "],\"router_dat_output_vcs\":[";
        first = true;
        for (const auto& [key, q] : router_dat_output_vcs_) {
            if (!first) os << ',';
            first = false;
            const auto& [router, port, vc] = key;
            os << "{\"router\":\"" << router << "\",\"port\":\"" << port << "\",\"vc\":" << vc
               << ",\"credit_block_cycles\":" << q.credit_block_cycles << '}';
        }
        os << "],\"router_dat_switches\":[";
        first = true;
        for (const auto& [key, activity] : router_dat_switches_) {
            if (!first) os << ',';
            first = false;
            os << "{\"router\":\"" << key.first << "\",\"port\":\"" << key.second
               << "\",\"eligible_vc_cycles\":" << activity.eligible_vc_cycles
               << ",\"grant_cycles\":" << activity.grant_cycles
               << ",\"arbitration_wait_vc_cycles\":"
               << activity.eligible_vc_cycles - activity.grant_cycles
               << ",\"samples\":" << activity.samples << '}';
        }
        os << "],\"nmu_requests\":[";
        first = true;
        for (const auto& [key, request] : nmu_requests_) {
            if (!first) os << ',';
            first = false;
            os << "{\"node\":\"" << key.first << "\",\"channel\":\"" << key.second
               << "\",\"ordering_wait_cycles\":" << request.ordering
               << ",\"order_list_full_cycles\":" << request.order_list
               << ",\"reorder_storage_full_cycles\":" << request.storage
               << ",\"downstream_wait_cycles\":" << request.downstream
               << ",\"samples\":" << request.samples << '}';
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
    bool active_ = false;
    std::map<std::string, Router> routers_;
    std::map<DatVcKey, DatInputVc> router_dat_input_vcs_;
    std::map<DatVcKey, DatOutputVc> router_dat_output_vcs_;
    std::map<std::pair<std::string, std::string>, DatSwitch> router_dat_switches_;
    std::map<std::tuple<std::string, std::string, uint64_t, std::string>, AllocationWait>
        router_dat_allocations_;
    std::map<std::pair<std::string, std::string>, NmuRequest> nmu_requests_;
    std::map<std::string, Link> links_;
};

}  // namespace ni::cmodel::wrap

#endif  // NI_CMODEL_COSIM_PERF_COLLECTOR_HPP
