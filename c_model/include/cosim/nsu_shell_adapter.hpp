// NsuShellAdapter — Stage 5b ShellAdapter for the Nsu component.
//
// Owns an NsuStandalone (T3 hermetic wrapper). Nsu is the inverse of Nmu:
// it has a NoC consumer side (receives req flits from ChannelModel), a NoC
// producer side (sends rsp flits to ChannelModel), and an AXI master side
// (drives AW/W/AR to a subordinate; consumes B/R from that subordinate).
//
// Each tick follows the 3-step pattern:
//   set_inputs(in)   → latch NsuInputs
//   tick()           → inject NoC req flit (if valid) into NsuStandalone,
//                      advance nsu_.tick(), drain rsp flits + AXI master
//                      AW/W/AR beats into out_
//   get_outputs(out) → copy output latch to caller
//
// Wire interception:
//   NoC req side:  inject_req_flit() on NsuStandalone inserts flits before
//                  tick() so Depacketize can consume them this cycle.
//   NoC rsp side:  pop_rsp_flit() on NsuStandalone drains flits produced by
//                  the Packetize stage (captured in NullNocRspOut queue).
//   AXI master side: AxiMasterPort.pop_aw/pop_w/pop_ar() drains beats that
//                  Depacketize deposited; push_b/push_r() feeds subordinate
//                  responses back to Packetize.
//
// AW/W/AR held-latch pattern (AXI4 §A3.2.1): awvalid/wvalid/arvalid must
// not deassert until awready/wready/arready is observed from the subordinate.
// Pending beats are held in held_aw_/held_w_/held_ar_ until the subordinate
// asserts the corresponding ready.
//
// Hermetic invariant: no refs to other ShellAdapters.
#pragma once
#include "axi/types.hpp"
#include "cosim/channel_model_shell_io.hpp"  // FlitBytes, FLIT_BYTES
#include "cosim/flit_byte_conv.hpp"          // flit_from_bytes, flit_to_bytes
#include "cosim/nsu_shell_io.hpp"
#include "cosim/poc_defaults.hpp"  // kPoC* depths
#include "flit.hpp"
#include "nsu/nsu.hpp"
#include <array>
#include <memory>
#include <optional>

namespace ni::cmodel::cosim {

class NsuShellAdapter {
  public:
    // init — construct NsuStandalone with a minimal PoC NsuConfig.
    // Defaults: 1 VC, ReadWriteSplit, queue_depth = kPoCAxiQueueDepth per channel.
    void init(uint8_t src_id = 0, std::size_t queue_depth = kPoCAxiQueueDepth) {
        using namespace ni::cmodel::nsu;
        NsuConfig cfg{};
        cfg.src_id = src_id;
        cfg.num_vc = 1;
        cfg.vc_mode = VcMode::ReadWriteSplit;
        cfg.write_rsp_vc = 0;
        cfg.read_rsp_vc = 0;
        cfg.port_params.aw_queue_depth = queue_depth;
        cfg.port_params.w_queue_depth = queue_depth;
        cfg.port_params.ar_queue_depth = queue_depth;
        cfg.port_params.b_queue_depth = queue_depth;
        cfg.port_params.r_queue_depth = queue_depth;
        cfg.port_params.depkt_aw_q_depth = queue_depth;
        cfg.port_params.depkt_w_q_depth = queue_depth;
        cfg.port_params.depkt_ar_q_depth = queue_depth;
        cfg.port_params.meta_buffer_per_id_depth = kPoCMetaBufferPerIdDepth;
        cfg.wormhole_per_input_depth = kPoCArbiterFifoDepth;
        cfg.vc_arbiter_pending_depth = kPoCArbiterFifoDepth;
        nsu_ = std::make_unique<nsu::NsuStandalone>(std::move(cfg));
        in_ = NsuInputs{};
        out_ = NsuOutputs{};
        held_aw_ = std::nullopt;
        held_w_ = std::nullopt;
        held_ar_ = std::nullopt;
    }

    void set_inputs(const NsuInputs& in) { in_ = in; }

    void tick() {
        if (!nsu_) return;
        auto& port = nsu_->axi_master_port();

        // Step 1: inject NoC req flit (if valid) BEFORE nsu_.tick() so the
        // Depacketize stage can process it this cycle.
        if (in_.noc_req_valid) {
            nsu_->inject_req_flit(flit_from_bytes(in_.noc_req_flit));
        }

        // Step 2: advance Nsu one cycle (Depacketize + AxiMasterPort +
        // WormholeArbiter + VcArbiter, in upstream-first order per nsu.hpp).
        nsu_->tick();

        // Step 3: build NsuOutputs.
        out_ = NsuOutputs{};

        // AXI master side — drain AW/W/AR beats produced by Depacketize.
        // Held-latch pattern: hold each beat until the subordinate asserts
        // awready/wready/arready (AXI4 §A3.2.1 — master must not deassert
        // valid until ready is seen).

        // AW: consume held beat on awready; try to pop the next.
        if (held_aw_ && in_.awready) {
            held_aw_ = std::nullopt;
        }
        if (!held_aw_) {
            held_aw_ = port.pop_aw();
        }
        if (held_aw_) {
            out_.awvalid = true;
            out_.awid = held_aw_->id;
            out_.awaddr = held_aw_->addr;
            out_.awlen = held_aw_->len;
            out_.awsize = held_aw_->size;
            out_.awburst = static_cast<uint8_t>(held_aw_->burst);
            out_.awlock = held_aw_->lock;
            out_.awcache = held_aw_->cache;
            out_.awprot = held_aw_->prot;
            out_.awqos = held_aw_->qos;
        }

        // W: consume held beat on wready; try to pop the next.
        if (held_w_ && in_.wready) {
            held_w_ = std::nullopt;
        }
        if (!held_w_) {
            held_w_ = port.pop_w();
        }
        if (held_w_) {
            out_.wvalid = true;
            out_.wdata = held_w_->data;
            out_.wstrb = held_w_->strb;
            out_.wlast = held_w_->last;
        }

        // AR: consume held beat on arready; try to pop the next.
        if (held_ar_ && in_.arready) {
            held_ar_ = std::nullopt;
        }
        if (!held_ar_) {
            held_ar_ = port.pop_ar();
        }
        if (held_ar_) {
            out_.arvalid = true;
            out_.arid = held_ar_->id;
            out_.araddr = held_ar_->addr;
            out_.arlen = held_ar_->len;
            out_.arsize = held_ar_->size;
            out_.arburst = static_cast<uint8_t>(held_ar_->burst);
            out_.arlock = held_ar_->lock;
            out_.arcache = held_ar_->cache;
            out_.arprot = held_ar_->prot;
            out_.arqos = held_ar_->qos;
        }

        // B/R: push subordinate responses into AxiMasterPort so Packetize
        // can pick them up next cycle. bready/rready: Nsu is always ready to
        // accept subordinate responses (PoC — master port queue depth=16).
        if (in_.bvalid) {
            axi::BBeat b{};
            b.id = in_.bid;
            b.resp = static_cast<axi::Resp>(in_.bresp & 0x3u);
            b.user = 0;
            port.push_b(b);
        }
        if (in_.rvalid) {
            axi::RBeat r{};
            r.id = in_.rid;
            r.data = in_.rdata;
            r.resp = static_cast<axi::Resp>(in_.rresp & 0x3u);
            r.last = in_.rlast;
            r.user = 0;
            port.push_r(r);
        }

        // bready/rready: Nsu is always ready to accept from subordinate
        // (queue depth=16 >> PoC traffic; real backpressure would use can_accept_b/r).
        out_.bready = port.can_accept_b();
        out_.rready = port.can_accept_r();

        // NoC rsp side: pop one rsp flit produced by Packetize this cycle.
        if (auto f = nsu_->pop_rsp_flit()) {
            out_.noc_rsp_valid = true;
            out_.noc_rsp_flit = flit_to_bytes(*f);
        }

        // NoC req credit: PoC always 0 (NsuStandalone owns its own req queue;
        // no upstream credit signalling needed for the PoC single-shell setup).
        out_.noc_req_credit_return = false;
    }

    void get_outputs(NsuOutputs& out) const { out = out_; }

  private:
    std::unique_ptr<nsu::NsuStandalone> nsu_;
    NsuInputs in_{};
    NsuOutputs out_{};
    std::optional<axi::AwBeat> held_aw_;
    std::optional<axi::WBeat> held_w_;
    std::optional<axi::ArBeat> held_ar_;

    // Flit <-> FlitBytes helpers live in cosim/flit_byte_conv.hpp; calls use
    // flit_from_bytes(...) / flit_to_bytes(...) directly via ADL.
};

}  // namespace ni::cmodel::cosim
