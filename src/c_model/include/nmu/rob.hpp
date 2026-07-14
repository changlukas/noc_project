#pragma once
#include "axi/types.hpp"
#include "ni_flit_constants.h"
#include "ni_params.h"
#include "nmu/addr_trans.hpp"
#include "nmu/packetize.hpp"
#include "request_io.hpp"
#include "response_io.hpp"
#include <algorithm>
#include <array>
#include <bitset>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>
#include <utility>

namespace ni::cmodel::nmu {

enum class RobMode { Disabled, Enabled };

// In-line layer between AxiSlavePort and {Packetize, Depacketize}.
// Implements RequestPacketizer (request gate: push_aw/w/ar) and
// ResponseDepacketizer (response observe: pop_b/r).
//
// Disabled mode: per-AXI-ID single-outstanding interlock.
//   - Any id with one outstanding AW/AR -> stall further same-id requests
//   - Different id -> independent
//   - Response B / R(last) observe clears the per-id outstanding flag
//
// Enabled mode: per-beat slot pool + rob_idx allocator (implemented; the
//   asserts in the pop paths are integrity guards, not stubs).
//
// Single-threaded tick model: state mutations from RequestPacketizer-side (push_aw/ar)
// and ResponseDepacketizer-side (pop_b/r) happen in the same thread, no synchronization.
// Tick order: standalone AxiSlavePort::tick() drains B/R before forwarding AW/W/AR.
// The integrated Nmu::tick() (nmu.hpp) runs the opposite order -- request side
// (push_aw/w/ar via axi_slave_port_.tick_req()) before response drain -- so an id
// freed by this cycle's B/R is not available to this cycle's request side.
//
// RoB-less (Disabled) same-id response ordering is guaranteed by the NMU
// single-outstanding interlock (one transaction in flight per id), not by
// AxiMaster ordering or XY routing. Enabled mode uses rob_idx for per-beat
// reorder buffering and does not rely on this interlock.
// B RoB is unconditional in this model (cheap, no payload) -- a deliberate
// divergence from FlooNoC's optional BRoBType=NoRoB. Only R keeps a Disabled mode.
class Rob : public RequestPacketizer, public ResponseDepacketizer {
  public:
    Rob(NmuPacketizeSink& next_pkt, ResponseDepacketizer& next_depkt, RobMode mode_r,
        addr_trans::SamTable sam, std::size_t b_rob_depth = ni::NMU_ROB_B_DEPTH,
        std::size_t r_rob_depth = ni::NMU_ROB_R_DEPTH,
        std::size_t max_txns_per_id = ni::NMU_MAX_TXNS_PER_ID)
        : next_pkt_(next_pkt),
          next_depkt_(next_depkt),
          mode_r_(mode_r),
          sam_(std::move(sam)),
          b_rob_depth_(b_rob_depth),
          r_rob_depth_(r_rob_depth),
          max_txns_per_id_(max_txns_per_id) {
        assert(b_rob_depth_ >= 1 && b_rob_depth_ <= ROB_IDX_SPACE &&
               "nmu::Rob: b_rob_depth outside [1, ROB_IDX_SPACE]");
        assert(r_rob_depth_ >= 1 && r_rob_depth_ <= ROB_IDX_SPACE &&
               "nmu::Rob: r_rob_depth outside [1, ROB_IDX_SPACE]");
        if (b_rob_depth_ < 1 || b_rob_depth_ > ROB_IDX_SPACE) std::abort();
        if (r_rob_depth_ < 1 || r_rob_depth_ > ROB_IDX_SPACE) std::abort();
        assert(max_txns_per_id_ >= 1 && "nmu::Rob: max_txns_per_id must be positive");
        if (max_txns_per_id_ < 1) std::abort();
    }

    // ===== RequestPacketizer interface =====
    bool push_aw(const axi::AwBeat& b) override;
    bool push_w(const axi::WBeat& b) override;
    bool push_ar(const axi::ArBeat& b) override;

    // ===== ResponseDepacketizer interface =====
    std::optional<axi::BBeat> pop_b() override;
    std::optional<axi::RBeat> pop_r() override;

    struct CommittedBEntry {
        axi::BBeat beat;
        uint8_t rob_idx;
        uint8_t axi_id;
        bool rob_req;  // false => bypassed, owns no slot, must skip commit_b_exit
    };
    struct CommittedREntry {
        axi::RBeat beat;
        uint8_t rob_idx;
        uint8_t axi_id;
        bool rob_req;
    };
    std::optional<CommittedBEntry> pop_b_staged();
    std::optional<CommittedREntry> pop_r_staged();
    void commit_b_exit(uint8_t rob_idx, uint8_t axi_id);
    void commit_r_exit(uint8_t rob_idx, uint8_t axi_id);

    // === Enabled mode public constants (for testing + caller info) ===
    // Addressable range of the rob_idx header field, NOT the pool depth.
    // Pool depths are b_rob_depth_ / r_rob_depth_ and may be smaller.
    static constexpr std::size_t ROB_IDX_SPACE = 1u << ni::header::ROB_IDX_WIDTH;  // 256
    // AXI ID space alias — single source of truth lives in axi::AXI_ID_SPACE.
    static constexpr std::size_t AXI_ID_SPACE = axi::AXI_ID_SPACE;  // 256

    std::size_t b_rob_depth() const noexcept { return b_rob_depth_; }
    std::size_t r_rob_depth() const noexcept { return r_rob_depth_; }
    std::size_t max_txns_per_id() const noexcept { return max_txns_per_id_; }

    // lzc over the allocation bitmap (floo_rob.sv:155-164). Free space is what lies
    // above the high-water mark; the next base is the index just past it.
    std::size_t write_free_space() const noexcept {
        const int msb = highest_set(alloc_write_, b_rob_depth_);
        return msb < 0 ? b_rob_depth_ : b_rob_depth_ - 1u - static_cast<std::size_t>(msb);
    }
    std::size_t read_free_space() const noexcept {
        const int msb = highest_set(alloc_read_, r_rob_depth_);
        return msb < 0 ? r_rob_depth_ : r_rob_depth_ - 1u - static_cast<std::size_t>(msb);
    }

    // Test introspection: current read outstanding-entry count summed over all ids
    // (the R Disabled-mode per-id interlock flags). Writes are always RoB'd, so there
    // is no write interlock to count.
    std::size_t read_occupancy() const {
        std::size_t n = 0;
        for (bool s : read_outstanding_) n += s ? 1 : 0;
        return n;
    }
    // High-water mark of occupied R RoB slots, updated on every AR that
    // allocates a slot in Enabled mode. Measurement-only; no behaviour effect.
    std::size_t read_slot_hwm() const noexcept { return read_slot_hwm_; }

  private:
    // Drain ready order-list heads into the committed queues (the pop-side loops,
    // reused by the direct-forward bypass arms). Stops at a bypassed head, which
    // waits on its own response through pop_*_staged.
    void drain_ready_write_heads_(uint8_t id);
    void drain_ready_read_heads_(uint8_t id);

    NmuPacketizeSink& next_pkt_;
    ResponseDepacketizer& next_depkt_;
    RobMode mode_r_;
    addr_trans::SamTable sam_;
    std::size_t b_rob_depth_;
    std::size_t r_rob_depth_;
    std::size_t max_txns_per_id_;

    // Per-AXI-ID single-outstanding flag for the R Disabled path. True while one AR
    // is in flight for that id; cleared by R(last) in pop_r.
    std::array<bool, axi::AXI_ID_SPACE> read_outstanding_{};

    // AW-before-W interlock: AW-accepted bursts whose W beats are still owed.
    // Prevents W beats from reaching Packetize before their corresponding AW
    // has been ROB-accepted. Single counter (not per-id) because AXI4 W beats
    // follow AW issue order strictly (no WID).
    uint32_t w_bursts_owed_ = 0;

    // === Enabled mode (per-beat slot pool) ===

    struct WriteEntry {
        bool occupied = false;
        bool ready = false;
        uint8_t axi_id = 0;
        axi::BBeat b_beat = {};
    };
    struct ReadEntry {
        bool occupied = false;
        bool ready = false;
        uint8_t axi_id = 0;
        axi::RBeat r_beat = {};
    };
    std::array<WriteEntry, ROB_IDX_SPACE> write_entries_;
    std::array<ReadEntry, ROB_IDX_SPACE> read_entries_;

    // FlooNoC's rob_alloc_q (floo_rob.sv:146): one bit per allocated RANGE, set at
    // the range's top index. Free space is the leading-zero count above it, so the
    // pool behaves as a stack: space returns only from the top.
    std::bitset<ROB_IDX_SPACE> alloc_write_;
    std::bitset<ROB_IDX_SPACE> alloc_read_;

    // Highest set bit below `depth`, or -1 if none. std::bitset has no such query.
    // Scanning above `depth` would be wrong as well as wasteful: a stray bit there
    // makes `depth - 1 - msb` underflow, and std::size_t underflow is silent.
    // Allocation never sets one (base + n - 1 <= depth - 1), so the bound is also
    // the assertion.
    static int highest_set(const std::bitset<ROB_IDX_SPACE>& b, std::size_t depth) {
        for (std::size_t i = depth; i-- > 0;) {
            if (b.test(i)) return static_cast<int>(i);
        }
        return -1;
    }

    // Per-id ordered range list. AW = {base, 1}; AR = {base, len+1}.
    // rob_req == false: bypassed, no slot reserved, base is meaningless.
    struct BeatRange {
        uint8_t base;
        uint16_t len_plus_1;  // up to 256
        bool rob_req;
    };
    std::array<std::deque<BeatRange>, AXI_ID_SPACE> write_order_by_id_;
    std::array<std::deque<BeatRange>, AXI_ID_SPACE> read_order_by_id_;

    // Same-destination bypass state (floo_rob.sv:399,417-420,427-428). prev_dest_* is
    // the dst_id of the last accepted push for that id, updated on every push.
    // fallen_back_* is the sticky same-destination-bypass flag: once a push
    // allocates a RoB slot, every later push for that id allocates one too (even
    // if dest matches an earlier one) until a new streak begins. The reset is the idle-ID bypass
    // branch itself: an id's first push (empty order list) sets fallen_back_*=false, so the flag is
    // fresh at every streak start. FlooNoC instead clears at drain (floo_rob.sv:435-441)
    // because its idle-ID bypass READS the sticky flag (!ax_rob_req_q); ours tests the empty
    // list, which makes a drain-time clear a dead store -- so it is omitted here.
    std::array<uint8_t, AXI_ID_SPACE> prev_dest_write_{};
    std::array<uint8_t, AXI_ID_SPACE> prev_dest_read_{};
    std::array<bool, AXI_ID_SPACE> fallen_back_write_{};
    std::array<bool, AXI_ID_SPACE> fallen_back_read_{};

    // Per-base (keyed by rob_idx base) arrival counter. NSU stamps every
    // R beat of a burst with rob_idx=base; this counter positions beat i at base+i.
    // Reset when the range is popped from read_order_by_id_ (ties counter lifecycle
    // to slot-reuse eligibility). read_range_len_[base] = burst length n, set in
    // push_ar, used to bound the counter (beat past burst length is malformed).
    std::array<uint16_t, ROB_IDX_SPACE> read_arrival_offset_{};
    std::array<uint16_t, ROB_IDX_SPACE> read_range_len_{};

    // Beats of the head burst released so far, keyed by range base. FlooNoC keys the
    // same counter by ID (read_rob_idx_offset_q, floo_rob.sv:177-180); a range belongs
    // to exactly one ID, so keying by base carries the same information. Distinct from
    // read_arrival_offset_: arrival places incoming beats, release tracks how many left.
    std::array<uint16_t, ROB_IDX_SPACE> read_release_offset_{};

    // High-water mark backing read_slot_hwm(). See getter for details.
    std::size_t read_slot_hwm_ = 0;

    // Ready-to-emit beats drained by pop_b / pop_r.
    std::deque<CommittedBEntry> committed_b_queue_;
    std::deque<CommittedREntry> committed_r_queue_;
    std::array<uint8_t, ROB_IDX_SPACE> committed_b_pending_{};
    std::array<uint8_t, ROB_IDX_SPACE> committed_r_pending_{};
};

// ===== inline impl =====

inline bool Rob::push_aw(const axi::AwBeat& b) {
    // ax_gnt_o: the per-id order list is FlooNoC's status FIFO (floo_rob.sv:414).
    if (write_order_by_id_[b.id].size() >= max_txns_per_id_) return false;
    auto t = sam_.translate(b.addr);
    const uint8_t dst = t.dst_id;
    const bool empty = write_order_by_id_[b.id].empty();
    bool needs_rob;
    bool fallen_back;  // trial value; committed only once the push is accepted
    if (empty) {
        // Idle-ID bypass: nothing in flight for this id, so nothing can overtake this
        // response. No reorder storage needed. Ported from floo_rob.sv:422-425.
        needs_rob = false;
        fallen_back = false;  // fresh streak
    } else if (!fallen_back_write_[b.id] && dst == prev_dest_write_[b.id]) {
        // Same-destination bypass: same dest as the previous same-id push, not yet reordering.
        // Ported from floo_rob.sv:427-428.
        needs_rob = false;
        fallen_back = false;
    } else {
        // Ported from floo_rob.sv:430-432.
        needs_rob = true;
        fallen_back = true;  // sticky
    }
    std::size_t base = 0;
    if (needs_rob) {
        if (write_free_space() < 1) return false;
        base = b_rob_depth_ - write_free_space();
    }
    if (!next_pkt_.push_aw_with_meta(
            b, {t.dst_id, t.local_addr, static_cast<uint8_t>(needs_rob ? 1 : 0),
                static_cast<uint8_t>(needs_rob ? base : 0)})) {
        return false;  // downstream backpressure: no state mutation
    }
    prev_dest_write_[b.id] = dst;  // updated on every accepted push (floo_rob.sv:417-420)
    fallen_back_write_[b.id] = fallen_back;
    if (needs_rob) {
        alloc_write_.set(base);  // a 1-slot range: base is its own top
        write_entries_[base] = WriteEntry{/*occupied=*/true, /*ready=*/false, b.id, /*b_beat=*/{}};
    }
    write_order_by_id_[b.id].push_back({static_cast<uint8_t>(base), 1, needs_rob});
    ++w_bursts_owed_;
    return true;
}

inline bool Rob::push_w(const axi::WBeat& b) {
    if (w_bursts_owed_ == 0) return false;   // W cannot proceed before its AW
    if (!next_pkt_.push_w(b)) return false;  // downstream backpressure: NO change to owed count
    if (b.last) w_bursts_owed_--;
    return true;
}

inline bool Rob::push_ar(const axi::ArBeat& b) {
    if (mode_r_ == RobMode::Enabled) {
        if (read_order_by_id_[b.id].size() >= max_txns_per_id_) return false;
        const std::size_t n = static_cast<std::size_t>(b.len) + 1u;
        auto t = sam_.translate(b.addr);
        const uint8_t dst = t.dst_id;
        const bool empty = read_order_by_id_[b.id].empty();
        bool needs_rob;
        bool fallen_back;  // trial value; committed only once the push is accepted
        if (empty) {
            // Idle-ID bypass: an idle id's burst cannot be overtaken, so it needs no slots.
            // A bypassed burst of any length is admissible. Ported from floo_rob.sv:422-425.
            needs_rob = false;
            fallen_back = false;  // fresh streak
        } else if (!fallen_back_read_[b.id] && dst == prev_dest_read_[b.id]) {
            // Same-destination bypass: same dest as the previous same-id push, not yet reordering.
            // Ported from floo_rob.sv:427-428.
            needs_rob = false;
            fallen_back = false;
        } else {
            // Ported from floo_rob.sv:430-432.
            needs_rob = true;
            fallen_back = true;  // sticky
        }
        std::size_t base = 0;
        if (needs_rob) {
            if (read_free_space() < n) return false;  // subsumes the old n > r_rob_depth_ check
            base = r_rob_depth_ - read_free_space();
        }
        if (!next_pkt_.push_ar_with_meta(
                b, {t.dst_id, t.local_addr, static_cast<uint8_t>(needs_rob ? 1 : 0),
                    static_cast<uint8_t>(needs_rob ? base : 0)})) {
            return false;  // downstream backpressure: no state mutation
        }
        prev_dest_read_[b.id] = dst;  // updated on every accepted push (floo_rob.sv:417-420)
        fallen_back_read_[b.id] = fallen_back;
        if (needs_rob) {
            for (std::size_t i = 0; i < n; ++i) {
                read_entries_[base + i] =
                    ReadEntry{/*occupied=*/true, /*ready=*/false, b.id, /*r_beat=*/{}};
            }
            alloc_read_.set(base + n - 1);  // only the range TOP is marked
            read_slot_hwm_ =
                std::max<std::size_t>(read_slot_hwm_, r_rob_depth_ - read_free_space());
            read_range_len_[base] = static_cast<uint16_t>(n);
        }
        read_order_by_id_[b.id].push_back(
            {static_cast<uint8_t>(base), static_cast<uint16_t>(n), needs_rob});
        return true;
    }
    auto t = sam_.translate(b.addr);
    if (read_outstanding_[b.id]) return false;  // single-outstanding per id
    if (!next_pkt_.push_ar_with_meta(b, {t.dst_id, t.local_addr, 0, 0})) {
        return false;
    }
    read_outstanding_[b.id] = true;
    return true;
}

inline std::optional<axi::BBeat> Rob::pop_b() {
    auto out = pop_b_staged();
    if (!out) return std::nullopt;
    if (out->rob_req) commit_b_exit(out->rob_idx, out->axi_id);
    return out->beat;
}

inline std::optional<axi::RBeat> Rob::pop_r() {
    if (mode_r_ == RobMode::Enabled) {
        auto out = pop_r_staged();
        if (!out) return std::nullopt;
        if (out->rob_req) commit_r_exit(out->rob_idx, out->axi_id);
        return out->beat;
    }
    auto opt = next_depkt_.pop_r();
    if (!opt) return std::nullopt;
    if (opt->last) {
        assert(read_outstanding_[opt->id] && "R(last) for id with no outstanding read");
        read_outstanding_[opt->id] = false;
    }
    return opt;
}

inline void Rob::drain_ready_write_heads_(uint8_t id) {
    while (!write_order_by_id_[id].empty()) {
        const BeatRange head = write_order_by_id_[id].front();
        if (!head.rob_req) break;  // waiting on a bypassed response
        if (!write_entries_[head.base].ready) break;
        committed_b_queue_.push_back({write_entries_[head.base].b_beat, head.base, id, true});
        ++committed_b_pending_[head.base];
        write_order_by_id_[id].pop_front();
    }
}

inline void Rob::drain_ready_read_heads_(uint8_t id) {
    while (!read_order_by_id_[id].empty()) {
        const BeatRange head = read_order_by_id_[id].front();
        if (!head.rob_req) break;  // waiting on a bypassed response
        uint16_t& release_off = read_release_offset_[head.base];
        while (release_off < head.len_plus_1 && read_entries_[head.base + release_off].ready) {
            const std::size_t idx = static_cast<std::size_t>(head.base) + release_off;
            committed_r_queue_.push_back(
                {read_entries_[idx].r_beat, static_cast<uint8_t>(idx), id, true});
            ++committed_r_pending_[idx];
            ++release_off;
        }
        if (release_off < head.len_plus_1) break;  // burst not drained yet
        read_arrival_offset_[head.base] = 0;
        release_off = 0;
        read_order_by_id_[id].pop_front();
    }
}

inline std::optional<Rob::CommittedBEntry> Rob::pop_b_staged() {
    if (!committed_b_queue_.empty()) {
        auto b = committed_b_queue_.front();
        committed_b_queue_.pop_front();
        return b;
    }
    auto opt = next_depkt_.pop_b_with_meta();
    if (!opt) return std::nullopt;
    auto [b, meta] = *opt;
    if (meta.rob_req == 0) {
        // Bypassed B: by the head invariant it is the head of its id's order list.
        const uint8_t id = b.id;
        if (write_order_by_id_[id].empty() || write_order_by_id_[id].front().rob_req) {
            assert(false && "bypassed B does not match the head of its id's order list");
            std::abort();
        }
        write_order_by_id_[id].pop_front();
        drain_ready_write_heads_(id);  // the entry behind it may already be ready
        return CommittedBEntry{b, 0, id, /*rob_req=*/false};
    }
    if (!(meta.rob_idx < ROB_IDX_SPACE)) {
        assert(false && "rob_idx out of range");
        std::abort();
    }
    auto& slot = write_entries_[meta.rob_idx];
    if (!(slot.occupied && !slot.ready)) {
        assert(false && "B for unallocated or already-completed rob_idx");
        std::abort();
    }
    slot.b_beat = b;
    slot.ready = true;
    uint8_t id = slot.axi_id;
    drain_ready_write_heads_(id);
    if (committed_b_queue_.empty()) return std::nullopt;
    auto out = committed_b_queue_.front();
    committed_b_queue_.pop_front();
    return out;
}

inline std::optional<Rob::CommittedREntry> Rob::pop_r_staged() {
    if (mode_r_ != RobMode::Enabled) return std::nullopt;
    if (!committed_r_queue_.empty()) {
        auto r = committed_r_queue_.front();
        committed_r_queue_.pop_front();
        return r;
    }
    auto opt = next_depkt_.pop_r_with_meta();
    if (!opt) return std::nullopt;
    auto [r, meta] = *opt;
    if (meta.rob_req == 0) {
        // Bypassed R streams beat by beat; the list entry pops on last, and only
        // then can a robbed entry behind it become releasable.
        const uint8_t id = r.id;
        if (read_order_by_id_[id].empty() || read_order_by_id_[id].front().rob_req) {
            assert(false && "bypassed R does not match the head of its id's order list");
            std::abort();
        }
        if (r.last) {
            read_order_by_id_[id].pop_front();
            drain_ready_read_heads_(id);
        }
        return CommittedREntry{r, 0, id, /*rob_req=*/false};
    }
    if (!(meta.rob_idx < ROB_IDX_SPACE)) {
        assert(false && "rob_idx out of range");
        std::abort();
    }
    uint8_t base = meta.rob_idx;
    std::size_t arrival_offset = read_arrival_offset_[base];
    if (!(arrival_offset < read_range_len_[base])) {
        assert(false &&
               "nmu::Rob::pop_r_staged: R beat past the burst's reserved slot range "
               "-- malformed burst");
        std::abort();
    }
    std::size_t slot_idx = static_cast<std::size_t>(base) + arrival_offset;
    if (!(slot_idx < ROB_IDX_SPACE)) {
        assert(false && "computed read slot out of range");
        std::abort();
    }
    auto& slot = read_entries_[slot_idx];
    if (!(slot.occupied && !slot.ready)) {
        assert(false && "computed read slot unallocated or already filled");
        std::abort();
    }
    slot.r_beat = r;
    slot.ready = true;
    ++read_arrival_offset_[base];
    uint8_t id = slot.axi_id;
    drain_ready_read_heads_(id);
    if (committed_r_queue_.empty()) return std::nullopt;
    auto out = committed_r_queue_.front();
    committed_r_queue_.pop_front();
    return out;
}

inline void Rob::commit_b_exit(uint8_t rob_idx, uint8_t axi_id) {
    assert(rob_idx < ROB_IDX_SPACE);
    assert(committed_b_pending_[rob_idx] > 0);
    --committed_b_pending_[rob_idx];
    if (committed_b_pending_[rob_idx] == 0) {
        alloc_write_.reset(rob_idx);  // no-op unless rob_idx is a range top
        write_entries_[rob_idx] = WriteEntry{};
    }
}

inline void Rob::commit_r_exit(uint8_t rob_idx, uint8_t axi_id) {
    assert(rob_idx < ROB_IDX_SPACE);
    assert(committed_r_pending_[rob_idx] > 0);
    --committed_r_pending_[rob_idx];
    if (committed_r_pending_[rob_idx] == 0) {
        alloc_read_.reset(rob_idx);  // no-op unless rob_idx is a range top
        read_entries_[rob_idx] = ReadEntry{};
    }
}

}  // namespace ni::cmodel::nmu
