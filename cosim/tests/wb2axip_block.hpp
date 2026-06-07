// Returns a SKIP reason string if this scenario's content violates a known
// wb2axip structural constraint (or names a fault-injection mode reserved
// for dedicated tests). nullopt = wb2axip can in principle run it.
//
// Verified constraints (wb2axip/rtl/faxi_slave.v audit, Codex review):
//   - AWLEN must be 0 (single-beat write)         line 805-807
//   - wr_pending <= 1 (single outstanding write)  line 805-807
//   - No exclusive-access monitor
//
// Not blockers (rejected after source audit):
//   - max_outstanding_read > 1 — wb2axip permits multiple outstanding reads
//   - burst != INCR — wb2axip handles WRAP/FIXED for single beat (len=0);
//     multi-beat variants are already caught by the len>0 check
//   - OOB address — wb2axip is a protocol checker, not address-map predictor
#pragma once
#include "axi/scenario_parser.hpp"
#include <optional>
#include <string>

namespace noc::tests {

inline std::optional<std::string> wb2axip_block_reason(ni::cmodel::axi::Scenario const& sc) {
    using ni::cmodel::axi::Burst;
    using ni::cmodel::axi::InjectConfig;
    using ni::cmodel::axi::LockType;
    if (sc.config.max_outstanding_write > 1) return "WB2AXIP_MAX_OUT_WRITE";
    if (sc.config.inject.mode != InjectConfig::Mode::None) return "INJECTION_DEDICATED_TEST";
    for (auto const& t : sc.transactions) {
        if (t.len > 0) return "WB2AXIP_MULTI_BEAT";
        if (t.lock == LockType::Exclusive) return "WB2AXIP_EXCLUSIVE";
    }
    return std::nullopt;
}

}  // namespace noc::tests
