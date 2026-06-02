#pragma once
// NMU-side request packetizer. Stateless except for a small write-metadata
// FIFO (populated by push_aw, consumed by W beats with wlast=1). Implements
// the 5-method Packetizer interface; push_b/push_r assert false (NMU never
// emits responses).
//
// Header fields per push:
//   src_id      — constructor arg (NMU tile coord, fixed per instance)
//   dst_id      — set via set_aw/ar_header_extras (sticky, fail-loud); for W,
//                 inherited from the AW write-meta FIFO front
//   vc_id       — hardcoded 0 (NUM_VC=1)
//   axi_ch      — implicit per push_* method
//   last        — always 1 (1 beat = 1 flit = 1 packet)
//   rob_req,
//   rob_idx     — set via set_aw/ar_header_extras
//   commtype,
//   multicast,
//   noc_qos,
//   route_par,
//   flit_ecc    — 0-filled (deferred to future tasks)
//   rsvd        — 0 by Flit default
#include "axi/types.hpp"
#include "ni/flit.hpp"
#include "ni/packetizer.hpp"
#include "noc/noc_req_out.hpp"
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>

namespace ni::cmodel::nmu {

class Packetize : public Packetizer {
public:
  Packetize(noc::NocReqOut& req_out, uint8_t src_id)
      : req_out_(req_out), src_id_(src_id) {}

  // ---- Packetizer interface (request methods are real) ----
  bool push_aw(const axi::AwBeat& b) override;
  bool push_w (const axi::WBeat&  b) override;
  bool push_ar(const axi::ArBeat& b) override;

  // ---- Packetizer interface (response methods assert false) ----
  bool push_b(const axi::BBeat&) override {
    assert(false && "NMU packetize: B not applicable");
    std::abort();
    return false;
  }
  bool push_r(const axi::RBeat&) override {
    assert(false && "NMU packetize: R not applicable");
    std::abort();
    return false;
  }

  // ---- Sticky setter (fail-loud) ----
  void set_aw_header_extras(uint8_t dst_id, uint8_t rob_req = 0, uint8_t rob_idx = 0) {
    assert(!aw_extras_pending_ && "previous set_aw_header_extras not yet consumed by push_aw");
    aw_dst_id_ = dst_id; aw_rob_req_ = rob_req; aw_rob_idx_ = rob_idx;
    aw_extras_pending_ = true;
  }
  void set_ar_header_extras(uint8_t dst_id, uint8_t rob_req = 0, uint8_t rob_idx = 0) {
    assert(!ar_extras_pending_ && "previous set_ar_header_extras not yet consumed by push_ar");
    ar_dst_id_ = dst_id; ar_rob_req_ = rob_req; ar_rob_idx_ = rob_idx;
    ar_extras_pending_ = true;
  }

private:
  noc::NocReqOut& req_out_;
  uint8_t src_id_;
  bool aw_extras_pending_ = false;
  bool ar_extras_pending_ = false;
  uint8_t aw_dst_id_ = 0, aw_rob_req_ = 0, aw_rob_idx_ = 0;
  uint8_t ar_dst_id_ = 0, ar_rob_req_ = 0, ar_rob_idx_ = 0;

  struct WriteMeta { uint8_t dst_id; uint8_t rob_req; uint8_t rob_idx; };
  std::deque<WriteMeta> w_meta_fifo_;
};

// ---- inline impl ----

inline bool Packetize::push_aw(const axi::AwBeat& b) {
  assert(aw_extras_pending_ && "set_aw_header_extras must be called before push_aw");
  Flit f;
  f.set_header_field("axi_ch",  ni::AXI_CH_AW);
  f.set_header_field("src_id",  src_id_);
  f.set_header_field("dst_id",  aw_dst_id_);
  f.set_header_field("vc_id",   0);
  f.set_header_field("last",    1);
  f.set_header_field("rob_req", aw_rob_req_);
  f.set_header_field("rob_idx", aw_rob_idx_);
  f.set_payload_field("AW", "awid",     b.id);
  f.set_payload_field("AW", "awaddr",   b.addr);
  f.set_payload_field("AW", "awlen",    b.len);
  f.set_payload_field("AW", "awsize",   b.size);
  f.set_payload_field("AW", "awburst",  static_cast<uint64_t>(b.burst));
  f.set_payload_field("AW", "awcache",  b.cache);
  f.set_payload_field("AW", "awlock",   b.lock);
  f.set_payload_field("AW", "awprot",   b.prot);
  f.set_payload_field("AW", "awregion", b.region);
  f.set_payload_field("AW", "awuser",   b.user);
  if (!req_out_.push_flit(f)) return false;
  w_meta_fifo_.push_back({aw_dst_id_, aw_rob_req_, aw_rob_idx_});
  aw_extras_pending_ = false;
  return true;
}

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

inline bool Packetize::push_ar(const axi::ArBeat& b) {
  assert(ar_extras_pending_ && "set_ar_header_extras must be called before push_ar");
  Flit f;
  f.set_header_field("axi_ch",  ni::AXI_CH_AR);
  f.set_header_field("src_id",  src_id_);
  f.set_header_field("dst_id",  ar_dst_id_);
  f.set_header_field("vc_id",   0);
  f.set_header_field("last",    1);
  f.set_header_field("rob_req", ar_rob_req_);
  f.set_header_field("rob_idx", ar_rob_idx_);
  f.set_payload_field("AR", "arid",     b.id);
  f.set_payload_field("AR", "araddr",   b.addr);
  f.set_payload_field("AR", "arlen",    b.len);
  f.set_payload_field("AR", "arsize",   b.size);
  f.set_payload_field("AR", "arburst",  static_cast<uint64_t>(b.burst));
  f.set_payload_field("AR", "arcache",  b.cache);
  f.set_payload_field("AR", "arlock",   b.lock);
  f.set_payload_field("AR", "arprot",   b.prot);
  f.set_payload_field("AR", "arregion", b.region);
  f.set_payload_field("AR", "aruser",   b.user);
  if (!req_out_.push_flit(f)) return false;
  ar_extras_pending_ = false;
  return true;
}

}  // namespace ni::cmodel::nmu
