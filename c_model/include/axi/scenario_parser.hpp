// Independent design (cocotbext-axi has no scenario file format) — see axi/ATTRIBUTION.md
#pragma once
#include "axi/protocol_rules.hpp"
#include "axi/types.hpp"
#include <cstdint>
#include <filesystem>
#include <map>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

namespace ni::cmodel::axi {

struct Metadata {
    std::string name;
    std::string category;
};

// Fault injection configuration parsed from YAML config.inject:
// When mode == None (default, no inject: field present), tick() overhead is
// one bool check per cycle — zero behavioral change.
struct InjectConfig {
    enum class Mode { None, AwUnstable };
    Mode mode = Mode::None;
    std::size_t cycle = 0;  // tick index at which to trigger the violation
};

struct ScenarioConfig {
    uint64_t memory_base = 0;
    std::size_t memory_size = 0x10000;
    std::size_t write_latency = 1;
    std::size_t read_latency = 1;
    std::size_t max_outstanding_write = 1;
    std::size_t max_outstanding_read = 1;
    InjectConfig inject{};  // optional; defaults to Mode::None
};

struct ScenarioTransaction {
    enum class Op { Write, Read };
    Op op;
    uint64_t addr;
    uint8_t id;
    uint8_t len, size;
    Burst burst;
    std::string data_file;
    std::string dump_file;
    std::string strb_file;             // optional; empty = full WSTRB per beat
    LockType lock = LockType::Normal;  // optional; YAML "normal" or "exclusive"
    uint8_t qos = 0;                   // optional; YAML "qos" (default 0)
    std::size_t scenario_line;
};

struct Scenario {
    Metadata metadata;
    ScenarioConfig config;
    std::vector<ScenarioTransaction> transactions;
};

inline Burst parse_burst(const std::string& s) {
    if (s == "INCR") return Burst::INCR;
    if (s == "WRAP") return Burst::WRAP;
    if (s == "FIXED") return Burst::FIXED;
    throw std::runtime_error("scenario: unknown burst '" + s + "'");
}

inline Scenario load_scenario(const std::string& path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::Exception& e) {
        throw std::runtime_error("scenario: parse failed for '" + path + "': " + e.what());
    }
    // Resolve data_file / dump_file / strb_file relative to the YAML's own
    // directory so a scenario can be consumed from any cwd. Absolute paths
    // are honored as-is (escape hatch for ad-hoc tests).
    const std::filesystem::path yaml_dir = std::filesystem::path(path).parent_path();
    auto resolve_data_path = [&yaml_dir](std::string raw) -> std::string {
        if (raw.empty()) return raw;
        std::filesystem::path p(raw);
        if (p.is_absolute()) return raw;
        return (yaml_dir / p).string();
    };
    static const std::regex kNameRegex(
        R"(^AX4-(BAS|BUR|BND|ORD|EXC|RSP|STR|HSH|INF|QOS)-\d{3}_[a-z0-9_]+$)");
    static const std::map<std::string, std::string> kCatCategory = {
        {"BAS", "basic"},    {"BUR", "burst"},     {"BND", "boundary"},
        {"ORD", "ordering"}, {"EXC", "exclusive"}, {"RSP", "response"},
        {"STR", "stress"},   {"HSH", "handshake"}, {"INF", "infrastructure"},
        {"QOS", "qos"},
    };

    bool strict = false;
    if (root["schema_version"]) {
        auto v = root["schema_version"].as<int>();
        if (v != 1) {
            throw std::runtime_error("scenario: unsupported schema_version " + std::to_string(v));
        }
        strict = true;
    }

    Scenario sc;

    if (strict && !root["metadata"]) {
        throw std::runtime_error("scenario: schema_version 1 requires metadata block");
    }

    if (root["metadata"]) {
        auto md = root["metadata"];
        if (md["name"]) sc.metadata.name = md["name"].as<std::string>();
        if (md["category"]) sc.metadata.category = md["category"].as<std::string>();

        if (strict) {
            if (sc.metadata.name.empty() || sc.metadata.category.empty()) {
                throw std::runtime_error("scenario: metadata.name and metadata.category required");
            }
            if (!std::regex_match(sc.metadata.name, kNameRegex)) {
                throw std::runtime_error("scenario: metadata.name '" + sc.metadata.name +
                                         "' does not match AX4-CAT-NNN_slug regex");
            }
            auto cat3 = sc.metadata.name.substr(4, 3);
            auto it = kCatCategory.find(cat3);
            if (it == kCatCategory.end() || it->second != sc.metadata.category) {
                throw std::runtime_error("scenario: metadata.name CAT '" + cat3 +
                                         "' does not match metadata.category '" +
                                         sc.metadata.category + "'");
            }
        }
    }

    if (root["config"]) {
        auto cfg = root["config"];
        static const std::vector<std::string> known_cfg = {
            "memory_base",           "memory_size",          "write_latency", "read_latency",
            "max_outstanding_write", "max_outstanding_read", "inject"};
        for (auto it = cfg.begin(); it != cfg.end(); ++it) {
            auto key = it->first.as<std::string>();
            bool ok = false;
            for (auto& k : known_cfg)
                if (k == key) {
                    ok = true;
                    break;
                }
            if (!ok) throw std::runtime_error("scenario config: unknown field '" + key + "'");
        }
        if (cfg["memory_base"]) sc.config.memory_base = cfg["memory_base"].as<uint64_t>();
        if (cfg["memory_size"]) sc.config.memory_size = cfg["memory_size"].as<std::size_t>();
        if (cfg["write_latency"]) sc.config.write_latency = cfg["write_latency"].as<std::size_t>();
        if (cfg["read_latency"]) sc.config.read_latency = cfg["read_latency"].as<std::size_t>();
        if (cfg["max_outstanding_write"])
            sc.config.max_outstanding_write = cfg["max_outstanding_write"].as<std::size_t>();
        if (cfg["max_outstanding_read"])
            sc.config.max_outstanding_read = cfg["max_outstanding_read"].as<std::size_t>();
        if (cfg["inject"]) {
            auto inj = cfg["inject"];
            const std::string mode_str = inj["mode"].as<std::string>();
            if (mode_str == "aw_unstable") {
                sc.config.inject.mode = InjectConfig::Mode::AwUnstable;
            } else {
                throw std::runtime_error("scenario: unknown +inject mode '" + mode_str +
                                         "' (allowlist: aw_unstable)");
            }
            sc.config.inject.cycle = inj["cycle"].as<std::size_t>();
        }
    }

    if (!root["transactions"] || !root["transactions"].IsSequence() ||
        root["transactions"].size() == 0) {
        throw std::runtime_error("scenario: 'transactions' must be a non-empty sequence");
    }

    std::size_t line = 0;
    for (const auto& txn : root["transactions"]) {
        ++line;
        ScenarioTransaction t{};
        t.scenario_line = line;
        auto op = txn["op"].as<std::string>();
        if (op == "write")
            t.op = ScenarioTransaction::Op::Write;
        else if (op == "read")
            t.op = ScenarioTransaction::Op::Read;
        else
            throw std::runtime_error("scenario txn " + std::to_string(line) + ": unknown op '" +
                                     op + "'");
        t.addr = txn["addr"].as<uint64_t>();
        t.id = txn["id"].as<uint8_t>();
        t.len = txn["len"].as<uint8_t>();
        t.size = txn["size"].as<uint8_t>();
        if (t.size > rules::kMaxSize) {
            throw std::runtime_error("scenario txn " + std::to_string(line) +
                                     ": size must be <= " + std::to_string(rules::kMaxSize) +
                                     " (max beat = " + std::to_string(DATA_BYTES) +
                                     " bytes, log2(DATA_BYTES))");
        }
        t.burst = parse_burst(txn["burst"].as<std::string>());
        // AXI4 WRAP constraints (IHI 0022 B1.4.3 Address structure of bursts):
        //   - len ∈ {1, 3, 7, 15} so that total_burst_bytes is a power of 2
        //   - addr aligned to (1<<size) so per-beat addresses stay grid-aligned
        if (t.burst == Burst::WRAP) {
            if (t.len != 1 && t.len != 3 && t.len != 7 && t.len != 15) {
                throw std::runtime_error("scenario txn " + std::to_string(line) +
                                         ": WRAP burst len must be 1, 3, 7, or 15");
            }
            if ((t.addr & ((1ull << t.size) - 1)) != 0) {
                throw std::runtime_error("scenario txn " + std::to_string(line) +
                                         ": WRAP burst addr must be aligned to (1<<size)");
            }
        }
        if (t.op == ScenarioTransaction::Op::Write)
            t.data_file = resolve_data_path(txn["data_file"].as<std::string>());
        if (t.op == ScenarioTransaction::Op::Read)
            t.dump_file = resolve_data_path(txn["dump_file"].as<std::string>());
        if (t.op == ScenarioTransaction::Op::Write && txn["strb_file"]) {
            t.strb_file = resolve_data_path(txn["strb_file"].as<std::string>());
        }
        if (txn["lock"]) {
            auto lock_str = txn["lock"].as<std::string>();
            if (lock_str == "normal") {
                t.lock = LockType::Normal;
            } else if (lock_str == "exclusive") {
                t.lock = LockType::Exclusive;
            } else {
                throw std::runtime_error("scenario txn " + std::to_string(line) +
                                         ": lock must be 'normal' or 'exclusive' (got '" +
                                         lock_str + "')");
            }
        }
        if (auto q = txn["qos"]) t.qos = static_cast<uint8_t>(q.as<unsigned>());
        sc.transactions.push_back(t);
    }
    return sc;
}

}  // namespace ni::cmodel::axi
