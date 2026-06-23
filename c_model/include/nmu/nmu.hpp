#pragma once
// NMU top-level assembly. Encapsulates Stage 3 NI sub-modules into one
// class with a single tick() entrypoint, hiding the manual wiring that
// previously lived in test_request_response_loopback.cpp.
//
// Pipeline (req path):
//   external AXI master ──> AxiSlavePort ──> Rob ──> Packetize{aw,w,ar}
//     ──> WormholeArbiter<NocReqOut>(3 in, pairing {{0,1}}) ──> VcArbiter
//     ──> external NocReqOut (ChannelModel or DPI bridge)
//
// Pipeline (rsp path):
//   external NocRspIn ──> Depacketize ──> Rob ──> AxiSlavePort
//     ──> back to external AXI master
//
// Per-cycle tick order (upstream-first; matches vc_arb/wormhole_arbiter
// round established pattern):
//   depacketize_.tick(); axi_slave_port_.tick();
//   wormhole_arbiter_.tick(); vc_arbiter_.tick();
//
// Lifetime: Nmu deletes move/copy (WormholeArbiter is non-movable).
// Member declaration order respects ctor ref dependencies — see private
// section comment for explanation.
//
// AXI binding: NOT via ctor (AxiMasterT<AxiSlavePort> template type
// collision in testbench). Use axi_slave_port() getter to obtain the
// AxiSlavePort& for the testbench's AxiMaster<AxiSlavePort> wiring.
//
// References:
//   docs/superpowers/specs/2026-06-04-nmu-nsu-top-level-design.md
#include "nmu/axi_slave_port.hpp"
#include "ni/ni_stage.hpp"
#include "nmu/depacketize.hpp"
#include "nmu/packetize.hpp"
#include "nmu/rob.hpp"
#include "nmu/vc_arbiter.hpp"
#include "nmu/ni_tokens.hpp"
#include "router/req_out.hpp"
#include "router/rsp_in.hpp"
#include "ni/pipeline_stage.hpp"
#include "ni/wormhole_arbiter.hpp"
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace ni::cmodel::nmu {

class NmuReqS1Bridge : public NmuPacketizeSink {
  public:
    bool push_aw_with_meta(const axi::AwBeat& b, AwHeaderMeta meta) override {
        if (s1_aw_.full()) return false;
        s1_aw_.accept({b, meta.dst_id, meta.local_addr, meta.rob_req, meta.rob_idx});
        return true;
    }
    bool push_w(const axi::WBeat& b) override {
        if (s1_w_.full()) return false;
        s1_w_.accept({b});
        return true;
    }
    bool push_ar_with_meta(const axi::ArBeat& b, AwHeaderMeta meta) override {
        if (s1_ar_.full()) return false;
        s1_ar_.accept({b, meta.dst_id, meta.local_addr, meta.rob_req, meta.rob_idx});
        return true;
    }

    void tick(Packetize& packetize) {
        if (s1_aw_.full()) {
            const auto& e = s1_aw_.peek();
            if (packetize.push_aw_with_meta(e.beat,
                                            {e.dst_id, e.local_addr, e.rob_req, e.rob_idx})) {
                s1_aw_.take();
            }
        }
        if (s1_aw_.full()) return;

        if (s1_w_.full()) {
            const auto& e = s1_w_.peek();
            if (packetize.push_w(e.beat)) {
                s1_w_.take();
            }
        }
        if (s1_ar_.full()) {
            const auto& e = s1_ar_.peek();
            if (packetize.push_ar_with_meta(e.beat,
                                            {e.dst_id, e.local_addr, e.rob_req, e.rob_idx})) {
                s1_ar_.take();
            }
        }
    }

    std::size_t occupancy(uint8_t axi_ch) const noexcept {
        if (axi_ch == ni::AXI_CH_AW) return s1_aw_.occupancy();
        if (axi_ch == ni::AXI_CH_W) return s1_w_.occupancy();
        if (axi_ch == ni::AXI_CH_AR) return s1_ar_.occupancy();
        return 0;
    }

  private:
    router::PipelineStage<AdmittedAw> s1_aw_;
    router::PipelineStage<AdmittedW> s1_w_;
    router::PipelineStage<AdmittedAr> s1_ar_;
};

struct NmuRspBEntry {
    axi::BBeat beat;
    uint8_t rob_idx = 0;
    uint8_t axi_id = 0;
    bool rob_enabled = false;
};

struct NmuRspREntry {
    axi::RBeat beat;
    uint8_t rob_idx = 0;
    uint8_t axi_id = 0;
    bool rob_enabled = false;
};

struct NmuConfig {
    uint8_t src_id = 0;
    RobMode read_rob_mode = RobMode::Disabled;
    RobMode write_rob_mode = RobMode::Disabled;
    nmu::PortParams port_params{};
    std::size_t num_vc = 1;
    VcMode vc_mode = VcMode::ReadWriteSplit;
    uint8_t write_vc = 0;
    uint8_t read_vc = 0;
    // Mode B candidate arrays; ignored when vc_mode == ReadWriteSplit.
    // Indexed by axi_ch (AW=0, W=1, AR=2, B=3, R=4 per ni_flit_constants).
    std::array<std::vector<uint8_t>, 5> vc_candidates{};
    std::size_t wormhole_per_input_depth = 4;
    std::size_t vc_arbiter_pending_depth = 4;
    std::size_t ni_req_extra_depth = 0;  // extra shift stages on the request path
    std::size_t ni_rsp_extra_depth = 0;  // extra shift stages on the response path
};

class Nmu {
  public:
    Nmu(NmuConfig cfg, router::NocReqOut& downstream_req, router::NocRspIn& downstream_rsp);

    Nmu(const Nmu&) = delete;
    Nmu(Nmu&&) = delete;
    Nmu& operator=(const Nmu&) = delete;
    Nmu& operator=(Nmu&&) = delete;

    // AXI facade for testbench wiring (AxiMaster<AxiSlavePort> binds here).
    AxiSlavePort& axi_slave_port() noexcept { return axi_slave_port_; }

    // Per-cycle tick — orchestrates sub-modules in upstream-first order.
    void tick();

    // Test introspection (optional getters; add only as test code needs)
    const Rob& rob() const noexcept { return rob_; }
    const VcArbiter& vc_arbiter() const noexcept { return vc_arbiter_; }
    std::size_t stage_occupancy(NiPath path, std::size_t stage, uint8_t axi_ch) const {
        if (path == NiPath::NmuReq) {
            // NmuReq: 3 stages
            //   S0 = NmuReqS1Bridge (AxiSlavePort→Rob→S1Bridge register)
            //   S1 = WormholeArbiter per-input pending (Packetize output)
            //   S2 = VcArbiter pending (toward NoC)
            if (stage == 0) return req_s1_bridge_.occupancy(axi_ch);
            if (stage == 1) {
                // WormholeArbiter inputs: 0=AW, 1=W, 2=AR
                if (axi_ch == ni::AXI_CH_AW) return wormhole_arbiter_.pending_size(0);
                if (axi_ch == ni::AXI_CH_W) return wormhole_arbiter_.pending_size(1);
                if (axi_ch == ni::AXI_CH_AR) return wormhole_arbiter_.pending_size(2);
            }
            if (stage == 2) {
                // VcArbiter: single VC in default config; sum over all VCs per channel
                // (in VC=1 mode this is just vc_arbiter_.pending_size(0))
                std::size_t total = 0;
                for (std::size_t v = 0; v < VcArbiter::NUM_VC_MAX; ++v)
                    total += vc_arbiter_.pending_size(static_cast<uint8_t>(v));
                return total;
            }
        }
        if (path == NiPath::NmuRsp) {
            // NmuRsp ROB Enabled: 3 stages
            //   S0 = Depacketize deque (b_q_/r_q_)
            //   S1 = s2_rsp_b_/s2_rsp_r_ PipelineStage (Rob re-order stage)
            //   S2 = AxiSlavePort b_q/r_q (final output)
            // NmuRsp ROB Disabled: 2 stages
            //   S0 = Depacketize deque
            //   S1 = AxiSlavePort b_q/r_q
            bool rob_enabled = (axi_ch == ni::AXI_CH_B) ? (cfg_.write_rob_mode == RobMode::Enabled)
                                                        : (cfg_.read_rob_mode == RobMode::Enabled);
            if (stage == 0) {
                if (axi_ch == ni::AXI_CH_B) return depacketize_.b_occupancy();
                if (axi_ch == ni::AXI_CH_R) return depacketize_.r_occupancy();
            }
            if (rob_enabled) {
                if (stage == 1) {
                    if (axi_ch == ni::AXI_CH_B) return s2_rsp_b_.occupancy();
                    if (axi_ch == ni::AXI_CH_R) return s2_rsp_r_.occupancy();
                }
                if (stage == 2) {
                    if (axi_ch == ni::AXI_CH_B) return axi_slave_port_.b_q_size();
                    if (axi_ch == ni::AXI_CH_R) return axi_slave_port_.r_q_size();
                }
            } else {
                if (stage == 1) {
                    if (axi_ch == ni::AXI_CH_B) return axi_slave_port_.b_q_size();
                    if (axi_ch == ni::AXI_CH_R) return axi_slave_port_.r_q_size();
                }
            }
        }
        return 0;
    }

  private:
    bool push_rsp_b_to_axi_(const NmuRspBEntry& entry);
    bool push_rsp_r_to_axi_(const NmuRspREntry& entry);
    bool accept_rsp_b_entry_(NmuRspBEntry entry);
    bool accept_rsp_r_entry_(NmuRspREntry entry);
    void drain_rsp_b_output_();
    void drain_rsp_r_output_();
    void advance_rsp_b_shift_();
    void advance_rsp_r_shift_();
    void drain_rsp_s2_b_();
    void drain_rsp_s2_r_();
    void advance_rsp_s2_b_();
    void advance_rsp_s2_r_();
    void drain_rsp_robless_b_();
    void drain_rsp_robless_r_();

    // Declaration order respects ctor ref dependencies:
    //   1. cfg_ + external downstream refs (no deps).
    //   2. vc_arbiter_ wraps downstream_req_.
    //   3. wormhole_arbiter_ wraps vc_arbiter_ as its Downstream.
    //   4. depacketize_ wraps downstream_rsp_ (req path independent).
    //   5. packetize_ takes wormhole_arbiter_.input(0/1/2) (req path).
    //   6. req_s1_bridge_ stages ROB-admitted requests before Packetize.
    //   7. rob_ takes req_s1_bridge_ + depacketize_.
    //   7. axi_slave_port_ takes rob_ (as Packetizer + Depacketizer via multi-inherit).
    NmuConfig cfg_;
    router::NocReqOut& downstream_req_;
    router::NocRspIn& downstream_rsp_;
    VcArbiter vc_arbiter_;
    router::WormholeArbiter<router::NocReqOut> wormhole_arbiter_;
    Depacketize depacketize_;
    Packetize packetize_;
    NmuReqS1Bridge req_s1_bridge_;
    Rob rob_;
    AxiSlavePort axi_slave_port_;
    router::PipelineStage<NmuRspBEntry> s2_rsp_b_;
    router::PipelineStage<NmuRspREntry> s2_rsp_r_;
    std::vector<router::PipelineStage<NmuRspBEntry>> rsp_extra_b_shift_;
    std::vector<router::PipelineStage<NmuRspREntry>> rsp_extra_r_shift_;
};

namespace detail {

inline VcArbiter make_vc_arbiter(const NmuConfig& cfg, router::NocReqOut& downstream) {
    if (cfg.vc_mode == VcMode::ReadWriteSplit) {
        return VcArbiter::read_write_split(downstream, cfg.num_vc, cfg.write_vc, cfg.read_vc,
                                           cfg.vc_arbiter_pending_depth);
    }
    auto candidates = cfg.vc_candidates;  // copy; factory consumes by value
    return VcArbiter::multi_candidate(downstream, cfg.num_vc, std::move(candidates),
                                      cfg.vc_arbiter_pending_depth);
}

}  // namespace detail

inline Nmu::Nmu(NmuConfig cfg, router::NocReqOut& downstream_req, router::NocRspIn& downstream_rsp)
    : cfg_(std::move(cfg)),
      downstream_req_(downstream_req),
      downstream_rsp_(downstream_rsp),
      vc_arbiter_(detail::make_vc_arbiter(cfg_, downstream_req_)),
      wormhole_arbiter_(vc_arbiter_, /*num_inputs=*/3, std::vector<router::ChannelPairing>{{0, 1}},
                        cfg_.wormhole_per_input_depth),
      depacketize_(downstream_rsp_, cfg_.port_params.depkt_b_q_depth,
                   cfg_.port_params.depkt_r_q_depth),
      packetize_(wormhole_arbiter_.input(0), wormhole_arbiter_.input(1), wormhole_arbiter_.input(2),
                 cfg_.src_id),
      req_s1_bridge_(),
      rob_(req_s1_bridge_, depacketize_, cfg_.write_rob_mode, cfg_.read_rob_mode),
      axi_slave_port_(rob_, rob_, cfg_.port_params),
      s2_rsp_b_(),
      s2_rsp_r_(),
      rsp_extra_b_shift_(cfg_.ni_rsp_extra_depth),
      rsp_extra_r_shift_(cfg_.ni_rsp_extra_depth) {
    rob_.set_drain_observer(
        [this](bool is_write, uint8_t id) { vc_arbiter_.on_id_drained(is_write, id); });
}

inline void Nmu::tick() {
    wormhole_arbiter_.tick();
    vc_arbiter_.tick();
    req_s1_bridge_.tick(packetize_);
    axi_slave_port_.tick_req();

    drain_rsp_b_output_();
    drain_rsp_r_output_();
    advance_rsp_b_shift_();
    advance_rsp_r_shift_();
    if (cfg_.write_rob_mode == RobMode::Enabled) {
        drain_rsp_s2_b_();
        advance_rsp_s2_b_();
    } else {
        drain_rsp_robless_b_();
    }
    if (cfg_.read_rob_mode == RobMode::Enabled) {
        drain_rsp_s2_r_();
        advance_rsp_s2_r_();
    } else {
        drain_rsp_robless_r_();
    }
    depacketize_.tick();
}

inline bool Nmu::push_rsp_b_to_axi_(const NmuRspBEntry& entry) {
    if (!axi_slave_port_.push_b_staged(entry.beat)) return false;
    if (entry.rob_enabled) rob_.commit_b_exit(entry.rob_idx, entry.axi_id);
    return true;
}

inline bool Nmu::push_rsp_r_to_axi_(const NmuRspREntry& entry) {
    if (!axi_slave_port_.push_r_staged(entry.beat)) return false;
    if (entry.rob_enabled) rob_.commit_r_exit(entry.rob_idx, entry.axi_id);
    return true;
}

inline bool Nmu::accept_rsp_b_entry_(NmuRspBEntry entry) {
    if (rsp_extra_b_shift_.empty()) return push_rsp_b_to_axi_(entry);
    if (rsp_extra_b_shift_.front().full()) return false;
    rsp_extra_b_shift_.front().accept(std::move(entry));
    return true;
}

inline bool Nmu::accept_rsp_r_entry_(NmuRspREntry entry) {
    if (rsp_extra_r_shift_.empty()) return push_rsp_r_to_axi_(entry);
    if (rsp_extra_r_shift_.front().full()) return false;
    rsp_extra_r_shift_.front().accept(std::move(entry));
    return true;
}

inline void Nmu::drain_rsp_b_output_() {
    if (rsp_extra_b_shift_.empty() || !rsp_extra_b_shift_.back().full()) return;
    const auto& entry = rsp_extra_b_shift_.back().peek();
    if (push_rsp_b_to_axi_(entry)) rsp_extra_b_shift_.back().take();
}

inline void Nmu::drain_rsp_r_output_() {
    if (rsp_extra_r_shift_.empty() || !rsp_extra_r_shift_.back().full()) return;
    const auto& entry = rsp_extra_r_shift_.back().peek();
    if (push_rsp_r_to_axi_(entry)) rsp_extra_r_shift_.back().take();
}

inline void Nmu::advance_rsp_b_shift_() {
    if (rsp_extra_b_shift_.size() < 2) return;
    for (std::size_t i = rsp_extra_b_shift_.size() - 1; i > 0; --i) {
        if (!rsp_extra_b_shift_[i].full() && rsp_extra_b_shift_[i - 1].full()) {
            rsp_extra_b_shift_[i].accept(rsp_extra_b_shift_[i - 1].take());
        }
    }
}

inline void Nmu::advance_rsp_r_shift_() {
    if (rsp_extra_r_shift_.size() < 2) return;
    for (std::size_t i = rsp_extra_r_shift_.size() - 1; i > 0; --i) {
        if (!rsp_extra_r_shift_[i].full() && rsp_extra_r_shift_[i - 1].full()) {
            rsp_extra_r_shift_[i].accept(rsp_extra_r_shift_[i - 1].take());
        }
    }
}

inline void Nmu::drain_rsp_s2_b_() {
    if (!s2_rsp_b_.full()) return;
    const auto& entry = s2_rsp_b_.peek();
    if (accept_rsp_b_entry_(entry)) s2_rsp_b_.take();
}

inline void Nmu::drain_rsp_s2_r_() {
    if (!s2_rsp_r_.full()) return;
    const auto& entry = s2_rsp_r_.peek();
    if (accept_rsp_r_entry_(entry)) s2_rsp_r_.take();
}

inline void Nmu::advance_rsp_s2_b_() {
    if (s2_rsp_b_.full()) return;
    auto b = rob_.pop_b_staged();
    if (!b) return;
    s2_rsp_b_.accept({b->beat, b->rob_idx, b->axi_id, true});
}

inline void Nmu::advance_rsp_s2_r_() {
    if (s2_rsp_r_.full()) return;
    auto r = rob_.pop_r_staged();
    if (!r) return;
    s2_rsp_r_.accept({r->beat, r->rob_idx, r->axi_id, true});
}

inline void Nmu::drain_rsp_robless_b_() {
    if (rsp_extra_b_shift_.empty() &&
        axi_slave_port_.b_q_size() >= axi_slave_port_.params().b_queue_depth) {
        return;
    }
    if (!rsp_extra_b_shift_.empty() && rsp_extra_b_shift_.front().full()) return;
    auto b = rob_.pop_b();
    if (!b) return;
    (void)accept_rsp_b_entry_({*b, 0, b->id, false});
}

inline void Nmu::drain_rsp_robless_r_() {
    if (rsp_extra_r_shift_.empty() &&
        axi_slave_port_.r_q_size() >= axi_slave_port_.params().r_queue_depth) {
        return;
    }
    if (!rsp_extra_r_shift_.empty() && rsp_extra_r_shift_.front().full()) return;
    auto r = rob_.pop_r();
    if (!r) return;
    (void)accept_rsp_r_entry_({*r, 0, r->id, false});
}

// -------------------------------------------------------------------------
// Stage 5b: NmuStandalone — hermetic wrapper, no external NoC refs.
//
// Wraps construct NmuStandalone(NmuConfig{...}) without supplying
// NocReqOut& / NocRspIn&. The wrapper owns null-stub implementations of
// both interfaces; the real DPI wiring replaces them at the Wrap
// tick boundary. The wrapper's NullNocReqOut/NullNocRspIn are real queues
// (not no-op stubs): push_flit enqueues, pop_flit dequeues.
//
// Invariant: NmuStandalone is non-copyable and non-movable (same as Nmu).
// -------------------------------------------------------------------------

// NullNocReqOut / NullNocRspIn are queue-backed terminal endpoints so that
// NmuWrap can drain produced req flits and inject incoming rsp flits
// at the DPI boundary without modifying the Nmu internals.
//
// NullNocReqOut: push_flit enqueues into an internal deque (capped at
//   kMaxQueueDepth as a drain-forgotten sanity check). With FlooNoC credit
//   enabled (cosim opt-in) push_flit also gates+consumes per-VC credit and
//   credit_avail reflects the counter; with credit OFF (default) it accepts
//   unconditionally. Wrap drains via pop_req_flit() each tick.
//
// NullNocRspIn: Wrap injects flits via inject_rsp_flit() before
//   calling nmu_.tick(); Nmu's Depacketize stage drains via pop_flit().
namespace detail {

struct NullNocReqOut : router::NocReqOut {
    // Sanity cap: a real Wrap drains every tick, so a queue this deep
    // means the test forgot to drain. Asserts in debug; release builds skip
    // the check (and the queue is still allowed to grow unboundedly).
    static constexpr std::size_t kMaxQueueDepth = 1024;

    // R2 opt-in FlooNoC sender credit (default OFF = today's always-available).
    // When enabled, this models the InjectAdapter credit pattern: a per-VC
    // counter seeded to the downstream (router LOCAL input) depth; push_flit
    // decrements on accept, receive_credit increments on a credit pulse.
    // INVARIANT: credit_[vc] is decremented ONLY in push_flit and incremented
    // ONLY in receive_credit, so credit_[vc] + outstanding == seed holds.
    void enable_credit(uint8_t num_vc, std::size_t seed) {
        credit_enabled_ = true;
        credit_.assign(num_vc, seed);
    }
    void receive_credit(uint8_t vc) { ++credit_[vc]; }

    // Accept a flit into the queue. When credit is disabled this models
    // infinite downstream bandwidth (always accept). When enabled it gates on
    // and consumes one per-VC credit, returning false (backpressure) if none.
    bool push_flit(const Flit& f) override {
        if (credit_enabled_) {
            const auto vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
            if (credit_[vc] == 0) return false;
            --credit_[vc];
        }
        assert(queue_.size() < kMaxQueueDepth &&
               "NullNocReqOut overflow — did the test Wrap forget to drain?");
        queue_.push_back(f);
        return true;
    }
    bool credit_avail(uint8_t vc) const override { return !credit_enabled_ || credit_[vc] > 0; }

    // Wrap accessor: pop one flit per tick for DPI forwarding.
    std::optional<Flit> pop_req_flit() {
        if (queue_.empty()) return std::nullopt;
        Flit f = queue_.front();
        queue_.pop_front();
        return f;
    }

  private:
    std::deque<Flit> queue_;
    bool credit_enabled_ = false;
    std::vector<std::size_t> credit_;
};

struct NullNocRspIn : router::NocRspIn {
    // Wrap accessor: inject one flit per tick from DPI wire.
    void inject_rsp_flit(const Flit& f) { queue_.push_back(f); }

    // R2 consumer-pulse: size the per-VC pending counter before traffic. Always
    // present (no enable flag needed — pending only matters when the wrap drains
    // it via take_credit, which is cosim-only). Defaults to 1 VC.
    void size_pending(uint8_t num_vc) { pending_.assign(num_vc, 0); }

    // Nmu's Depacketize stage drains via pop_flit() each tick. Depacketize may
    // call pop_flit() MULTIPLE times per tick (it drains in a while-loop), so
    // pending_ MUST accumulate (counter), not latch a single bit.
    std::optional<Flit> pop_flit() override {
        if (queue_.empty()) return std::nullopt;
        Flit f = queue_.front();
        queue_.pop_front();
        const auto vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
        ++pending_[vc];
        return f;
    }

    // Wrap accessor: drain one consumer credit pulse per tick (mirror of
    // router::LinkCreditOut::take). Returns true when a pulse was emitted.
    bool take_credit(uint8_t vc) {
        if (pending_[vc] == 0) return false;
        --pending_[vc];
        return true;
    }
    std::size_t pending(uint8_t vc) const { return pending_[vc]; }

  private:
    std::deque<Flit> queue_;
    // (count=1, value=0) vector ctor: one VC, pending pulse count 0. NOT a
    // 2-element {1, 0} initializer-list. size_pending() resizes for >1 VC.
    std::vector<std::size_t> pending_{1, 0};
};

}  // namespace detail

class NmuStandalone {
  public:
    explicit NmuStandalone(NmuConfig cfg)
        : num_vc_(static_cast<uint8_t>(cfg.num_vc)),
          null_req_out_(),
          null_rsp_in_(),
          nmu_(std::move(cfg), null_req_out_, null_rsp_in_) {
        null_rsp_in_.size_pending(num_vc_);
    }

    NmuStandalone(const NmuStandalone&) = delete;
    NmuStandalone(NmuStandalone&&) = delete;
    NmuStandalone& operator=(const NmuStandalone&) = delete;
    NmuStandalone& operator=(NmuStandalone&&) = delete;

    AxiSlavePort& axi_slave_port() noexcept { return nmu_.axi_slave_port(); }
    void tick() { nmu_.tick(); }
    const Rob& rob() const noexcept { return nmu_.rob(); }
    const VcArbiter& vc_arbiter() const noexcept { return nmu_.vc_arbiter(); }
    std::size_t stage_occupancy(NiPath path, std::size_t stage, uint8_t axi_ch) const {
        return nmu_.stage_occupancy(path, stage, axi_ch);
    }
    Nmu& nmu() noexcept { return nmu_; }

    // Stage 5b Wrap accessors — drain req side, inject rsp side.
    std::optional<Flit> pop_req_flit() { return null_req_out_.pop_req_flit(); }
    void inject_rsp_flit(const Flit& f) { null_rsp_in_.inject_rsp_flit(f); }
    bool req_credit_avail(uint8_t vc = 0) const { return null_req_out_.credit_avail(vc); }

    // R2 opt-in FlooNoC credit at the NoC terminal edge (cosim-only; default
    // OFF). Seeds the req-out sender counter; the rsp-in consumer pulse is
    // always active (sized in the ctor) but inert unless rsp_take_credit drains.
    void enable_noc_credit(std::size_t seed) { null_req_out_.enable_credit(num_vc_, seed); }
    void req_receive_credit(uint8_t vc = 0) { null_req_out_.receive_credit(vc); }
    bool rsp_take_credit(uint8_t vc = 0) { return null_rsp_in_.take_credit(vc); }

  private:
    uint8_t num_vc_;
    detail::NullNocReqOut null_req_out_;
    detail::NullNocRspIn null_rsp_in_;
    Nmu nmu_;
};

}  // namespace ni::cmodel::nmu
