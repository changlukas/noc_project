// SlaveShellAdapter — Stage 5b ShellAdapter for the AxiSlave component.
//
// Owns an AxiSlave (T3 standalone ctor with internal Memory). Each tick follows
// the 3-step pattern:
//   set_inputs(in)   → latch SlaveInputs (AW/W/AR beats + bready/rready)
//   tick()           → push accepted AW/W/AR into AxiSlave, advance slave tick,
//                      drain pending B/R beats gated by bready/rready
//   get_outputs(out) → copy output latch to caller
//
// Wire interception design: AxiSlave's existing push_aw/push_w/push_ar and
// pop_b/pop_r API already form the wire boundary — no WireMasterPort class is
// needed. awready/wready/arready are derived from queue capacity (size < depth);
// bvalid/rvalid are derived from a held latch that retains beats when the master
// drives bready=false / rready=false, matching AXI4 §A3.2.1 (valid must not
// deassert until ready).
//
// Hermetic invariant: no refs to other ShellAdapters.
#pragma once
#include "axi/axi_slave.hpp"
#include "axi/types.hpp"
#include "cosim/slave_shell_io.hpp"
#include <memory>
#include <optional>

namespace ni::cmodel::cosim {

class SlaveShellAdapter {
  public:
    // init — construct AxiSlave with owned Memory using default config.
    // Defaults match Stage 5b PoC: 64 KiB memory, 1-cycle latency, 32-deep queues.
    void init(uint64_t memory_base = 0, std::size_t memory_size = 65536, std::size_t write_lat = 1,
              std::size_t read_lat = 1, std::size_t queue_depth = 32) {
        axi::AxiSlaveConfig cfg;
        cfg.memory_base_addr = memory_base;
        cfg.memory_size = memory_size;
        cfg.write_latency = write_lat;
        cfg.read_latency = read_lat;
        cfg.channel_queue_depth = queue_depth;
        queue_depth_ = queue_depth;
        slave_ = std::make_unique<axi::AxiSlave>(cfg);
        in_ = SlaveInputs{};
        out_ = SlaveOutputs{};
        held_b_ = std::nullopt;
        held_r_ = std::nullopt;
        prev_awready_ = false;
        prev_wready_ = false;
        prev_arready_ = false;
        w_expected_ = 0;
    }

    void set_inputs(const SlaveInputs& in) { in_ = in; }

    void tick() {
        if (!slave_) return;

        // Step 1: push AW/W/AR beats into AxiSlave queues — ONLY on true
        // wire-handshake ticks (valid && our previously driven ready).
        // wait_valid policy: see nmu_shell_adapter.hpp / the policy spec.
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
            (void)slave_->push_aw(aw);
            w_expected_ += static_cast<uint32_t>(in_.awlen) + 1u;
        }

        if (in_.wvalid && prev_wready_) {
            axi::WBeat w{};
            w.data = in_.wdata;
            w.strb = in_.wstrb;
            w.last = in_.wlast;
            w.user = 0;
            (void)slave_->push_w(w);
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
            (void)slave_->push_ar(ar);
        }

        // Step 2: advance AxiSlave one cycle (processes AW→W→B and AR→R pipelines).
        slave_->tick();

        // Step 3: build SlaveOutputs.
        out_ = SlaveOutputs{};

        // wait_valid / context-gated ready policy (same as NmuShellAdapter;
        // see the policy spec): AW/AR one-shot wait_valid (AW gated while a
        // W burst is open); W pre-asserts on capacity for the burst window.
        out_.awready = in_.awvalid && !prev_awready_ && (w_expected_ == 0) &&
                       (slave_->aw_q_size() < queue_depth_);
        out_.wready = (w_expected_ > 0) && (slave_->w_q_size() < queue_depth_);
        out_.arready =
            in_.arvalid && !prev_arready_ && (slave_->ar_q_size() < queue_depth_);

        // B channel: held_b_ retains the beat while bready is low (AXI4 §A3.2.1:
        // bvalid must remain asserted until bready). On bready rising, consume the
        // held beat; then try to pop the next one so back-to-back transfers work.
        if (held_b_ && in_.bready) {
            held_b_ = std::nullopt;  // handshake complete — consume the held beat
        }
        if (!held_b_) {
            held_b_ = slave_->pop_b();  // try to fetch the next pending beat
        }
        if (held_b_) {
            out_.bvalid = true;
            out_.bid = held_b_->id;
            out_.bresp = static_cast<uint8_t>(held_b_->resp);
        }

        // R channel: same held-latch pattern as B.
        if (held_r_ && in_.rready) {
            held_r_ = std::nullopt;
        }
        if (!held_r_) {
            held_r_ = slave_->pop_r();
        }
        if (held_r_) {
            out_.rvalid = true;
            out_.rid = held_r_->id;
            out_.rdata = held_r_->data;
            out_.rresp = static_cast<uint8_t>(held_r_->resp);
            out_.rlast = held_r_->last;
        }

        // Save this tick's ready outputs for next tick's handshake detection.
        prev_awready_ = out_.awready;
        prev_wready_ = out_.wready;
        prev_arready_ = out_.arready;
    }

    void get_outputs(SlaveOutputs& out) const { out = out_; }

  private:
    std::unique_ptr<axi::AxiSlave> slave_;
    std::size_t queue_depth_ = 32;
    SlaveInputs in_{};
    SlaveOutputs out_{};
    std::optional<axi::BBeat> held_b_;  // beat held while bready is low
    std::optional<axi::RBeat> held_r_;  // beat held while rready is low
    bool prev_awready_ = false;  // ready driven last tick (wire value this tick)
    bool prev_wready_ = false;
    bool prev_arready_ = false;
    uint32_t w_expected_ = 0;  // W beats remaining of the open burst window
};

}  // namespace ni::cmodel::cosim
