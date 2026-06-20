#ifndef NI_CMODEL_COSIM_PERF_COLLECTOR_HPP
#define NI_CMODEL_COSIM_PERF_COLLECTOR_HPP

#include "nmu/addr_trans.hpp"

#include <cstdio>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace ni::cmodel::cosim {

// Single-run perf readout. SV monitors push per-txn + end-of-run counters;
// C-side sampling pushes router occupancy. dump() writes the spec §5.1 schema.
class PerfCollector {
  public:
    void set_scenario(std::string scenario) { scenario_ = std::move(scenario); }
    void set_window(uint64_t start_cyc, uint64_t end_cyc) {
        win_start_ = start_cyc;
        win_end_ = end_cyc;
    }

    void add_txn(const std::string& slot, uint32_t id, bool is_write, uint64_t addr, uint32_t len,
                 uint32_t size, uint64_t accept_cyc, uint64_t complete_cyc) {
        Slot& s = slot_(slot);
        Txn t;
        t.id = id;
        t.is_write = is_write;
        t.src_node = slot.substr(0, slot.find('.'));
        t.dst = nmu::addr_trans::xy_route(addr).dst_id;
        t.len = len;
        t.size = size;
        t.bytes = (static_cast<uint64_t>(len) + 1) << size;
        t.accept_cyc = accept_cyc;
        t.complete_cyc = complete_cyc;
        t.latency = complete_cyc - accept_cyc;
        s.txns.push_back(t);
    }

    void set_slot_backpressure(const std::string& slot, uint64_t slave_write_idle_cyc,
                               uint64_t master_read_idle_cyc) {
        Slot& s = slot_(slot);
        s.slave_write_idle_cyc = slave_write_idle_cyc;
        s.master_read_idle_cyc = master_read_idle_cyc;
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
        emit_slots(os);
        os << ',';
        emit_latency(os);
        os << ',';
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
    struct Txn {
        uint32_t id = 0;
        bool is_write = false;
        std::string src_node;
        uint32_t dst = 0;
        uint32_t len = 0;
        uint32_t size = 0;
        uint64_t bytes = 0;
        uint64_t accept_cyc = 0;
        uint64_t complete_cyc = 0;
        uint64_t latency = 0;
    };
    struct Slot {
        std::vector<Txn> txns;
        uint64_t slave_write_idle_cyc = 0;
        uint64_t master_read_idle_cyc = 0;
    };
    struct Router {
        uint64_t in_max = 0;
        uint64_t out_max = 0;
    };
    struct Link {
        uint64_t flit_count = 0;
        uint64_t stall_cyc = 0;
    };

    Slot& slot_(const std::string& name) {
        auto it = slots_.find(name);
        if (it == slots_.end()) it = slots_.emplace(name, Slot{}).first;
        return it->second;
    }
    static bool is_manager(const std::string& name) {
        return name.find("manager") != std::string::npos;
    }

    void emit_slot_entry(std::ostringstream& os, const std::string& name, const Slot& s) const {
        uint64_t wt = 0, rt = 0, wb = 0, rb = 0;
        for (const Txn& t : s.txns) {
            if (t.is_write) {
                ++wt;
                wb += t.bytes;
            } else {
                ++rt;
                rb += t.bytes;
            }
        }
        os << "{\"name\":\"" << name << "\",\"role\":\""
           << (is_manager(name) ? "manager" : "subordinate") << "\","
           << "\"write_txn_count\":" << wt << ",\"read_txn_count\":" << rt << ','
           << "\"write_byte_count\":" << wb << ",\"read_byte_count\":" << rb << ','
           << "\"slave_write_idle_cyc\":" << s.slave_write_idle_cyc
           << ",\"master_read_idle_cyc\":" << s.master_read_idle_cyc;
        if (!is_manager(name)) {
            os << ",\"service_latency\":{";
            emit_stats(os, "write", s.txns, true);
            os << ',';
            emit_stats(os, "read", s.txns, false);
            os << '}';
        }
        os << '}';
    }

    void emit_slots(std::ostringstream& os) const {
        os << "\"axi_slots\":[";
        bool first = true;
        for (const auto& [name, s] : slots_) {
            if (!first) os << ',';
            first = false;
            emit_slot_entry(os, name, s);
        }
        os << ']';
    }

    static void emit_stats(std::ostringstream& os, const char* key, const std::vector<Txn>& txns,
                           bool is_write) {
        uint64_t mn = 0, mx = 0, sum = 0, n = 0;
        for (const Txn& t : txns) {
            if (t.is_write != is_write) continue;
            if (n == 0 || t.latency < mn) mn = t.latency;
            if (t.latency > mx) mx = t.latency;
            sum += t.latency;
            ++n;
        }
        os << '"' << key << "\":{\"min\":" << mn << ",\"mean\":" << (n ? sum / n : 0)
           << ",\"max\":" << mx << '}';
    }

    void emit_latency(std::ostringstream& os) const {
        os << "\"latency\":{\"measured_at\":\"manager slot -- end-to-end\",\"transactions\":[";
        bool first = true;
        std::vector<Txn> mgr;
        for (const auto& [name, s] : slots_) {
            if (!is_manager(name)) continue;
            for (const Txn& t : s.txns) {
                mgr.push_back(t);
                if (!first) os << ',';
                first = false;
                os << "{\"id\":" << t.id << ",\"dir\":\"" << (t.is_write ? "write" : "read")
                   << "\",\"src\":\"" << t.src_node << "\",\"dst\":\"node" << t.dst
                   << "\",\"accept_cyc\":" << t.accept_cyc << ",\"complete_cyc\":" << t.complete_cyc
                   << ",\"latency\":" << t.latency << ",\"bytes\":" << t.bytes << '}';
            }
        }
        os << "],\"by_signature\":[";
        emit_signatures(os, mgr);
        os << "],\"histogram\":[";
        emit_histogram(os, mgr);
        os << "]}";
    }

    static void emit_signatures(std::ostringstream& os, const std::vector<Txn>& txns) {
        // key = (is_write, len, size, src_node, dst)
        std::map<std::tuple<bool, uint32_t, uint32_t, std::string, uint32_t>, std::vector<uint64_t>>
            g;
        for (const Txn& t : txns)
            g[{t.is_write, t.len, t.size, t.src_node, t.dst}].push_back(t.latency);
        bool first = true;
        for (const auto& [k, lats] : g) {
            if (!first) os << ',';
            first = false;
            uint64_t mn = lats[0], mx = lats[0], sum = 0;
            for (uint64_t l : lats) {
                if (l < mn) mn = l;
                if (l > mx) mx = l;
                sum += l;
            }
            os << "{\"op\":\"" << (std::get<0>(k) ? "write" : "read")
               << "\",\"len\":" << std::get<1>(k) << ",\"size\":" << std::get<2>(k) << ",\"src\":\""
               << std::get<3>(k) << "\",\"dst\":\"node" << std::get<4>(k)
               << "\",\"count\":" << lats.size() << ",\"min\":" << mn
               << ",\"mean\":" << (sum / lats.size()) << ",\"max\":" << mx << '}';
        }
    }

    void emit_histogram(std::ostringstream& os, const std::vector<Txn>& txns) const {
        // default ladder; last bin open-ended.
        static const uint64_t edges[] = {0, 16, 32, 64, 128, 256};
        const std::size_t n = sizeof(edges) / sizeof(edges[0]);
        for (std::size_t i = 0; i < n; ++i) {
            const uint64_t lo = edges[i];
            const uint64_t hi = (i + 1 < n) ? edges[i + 1] : 0;  // 0 = open
            uint64_t c = 0;
            for (const Txn& t : txns)
                if (t.latency >= lo && (hi == 0 || t.latency < hi)) ++c;
            if (i) os << ',';
            os << "{\"low\":" << lo << ",\"high\":" << hi << ",\"count\":" << c << '}';
        }
    }

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
    std::map<std::string, Slot> slots_;
    std::map<std::string, Router> routers_;
    std::map<std::string, Link> links_;
};

}  // namespace ni::cmodel::cosim

#endif  // NI_CMODEL_COSIM_PERF_COLLECTOR_HPP
