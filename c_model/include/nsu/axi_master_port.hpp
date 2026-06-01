#pragma once
// NSU AxiMasterPort — thin transparent AXI4 manager transport, peer of
// nmu/axi_slave_port.hpp.
//
// Role (per docs/noc_cmodel_rtl_plan.md §3): the NSU's downstream-facing
// AXI boundary. Pre-shaped AW / W / AR beats arrive from the NoC fabric
// via a Depacketizer; this port queues them and exposes a manager-side
// pop interface (pop_aw / pop_w / pop_ar) so an external AXI subordinate
// (e.g. AxiSlave + Memory) can drain them. B / R beats returned from that
// subordinate are pushed back in (push_b / push_r), queued, and handed to
// a Packetizer for NoC return-path transport.
//
// Scope mirror image of AxiSlavePort: 5-channel handshake + channel
// attribute pass-through + wlast/rlast framing as-is. EXPLICITLY NOT done:
//   - per-beat address generation, memory bounds, DECERR, burst splitting,
//     AXI ID reorder — these live at the endpoint or the ROB.
//
// Port contract: per-channel FIFO order for all beats regardless of AXI ID.
// Cross-ID completion ordering / per-ID response reordering is the ROB
// stage's responsibility (see plan §3.1), NOT this port's.
//
// Wiring convention: the upstream side (Depacketizer + Packetizer) is the
// NoC. The downstream side is the AXI subordinate; the integration
// harness pulls AW/W/AR via pop_* and pushes B/R via push_* once per tick.
// This keeps the port's constructor signature symmetric with AxiSlavePort
// (just NoC handles + PortParams) — the test rig owns the explicit
// AxiMasterPort <-> AxiSlave glue, exactly one cycle per cycle.
#include "axi/types.hpp"
#include "ni/depacketizer.hpp"
#include "ni/packetizer.hpp"
#include "ni/port_params.hpp"
#include <cstddef>
#include <deque>
#include <optional>

namespace ni::cmodel::nsu {

// Per-channel internal FIFO depths come from ni::cmodel::PortParams
// (shared with nmu::AxiSlavePort). Single source of truth is
// c_model/config/port_params.yaml; see ni/port_params.hpp for the loader
// helper and the "fail loud / no defaults" rationale.

class AxiMasterPort {
 public:
  AxiMasterPort(Depacketizer& depacketizer,
                Packetizer& packetizer,
                PortParams params)
      : depkt_(depacketizer), pkt_(packetizer), params_(params) {}

  // ---- Downstream-facing AXI manager API ----
  // Symmetric mirror of AxiSlavePort's push_*/pop_*: pop_* hands out AW/W/AR
  // beats that the NoC delivered to this NSU; push_* takes B/R beats coming
  // back from the local subordinate. The harness wires these one-for-one to
  // the AxiSlave's push_aw / push_w / push_ar / pop_b / pop_r each cycle.
  std::optional<axi::AwBeat> pop_aw() {
    if (aw_q_.empty()) return std::nullopt;
    auto r = aw_q_.front(); aw_q_.pop_front(); return r;
  }
  std::optional<axi::WBeat> pop_w() {
    if (w_q_.empty()) return std::nullopt;
    auto r = w_q_.front(); w_q_.pop_front(); return r;
  }
  std::optional<axi::ArBeat> pop_ar() {
    if (ar_q_.empty()) return std::nullopt;
    auto r = ar_q_.front(); ar_q_.pop_front(); return r;
  }
  bool push_b(const axi::BBeat& b) {
    if (b_q_.size() >= params_.b_queue_depth) return false;
    b_q_.push_back(b); return true;
  }
  bool push_r(const axi::RBeat& b) {
    if (r_q_.size() >= params_.r_queue_depth) return false;
    r_q_.push_back(b); return true;
  }

  void tick() {
    // Response forward to packetizer BEFORE request drain from depacketizer:
    // hand any pending B/R up to the packetizer first so this port's response
    // queue slots are freed before this cycle's new push_b / push_r calls,
    // then ingest fresh AW/W/AR from the depacketizer for downstream pop_*.
    // Mirrors the response-before-request ordering rationale of AxiSlavePort
    // + Stage 2 AxiMaster::tick (frees response-side backpressure first so a
    // new producer push lands in the same cycle).
    forward_b_to_packetizer_();
    forward_r_to_packetizer_();
    drain_aw_from_depacketizer_();
    drain_w_from_depacketizer_();
    drain_ar_from_depacketizer_();
  }

  // ---- Introspection (tests) ----
  std::size_t aw_q_size() const { return aw_q_.size(); }
  std::size_t w_q_size()  const { return w_q_.size();  }
  std::size_t ar_q_size() const { return ar_q_.size(); }
  std::size_t b_q_size()  const { return b_q_.size();  }
  std::size_t r_q_size()  const { return r_q_.size();  }
  const PortParams& params() const { return params_; }

 private:
  void drain_aw_from_depacketizer_() {
    while (aw_q_.size() < params_.aw_queue_depth) {
      auto a = depkt_.pop_aw();
      if (!a) break;
      aw_q_.push_back(*a);
    }
  }
  void drain_w_from_depacketizer_() {
    while (w_q_.size() < params_.w_queue_depth) {
      auto a = depkt_.pop_w();
      if (!a) break;
      w_q_.push_back(*a);
    }
  }
  void drain_ar_from_depacketizer_() {
    while (ar_q_.size() < params_.ar_queue_depth) {
      auto a = depkt_.pop_ar();
      if (!a) break;
      ar_q_.push_back(*a);
    }
  }
  void forward_b_to_packetizer_() {
    while (!b_q_.empty()) {
      if (!pkt_.push_b(b_q_.front())) break;
      b_q_.pop_front();
    }
  }
  void forward_r_to_packetizer_() {
    while (!r_q_.empty()) {
      if (!pkt_.push_r(r_q_.front())) break;
      r_q_.pop_front();
    }
  }

  Depacketizer& depkt_;
  Packetizer&   pkt_;
  PortParams    params_;
  std::deque<axi::AwBeat> aw_q_;
  std::deque<axi::WBeat>  w_q_;
  std::deque<axi::ArBeat> ar_q_;
  std::deque<axi::BBeat>  b_q_;
  std::deque<axi::RBeat>  r_q_;
};

}  // namespace ni::cmodel::nsu
