#pragma once
#include "axi/types.hpp"
#include "ni/depacketizer.hpp"
#include "ni/packetizer.hpp"
#include "nmu/addr_trans.hpp"
#include "nmu/packetize.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>

namespace ni::cmodel::nmu {

enum class RobMode { Disabled, Enabled };  // Enabled = next round

// In-line layer between AxiSlavePort and {Packetize, Depacketize}.
// Implements both Packetizer (request gate) and Depacketizer (response observe).
//
// Disabled mode (this round): per-AXI-ID transaction ordering filter.
//   - Same id + same dst -> pass; outstanding deque grows
//   - Same id + different dst -> stall until deque drained
//   - Different id -> independent
//   - Response B/R observe to pop_front per-id deque
//
// Enabled mode (next round): per-AXI-ID reorder buffer + rob_idx allocator.
//   This round leaves Enabled-mode method bodies as assert(false) + std::abort.
//
// Single-threaded tick model: state mutations from Packetizer-side (push_aw/ar)
// and Depacketizer-side (pop_b/r) happen in the same thread, no synchronization.
// Tick order (per AxiSlavePort): drain B/R before forwarding AW/W/AR -> response
// frees IDs in same cycle, request can use freed IDs after.
//
// ROB Disabled mode ONLY handles same-id DIFFERENT-dst stall. Same-id same-dst
// response ordering is guaranteed by AxiMaster's per-id FIFO + NoC's per-pair
// deterministic routing (XYRouting satisfies). ROB Disabled does NOT add that
// ordering -- implicit invariant.
class Rob : public Packetizer, public Depacketizer {
public:
    Rob(Packetize& next_pkt,
        Depacketizer& next_depkt,
        RobMode mode_w,
        RobMode mode_r)
        : next_pkt_(next_pkt), next_depkt_(next_depkt),
          mode_w_(mode_w), mode_r_(mode_r) {}

    // ===== Packetizer interface (request side; B/R assert+abort) =====
    bool push_aw(const axi::AwBeat& b) override;
    bool push_w (const axi::WBeat&  b) override;
    bool push_ar(const axi::ArBeat& b) override;
    bool push_b(const axi::BBeat&) override {
        assert(false && "Rob: push_b not applicable"); std::abort(); return false;
    }
    bool push_r(const axi::RBeat&) override {
        assert(false && "Rob: push_r not applicable"); std::abort(); return false;
    }

    // ===== Depacketizer interface (response side; AW/W/AR assert+abort) =====
    std::optional<axi::BBeat> pop_b() override;
    std::optional<axi::RBeat> pop_r() override;
    std::optional<axi::AwBeat> pop_aw() override {
        assert(false && "Rob: pop_aw not applicable"); std::abort(); return std::nullopt;
    }
    std::optional<axi::WBeat>  pop_w()  override {
        assert(false && "Rob: pop_w not applicable"); std::abort(); return std::nullopt;
    }
    std::optional<axi::ArBeat> pop_ar() override {
        assert(false && "Rob: pop_ar not applicable"); std::abort(); return std::nullopt;
    }

private:
    Packetize&     next_pkt_;
    Depacketizer&  next_depkt_;
    RobMode mode_w_, mode_r_;

    // Per-AXI-ID FIFO of outstanding entries. Disabled mode invariant:
    // for any non-empty deque, all entries share the same dst_id (gate enforces).
    // Forward-compat: OutstandingEntry adds rob_idx field for Enabled mode.
    struct OutstandingEntry {
        uint8_t dst_id;
        // future Enabled mode: uint8_t rob_idx;
    };
    struct WriteState { std::deque<OutstandingEntry> outstanding; };
    struct ReadState  { std::deque<OutstandingEntry> outstanding; };
    std::array<WriteState, 256> write_;
    std::array<ReadState,  256> read_;

    // W burst credit gate: prevents W beats from reaching Packetize before
    // their corresponding AW has been ROB-accepted. Single counter (not per-id)
    // because AXI4 W beats follow AW issue order strictly (no WID).
    uint32_t w_burst_credit_ = 0;
};

// ===== inline impl =====

inline bool Rob::push_aw(const axi::AwBeat& b) {
    if (mode_w_ == RobMode::Enabled) {
        assert(false && "Rob: Enabled mode push_aw not yet implemented (next round)");
        std::abort();
    }
    auto t = addr_trans::xy_route(b.addr);
    auto& s = write_[b.id];
    if (!s.outstanding.empty() && s.outstanding.front().dst_id != t.dst_id) {
        return false;  // stall: same id, different dst
    }
    if (!next_pkt_.push_aw_with_meta(b, {t.dst_id, t.local_addr, 0, 0})) {
        return false;  // downstream backpressure: NO state change
    }
    s.outstanding.push_back({t.dst_id});
    w_burst_credit_++;
    return true;
}

inline bool Rob::push_w(const axi::WBeat& b) {
    if (w_burst_credit_ == 0) return false;  // W cannot proceed before its AW
    if (!next_pkt_.push_w(b)) return false;  // downstream backpressure: NO credit change
    if (b.last) w_burst_credit_--;
    return true;
}

inline bool Rob::push_ar(const axi::ArBeat& b) {
    if (mode_r_ == RobMode::Enabled) {
        assert(false && "Rob: Enabled mode push_ar not yet implemented (next round)");
        std::abort();
    }
    auto t = addr_trans::xy_route(b.addr);
    auto& s = read_[b.id];
    if (!s.outstanding.empty() && s.outstanding.front().dst_id != t.dst_id) {
        return false;
    }
    if (!next_pkt_.push_ar_with_meta(b, {t.dst_id, t.local_addr, 0, 0})) {
        return false;
    }
    s.outstanding.push_back({t.dst_id});
    return true;
}

inline std::optional<axi::BBeat> Rob::pop_b() {
    if (mode_w_ == RobMode::Enabled) {
        assert(false && "Rob: Enabled mode pop_b not yet implemented (next round)");
        std::abort();
    }
    auto opt = next_depkt_.pop_b();
    if (!opt) return std::nullopt;
    auto& s = write_[opt->id];
    assert(!s.outstanding.empty() && "B for id with no outstanding write");
    s.outstanding.pop_front();
    return opt;
}

inline std::optional<axi::RBeat> Rob::pop_r() {
    if (mode_r_ == RobMode::Enabled) {
        assert(false && "Rob: Enabled mode pop_r not yet implemented (next round)");
        std::abort();
    }
    auto opt = next_depkt_.pop_r();
    if (!opt) return std::nullopt;
    if (opt->last) {
        auto& s = read_[opt->id];
        assert(!s.outstanding.empty() && "R(last) for id with no outstanding read");
        s.outstanding.pop_front();
    }
    return opt;
}

}  // namespace ni::cmodel::nmu
