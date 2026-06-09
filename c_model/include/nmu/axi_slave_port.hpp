#pragma once
// NMU AxiSlavePort — thin transparent AXI4 subordinate transport.
//
// Role (per docs/_archive/noc_cmodel_rtl_plan.md §3): the NMU's upstream-facing AXI
// boundary. An external manager (AxiMaster) drives AW / W / AR into this
// port; this port hands those beats unmodified to a Packetizer for NoC
// transport, and surfaces B / R beats popped from a Depacketizer back to
// the manager.
//
// Scope: 5-channel valid/ready handshake + channel-attribute pass-through +
// wlast / rlast framing as-is. EXPLICITLY NOT done here:
//   - per-beat address generation (FIXED/INCR/WRAP) — that's beat_addr at
//     the memory endpoint
//   - memory bounds / DECERR generation
//   - burst splitting (4KB cross etc) — the upstream manager already shapes
//     legal sub-bursts
//   - per-AXI-ID response reordering — that's the ROB stage (plan §3.1)
//
// Port contract: per-channel FIFO order for all beats regardless of AXI ID.
// Cross-ID completion ordering / per-ID response reordering is the ROB
// stage's responsibility (see plan §3.1), NOT this port's.
//
// Structure mirrors c_model/include/axi/axi_slave.hpp (Stage 2 canonical
// header-only pattern): one std::deque per channel, bounded by PortParams.
// push_* returns false on full; pop_* returns nullopt on empty. tick()
// drains responses BEFORE forwarding requests so a packetizer that empties
// its slot in the same cycle can be re-fed without a one-cycle bubble.
#include "axi/types.hpp"
#include "nmu/port_params.hpp"
#include "request_io.hpp"
#include "response_io.hpp"
#include <cstddef>
#include <deque>
#include <optional>

namespace ni::cmodel::nmu {

// Per-channel internal FIFO depths come from nmu::PortParams.
// Single source of truth is c_model/config/port_params.yaml nmu: block;
// see nmu/port_params.hpp for the loader helper and the "fail loud / no
// defaults" rationale.

class AxiSlavePort {
  public:
    AxiSlavePort(RequestPacketizer& packetizer, ResponseDepacketizer& depacketizer,
                 PortParams params)
        : pkt_(packetizer), depkt_(depacketizer), params_(params) {}

    // ---- Upstream-facing AXI subordinate API (mirrors axi/axi_slave.hpp) ----
    bool push_aw(const axi::AwBeat& b) {
        if (aw_q_.size() >= params_.aw_queue_depth) return false;
        aw_q_.push_back(b);
        return true;
    }
    bool push_w(const axi::WBeat& b) {
        if (w_q_.size() >= params_.w_queue_depth) return false;
        w_q_.push_back(b);
        return true;
    }
    bool push_ar(const axi::ArBeat& b) {
        if (ar_q_.size() >= params_.ar_queue_depth) return false;
        ar_q_.push_back(b);
        return true;
    }

    std::optional<axi::BBeat> pop_b() {
        if (b_q_.empty()) return std::nullopt;
        auto r = b_q_.front();
        b_q_.pop_front();
        return r;
    }
    std::optional<axi::RBeat> pop_r() {
        if (r_q_.empty()) return std::nullopt;
        auto r = r_q_.front();
        r_q_.pop_front();
        return r;
    }

    void tick() {
        // Response drain BEFORE request forward (mirrors Stage 2 AxiMaster::tick
        // and frees upstream-visible B/R slots before this cycle's new pushes).
        drain_b_from_depacketizer_();
        drain_r_from_depacketizer_();
        forward_aw_to_packetizer_();
        forward_w_to_packetizer_();
        forward_ar_to_packetizer_();
    }

    // ---- Introspection (tests) ----
    std::size_t aw_q_size() const { return aw_q_.size(); }
    std::size_t w_q_size() const { return w_q_.size(); }
    std::size_t ar_q_size() const { return ar_q_.size(); }
    std::size_t b_q_size() const { return b_q_.size(); }
    std::size_t r_q_size() const { return r_q_.size(); }
    const PortParams& params() const { return params_; }

    // Tick-end capacity queries (Stage 5b ShellAdapter contract per spec §6.4):
    // Returns true iff one more AW/W/AR beat can be pushed when the next tick
    // begins. MUST be called at tick end (after the c_model has drained /
    // produced for this cycle). ShellAdapter samples these to drive the
    // next-cycle ready outputs.
    [[nodiscard]] bool can_accept_aw() const noexcept {
        return aw_q_.size() < params_.aw_queue_depth;
    }
    [[nodiscard]] bool can_accept_w() const noexcept { return w_q_.size() < params_.w_queue_depth; }
    [[nodiscard]] bool can_accept_ar() const noexcept {
        return ar_q_.size() < params_.ar_queue_depth;
    }

    // Non-consuming peek at the front of each queue (returns nullopt if empty).
    // Used by AxiDpiAdapter to snapshot pin state without advancing the queue.
    std::optional<axi::AwBeat> peek_aw() const {
        if (aw_q_.empty()) return std::nullopt;
        return aw_q_.front();
    }
    std::optional<axi::WBeat> peek_w() const {
        if (w_q_.empty()) return std::nullopt;
        return w_q_.front();
    }
    std::optional<axi::ArBeat> peek_ar() const {
        if (ar_q_.empty()) return std::nullopt;
        return ar_q_.front();
    }
    std::optional<axi::BBeat> peek_b() const {
        if (b_q_.empty()) return std::nullopt;
        return b_q_.front();
    }
    std::optional<axi::RBeat> peek_r() const {
        if (r_q_.empty()) return std::nullopt;
        return r_q_.front();
    }

  private:
    void drain_b_from_depacketizer_() {
        while (b_q_.size() < params_.b_queue_depth) {
            auto b = depkt_.pop_b();
            if (!b) break;
            b_q_.push_back(*b);
        }
    }
    void drain_r_from_depacketizer_() {
        while (r_q_.size() < params_.r_queue_depth) {
            auto r = depkt_.pop_r();
            if (!r) break;
            r_q_.push_back(*r);
        }
    }
    // Per-channel pass-through. The packetizer's false return MUST not consume
    // the head — we only pop on a successful push, so a failed forward this
    // tick is safely retried next tick with the identical beat.
    void forward_aw_to_packetizer_() {
        while (!aw_q_.empty()) {
            if (!pkt_.push_aw(aw_q_.front())) break;
            aw_q_.pop_front();
        }
    }
    void forward_w_to_packetizer_() {
        while (!w_q_.empty()) {
            if (!pkt_.push_w(w_q_.front())) break;
            w_q_.pop_front();
        }
    }
    void forward_ar_to_packetizer_() {
        while (!ar_q_.empty()) {
            if (!pkt_.push_ar(ar_q_.front())) break;
            ar_q_.pop_front();
        }
    }

    RequestPacketizer& pkt_;
    ResponseDepacketizer& depkt_;
    PortParams params_;
    std::deque<axi::AwBeat> aw_q_;
    std::deque<axi::WBeat> w_q_;
    std::deque<axi::ArBeat> ar_q_;
    std::deque<axi::BBeat> b_q_;
    std::deque<axi::RBeat> r_q_;
};

}  // namespace ni::cmodel::nmu
