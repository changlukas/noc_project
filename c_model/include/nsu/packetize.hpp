#pragma once
// NSU-side response packetizer. Implements the S1 stage register (accepts
// B/R beats from AxiMasterPort) and the S2 transform (builds Flit from S1,
// pushes into the S2→S3 stage register = WormholeArbiter pending queue).
//
// Stage model (spec §5.0, §5.1, §5.2):
//   S1: push_b/r() accepts ≤1 beat/channel into s1_b_/s1_r_ stage registers.
//       Returns false (backpressure) when the S1 register is occupied.
//   S2: tick() reads S1, builds Flit (MetaBuffer peek), pushes to b_out_/r_out_
//       (= WormholeArbiter input = the S2→S3 stage register boundary).
//       MetaBuffer commit_* fires on successful push to the S2→S3 boundary
//       (spec §5.1: "commit moves to the stage that pushes into the S2→S3
//       register").
//   Arbiter-final-stage (spec §5.2): Packetize::tick() runs after
//   WormholeArbiter::tick() in the reverse-order tick sequence (Nsu::tick()),
//   so a flit written into the arbiter's pending queue in this tick cannot
//   escape to NoC until the next tick — no same-tick Packetize→NoC path.
//
// Implements ResponsePacketizer (B/R only; NSU never emits requests).
#include "axi/types.hpp"
#include "flit.hpp"
#include "router/rsp_out.hpp"
#include "router/pipeline_stage.hpp"
#include "nsu/meta_buffer.hpp"
#include "response_io.hpp"
#include <cassert>
#include <cstdint>
#include <cstdlib>

namespace ni::cmodel::nsu {

class Packetize : public ResponsePacketizer {
  public:
    Packetize(router::NocRspOut& b_out, router::NocRspOut& r_out, MetaBuffer& meta, uint8_t src_id)
        : b_out_(b_out), r_out_(r_out), meta_(meta), src_id_(src_id) {}

    // ---- ResponsePacketizer interface (S1 accept) ----
    // Accepts ≤1 beat/channel into the S1 stage register.
    // Returns false when the S1 register is full (backpressure to AxiMasterPort).
    bool push_b(const axi::BBeat& b) override;
    bool push_r(const axi::RBeat& b) override;

    // ---- S2 stage transform ----
    // Reads occupied S1 registers, builds Flit, pushes to WormholeArbiter
    // input (S2→S3 boundary). Called by Nsu::tick() AFTER WormholeArbiter::tick()
    // in reverse-order sequence, establishing the arbiter-final-stage property.
    void tick();

    // ---- Introspection ----
    std::size_t s1_b_occupancy() const noexcept { return s1_b_.occupancy(); }
    std::size_t s1_r_occupancy() const noexcept { return s1_r_.occupancy(); }

  private:
    router::NocRspOut& b_out_;
    router::NocRspOut& r_out_;
    MetaBuffer& meta_;
    uint8_t src_id_;

    // S1 stage registers: one per response channel. push_b/r() fills them;
    // tick() (S2) drains and transforms into Flits toward the arbiter.
    router::PipelineStage<axi::BBeat> s1_b_;
    router::PipelineStage<axi::RBeat> s1_r_;

    static Flit build_b_flit(const axi::BBeat& b, const MetaEntry& m, uint8_t src_id);
    static Flit build_r_flit(const axi::RBeat& b, const MetaEntry& m, uint8_t src_id);
};

// S1 accept: write into stage register (backpressure if full).
inline bool Packetize::push_b(const axi::BBeat& b) {
    if (s1_b_.full()) return false;
    s1_b_.accept(b);
    return true;
}

inline bool Packetize::push_r(const axi::RBeat& b) {
    if (s1_r_.full()) return false;
    s1_r_.accept(b);
    return true;
}

inline Flit Packetize::build_b_flit(const axi::BBeat& b, const MetaEntry& m, uint8_t src_id) {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_B);
    f.set_header_field("src_id", src_id);
    f.set_header_field("dst_id", m.src_id);
    f.set_header_field("vc_id", 0);
    f.set_header_field("last", 1);
    f.set_header_field("rob_req", m.rob_req);
    f.set_header_field("rob_idx", m.rob_idx);
    f.set_payload_field("B", "bid", b.id);
    f.set_payload_field("B", "bresp", static_cast<uint64_t>(b.resp));
    f.set_payload_field("B", "buser", b.user);
    return f;
}

inline Flit Packetize::build_r_flit(const axi::RBeat& b, const MetaEntry& m, uint8_t src_id) {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_R);
    f.set_header_field("src_id", src_id);
    f.set_header_field("dst_id", m.src_id);
    f.set_header_field("vc_id", 0);
    f.set_header_field("last", 1);
    f.set_header_field("rob_req", m.rob_req);
    f.set_header_field("rob_idx", m.rob_idx);
    f.set_payload_field("R", "rid", b.id);
    f.set_payload_field("R", "rresp", static_cast<uint64_t>(b.resp));
    f.set_payload_field("R", "ruser", b.user);
    f.set_payload_field("R", "rlast", b.last ? 1u : 0u);
    f.set_payload_bytes("R", "rdata", b.data.data(), axi::NOC_DATA_WIDTH_BITS);
    return f;
}

// S2 stage transform: ≤1 B and ≤1 R per tick.
// MetaBuffer assert: a B/R beat in S1 must have a matching MetaBuffer entry
// (snapshot was done at AW/AR ingress). Absence is a pipeline protocol
// violation (beat injected without a prior AW/AR flit), not recoverable.
inline void Packetize::tick() {
    // B channel: read S1, build flit, push to S2→S3 boundary.
    if (s1_b_.full()) {
        const axi::BBeat& b = s1_b_.peek();
        auto meta_opt = meta_.peek_write(b.id);
        if (!meta_opt.has_value()) {
            assert(false && "Packetize::tick: B in S1 with no matching AW MetaBuffer entry");
            std::abort();
        }
        Flit f = build_b_flit(b, *meta_opt, src_id_);
        if (b_out_.push_flit(f)) {
            s1_b_.take();
            meta_.commit_write(b.id);  // commit on successful S2→S3 push (spec §5.1)
        }
        // On push failure: beat stays in S1 register; arbiter will drain
        // its own pending queue next tick, freeing space.
    }

    // R channel: read S1, build flit, push to S2→S3 boundary.
    if (s1_r_.full()) {
        const axi::RBeat& b = s1_r_.peek();
        auto meta_opt = meta_.peek_read(b.id);
        if (!meta_opt.has_value()) {
            assert(false && "Packetize::tick: R in S1 with no matching AR MetaBuffer entry");
            std::abort();
        }
        Flit f = build_r_flit(b, *meta_opt, src_id_);
        if (r_out_.push_flit(f)) {
            s1_r_.take();
            if (b.last) meta_.commit_read(b.id);  // commit on rlast only (spec §5.1)
        }
    }
}

}  // namespace ni::cmodel::nsu
