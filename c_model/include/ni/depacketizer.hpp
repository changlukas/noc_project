#pragma once
// Depacketizer: NMU/NSU-shared abstract base class.
//
// Mirror of Packetizer: depacketizers surface beats that have been peeled
// out of NoC flits. The NMU side depacketizes RESPONSE beats (B / R); the
// NSU side depacketizes REQUEST beats (AW / W / AR). Same abstract base
// carries both halves so a single concrete impl works at either end (and
// the loopback test stub stays singular).
//
// Contract: pop_* returns nullopt when no beat is currently available.
// Callers must NOT busy-wait — they call once per tick and process
// whatever comes back. A returned beat is consumed; the source advances
// by one slot.
//
// A given port only USES its half; the other half is exercised by the
// peer port via the same concrete object.
#include "axi/types.hpp"
#include <cstdint>
#include <optional>
#include <utility>

namespace ni::cmodel {

// Response-side metadata extracted from flit header.
// rob_req=0 indicates Disabled-mode flit (no rob_idx semantics);
// rob_req=1 indicates Enabled-mode flit (rob_idx identifies the ROB slot).
struct ResponseMeta {
  uint8_t rob_idx;
  uint8_t rob_req;
};

class Depacketizer {
 public:
  virtual ~Depacketizer() = default;

  // Response side (used by NMU AxiSlavePort)
  virtual std::optional<axi::BBeat> pop_b() = 0;
  virtual std::optional<axi::RBeat> pop_r() = 0;

  // Response side with ROB metadata (used by NMU Rob in Enabled mode).
  // Default implementations forward to pop_b/pop_r and return meta={0,0}
  // so Disabled-mode callers and non-Rob-aware concrete classes keep
  // working without overriding.
  virtual std::optional<std::pair<axi::BBeat, ResponseMeta>> pop_b_with_meta() {
    auto b = pop_b();
    if (!b) return std::nullopt;
    return std::make_pair(*b, ResponseMeta{0, 0});
  }
  virtual std::optional<std::pair<axi::RBeat, ResponseMeta>> pop_r_with_meta() {
    auto r = pop_r();
    if (!r) return std::nullopt;
    return std::make_pair(*r, ResponseMeta{0, 0});
  }

  // Request side (used by NSU AxiMasterPort)
  virtual std::optional<axi::AwBeat> pop_aw() = 0;
  virtual std::optional<axi::WBeat>  pop_w()  = 0;
  virtual std::optional<axi::ArBeat> pop_ar() = 0;
};

}  // namespace ni::cmodel
