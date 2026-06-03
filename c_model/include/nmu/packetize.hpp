#pragma once
// NMU-side request packetizer. Stateless except for a small write-metadata
// FIFO (populated by push_aw, consumed by W beats with wlast=1). Implements
// the 5-method Packetizer interface; push_b/push_r assert false (NMU never
// emits responses).
//
// Header fields per push:
//   src_id      — constructor arg (NMU tile coord, fixed per instance)
//   dst_id      — frozen Packetizer interface path: derived from b.addr via
//                 addr_trans::xy_route. Rob-driven path (push_*_with_meta)
//                 supplies dst_id directly via AwHeaderMeta. For W beats, dst
//                 inherited from the AW write-meta FIFO front.
//   vc_id       — hardcoded 0 (NUM_VC=1)
//   axi_ch      — implicit per push_* method
//   last        — always 1 (1 beat = 1 flit = 1 packet)
//   rob_req,
//   rob_idx     — 0 in frozen interface path (Disabled mode); Rob path
//                 supplies via AwHeaderMeta (future Enabled mode).
//   commtype,
//   multicast,
//   noc_qos,
//   route_par,
//   flit_ecc    — 0-filled (deferred to future tasks)
//   rsvd        — 0 by Flit default
#include "axi/types.hpp"
#include "ni/flit.hpp"
#include "ni/packetizer.hpp"
#include "nmu/addr_trans.hpp"
#include "noc/noc_req_out.hpp"
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>

namespace ni::cmodel::nmu {

// Per-AW/AR metadata for header stamping. Used by both the frozen Packetizer
// interface (auto-filled from addr_trans + rob_*=0) and the explicit
// push_*_with_meta path (called by Rob with full metadata).
struct AwHeaderMeta {
    uint8_t  dst_id;     // from addr_trans
    uint64_t local_addr; // from addr_trans (= awaddr in c_model)
    uint8_t  rob_req;    // 0 in Disabled mode, 1 in Enabled mode
    uint8_t  rob_idx;    // 0 in Disabled, allocated in Enabled
};

class Packetize : public Packetizer {
public:
  Packetize(noc::NocReqOut& req_out, uint8_t src_id)
      : req_out_(req_out), src_id_(src_id) {}

  // ---- Packetizer interface (request methods are real) ----
  bool push_aw(const axi::AwBeat& b) override {
    auto t = addr_trans::xy_route(b.addr);
    return push_aw_with_meta(b, {t.dst_id, t.local_addr, 0, 0});
  }
  // INVARIANT: caller must push_aw before push_w for the same write txn. W
  // FIFO ordering inherits AW issue order; Rob layer enforces this via
  // w_burst_credit_ in Disabled mode (Task 7).
  bool push_w (const axi::WBeat&  b) override;
  bool push_ar(const axi::ArBeat& b) override {
    auto t = addr_trans::xy_route(b.addr);
    return push_ar_with_meta(b, {t.dst_id, t.local_addr, 0, 0});
  }

  // ---- Packetizer interface (response methods assert false) ----
  bool push_b(const axi::BBeat&) override {
    assert(false && "nmu::Packetize::push_b: NMU packetizer handles request side only "
                    "(AW/W/AR into NocReqOut) — B belongs on the response side "
                    "(NSU Packetize). Likely cause: AxiSlavePort wiring routed a response "
                    "beat into the NMU packetizer, or test fixture invoked the wrong "
                    "Packetizer instance.");
    std::abort();
    return false;
  }
  bool push_r(const axi::RBeat&) override {
    assert(false && "nmu::Packetize::push_r: NMU packetizer handles request side only "
                    "(AW/W/AR into NocReqOut) — R belongs on the response side "
                    "(NSU Packetize). Likely cause: AxiSlavePort wiring routed a response "
                    "beat into the NMU packetizer, or test fixture invoked the wrong "
                    "Packetizer instance.");
    std::abort();
    return false;
  }

  // ---- Non-interface methods, called by Rob with full metadata ----
  // Future Enabled mode supplies rob_idx via this path.
  bool push_aw_with_meta(const axi::AwBeat& b, AwHeaderMeta meta);
  bool push_ar_with_meta(const axi::ArBeat& b, AwHeaderMeta meta);

private:
  noc::NocReqOut& req_out_;
  uint8_t src_id_;

  // W FIFO carries the meta inherited from AW. local_addr NOT stored:
  // W payload has no address field; only header dst_id/rob_* needed.
  struct WMeta { uint8_t dst_id; uint8_t rob_req; uint8_t rob_idx; };
  std::deque<WMeta> w_meta_fifo_;
};

// ---- inline impl ----

inline bool Packetize::push_aw_with_meta(const axi::AwBeat& b, AwHeaderMeta meta) {
  Flit f;
  f.set_header_field("axi_ch",  ni::AXI_CH_AW);
  f.set_header_field("src_id",  src_id_);
  f.set_header_field("dst_id",  meta.dst_id);
  f.set_header_field("vc_id",   0);
  f.set_header_field("last",    1);
  f.set_header_field("rob_req", meta.rob_req);
  f.set_header_field("rob_idx", meta.rob_idx);
  f.set_payload_field("AW", "awid",     b.id);
  f.set_payload_field("AW", "awaddr",   meta.local_addr);  // NOT b.addr (future remap-safe)
  f.set_payload_field("AW", "awlen",    b.len);
  f.set_payload_field("AW", "awsize",   b.size);
  f.set_payload_field("AW", "awburst",  static_cast<uint64_t>(b.burst));
  f.set_payload_field("AW", "awcache",  b.cache);
  f.set_payload_field("AW", "awlock",   b.lock);
  f.set_payload_field("AW", "awprot",   b.prot);
  f.set_payload_field("AW", "awregion", b.region);
  f.set_payload_field("AW", "awuser",   b.user);
  if (!req_out_.push_flit(f)) return false;
  w_meta_fifo_.push_back({meta.dst_id, meta.rob_req, meta.rob_idx});
  return true;
}

// INVARIANT: caller must push_aw before push_w for the same write txn. W
// FIFO ordering inherits AW issue order; Rob layer enforces this via
// w_burst_credit_ in Disabled mode (Task 7).
inline bool Packetize::push_w(const axi::WBeat& b) {
  assert(!w_meta_fifo_.empty() && "push_w called before any push_aw");
  const auto& meta = w_meta_fifo_.front();
  Flit f;
  f.set_header_field("axi_ch",  ni::AXI_CH_W);
  f.set_header_field("src_id",  src_id_);
  f.set_header_field("dst_id",  meta.dst_id);
  f.set_header_field("vc_id",   0);
  f.set_header_field("last",    1);
  f.set_header_field("rob_req", meta.rob_req);
  f.set_header_field("rob_idx", meta.rob_idx);
  f.set_payload_field("W", "wlast", b.last ? 1u : 0u);
  f.set_payload_field("W", "wuser", b.user);
  f.set_payload_field("W", "wstrb", b.strb);
  f.set_payload_bytes("W", "wdata", b.data.data(), 256);
  if (!req_out_.push_flit(f)) return false;
  if (b.last) w_meta_fifo_.pop_front();
  return true;
}

inline bool Packetize::push_ar_with_meta(const axi::ArBeat& b, AwHeaderMeta meta) {
  Flit f;
  f.set_header_field("axi_ch",  ni::AXI_CH_AR);
  f.set_header_field("src_id",  src_id_);
  f.set_header_field("dst_id",  meta.dst_id);
  f.set_header_field("vc_id",   0);
  f.set_header_field("last",    1);
  f.set_header_field("rob_req", meta.rob_req);
  f.set_header_field("rob_idx", meta.rob_idx);
  f.set_payload_field("AR", "arid",     b.id);
  f.set_payload_field("AR", "araddr",   meta.local_addr);
  f.set_payload_field("AR", "arlen",    b.len);
  f.set_payload_field("AR", "arsize",   b.size);
  f.set_payload_field("AR", "arburst",  static_cast<uint64_t>(b.burst));
  f.set_payload_field("AR", "arcache",  b.cache);
  f.set_payload_field("AR", "arlock",   b.lock);
  f.set_payload_field("AR", "arprot",   b.prot);
  f.set_payload_field("AR", "arregion", b.region);
  f.set_payload_field("AR", "aruser",   b.user);
  if (!req_out_.push_flit(f)) return false;
  return true;
}

}  // namespace ni::cmodel::nmu
