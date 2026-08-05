// NsuWrap — Wrap for the Nsu component, three physical networks (S3a T5).
//
// Owns an NsuStandalone (hermetic wrapper). Nsu is the inverse of Nmu:
// it has three NoC faces (REQ ingress ready/valid, RSP egress ready/valid,
// DAT ingress+egress credit) and an AXI master side (drives AW/W/AR to a
// slave; consumes B/R from that slave).
//
// Each tick follows the 3-step pattern:
//   set_inputs(in)   -> latch NsuInputs
//   tick()           -> inject REQ/DAT req flits (if valid) into NsuStandalone,
//                      set RSP/DAT ready/credit state, advance nsu_.tick(),
//                      drain rsp flits + AXI master AW/W/AR beats into out_
//   get_outputs(out) -> copy output latch to caller
//
// REQ/RSP ready/valid (spec §4.3, stage design §5.3 "minimal path"): mirrors
// nmu_wrap.hpp's REQ face exactly, applied to Nsu's RSP egress:
// NsuStandalone::rsp_credit_avail(vc) is the SAME predicate name as before,
// now backed by a live ready flag instead of a credit pool. REQ ingress is
// tied to rx_req_ready = constant true, same "unbounded ingress queue"
// simplification as Nmu's RSP ingress.
//
// DAT face (credit, unchanged mechanism, wired to real DPI in T5): mirrors
// the pre-T5 REQ/RSP credit pattern exactly, just under the tx_dat_*/rx_dat_*
// names and NsuStandalone's dat_* accessors. Packetize steers Data-class R
// here (T6), so this face carries real traffic in co-sim.
//
// Wire interception:
//   REQ/DAT req side: inject_req_flit()/inject_dat_req_flit() on
//                  NsuStandalone insert flits before tick() so Depacketize
//                  can consume them this cycle.
//   RSP/DAT rsp side: pop_rsp_flit()/pop_dat_rsp_flit() on NsuStandalone
//                  drain flits produced by the Packetize stage.
//   AXI master side: AxiMasterPort.pop_aw/pop_w/pop_ar() drains beats that
//                  Depacketize deposited; push_b/push_r() feeds slave
//                  responses back to Packetize.
//
// AW/W/AR held-latch pattern (AXI4 §A3.2.1): awvalid/wvalid/arvalid must
// not deassert until awready/wready/arready is observed from the slave.
// Pending beats are held in held_aw_/held_w_/held_ar_ until the slave
// asserts the corresponding ready.
//
// Hermetic invariant: no refs to other Wraps.
#pragma once
#include "axi/types.hpp"
#include "wrap/flit_bytes.hpp"      // FlitBytes, FLIT_BYTES
#include "wrap/flit_byte_conv.hpp"  // flit_from_bytes, flit_to_bytes
#include "wrap/nsu_wrap_io.hpp"
#include "ni_params.h"  // NOC_ROUTER_VC_DEPTH — DAT sender credit seed
#include "flit.hpp"
#include "nsu/nsu_standalone.hpp"
#include <array>
#include <memory>
#include <optional>

namespace ni::cmodel::wrap {

class NsuWrap {
  public:
    // init — construct NsuStandalone with the co-sim default NsuConfig.
    // REQ/RSP are fixed single-VC (S1 Q2, spec TXREQREADY/TXRSPREADY are
    // scalar); dat_num_vc is the topology's VC count (mesh_4x4_vc{2,4,8}
    // reinterpret as DAT_NUM_VC per specgen T1 note). queue_depth = one per
    // AXI channel.
    void init(uint8_t src_id = 0, uint8_t dat_num_vc = 1,
              std::size_t queue_depth = ni::NSU_QUEUE_DEPTH,
              std::size_t max_unique_ids = ni::NSU_META_BUFFER_MAX_UNIQUE_IDS,
              std::size_t max_outstanding = ni::NSU_META_BUFFER_MAX_OUTSTANDING) {
        using namespace ni::cmodel::nsu;
        dat_num_vc_ = dat_num_vc;
        NsuConfig cfg{};
        cfg.src_id = src_id;
        // REQ/RSP fixed single-VC (S1 Q2); DAT keeps the topology's VC count.
        cfg.num_vc = 1;
        cfg.dat_num_vc = dat_num_vc;
        cfg.port_params.aw_queue_depth = queue_depth;
        cfg.port_params.w_queue_depth = queue_depth;
        cfg.port_params.ar_queue_depth = queue_depth;
        cfg.port_params.b_queue_depth = queue_depth;
        cfg.port_params.r_queue_depth = queue_depth;
        cfg.port_params.meta_buffer_max_outstanding = max_outstanding;
        cfg.port_params.meta_buffer_max_unique_ids = max_unique_ids;
        cfg.wormhole_per_input_depth = ni::NSU_ARBITER_FIFO_DEPTH;
        cfg.vc_allocator_pending_depth = ni::NSU_ARBITER_FIFO_DEPTH;
        nsu_ = std::make_unique<nsu::NsuStandalone>(std::move(cfg));
        // RSP egress: ready/valid, no credit (spec §4.3) — set_inputs feeds
        // the live ready flag from tx_rsp_ready every tick.
        nsu_->enable_rsp_ready_track();
        // DAT egress: unchanged credit mechanism, but the immediate downstream
        // is no longer the router directly — it's DatMergeWrap's per-input
        // pending stage (dat_merge_wrap.hpp), which NSU shares with NMU at the
        // DAT LOCAL port (controller ruling, floo_nw_chimney.sv wide-link
        // merge translate). Seed to that stage's own depth
        // (NSU_ARBITER_FIFO_DEPTH), not the router's LOCAL input depth — the
        // merge's own downstream credit pool (sized to NOC_ROUTER_VC_DEPTH)
        // is the one that actually tracks the router's real capacity.
        nsu_->enable_dat_noc_credit(static_cast<std::size_t>(::ni::NSU_ARBITER_FIFO_DEPTH));
        in_ = NsuInputs{};
        out_ = NsuOutputs{};
        held_aw_ = std::nullopt;
        held_w_ = std::nullopt;
        held_ar_ = std::nullopt;
        prev_bready_ = false;
        prev_rready_ = false;
        outstanding_w_ = 0;
        expected_r_beats_ = 0;
        w_pop_budget_ = 0;
    }

    void set_inputs(const NsuInputs& in) { in_ = in; }

    void tick() {
        if (!nsu_) return;
        auto& port = nsu_->axi_master_port();

        // Step 1: inject REQ/DAT req flits (if valid) BEFORE nsu_.tick() so
        // the Depacketize stage can process them this cycle.
        if (in_.rx_req_valid) {
            nsu_->inject_req_flit(flit_from_bytes(in_.rx_req_flit));
        }
        if (in_.rx_dat_valid) {
            nsu_->inject_dat_req_flit(flit_from_bytes(in_.rx_dat_flit));
        }

        // RSP egress ready — live signal from the router, sampled BEFORE
        // tick() so this cycle's VcAllocator drain sees it (credit_avail
        // self-gates). DAT egress credit — same pre-tick replenish pattern
        // as before, now via the dat_* accessor.
        nsu_->rsp_set_ready(in_.tx_rsp_ready);
        for (uint8_t vc = 0; vc < dat_num_vc_; ++vc) {
            if (in_.tx_dat_crdvalid[vc]) nsu_->dat_rsp_receive_credit(vc);
        }

        // Step 2: advance Nsu one cycle (Depacketize + AxiMasterPort +
        // WormholeArbiter + VcAllocator, in upstream-first order per nsu.hpp).
        nsu_->tick();

        // Step 3: build NsuOutputs.
        out_ = NsuOutputs{};

        // AXI master side — drain AW/W/AR beats produced by Depacketize.
        // Held-latch pattern: hold each beat until the slave asserts
        // awready/wready/arready (AXI4 §A3.2.1 — master must not deassert
        // valid until ready is seen).

        // AW: consume held beat on awready; try to pop the next. The consume
        // tick is the recognized AW handshake — grant the W presentation
        // budget (AWLEN+1 beats): WVALID must not rise before the write's
        // AW handshake completed. (The BREADY window opens at the WLAST
        // consume below — a write's B is only awaited once its data phase
        // completed.)
        if (held_aw_ && in_.awready) {
            w_pop_budget_ += static_cast<uint32_t>(held_aw_->len) + 1u;
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

        // W: consume held beat on wready; try to pop the next — but only
        // within the presentation budget granted by completed AW handshakes
        // (W beats of a not-yet-handshaken AW stay queued, invisible on the
        // wire). The WLAST consume completes the write request — the B
        // response is now owed, so open the BREADY context window.
        if (held_w_ && in_.wready) {
            if (held_w_->last) ++outstanding_w_;
            held_w_ = std::nullopt;
        }
        if (!held_w_ && w_pop_budget_ > 0) {
            held_w_ = port.pop_w();
            if (held_w_) --w_pop_budget_;
        }
        if (held_w_) {
            out_.wvalid = true;
            out_.wdata = held_w_->data;
            out_.wstrb = held_w_->strb;
            out_.wlast = held_w_->last;
        }

        // AR: consume held beat on arready; try to pop the next. The consume
        // tick is the recognized AR handshake — ARLEN+1 R beats are now owed,
        // so widen the RREADY context window before dropping the beat.
        if (held_ar_ && in_.arready) {
            expected_r_beats_ += static_cast<uint32_t>(held_ar_->len) + 1u;
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

        // B/R: push slave responses into AxiMasterPort — ONLY on true
        // wire-handshake ticks (valid && our previously driven ready). The
        // slave holds the beat (A3.2.1 held latch) while our ready is
        // low, so gating cannot lose beats — but pushing on bare valid WOULD
        // double-count once ready can be low while valid is held.
        if (in_.bvalid && prev_bready_) {
            axi::BBeat b{};
            b.id = in_.bid;
            b.resp = static_cast<axi::Resp>(in_.bresp & 0x3u);
            b.user = 0;
            port.push_b(b);
            if (outstanding_w_ > 0) --outstanding_w_;
        }
        if (in_.rvalid && prev_rready_) {
            axi::RBeat r{};
            r.id = in_.rid;
            r.data = in_.rdata;
            r.resp = static_cast<axi::Resp>(in_.rresp & 0x3u);
            r.last = in_.rlast;
            r.user = 0;
            port.push_r(r);
            if (expected_r_beats_ > 0) --expected_r_beats_;
        }

        // bready/rready: context-gated pre-assert (policy spec). Nsu issued
        // the requests, so it pre-asserts ready while responses are owed and
        // buffer capacity allows — without waiting for valid.
        out_.bready = (outstanding_w_ > 0) && port.can_accept_b();
        out_.rready = (expected_r_beats_ > 0) && port.can_accept_r();

        // RSP egress: pop one rsp flit produced by Packetize this cycle. The
        // push into the terminal queue (inside nsu_->tick() above) already
        // happened only when tx_rsp_ready was true this cycle, so a
        // successful pop here IS this cycle's transfer.
        if (auto f = nsu_->pop_rsp_flit()) {
            out_.tx_rsp_valid = true;
            out_.tx_rsp_flit = flit_to_bytes(*f);
        }
        // DAT egress: unchanged credit-gated pop.
        if (auto f = nsu_->pop_dat_rsp_flit()) {
            out_.tx_dat_valid = true;
            out_.tx_dat_flit = flit_to_bytes(*f);
        }

        // REQ ingress ready: tied constant true — the c_model's ingress
        // queue is unbounded (Depacketize always drains what's injected).
        out_.rx_req_ready = true;

        // DAT ingress credit OUT: unchanged consumer PULSE/VC.
        for (uint8_t vc = 0; vc < dat_num_vc_; ++vc) {
            out_.rx_dat_crdvalid[vc] = nsu_->dat_req_take_credit(vc);
        }

        // Save this tick's ready outputs for next tick's handshake detection.
        prev_bready_ = out_.bready;
        prev_rready_ = out_.rready;
    }

    void get_outputs(NsuOutputs& out) const { out = out_; }

    // DAT VC count — read by the DPI handlers to size the DAT per-VC credit
    // loops. REQ/RSP are fixed single-VC (no accessor needed).
    uint8_t num_vc() const { return dat_num_vc_; }

    // Fabric-state-dump introspection (read-only by convention).
    nsu::NsuStandalone* standalone() { return nsu_.get(); }
    bool holding_aw() const { return held_aw_.has_value(); }
    bool holding_w() const { return held_w_.has_value(); }
    bool holding_ar() const { return held_ar_.has_value(); }
    uint32_t outstanding_w() const { return outstanding_w_; }
    uint32_t expected_r_beats() const { return expected_r_beats_; }
    uint32_t w_pop_budget() const { return w_pop_budget_; }

  private:
    uint8_t dat_num_vc_ = 1;
    std::unique_ptr<nsu::NsuStandalone> nsu_;
    NsuInputs in_{};
    NsuOutputs out_{};
    std::optional<axi::AwBeat> held_aw_;
    std::optional<axi::WBeat> held_w_;
    std::optional<axi::ArBeat> held_ar_;
    bool prev_bready_ = false;  // ready driven last tick (wire value this tick)
    bool prev_rready_ = false;
    uint32_t outstanding_w_ = 0;     // writes issued, B response still owed
    uint32_t expected_r_beats_ = 0;  // R beats owed from issued ARs
    uint32_t w_pop_budget_ = 0;      // W beats presentable (AWs already handshaken)

    // Flit <-> FlitBytes helpers live in wrap/flit_byte_conv.hpp; calls use
    // flit_from_bytes(...) / flit_to_bytes(...) directly via ADL.
};

}  // namespace ni::cmodel::wrap
