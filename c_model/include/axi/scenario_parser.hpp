// Algorithms ported from cocotbext-axi (MIT) — see axi/ATTRIBUTION.md
#pragma once
#include "axi/types.hpp"
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

namespace ni::cmodel::axi {

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
    std::size_t scenario_line;
};

struct Scenario {
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
    Scenario sc;

    if (root["config"]) {
        auto cfg = root["config"];
        static const std::vector<std::string> known_cfg = {
            "memory_base",  "memory_size",           "write_latency",
            "read_latency", "max_outstanding_write", "max_outstanding_read",
            "inject"};
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
        if (t.size > 5) {
            throw std::runtime_error("scenario txn " + std::to_string(line) +
                                     ": size must be <= 5 (max beat = 32 bytes, log2(DATA_BYTES))");
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
            t.data_file = txn["data_file"].as<std::string>();
        if (t.op == ScenarioTransaction::Op::Read) t.dump_file = txn["dump_file"].as<std::string>();
        if (t.op == ScenarioTransaction::Op::Write && txn["strb_file"]) {
            t.strb_file = txn["strb_file"].as<std::string>();
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
        sc.transactions.push_back(t);
    }
    return sc;
}

}  // namespace ni::cmodel::axi
