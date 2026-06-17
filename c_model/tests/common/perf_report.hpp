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
struct ComponentRecord {
    std::string name;
    std::string kind;
    uint64_t hop_min;
    double hop_mean;
    uint64_t hop_max;
    std::size_t occ_max;
    std::size_t occ_capacity;
};

// Assembles the Section 8 JSON object + a one-line-per-component stdout summary.
class PerfReport {
  public:
    void set_scenario(std::string s) { scenario_ = std::move(s); }
    void add_transaction(TxnRecord t) { txns_.push_back(std::move(t)); }
    void add_ni(ComponentRecord c) { ni_.push_back(std::move(c)); }
    void add_router(ComponentRecord c) { router_.push_back(std::move(c)); }

    void write_summary(std::ostream& os) const {
        for (const auto& c : ni_) write_component_line(os, c);
        for (const auto& c : router_) write_component_line(os, c);
    }

    void write_json(std::ostream& os) const {
        os << "{\"scenario\":\"" << scenario_ << "\",\"transactions\":[";
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
        os << "}}";
    }

    // stdout summary always; JSON to build/cmodel/perf/<scenario>.json (or
    // NOC_PERF_FILE). Creates the perf dir if std::filesystem is available.
    void emit() const {
        write_summary(std::cout);
        const char* f = std::getenv("NOC_PERF_FILE");
        std::string path;
        if (f) {
            path = f;
        } else {
            path = "build/cmodel/perf/" + scenario_ + ".json";
#ifdef NI_PERF_HAS_FILESYSTEM
            std::error_code ec;
            std::filesystem::create_directories("build/cmodel/perf", ec);
#endif
        }
        std::ofstream js(path);
        if (js) write_json(js);
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
        o += "\"hop_latency_cyc\":{\"min\":" + std::to_string(c.hop_min) +
             ",\"mean\":" + std::to_string(c.hop_mean) + ",\"max\":" + std::to_string(c.hop_max) +
             "},\"occupancy\":{\"max\":" + std::to_string(c.occ_max) +
             ",\"capacity\":" + std::to_string(c.occ_capacity) + "}}";
        return o;
    }
    static void write_component_line(std::ostream& os, const ComponentRecord& c) {
        os << "[perf:" << c.name << "] kind=" << c.kind << " hop(min/mean/max)=" << c.hop_min << '/'
           << c.hop_mean << '/' << c.hop_max << " occ(max/cap)=" << c.occ_max << '/'
           << c.occ_capacity << '\n';
    }

    std::string scenario_;
    std::vector<TxnRecord> txns_;
    std::vector<ComponentRecord> ni_;
    std::vector<ComponentRecord> router_;
};

}  // namespace ni::cmodel::testing
