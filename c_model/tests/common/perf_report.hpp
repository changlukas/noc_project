#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <ostream>
#include <string>
#include <vector>

#if defined(__has_include)
#if __has_include(<filesystem>)
#include <filesystem>
#define NI_PERF_HAS_FILESYSTEM 1
#endif
#endif

namespace ni::cmodel::testing {

// One transaction row (spec Section 8 transactions[]). queueing is derived.
struct TxnRecord {
    std::size_t line;
    std::string type;  // "read" | "write"
    uint8_t id;
    std::string src;
    std::string dst;
    std::vector<std::string> request_path;
    std::vector<std::string> response_path;
    uint64_t measured_cyc;
    uint64_t zero_load_cyc;
};

// One component row (NI entry or router). kind: "nmu" | "nsu" | "router".
// occ_available=false emits occupancy as the JSON null literal / "n/a" stdout,
// so an unavailable occupancy is never printed as a measured 0.
struct ComponentRecord {
    std::string name;
    std::string kind;
    uint64_t hop_min;
    double hop_mean;
    uint64_t hop_max;
    std::size_t occ_max;
    std::size_t occ_capacity;
    bool occ_available = true;
};

// Run metadata block (spec Section 4 "No run metadata" fix).
struct RunMeta {
    std::string scenario;
    uint8_t mesh_x = 0;
    uint8_t mesh_y = 0;
    uint8_t num_vc = 0;
    uint64_t total_cycles = 0;
    std::size_t txn_count = 0;
    std::string json_path;
};

// Assembles the Section 8 JSON object + a one-line-per-component stdout summary.
class PerfReport {
  public:
    void set_scenario(std::string s) { scenario_ = std::move(s); }
    void set_run_meta(RunMeta m) { meta_ = std::move(m); }
    void set_slave_remainder(uint64_t cyc) { slave_remainder_ = cyc; }
    void add_transaction(TxnRecord t) { txns_.push_back(std::move(t)); }
    void add_ni(ComponentRecord c) { ni_.push_back(std::move(c)); }
    void add_router(ComponentRecord c) { router_.push_back(std::move(c)); }

    void write_summary(std::ostream& os) const {
        os << "[perf:run] scenario=" << meta_.scenario
           << " mesh=" << static_cast<unsigned>(meta_.mesh_x) << 'x'
           << static_cast<unsigned>(meta_.mesh_y)
           << " num_vc=" << static_cast<unsigned>(meta_.num_vc)
           << " total_cycles=" << meta_.total_cycles << " transactions=" << meta_.txn_count
           << " json=" << meta_.json_path << '\n';
        for (const auto& c : ni_) write_component_line(os, c);
        for (const auto& c : router_) write_component_line(os, c);
        os << "[perf:slave] remainder_cyc=" << slave_remainder_ << '\n';
    }

    void write_json(std::ostream& os) const {
        os << "{\"scenario\":\"" << scenario_ << "\",\"run\":{\"scenario\":\"" << meta_.scenario
           << "\",\"mesh_x\":" << static_cast<unsigned>(meta_.mesh_x)
           << ",\"mesh_y\":" << static_cast<unsigned>(meta_.mesh_y)
           << ",\"num_vc\":" << static_cast<unsigned>(meta_.num_vc)
           << ",\"total_cycles\":" << meta_.total_cycles
           << ",\"transaction_count\":" << meta_.txn_count << ",\"json_path\":\"" << meta_.json_path
           << "\"},\"transactions\":[";
        for (std::size_t i = 0; i < txns_.size(); ++i) {
            const auto& t = txns_[i];
            if (i) os << ',';
            const uint64_t q =
                t.measured_cyc >= t.zero_load_cyc ? (t.measured_cyc - t.zero_load_cyc) : 0;
            os << "{\"line\":" << t.line << ",\"type\":\"" << t.type
               << "\",\"id\":" << static_cast<unsigned>(t.id) << ",\"src\":\"" << t.src
               << "\",\"dst\":\"" << t.dst << "\",\"request_path\":" << path_json(t.request_path)
               << ",\"response_path\":" << path_json(t.response_path)
               << ",\"measured_latency_cyc\":" << t.measured_cyc
               << ",\"zero_load_cyc\":" << t.zero_load_cyc << ",\"queueing_cyc\":" << q << '}';
        }
        os << "],\"ni\":{";
        for (std::size_t i = 0; i < ni_.size(); ++i) {
            if (i) os << ',';
            os << '"' << ni_[i].name << "\":" << component_json(ni_[i], /*with_kind=*/true);
        }
        os << "},\"router\":{";
        for (std::size_t i = 0; i < router_.size(); ++i) {
            if (i) os << ',';
            os << '"' << router_[i].name
               << "\":" << component_json(router_[i], /*with_kind=*/false);
        }
        os << "},\"slave\":{\"remainder_cyc\":" << slave_remainder_ << "}}";
    }

    // stdout summary always. JSON path priority: NOC_PERF_FILE (full path, single
    // scenario) > NOC_PERF_DIR/<scenario>.json (per-scenario, batch run -- avoids
    // every scenario overwriting one shared file) > the CWD-relative default.
    void emit() const {
        write_summary(std::cout);
        const char* file = std::getenv("NOC_PERF_FILE");
        const char* dir = std::getenv("NOC_PERF_DIR");
        std::string path, mkdir_target;
        if (file) {
            path = file;
        } else if (dir) {
            path = std::string(dir) + "/" + scenario_ + ".json";
            mkdir_target = dir;
        } else {
            path = "build/cmodel/perf/" + scenario_ + ".json";
            mkdir_target = "build/cmodel/perf";
        }
#ifdef NI_PERF_HAS_FILESYSTEM
        if (!mkdir_target.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(mkdir_target, ec);
        }
#endif
        std::ofstream js(path);
        if (js) write_json(js);
        std::cout << "[perf:run] wrote " << path << '\n';
    }

  private:
    static std::string path_json(const std::vector<std::string>& p) {
        std::string o = "[";
        for (std::size_t i = 0; i < p.size(); ++i) o += (i ? ",\"" : "\"") + p[i] + "\"";
        return o + "]";
    }
    static std::string component_json(const ComponentRecord& c, bool with_kind) {
        std::string o = "{";
        if (with_kind) o += "\"kind\":\"" + c.kind + "\",";
        o += "\"latency_cyc\":{\"min\":" + std::to_string(c.hop_min) +
             ",\"mean\":" + std::to_string(c.hop_mean) + ",\"max\":" + std::to_string(c.hop_max) +
             "},\"occupancy\":";
        if (c.occ_available) {
            o += "{\"max\":" + std::to_string(c.occ_max) +
                 ",\"capacity\":" + std::to_string(c.occ_capacity) + "}}";
        } else {
            o += "{\"max\":null,\"capacity\":null}}";
        }
        return o;
    }
    static void write_component_line(std::ostream& os, const ComponentRecord& c) {
        os << "[perf:" << c.name << "] latency_cyc(min/mean/max)=" << c.hop_min << '/' << c.hop_mean
           << '/' << c.hop_max << " occupancy(max/capacity)=";
        if (c.occ_available) {
            os << c.occ_max << '/' << c.occ_capacity;
        } else {
            os << "n/a";
        }
        os << '\n';
    }

    std::string scenario_;
    RunMeta meta_{};
    uint64_t slave_remainder_ = 0;
    std::vector<TxnRecord> txns_;
    std::vector<ComponentRecord> ni_;
    std::vector<ComponentRecord> router_;
};

}  // namespace ni::cmodel::testing
