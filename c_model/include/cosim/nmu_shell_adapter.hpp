// NmuShellAdapter — Stage 5b ShellAdapter for the Nmu component.
//
// Owns an NmuStandalone (T3 hermetic wrapper). The Nmu is the most complex
// shell — it has BOTH an AXI slave side (incoming AW/W/AR, outgoing B/R)
// AND NoC sides (req_out producer toward ChannelModel, rsp_in consumer from
// ChannelModel). Each tick follows the 3-step pattern:
//   set_inputs(in)   → latch NmuInputs
//   tick()           → inject NoC rsp flit (if valid) into NmuStandalone,
//                      push AW/W/AR beats (if valid) into axi_slave_port(),
//                      advance nmu_.tick(), drain B/R + NoC req flits into out_
//   get_outputs(out) → copy output latch to caller
//
// Wire interception:
//   AXI slave side:  push_aw/push_w/push_ar API on axi_slave_port(); ready
//                    reported via can_accept_aw/w/ar() after tick.
//   NoC req side:    pop_req_flit() on NmuStandalone drains flits produced by
//                    the Packetize stage (captured in NullNocReqOut queue).
//   NoC rsp side:    inject_rsp_flit() on NmuStandalone inserts flits before
//                    tick() so Depacketize can consume them this cycle.
//
// B/R held-latch pattern (AXI4 §A3.2.1): bvalid / rvalid must not deassert
// until bready / rready is observed. Same pattern as SlaveShellAdapter (T9).
//
// Hermetic invariant: no refs to other ShellAdapters.
#pragma once
#include "axi/types.hpp"
#include "cosim/channel_model_shell_io.hpp"  // FlitBytes, FLIT_BYTES
#include "cosim/flit_byte_conv.hpp"          // flit_from_bytes, flit_to_bytes
#include "cosim/nmu_shell_io.hpp"
#include "cosim/poc_defaults.hpp"  // kPoC* depths
#include "flit.hpp"
#include "nmu/nmu.hpp"
#include <array>
#include <memory>
#include <optional>

namespace ni::cmodel::cosim {

class NmuShellAdapter {
  public:
    // init — construct NmuStandalone with a minimal PoC NmuConfig.
    // Defaults: 1 VC, ReadWriteSplit, queue_depth = kPoCAxiQueueDepth per channel.
    void init(uint8_t src_id = 0, std::size_t queue_depth = kPoCAxiQueueDepth) {
        using namespace ni::cmodel::nmu;
        NmuConfig cfg{};
        cfg.src_id = src_id;
        cfg.num_vc = 1;
        cfg.vc_mode = VcMode::ReadWriteSplit;
        cfg.write_vc = 0;
        cfg.read_vc = 0;
        cfg.port_params.aw_queue_depth = queue_depth;
        cfg.port_params.w_queue_depth = queue_depth;
        cfg.port_params.ar_queue_depth = queue_depth;
        cfg.port_params.b_queue_depth = queue_depth;
        cfg.port_params.r_queue_depth = queue_depth;
        cfg.port_params.depkt_aw_q_depth = queue_depth;
        cfg.port_params.depkt_w_q_depth = queue_depth;
        cfg.port_params.depkt_ar_q_depth = queue_depth;
        cfg.port_params.depkt_b_q_depth = queue_depth;
        cfg.port_params.depkt_r_q_depth = queue_depth;
        cfg.port_params.channel_model_req_depth = kPoCChannelModelDepth;
        cfg.port_params.channel_model_rsp_depth = kPoCChannelModelDepth;
        cfg.port_params.meta_buffer_per_id_depth = kPoCMetaBufferPerIdDepth;
        cfg.depkt_b_q_depth = queue_depth;
        cfg.depkt_r_q_depth = queue_depth;
        cfg.wormhole_per_input_depth = kPoCArbiterFifoDepth;
        cfg.vc_arbiter_pending_depth = kPoCArbiterFifoDepth;
        nmu_ = std::make_unique<nmu::NmuStandalone>(std::move(cfg));
        in_ = NmuInputs{};
        out_ = NmuOutputs{};
        held_b_ = std::nullopt;
        held_r_ = std::nullopt;
    }

    void set_inputs(const NmuInputs& in) { in_ = in; }

    void tick() {
        if (!nmu_) return;
        auto& port = nmu_->axi_slave_port();

        // Step 1a: inject NoC rsp flit (if valid) BEFORE nmu_.tick() so the
        // Depacketize stage can process it this cycle.
        if (in_.noc_rsp_valid) {
            nmu_->inject_rsp_flit(flit_from_bytes(in_.noc_rsp_flit));
        }

        // Step 1b: push AW/W/AR beats from wire into axi_slave_port queues.
        // push_* returns false if the queue is full (backpressure this cycle).
        bool aw_accepted = false;
        if (in_.awvalid) {
            axi::AwBeat aw{};
            aw.id = in_.awid;
            aw.addr = in_.awaddr;
            aw.len = in_.awlen;
            aw.size = in_.awsize;
            aw.burst = static_cast<axi::Burst>(in_.awburst);
            aw.lock = in_.awlock;
            aw.cache = in_.awcache;
            aw.prot = in_.awprot;
            aw.qos = in_.awqos;
            aw.user = 0;
            aw_accepted = port.push_aw(aw);
        }

        bool w_accepted = false;
        if (in_.wvalid) {
            axi::WBeat w{};
            w.data = in_.wdata;
            w.strb = in_.wstrb;
            w.last = in_.wlast;
            w.user = 0;
            w_accepted = port.push_w(w);
        }

        bool ar_accepted = false;
        if (in_.arvalid) {
            axi::ArBeat ar{};
            ar.id = in_.arid;
            ar.addr = in_.araddr;
            ar.len = in_.arlen;
            ar.size = in_.arsize;
            ar.burst = static_cast<axi::Burst>(in_.arburst);
            ar.lock = in_.arlock;
            ar.cache = in_.arcache;
            ar.prot = in_.arprot;
            ar.qos = in_.arqos;
            ar.user = 0;
            ar_accepted = port.push_ar(ar);
        }

        // Step 2: advance Nmu one cycle (Depacketize + AxiSlavePort +
        // WormholeArbiter + VcArbiter, in upstream-first order per nmu.hpp).
        nmu_->tick();

        // Step 3: build NmuOutputs.
        out_ = NmuOutputs{};

        // awready/wready/arready: if master drove valid, report whether accepted;
        // otherwise report queue vacancy (mirrors SlaveShellAdapter T9 pattern).
        if (in_.awvalid) {
            out_.awready = aw_accepted;
        } else {
            out_.awready = port.can_accept_aw();
        }

        if (in_.wvalid) {
            out_.wready = w_accepted;
        } else {
            out_.wready = port.can_accept_w();
        }

        if (in_.arvalid) {
            out_.arready = ar_accepted;
        } else {
            out_.arready = port.can_accept_ar();
        }

        // B channel: held-latch pattern (AXI4 §A3.2.1 — bvalid must hold
        // until bready). Consume held beat on bready; try to pop the next.
        if (held_b_ && in_.bready) {
            held_b_ = std::nullopt;
        }
        if (!held_b_) {
            held_b_ = port.pop_b();
        }
        if (held_b_) {
            out_.bvalid = true;
            out_.bid = held_b_->id;
            out_.bresp = static_cast<uint8_t>(held_b_->resp);
        }

        // R channel: same held-latch pattern.
        if (held_r_ && in_.rready) {
            held_r_ = std::nullopt;
        }
        if (!held_r_) {
            held_r_ = port.pop_r();
        }
        if (held_r_) {
            out_.rvalid = true;
            out_.rid = held_r_->id;
            out_.rdata = held_r_->data;
            out_.rresp = static_cast<uint8_t>(held_r_->resp);
            out_.rlast = held_r_->last;
        }

        // NoC req side: pop one req flit produced by Packetize this cycle.
        if (auto f = nmu_->pop_req_flit()) {
            out_.noc_req_valid = true;
            out_.noc_req_flit = flit_to_bytes(*f);
        }

        // NoC rsp credit: PoC always 0 (NmuStandalone owns its own rsp queue;
        // no upstream credit signalling needed for the PoC single-shell setup).
        out_.noc_rsp_credit_return = false;
    }

    void get_outputs(NmuOutputs& out) const { out = out_; }

  private:
    std::unique_ptr<nmu::NmuStandalone> nmu_;
    NmuInputs in_{};
    NmuOutputs out_{};
    std::optional<axi::BBeat> held_b_;
    std::optional<axi::RBeat> held_r_;

    // Flit <-> FlitBytes helpers live in cosim/flit_byte_conv.hpp; calls use
    // flit_from_bytes(...) / flit_to_bytes(...) directly via ADL.
};

}  // namespace ni::cmodel::cosim
