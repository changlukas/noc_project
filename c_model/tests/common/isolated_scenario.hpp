#pragma once
#include "axi/scenario_parser.hpp"
#include "axi/types.hpp"
#include <cstdint>
#include <fstream>
#include <string>

namespace ni::cmodel::testing {

// Lossless single-transaction scenario writer for Pass-1 characterization.
// Copies EVERY transaction + config field from `sc` for the transaction at
// `txn_index`, applies `dst_offset` to the transaction addr and to memory_base
// (so xy_route picks up the destination bit and the NSU Memory still covers the
// local address). Unlike shifted_scenario_path it preserves strb_file, lock,
// qos, inject, and the max-outstanding fields. Returns `out_path`.
inline std::string write_isolated_scenario(const axi::Scenario& sc, std::size_t txn_index,
                                           uint64_t dst_offset, const std::string& out_path) {
    const axi::ScenarioTransaction& t = sc.transactions.at(txn_index);
    std::ofstream f(out_path);
    f << "schema_version: 1\n";
    f << "metadata:\n";
    f << "  name: " << sc.metadata.name << "\n";
    f << "  category: " << sc.metadata.category << "\n";
    f << "config:\n";
    f << "  memory_base: 0x" << std::hex << (sc.config.memory_base + dst_offset) << std::dec
      << "\n";
    f << "  memory_size: " << sc.config.memory_size << "\n";
    f << "  write_latency: " << sc.config.write_latency << "\n";
    f << "  read_latency: " << sc.config.read_latency << "\n";
    f << "  max_outstanding_write: " << sc.config.max_outstanding_write << "\n";
    f << "  max_outstanding_read: " << sc.config.max_outstanding_read << "\n";
    if (sc.config.inject.mode == axi::InjectConfig::Mode::AwUnstable) {
        f << "  inject:\n    mode: aw_unstable\n    cycle: " << sc.config.inject.cycle << "\n";
    }
    f << "transactions:\n";
    const bool is_write = (t.op == axi::ScenarioTransaction::Op::Write);
    f << "  - op: " << (is_write ? "write" : "read") << "\n";
    f << "    addr: 0x" << std::hex << (t.addr + dst_offset) << std::dec << "\n";
    f << "    id: 0x" << std::hex << static_cast<unsigned>(t.id) << std::dec << "\n";
    f << "    len: " << static_cast<unsigned>(t.len) << "\n";
    f << "    size: " << static_cast<unsigned>(t.size) << "\n";
    const char* burst =
        (t.burst == axi::Burst::INCR) ? "INCR" : (t.burst == axi::Burst::WRAP ? "WRAP" : "FIXED");
    f << "    burst: " << burst << "\n";
    if (is_write) {
        f << "    data_file: " << t.data_file << "\n";  // absolute (resolved on load)
        if (!t.strb_file.empty()) f << "    strb_file: " << t.strb_file << "\n";
    } else {
        f << "    dump_file: " << (t.dump_file.empty() ? std::string("unused") : t.dump_file)
          << "\n";
    }
    f << "    lock: " << (t.lock == axi::LockType::Exclusive ? "exclusive" : "normal") << "\n";
    f << "    qos: " << static_cast<unsigned>(t.qos) << "\n";
    return out_path;
}

}  // namespace ni::cmodel::testing
