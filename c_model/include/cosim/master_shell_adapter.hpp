// MasterShellAdapter — Stage 5b ShellAdapter for the AxiMaster component.
//
// Owns an AxiMasterStandalone (T3 wrapper) which internally uses a WireSlavePort
// (T8 additive change to axi_master.hpp). Each tick follows the 3-step pattern:
//   set_inputs(in)   → latch MasterInputs (ready signals + B/R beats)
//   tick()           → inject B/R into WireSlavePort, set awready/wready/arready,
//                      run master tick, drain AW/W/AR from WireSlavePort
//   get_outputs(out) → copy output latch to caller
//
// Hermetic invariant: no refs to other ShellAdapters.
// Scoreboard callbacks fire inside AxiMasterT::tick() (on_write_completed /
// on_read_observed) — no DPI exposure needed for the PoC.
//
// configure_inject() is called at cmodel_init per scenario for T4 fault injection.
// The adapter exposes a done() predicate so cmodel_finalize can drain in-flight ops.
#pragma once
#include "axi/axi_master.hpp"
#include "axi/scenario_parser.hpp"
#include "axi/types.hpp"
#include "cosim/master_shell_io.hpp"
#include <memory>
#include <string>

namespace ni::cmodel::cosim {

class MasterShellAdapter {
  public:
    // init — construct AxiMasterStandalone from a scenario YAML path.
    // read_dump_path defaults to a temp file so unit tests without a read-side
    // scenario can still call init without a real dump path.
    void init(const std::string& scenario_yaml,
              const std::string& read_dump_path = "",
              std::size_t max_outstanding_write = 1,
              std::size_t max_outstanding_read  = 1) {
        axi::AxiMasterConfig cfg;
        cfg.scenario_yaml        = scenario_yaml;
        cfg.read_dump_path       = read_dump_path.empty() ? make_tmp_dump() : read_dump_path;
        cfg.max_outstanding_write = max_outstanding_write;
        cfg.max_outstanding_read  = max_outstanding_read;
        read_dump_path_          = cfg.read_dump_path;  // cache for cmodel_dump_scoreboard
        master_ = std::make_unique<axi::AxiMasterStandalone>(cfg);
        in_  = MasterInputs{};
        out_ = MasterOutputs{};
    }

    // Path of the read-data dump file written by AxiMaster on each R-channel
    // completion. Empty if init() was never called.
    const std::string& read_dump_path() const { return read_dump_path_; }

    // configure_inject — forward T4 fault injection config to inner master.
    // Call after init(); no-op if inject.mode == None (default).
    void configure_inject(const axi::InjectConfig& inj) {
        if (master_) master_->configure_inject(inj);
    }

    // Scoreboard integration: forward callbacks to inner AxiMasterStandalone.
    // Must be called after init() and before the first tick().
    void on_write_completed(std::function<void(const axi::WriteResult&)> cb) {
        if (master_) master_->on_write_completed(std::move(cb));
    }
    void on_read_observed(std::function<void(const axi::ReadResult&)> cb) {
        if (master_) master_->on_read_observed(std::move(cb));
    }

    void set_inputs(const MasterInputs& in) { in_ = in; }

    void tick() {
        if (!master_) return;
        auto& wp = master_->wire_port();

        // Step 1a: set per-channel ready backpressure from wire inputs.
        //
        // Beta-tick discipline: the incoming ready signals are the PREVIOUS cycle's
        // registered NMU/NSU outputs. A beat is only accepted (handshake complete)
        // when VALID was high ON THE WIRE last cycle AND ready is high this cycle.
        // "Valid on the wire last cycle" = the master drove valid_q=1 last cycle,
        // captured in prev_out_. Without this guard, the WireSlavePort would accept
        // a beat the moment the downstream reports queue-space (ready=1), BEFORE
        // the downstream ever saw VALID=1, causing the downstream to miss the beat.
        wp.set_awready(in_.awready && prev_out_.awvalid);
        wp.set_wready(in_.wready   && prev_out_.wvalid);
        wp.set_arready(in_.arready && prev_out_.arvalid);

        // Request handshakes recognized this tick open the response-ready
        // context windows (policy spec: B/R ready pre-assert only while a
        // response is actually owed).
        if (in_.awready && prev_out_.awvalid) {
            ++outstanding_w_;
        }
        if (in_.arready && prev_out_.arvalid) {
            expected_r_beats_ += static_cast<uint32_t>(prev_out_.arlen) + 1u;
        }

        // Step 1b: inject B/R response beats into WireSlavePort — ONLY on
        // true wire-handshake ticks (valid && our previously driven ready).
        // The responder holds the beat (A3.2.1) while our ready is low;
        // injecting on bare valid would double-count held beats.
        if (in_.bvalid && prev_bready_) {
            axi::BBeat b{};
            b.id   = in_.bid;
            b.resp = static_cast<axi::Resp>(in_.bresp & 0x3u);
            b.user = 0;
            wp.inject_b(b);
            if (outstanding_w_ > 0) --outstanding_w_;
        }
        if (in_.rvalid && prev_rready_) {
            static_assert(AXI_DATA_BYTES == axi::DATA_BYTES,
                          "MasterInputs::rdata size must equal axi::DATA_BYTES");
            axi::RBeat r{};
            r.id   = in_.rid;
            r.resp = static_cast<axi::Resp>(in_.rresp & 0x3u);
            r.last = in_.rlast;
            r.user = 0;
            r.data = in_.rdata;
            wp.inject_r(r);
            if (expected_r_beats_ > 0) --expected_r_beats_;
        }

        // Step 2: advance AxiMasterT one cycle. Scoreboard callbacks
        // (on_write_completed / on_read_observed) fire here if applicable.
        master_->tick();

        // Step 3: sample the pending beats from WireSlavePort to populate
        // MasterOutputs. pending_aw/w/ar reflect what the master is currently
        // driving onto the channel this cycle (valid = pending beat exists).
        out_ = MasterOutputs{};

        // bready / rready: context-gated pre-assert (policy spec) — the
        // master pre-asserts ready only while a response is owed for a
        // request it actually issued; idle wires sit at 0.
        out_.bready = (outstanding_w_ > 0);
        out_.rready = (expected_r_beats_ > 0);

        if (auto aw = wp.pending_aw()) {
            out_.awvalid  = true;
            out_.awid     = aw->id;
            out_.awaddr   = aw->addr;
            out_.awlen    = aw->len;
            out_.awsize   = aw->size;
            out_.awburst  = static_cast<uint8_t>(aw->burst);
            out_.awlock   = aw->lock;
            out_.awcache  = aw->cache;
            out_.awprot   = aw->prot;
            out_.awqos    = aw->qos;
        }

        if (auto w = wp.pending_w()) {
            out_.wvalid = true;
            out_.wdata  = w->data;
            out_.wstrb  = w->strb;
            out_.wlast  = w->last;
        }

        if (auto ar = wp.pending_ar()) {
            out_.arvalid  = true;
            out_.arid     = ar->id;
            out_.araddr   = ar->addr;
            out_.arlen    = ar->len;
            out_.arsize   = ar->size;
            out_.arburst  = static_cast<uint8_t>(ar->burst);
            out_.arlock   = ar->lock;
            out_.arcache  = ar->cache;
            out_.arprot   = ar->prot;
            out_.arqos    = ar->qos;
        }

        // Save this cycle's outputs for beta-tick handshake guard next cycle.
        prev_out_ = out_;
        prev_bready_ = out_.bready;
        prev_rready_ = out_.rready;
    }

    void get_outputs(MasterOutputs& out) const { out = out_; }

    bool done() const { return !master_ || master_->done(); }

  private:
    std::unique_ptr<axi::AxiMasterStandalone> master_;
    std::string   read_dump_path_;
    MasterInputs  in_{};
    MasterOutputs out_{};
    MasterOutputs prev_out_{};  // previous cycle's output for beta-tick guard
    bool prev_bready_ = false;   // ready driven last tick (wire value this tick)
    bool prev_rready_ = false;
    uint32_t outstanding_w_ = 0;     // writes issued, B response still owed
    uint32_t expected_r_beats_ = 0;  // R beats owed from issued ARs

    // Default read-dump path when init() is called without an explicit one.
    // Kept cwd-relative so dumps land inside the project (cosim/verilator/
    // when Vtb_top runs, c_model/build/ when ctest runs) rather than the OS
    // temp dir. Overwritten on each run.
    static std::string make_tmp_dump() {
        return "master_shell_read_dump.txt";
    }
};

}  // namespace ni::cmodel::cosim
