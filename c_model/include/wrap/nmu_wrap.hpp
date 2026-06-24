// NmuWrap — Stage 5b Wrap for the Nmu component.
//
// Owns an NmuStandalone (T3 hermetic wrapper). The Nmu is the most complex
// wrap — it has BOTH an AXI slave side (incoming AW/W/AR, outgoing B/R)
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
// until bready / rready is observed. Same pattern as SlaveWrap (T9).
//
// Hermetic invariant: no refs to other Wraps.
#pragma once
#include "axi/types.hpp"
#include "wrap/channel_model_wrap_io.hpp"  // FlitBytes, FLIT_BYTES
#include "wrap/flit_byte_conv.hpp"         // flit_from_bytes, flit_to_bytes
#include "wrap/nmu_wrap_io.hpp"
#include "wrap/poc_defaults.hpp"  // kPoC* depths (kPoCChannelModelDepth kept for ChannelModel stub)
#include "ni_params.h"             // NOC_ROUTER_VC_DEPTH — LOCAL sender credit seed
#include "flit.hpp"
#include "nmu/nmu_standalone.hpp"
#include <array>
#include <memory>
#include <optional>

namespace ni::cmodel::wrap {

class NmuWrap {
  public:
    // init — construct NmuStandalone with a minimal PoC NmuConfig.
    // ReadWriteSplit, queue_depth = kPoCAxiQueueDepth per channel. num_vc comes
    // from the create param (cmodel_nmu_create): write packets on write_vc=0,
    // read packets on read_vc=(num_vc>=2)?1:0 — Mode A, mirrors the MultiVc test.
    void init(uint8_t src_id = 0, uint8_t num_vc = 1, std::size_t queue_depth = kPoCAxiQueueDepth) {
        using namespace ni::cmodel::nmu;
        num_vc_ = num_vc;
        NmuConfig cfg{};
        cfg.src_id = src_id;
        cfg.num_vc = num_vc;
        cfg.vc_mode = VcMode::ReadWriteSplit;
        cfg.write_vc = 0;
        cfg.read_vc = (num_vc >= 2) ? 1u : 0u;
        cfg.port_params.aw_queue_depth = queue_depth;
        cfg.port_params.w_queue_depth = queue_depth;
        cfg.port_params.ar_queue_depth = queue_depth;
        cfg.port_params.b_queue_depth = queue_depth;
        cfg.port_params.r_queue_depth = queue_depth;
        cfg.port_params.depkt_b_q_depth = queue_depth;
        cfg.port_params.depkt_r_q_depth = queue_depth;
        cfg.wormhole_per_input_depth = kPoCArbiterFifoDepth;
        cfg.vc_arbiter_pending_depth = kPoCArbiterFifoDepth;
        nmu_ = std::make_unique<nmu::NmuStandalone>(std::move(cfg));
        // R2: close the NI-edge credit loop. Seed the req-out sender counter to
        // the router LOCAL input VC FIFO depth (NOC_ROUTER_VC_DEPTH from
        // constants.yaml) — the single source of truth that also seeds the
        // router_wrap's LOCAL input buffer and the link_perf_monitor assertion.
        // kPoCChannelModelDepth (64) is reserved for the ChannelModel stub only.
        nmu_->enable_noc_credit(static_cast<std::size_t>(::ni::NOC_ROUTER_VC_DEPTH));
        in_ = NmuInputs{};
        out_ = NmuOutputs{};
        held_b_ = std::nullopt;
        held_r_ = std::nullopt;
        prev_awready_ = false;
        prev_wready_ = false;
        prev_arready_ = false;
        w_expected_ = 0;
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

        // R2: incoming credit pulse — the router's LOCAL input drained an NMU
        // req flit, so replenish the req-out sender counter BEFORE tick() so this
        // cycle's VcArbiter sees the credit (VcArbiter self-gates on credit_avail).
        // Per-VC: replenish each VC that pulsed this cycle.
        for (uint8_t vc = 0; vc < num_vc_; ++vc) {
            if (in_.noc_req_credit_return[vc]) {
                nmu_->req_receive_credit(vc);
            }
        }

        // Step 1b: push AW/W/AR beats into axi_slave_port queues — ONLY on
        // true wire-handshake ticks (valid && our previously driven ready).
        // wait_valid policy: ready is never pre-asserted for the address
        // channels, so first sight of valid is NOT an accept.
        if (in_.awvalid && prev_awready_) {
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
            // Capacity was a condition of asserting ready last tick and only
            // this adapter pushes, so the push cannot fail here.
            (void)port.push_aw(aw);
            // Widen the W window: AWLEN+1 more beats are now owed. The
            // counter accumulates across accepted AWs (multi-outstanding),
            // keeping WREADY pre-asserted until all owed beats arrive.
            w_expected_ += static_cast<uint32_t>(in_.awlen) + 1u;
        }

        if (in_.wvalid && prev_wready_) {
            axi::WBeat w{};
            w.data = in_.wdata;
            w.strb = in_.wstrb;
            w.last = in_.wlast;
            w.user = 0;
            (void)port.push_w(w);
            if (w_expected_ > 0) --w_expected_;
        }

        if (in_.arvalid && prev_arready_) {
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
            (void)port.push_ar(ar);
        }

        // Step 2: advance Nmu one cycle (Depacketize + AxiSlavePort +
        // WormholeArbiter + VcArbiter, in upstream-first order per nmu.hpp).
        nmu_->tick();

        // Step 3: build NmuOutputs.
        out_ = NmuOutputs{};

        // wait_valid / context-gated ready policy (see
        // docs/superpowers/specs/2026-06-12-wait-valid-ready-policy-design.md):
        // - AW/AR (address channels): one-shot wait_valid — ready stays low
        //   until VALID is observed, pulses for exactly one wire cycle (the
        //   handshake completes on that cycle), then returns low. AW is NOT
        //   gated on W-burst completion: multi-outstanding AW (post several
        //   AWs, then stream the data) is legitimate AXI4 and load-bearing
        //   for the RoB/multi-ID paths.
        // - W (follow-on channel): wready pre-asserts on buffer capacity
        //   WITHOUT waiting for WVALID while any accepted AW still has W
        //   beats owed (w_expected_ accumulates across accepted bursts),
        //   and drops once all owed beats arrived.
        out_.awready = in_.awvalid && !prev_awready_ && port.can_accept_aw();
        out_.wready = (w_expected_ > 0) && port.can_accept_w();
        out_.arready = in_.arvalid && !prev_arready_ && port.can_accept_ar();

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

        // NoC rsp credit OUT: consumer PULSE/VC — the NMU's Depacketize consumed an
        // injected rsp flit this tick, so return one credit upstream (to the
        // router's LOCAL output sender counter). Drains one pending pulse/VC/tick.
        for (uint8_t vc = 0; vc < num_vc_; ++vc) {
            out_.noc_rsp_credit_return[vc] = nmu_->rsp_take_credit(vc);
        }

        // Save this tick's ready outputs: next tick, valid && prev_ready
        // identifies the wire-handshake cycle.
        prev_awready_ = out_.awready;
        prev_wready_ = out_.wready;
        prev_arready_ = out_.arready;
    }

    void get_outputs(NmuOutputs& out) const { out = out_; }

    // VC count — read by the DPI handlers to size the per-VC credit loops.
    uint8_t num_vc() const { return num_vc_; }

  private:
    uint8_t num_vc_ = 1;
    std::unique_ptr<nmu::NmuStandalone> nmu_;
    NmuInputs in_{};
    NmuOutputs out_{};
    std::optional<axi::BBeat> held_b_;
    std::optional<axi::RBeat> held_r_;
    bool prev_awready_ = false;  // ready driven last tick (wire value this tick)
    bool prev_wready_ = false;
    bool prev_arready_ = false;
    uint32_t w_expected_ = 0;  // W beats remaining of the open burst window

    // Flit <-> FlitBytes helpers live in wrap/flit_byte_conv.hpp; calls use
    // flit_from_bytes(...) / flit_to_bytes(...) directly via ADL.
};

}  // namespace ni::cmodel::wrap
