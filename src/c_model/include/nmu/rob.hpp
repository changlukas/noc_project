#pragma once
#include "axi/types.hpp"
#include "ni_flit_constants.h"
#include "nmu/addr_trans.hpp"
#include "nmu/packetize.hpp"
#include "request_io.hpp"
#include "response_io.hpp"
#include <array>
#include <bitset>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>
#include <utility>

namespace ni::cmodel::nmu {

enum class RobMode { Disabled, Enabled };  // Enabled = next round

// In-line layer between AxiSlavePort and {Packetize, Depacketize}.
// Implements RequestPacketizer (request gate: push_aw/w/ar) and
// ResponseDepacketizer (response observe: pop_b/r).
//
// Disabled mode (this round): per-AXI-ID single-outstanding interlock.
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
class Rob : public RequestPacketizer, public ResponseDepacketizer {
  public:
    Rob(NmuPacketizeSink& next_pkt, ResponseDepacketizer& next_depkt, RobMode mode_w,
        RobMode mode_r, addr_trans::SamTable sam, std::size_t b_rob_depth = 32,
        std::size_t r_rob_depth = 32)
        : next_pkt_(next_pkt),
          next_depkt_(next_depkt),
          mode_w_(mode_w),
          mode_r_(mode_r),
          sam_(std::move(sam)),
          b_rob_depth_(b_rob_depth),
          r_rob_depth_(r_rob_depth) {
        assert(b_rob_depth_ >= 1 && b_rob_depth_ <= ROB_IDX_SPACE &&
               "nmu::Rob: b_rob_depth outside [1, ROB_IDX_SPACE]");
        assert(r_rob_depth_ >= 1 && r_rob_depth_ <= ROB_IDX_SPACE &&
               "nmu::Rob: r_rob_depth outside [1, ROB_IDX_SPACE]");
        if (b_rob_depth_ < 1 || b_rob_depth_ > ROB_IDX_SPACE) std::abort();
        if (r_rob_depth_ < 1 || r_rob_depth_ > ROB_IDX_SPACE) std::abort();
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
    };
    struct CommittedREntry {
        axi::RBeat beat;
        uint8_t rob_idx;
        uint8_t axi_id;
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

    // Test introspection: current outstanding-entry count summed over all ids.
    // Counts Disabled-mode per-id flags (one per id at most). Enabled-mode
    // slot-pool occupancy (write_entries_) is not counted -- extend here if/when
    // Enabled mode is exercised.
    std::size_t write_occupancy() const {
        std::size_t n = 0;
        for (bool s : write_outstanding_) n += s ? 1 : 0;
        return n;
    }
    std::size_t read_occupancy() const {
        std::size_t n = 0;
        for (bool s : read_outstanding_) n += s ? 1 : 0;
        return n;
    }

  private:
    NmuPacketizeSink& next_pkt_;
    ResponseDepacketizer& next_depkt_;
    RobMode mode_w_, mode_r_;
    addr_trans::SamTable sam_;
    std::size_t b_rob_depth_;
    std::size_t r_rob_depth_;

    // Per-AXI-ID single-outstanding flag. True while one AW/AR is in flight for
    // that id; cleared by B (for writes) or R(last) (for reads) in pop_b/pop_r.
    std::array<bool, axi::AXI_ID_SPACE> write_outstanding_{};
    std::array<bool, axi::AXI_ID_SPACE> read_outstanding_{};

    // W burst credit gate: prevents W beats from reaching Packetize before
    // their corresponding AW has been ROB-accepted. Single counter (not per-id)
    // because AXI4 W beats follow AW issue order strictly (no WID).
    uint32_t w_burst_credit_ = 0;

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
    struct BeatRange {
        uint8_t base;
        uint16_t len_plus_1;  // up to 256
    };
    std::array<std::deque<BeatRange>, AXI_ID_SPACE> write_order_by_id_;
    std::array<std::deque<BeatRange>, AXI_ID_SPACE> read_order_by_id_;

    // Family C: per-base (keyed by rob_idx base) arrival counter. NSU stamps every
    // R beat of a burst with rob_idx=base; this counter positions beat i at base+i.
    // Reset when the range is popped from read_order_by_id_ (ties counter lifecycle
    // to slot-reuse eligibility). read_range_len_[base] = burst length n, set in
    // push_ar, used to bound the counter (beat past burst length is malformed).
    std::array<uint16_t, ROB_IDX_SPACE> read_arrival_offset_{};
    std::array<uint16_t, ROB_IDX_SPACE> read_range_len_{};

    // Ready-to-emit beats drained by pop_b / pop_r (Task 3).
    std::deque<CommittedBEntry> committed_b_queue_;
    std::deque<CommittedREntry> committed_r_queue_;
    std::array<uint8_t, ROB_IDX_SPACE> committed_b_pending_{};
    std::array<uint8_t, ROB_IDX_SPACE> committed_r_pending_{};
};

// ===== inline impl =====

inline bool Rob::push_aw(const axi::AwBeat& b) {
    if (mode_w_ == RobMode::Enabled) {
        if (write_free_space() < 1) return false;
        const std::size_t base = b_rob_depth_ - write_free_space();
        auto t = sam_.translate(b.addr);
        if (!next_pkt_.push_aw_with_meta(b, {t.dst_id, t.local_addr, /*rob_req=*/1,
                                             /*rob_idx=*/static_cast<uint8_t>(base)})) {
            return false;  // downstream backpressure: no state mutation
        }
        alloc_write_.set(base);  // a 1-slot range: base is its own top
        write_entries_[base] = WriteEntry{/*occupied=*/true, /*ready=*/false, b.id, /*b_beat=*/{}};
        write_order_by_id_[b.id].push_back({static_cast<uint8_t>(base), 1});
        ++w_burst_credit_;
        return true;
    }
    auto t = sam_.translate(b.addr);
    if (write_outstanding_[b.id]) return false;  // single-outstanding per id
    if (!next_pkt_.push_aw_with_meta(b, {t.dst_id, t.local_addr, 0, 0})) {
        return false;  // downstream backpressure: NO state change
    }
    write_outstanding_[b.id] = true;
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
        const std::size_t n = static_cast<std::size_t>(b.len) + 1u;
        if (read_free_space() < n) return false;  // subsumes the old n > r_rob_depth_ check
        const std::size_t base = r_rob_depth_ - read_free_space();
        auto t = sam_.translate(b.addr);
        if (!next_pkt_.push_ar_with_meta(b, {t.dst_id, t.local_addr, /*rob_req=*/1,
                                             /*rob_idx=*/static_cast<uint8_t>(base)})) {
            return false;  // downstream backpressure: no state mutation
        }
        for (std::size_t i = 0; i < n; ++i) {
            read_entries_[base + i] =
                ReadEntry{/*occupied=*/true, /*ready=*/false, b.id, /*r_beat=*/{}};
        }
        alloc_read_.set(base + n - 1);  // only the range TOP is marked
        read_range_len_[base] = static_cast<uint16_t>(n);
        read_order_by_id_[b.id].push_back({static_cast<uint8_t>(base), static_cast<uint16_t>(n)});
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
    if (mode_w_ == RobMode::Enabled) {
        auto out = pop_b_staged();
        if (!out) return std::nullopt;
        commit_b_exit(out->rob_idx, out->axi_id);
        return out->beat;
    }
    auto opt = next_depkt_.pop_b();
    if (!opt) return std::nullopt;
    assert(write_outstanding_[opt->id] && "B for id with no outstanding write");
    write_outstanding_[opt->id] = false;
    return opt;
}

inline std::optional<axi::RBeat> Rob::pop_r() {
    if (mode_r_ == RobMode::Enabled) {
        auto out = pop_r_staged();
        if (!out) return std::nullopt;
        commit_r_exit(out->rob_idx, out->axi_id);
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

inline std::optional<Rob::CommittedBEntry> Rob::pop_b_staged() {
    if (mode_w_ != RobMode::Enabled) return std::nullopt;
    if (!committed_b_queue_.empty()) {
        auto b = committed_b_queue_.front();
        committed_b_queue_.pop_front();
        return b;
    }
    auto opt = next_depkt_.pop_b_with_meta();
    if (!opt) return std::nullopt;
    auto [b, meta] = *opt;
    if (!(meta.rob_req == 1)) {
        assert(false && "Enabled Rob received Disabled flit");
        std::abort();
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
    while (!write_order_by_id_[id].empty()) {
        BeatRange head = write_order_by_id_[id].front();
        if (!write_entries_[head.base].ready) break;
        committed_b_queue_.push_back({write_entries_[head.base].b_beat, head.base, id});
        ++committed_b_pending_[head.base];
        write_order_by_id_[id].pop_front();
    }
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
    if (!(meta.rob_req == 1)) {
        assert(false && "Enabled Rob received Disabled flit");
        std::abort();
    }
    if (!(meta.rob_idx < ROB_IDX_SPACE)) {
        assert(false && "rob_idx out of range");
        std::abort();
    }
    uint8_t base = meta.rob_idx;
    std::size_t arrival_offset = read_arrival_offset_[base];
    if (!(arrival_offset < read_range_len_[base])) {
        assert(false &&
               "nmu::Rob::pop_r_staged: R beat past burst length (Family C: "
               "more beats than reserved for this base -- malformed burst)");
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
    while (!read_order_by_id_[id].empty()) {
        BeatRange head = read_order_by_id_[id].front();
        bool all_ready = true;
        for (std::size_t i = 0; i < head.len_plus_1; ++i) {
            if (!read_entries_[head.base + i].ready) {
                all_ready = false;
                break;
            }
        }
        if (!all_ready) break;
        for (std::size_t i = 0; i < head.len_plus_1; ++i) {
            const std::size_t idx = static_cast<std::size_t>(head.base) + i;
            committed_r_queue_.push_back(
                {read_entries_[idx].r_beat, static_cast<uint8_t>(idx), id});
            ++committed_r_pending_[idx];
        }
        read_arrival_offset_[head.base] = 0;
        read_order_by_id_[id].pop_front();
    }
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
