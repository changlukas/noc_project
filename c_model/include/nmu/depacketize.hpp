#pragma once
#include "axi/types.hpp"
#include "ni/depacketizer.hpp"         // from Stage 3 port-pair task
#include "ni/flit.hpp"
#include "noc/noc_rsp_in.hpp"
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>
#include <utility>

namespace ni::cmodel::nmu {

// NMU-side response depacketizer. Stateful demux: tick() pulls from
// NocRspIn and routes B/R flits into per-channel deques. Upstream port
// calls pop_b/pop_r to serve from those queues.
//
// Pending-flit stash semantics: if a pulled flit's target queue is full,
// the flit is held in `pending_` and re-attempted next tick. This blocks
// any other flits behind it (head-of-line blocking on single-FIFO ingress).
// Documented in spec §4.4; not a bug.
class Depacketize : public Depacketizer {
public:
  Depacketize(noc::NocRspIn& rsp_in,
              std::size_t b_q_depth, std::size_t r_q_depth)
      : rsp_in_(rsp_in), b_q_depth_(b_q_depth), r_q_depth_(r_q_depth) {}

  void tick();

  // Depacketizer interface — response methods are real
  std::optional<axi::BBeat> pop_b() override;
  std::optional<axi::RBeat> pop_r() override;
  std::optional<std::pair<axi::BBeat, ResponseMeta>> pop_b_with_meta() override;
  std::optional<std::pair<axi::RBeat, ResponseMeta>> pop_r_with_meta() override;
  // Request methods assert false
  std::optional<axi::AwBeat> pop_aw() override {
      assert(false && "nmu::Depacketize::pop_aw: NMU depacketizer handles response side only "
                      "(B/R from NocRspIn) — AW belongs on the request side "
                      "(NSU Depacketize or testbench bypass). Likely cause: "
                      "AxiSlavePort wiring routed a request-channel pop into the NMU "
                      "depacketizer, or test fixture invoked the wrong Depacketizer instance.");
      std::abort();
      return std::nullopt;
  }
  std::optional<axi::WBeat>  pop_w () override {
      assert(false && "nmu::Depacketize::pop_w: NMU depacketizer handles response side only "
                      "(B/R from NocRspIn) — W belongs on the request side "
                      "(NSU Depacketize or testbench bypass). Likely cause: "
                      "AxiSlavePort wiring routed a request-channel pop into the NMU "
                      "depacketizer, or test fixture invoked the wrong Depacketizer instance.");
      std::abort();
      return std::nullopt;
  }
  std::optional<axi::ArBeat> pop_ar() override {
      assert(false && "nmu::Depacketize::pop_ar: NMU depacketizer handles response side only "
                      "(B/R from NocRspIn) — AR belongs on the request side "
                      "(NSU Depacketize or testbench bypass). Likely cause: "
                      "AxiSlavePort wiring routed a request-channel pop into the NMU "
                      "depacketizer, or test fixture invoked the wrong Depacketizer instance.");
      std::abort();
      return std::nullopt;
  }

private:
  struct BWithMeta { axi::BBeat beat; ResponseMeta meta; };
  struct RWithMeta { axi::RBeat beat; ResponseMeta meta; };

  noc::NocRspIn& rsp_in_;
  std::deque<BWithMeta> b_q_;
  std::deque<RWithMeta> r_q_;
  std::size_t b_q_depth_, r_q_depth_;
  std::optional<Flit> pending_;

  static axi::BBeat decode_b(const Flit& f);
  static axi::RBeat decode_r(const Flit& f);
};

inline axi::BBeat Depacketize::decode_b(const Flit& f) {
  axi::BBeat b{};
  b.id    = static_cast<uint8_t>(f.get_payload_field("B", "bid"));
  b.resp  = static_cast<axi::Resp>(f.get_payload_field("B", "bresp"));
  b.user  = static_cast<uint8_t>(f.get_payload_field("B", "buser"));
  return b;
}

inline axi::RBeat Depacketize::decode_r(const Flit& f) {
  axi::RBeat r{};
  r.id    = static_cast<uint8_t>(f.get_payload_field("R", "rid"));
  r.resp  = static_cast<axi::Resp>(f.get_payload_field("R", "rresp"));
  r.user  = static_cast<uint8_t>(f.get_payload_field("R", "ruser"));
  r.last  = f.get_payload_field("R", "rlast") != 0;
  f.get_payload_bytes("R", "rdata", r.data.data(), 256);
  return r;
}

inline void Depacketize::tick() {
  while (true) {
    Flit f;
    if (pending_) {
      f = *pending_;
    } else {
      auto opt = rsp_in_.pop_flit();
      if (!opt) return;
      f = *opt;
    }
    uint64_t ch = f.get_header_field("axi_ch");
    switch (ch) {
      case ni::AXI_CH_B: {
        if (b_q_.size() >= b_q_depth_) { pending_ = f; return; }
        ResponseMeta meta{
            static_cast<uint8_t>(f.get_header_field("rob_idx")),
            static_cast<uint8_t>(f.get_header_field("rob_req"))
        };
        b_q_.push_back({decode_b(f), meta});
        break;
      }
      case ni::AXI_CH_R: {
        if (r_q_.size() >= r_q_depth_) { pending_ = f; return; }
        ResponseMeta meta{
            static_cast<uint8_t>(f.get_header_field("rob_idx")),
            static_cast<uint8_t>(f.get_header_field("rob_req"))
        };
        r_q_.push_back({decode_r(f), meta});
        break;
      }
      default:
        assert(false && "nmu::Depacketize::tick: NocRspIn delivered flit with axi_ch outside "
                        "{B, R} — NMU response path only accepts response channels. Likely cause: "
                        "NSU packetizer stamped wrong axi_ch into a response flit, NoC fabric "
                        "misrouted a request flit into the response ingress, or codegen drift "
                        "changed ni::AXI_CH_* encoding without rebuilding both sides.");
        std::abort();
    }
    pending_.reset();
  }
}

inline std::optional<axi::BBeat> Depacketize::pop_b() {
  if (b_q_.empty()) return std::nullopt;
  auto entry = b_q_.front();
  b_q_.pop_front();
  return entry.beat;
}

inline std::optional<axi::RBeat> Depacketize::pop_r() {
  if (r_q_.empty()) return std::nullopt;
  auto entry = r_q_.front();
  r_q_.pop_front();
  return entry.beat;
}

inline std::optional<std::pair<axi::BBeat, ResponseMeta>>
Depacketize::pop_b_with_meta() {
  if (b_q_.empty()) return std::nullopt;
  auto entry = b_q_.front();
  b_q_.pop_front();
  return std::make_pair(entry.beat, entry.meta);
}

inline std::optional<std::pair<axi::RBeat, ResponseMeta>>
Depacketize::pop_r_with_meta() {
  if (r_q_.empty()) return std::nullopt;
  auto entry = r_q_.front();
  r_q_.pop_front();
  return std::make_pair(entry.beat, entry.meta);
}

}  // namespace ni::cmodel::nmu
