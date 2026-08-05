// NmuWrap — Wrap for the Nmu component, three physical networks (S3a T5).
//
// Owns an NmuStandalone (hermetic wrapper). The Nmu is the most complex
// wrap — it has an AXI slave side (incoming AW/W/AR, outgoing B/R) AND three
// NoC faces: REQ egress (ready/valid), RSP ingress (ready/valid), DAT
// ingress+egress (credit, unchanged flow control). Each tick follows the
// 3-step pattern:
//   set_inputs(in)   -> latch NmuInputs
//   tick()           -> inject RSP/DAT rsp flits (if valid) into NmuStandalone,
//                      set REQ/DAT ready/credit state, push AW/W/AR beats (if
//                      valid) into axi_slave_port(), advance nmu_.tick(),
//                      drain B/R + REQ/DAT req flits into out_
//   get_outputs(out) -> copy output latch to caller
//
// REQ/RSP ready/valid (spec §4.3, stage design §5.3 "minimal path"):
//   REQ egress: NmuStandalone::req_credit_avail(vc) is the SAME predicate
//     name as before, now backed by a live ready flag instead of a credit
//     pool (queue_req_out_.enable_ready_track()/set_ready()). VcArbiter's
//     existing credit-gated drain needs no structural change. Because the
//     push (inside nmu_->tick()) and the pop (this same tick, Step 3) happen
//     within one wrap tick, a successful push IS the transfer -- no held-
//     latch pattern is needed on this side (unlike the AXI4 channels below).
//   RSP ingress: the c_model's ingress queue is unbounded (Depacketize
//     always drains what's pushed), so rx_rsp_ready is tied constant true --
//     an honest simplification given today's ingress modeling, not a
//     shortcut specific to this stage (DAT's ingress already behaves this
//     way, just without needing a ready wire since it's credit-based).
//
// DAT face (credit, unchanged mechanism, wired to real DPI in T5): mirrors
//   the pre-T5 REQ/RSP credit pattern exactly, just under the
//   tx_dat_*/rx_dat_* names and NmuStandalone's dat_* accessors. Packetize
//   steers Data-class AW/W here (T6), so this face carries real traffic in
//   co-sim.
//
// Wire interception:
//   AXI slave side:  push_aw/push_w/push_ar API on axi_slave_port(); ready
//                    reported via can_accept_aw/w/ar() after tick.
//   REQ/DAT req side: pop_req_flit()/pop_dat_req_flit() on NmuStandalone
//                    drain flits produced by the Packetize stage.
//   RSP/DAT rsp side: inject_rsp_flit()/inject_dat_rsp_flit() on
//                    NmuStandalone insert flits before tick() so Depacketize
//                    can consume them this cycle.
//
// B/R held-latch pattern (AXI4 §A3.2.1): bvalid / rvalid must not deassert
// until bready / rready is observed. Same pattern as SlaveWrap.
//
// Hermetic invariant: no refs to other Wraps.
#pragma once
#include "axi/types.hpp"
#include "wrap/flit_bytes.hpp"      // FlitBytes, FLIT_BYTES
#include "wrap/flit_byte_conv.hpp"  // flit_from_bytes, flit_to_bytes
#include "wrap/nmu_wrap_io.hpp"
#include "ni_params.h"  // NOC_ROUTER_VC_DEPTH — DAT sender credit seed
#include "flit.hpp"
#include "nmu/nmu_standalone.hpp"
#include "nmu/sam_yaml.hpp"
#include <array>
#include <memory>
#include <optional>

namespace ni::cmodel::wrap {

class NmuWrap {
  public:
    // init — construct NmuStandalone with the co-sim default NmuConfig.
    // REQ/RSP are fixed single-VC (S1 Q2, spec TXREQREADY/TXRSPREADY are
    // scalar); dat_num_vc is the topology's VC count (mesh_4x4_vc{2,4,8}
    // reinterpret as DAT_NUM_VC per specgen T1 note). queue_depth = one per
    // AXI channel. config_path: topology YAML with an `address_map` block.
    // Null/empty (the default) keeps the legacy co-sim default SAM below so
    // existing unit-test callers are unaffected.
    void init(uint8_t src_id = 0, uint8_t dat_num_vc = 1,
              std::size_t queue_depth = ni::NMU_QUEUE_DEPTH,
              nmu::RobMode rob_mode = nmu::RobMode::Disabled, const char* config_path = nullptr,
              std::size_t b_rob_depth = ni::NMU_ROB_B_DEPTH,
              std::size_t r_rob_depth = ni::NMU_ROB_R_DEPTH,
              std::size_t max_txns_per_id = ni::NMU_MAX_TXNS_PER_ID,
              std::size_t outstanding_depth = ni::NMU_OUTSTANDING_DEPTH) {
        using namespace ni::cmodel::nmu;
        dat_num_vc_ = dat_num_vc;
        NmuConfig cfg{};
        cfg.src_id = src_id;
        if (config_path != nullptr && config_path[0] != '\0') {
            cfg.sam = addr_trans::load_sam_table(config_path);
        } else {
            // Default SAM when no config path: 16x16 uniform, 4 GB/tile. dst =
            // addr[39:32] (matches the retired xy_route decode); local_addr is
            // rebased (addr - tile base), as in every real config.
            cfg.sam = addr_trans::SamTable::uniform(16, 16, 0x100000000ull);
        }
        // REQ/RSP fixed single-VC (S1 Q2); DAT keeps the topology's VC count.
        cfg.num_vc = 1;
        cfg.dat_num_vc = dat_num_vc;
        // rob_mode / the tb's `_rob` suffix controls the R RoB only; B RoB is always on.
        cfg.read_rob_mode = rob_mode;
        cfg.b_rob_depth = b_rob_depth;
        cfg.r_rob_depth = r_rob_depth;
        cfg.max_txns_per_id = max_txns_per_id;
        cfg.outstanding_depth = outstanding_depth;
        cfg.port_params.aw_queue_depth = queue_depth;
        cfg.port_params.w_queue_depth = queue_depth;
        cfg.port_params.ar_queue_depth = queue_depth;
        cfg.port_params.b_queue_depth = queue_depth;
        cfg.port_params.r_queue_depth = queue_depth;
        cfg.port_params.depkt_b_q_depth = ni::NMU_DEPKT_Q_DEPTH;
        cfg.port_params.depkt_r_q_depth = ni::NMU_DEPKT_Q_DEPTH;
        cfg.wormhole_per_input_depth = ni::NMU_ARBITER_FIFO_DEPTH;
        cfg.vc_arbiter_pending_depth = ni::NMU_ARBITER_FIFO_DEPTH;
        nmu_ = std::make_unique<nmu::NmuStandalone>(std::move(cfg));
        // REQ egress: ready/valid, no credit (spec §4.3) — set_inputs feeds
        // the live ready flag from tx_req_ready every tick.
        nmu_->enable_req_ready_track();
        // DAT egress: unchanged credit mechanism, but the immediate downstream
        // is no longer the router directly — it's DatMergeWrap's per-input
        // pending stage (dat_merge_wrap.hpp), which NMU shares with NSU at the
        // DAT LOCAL port (controller ruling, floo_nw_chimney.sv wide-link
        // merge translate). Seed to that stage's own depth
        // (NMU_ARBITER_FIFO_DEPTH), not the router's LOCAL input depth — the
        // merge's own downstream credit pool (sized to NOC_ROUTER_VC_DEPTH)
        // is the one that actually tracks the router's real capacity.
        nmu_->enable_dat_noc_credit(static_cast<std::size_t>(::ni::NMU_ARBITER_FIFO_DEPTH));
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

        // Step 1a: inject RSP/DAT rsp flits (if valid) BEFORE nmu_.tick() so
        // the Depacketize stage can process them this cycle.
        if (in_.rx_rsp_valid) {
            nmu_->inject_rsp_flit(flit_from_bytes(in_.rx_rsp_flit));
        }
        if (in_.rx_dat_valid) {
            nmu_->inject_dat_rsp_flit(flit_from_bytes(in_.rx_dat_flit));
        }

        // REQ egress ready — live signal from the router, sampled BEFORE
        // tick() so this cycle's VcArbiter drain sees it (credit_avail
        // self-gates). DAT egress credit — same pre-tick replenish pattern
        // as before, now via the dat_* accessor.
        nmu_->req_set_ready(in_.tx_req_ready);
        for (uint8_t vc = 0; vc < dat_num_vc_; ++vc) {
            if (in_.tx_dat_crdvalid[vc]) nmu_->dat_req_receive_credit(vc);
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

        // wait_valid / context-gated ready policy:
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

        // REQ egress: pop one req flit produced by Packetize this cycle. The
        // push into the terminal queue (inside nmu_->tick() above) already
        // happened only when tx_req_ready was true this cycle, so a
        // successful pop here IS this cycle's transfer.
        if (auto f = nmu_->pop_req_flit()) {
            out_.tx_req_valid = true;
            out_.tx_req_flit = flit_to_bytes(*f);
        }
        // DAT egress: unchanged credit-gated pop.
        if (auto f = nmu_->pop_dat_req_flit()) {
            out_.tx_dat_valid = true;
            out_.tx_dat_flit = flit_to_bytes(*f);
        }

        // RSP ingress ready: tied constant true — the c_model's ingress
        // queue is unbounded (Depacketize always drains what's injected).
        out_.rx_rsp_ready = true;

        // DAT ingress credit OUT: unchanged consumer PULSE/VC.
        for (uint8_t vc = 0; vc < dat_num_vc_; ++vc) {
            out_.rx_dat_crdvalid[vc] = nmu_->dat_rsp_take_credit(vc);
        }

        // Save this tick's ready outputs: next tick, valid && prev_ready
        // identifies the wire-handshake cycle.
        prev_awready_ = out_.awready;
        prev_wready_ = out_.wready;
        prev_arready_ = out_.arready;
    }

    void get_outputs(NmuOutputs& out) const { out = out_; }

    // DAT VC count — read by the DPI handlers to size the DAT per-VC credit
    // loops. REQ/RSP are fixed single-VC (no accessor needed).
    uint8_t num_vc() const { return dat_num_vc_; }

    // Fabric-state-dump introspection (read-only by convention).
    nmu::NmuStandalone* standalone() { return nmu_.get(); }
    bool holding_b() const { return held_b_.has_value(); }
    bool holding_r() const { return held_r_.has_value(); }
    uint32_t w_expected() const { return w_expected_; }

  private:
    uint8_t dat_num_vc_ = 1;
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
