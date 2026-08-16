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

// The read RoB mode, from where it is defined: nmu.READ_ROB_ENABLED in
// specgen/source/constants.yaml. The testbench parameter READ_ROB_ENABLED
// reads the SV half of the same definition, so a build cannot come out with
// the model in one mode and the top in the other.
inline constexpr RobMode DEFAULT_ROB_MODE =
    ni::NMU_READ_ROB_ENABLED ? RobMode::Enabled : RobMode::Disabled;

// In-line layer between AxiSlavePort and {Packetize, Depacketize}.
// Implements RequestPacketizer (request gate: push_aw/w/ar) and
// ResponseDepacketizer (response observe: pop_b/r).
//
// Disabled mode: per-AXI-ID single-outstanding interlock.
//   - Any id with one outstanding AW/AR -> stall further same-id requests
//   - Different id -> independent
//   - Response B / R(last) observe clears the per-id outstanding flag
//
// Enabled mode: per-beat slot pool + ordering_tag allocator (implemented; the
//   asserts in the pop paths are integrity guards, not stubs).
//
// Both modes: no aggregate outstanding-transaction pool. The per-id order list
// is the admission bound, so in-flight requests cap at max_txns_per_id_ x
// 2**AXI_ID_WIDTH = 32 x 8 = 256 writes, and at 1 x 8 = 8 reads in
// RobMode::Disabled where the per-id single-outstanding interlock applies.
// write_txns_ / read_txns_ still count the in-flight transactions per direction,
// but only as the unmatched-response guard (retire_b / retire_r) and as the
// high-water measurement.
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
// AxiMaster ordering or XY routing. Enabled mode uses ordering_tag for per-beat
// reorder buffering and does not rely on this interlock.
// B RoB is unconditional in this model (cheap, no payload) -- a deliberate
// divergence from FlooNoC's optional BRoBType=NoRoB. Only R keeps a Disabled mode.
class Rob : public RequestPacketizer, public ResponseDepacketizer {
  public:
    // port_id is this NMU's own boundary port, needed by collective_translate to
    // check that a collective is issued from a tile and not from a peripheral.
    // It sits last, after the depths, so the existing positional call sites keep
    // their meaning; every assembly site (Nmu's constructor) passes cfg_.port_id,
    // and the default names the router's LOCAL port, which is a tile.
    Rob(NmuPacketizeSink& next_pkt, ResponseDepacketizer& next_depkt, RobMode mode_r,
        addr_trans::SamTable sam, std::size_t b_rob_depth = ni::NMU_ROB_B_DEPTH,
        std::size_t r_rob_depth = ni::NMU_ROB_R_DEPTH,
        std::size_t max_txns_per_id = ni::NMU_MAX_TXNS_PER_ID, uint8_t port_id = 0)
        : next_pkt_(next_pkt),
          next_depkt_(next_depkt),
          mode_r_(mode_r),
          sam_(std::move(sam)),
          b_rob_depth_(b_rob_depth),
          r_rob_depth_(r_rob_depth),
          max_txns_per_id_(max_txns_per_id),
          port_id_(port_id) {
        assert(b_rob_depth_ >= 1 && b_rob_depth_ <= ORDERING_TAG_SPACE &&
               "nmu::Rob: b_rob_depth outside [1, ORDERING_TAG_SPACE]");
        assert(r_rob_depth_ >= 1 && r_rob_depth_ <= ORDERING_TAG_SPACE &&
               "nmu::Rob: r_rob_depth outside [1, ORDERING_TAG_SPACE]");
        if (b_rob_depth_ < 1 || b_rob_depth_ > ORDERING_TAG_SPACE) std::abort();
        if (r_rob_depth_ < 1 || r_rob_depth_ > ORDERING_TAG_SPACE) std::abort();
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
        uint8_t ordering_tag;
        uint8_t axi_id;
        bool ordering_req;  // false => bypassed, owns no slot, must skip commit_b_exit
    };
    struct CommittedREntry {
        axi::RBeat beat;
        uint8_t ordering_tag;
        uint8_t axi_id;
        bool ordering_req;
    };
    std::optional<CommittedBEntry> pop_b_staged();
    std::optional<CommittedREntry> pop_r_staged();
    void commit_b_exit(uint8_t ordering_tag, uint8_t axi_id);
    void commit_r_exit(uint8_t ordering_tag, uint8_t axi_id);

    // Transaction retirement, at the AXI-side response acceptance point and nowhere
    // else (floo_meta_buffer.sv:205-206,210 pops on the AXI response handshake). One
    // event decrements the in-flight count, releases the RoB slot when the response
    // owns one, and -- Disabled mode only -- clears the per-id single-outstanding flag.
    // Callers: Nmu::push_rsp_{b,r}_to_axi_ integrated; Rob::pop_{b,r} standalone, where
    // AxiSlavePort pops only into a slot it already has room for.
    // R retires on its last beat: a burst is one transaction, n RoB slots.
    void retire_b(bool ordering_req, uint8_t ordering_tag, uint8_t axi_id);
    void retire_r(bool ordering_req, uint8_t ordering_tag, uint8_t axi_id, bool last);

    // Non-retiring RoBless R pop for a pipeline that retires downstream (Nmu).
    std::optional<axi::RBeat> pop_r_robless();

    // === Enabled mode public constants (for testing + caller info) ===
    // Addressable range of the ordering_tag header field, NOT the pool depth.
    // Pool depths are b_rob_depth_ / r_rob_depth_ and may be smaller.
    static constexpr std::size_t ORDERING_TAG_SPACE = 1u << ni::header::ORDERING_TAG_WIDTH;  // 256
    // AXI ID space alias — single source of truth lives in axi::AXI_ID_SPACE.
    static constexpr std::size_t AXI_ID_SPACE = axi::AXI_ID_SPACE;  // 8

    std::size_t b_rob_depth() const noexcept { return b_rob_depth_; }
    std::size_t r_rob_depth() const noexcept { return r_rob_depth_; }
    std::size_t max_txns_per_id() const noexcept { return max_txns_per_id_; }

    // In-flight transaction count, per direction. Test introspection.
    std::size_t write_txns() const noexcept { return write_txns_; }
    std::size_t read_txns() const noexcept { return read_txns_; }

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

    // Admission clause counts (spec SPEC 17 / Section 2.5): every ACCEPTED push
    // lands in exactly one of the three branches, so the three sum to the
    // accepted-push count for that direction. AW and AR are counted separately
    // because they answer different questions -- the R-RoB high-water mark moves
    // with the AR split alone, and the AR path classifies only in
    // RobMode::Enabled (Disabled reads take the single-outstanding interlock and
    // are counted nowhere). Measurement-only; no behaviour effect.
    std::size_t aw_idle_bypass_count() const noexcept { return aw_idle_bypass_count_; }
    std::size_t aw_same_dest_bypass_count() const noexcept { return aw_same_dest_bypass_count_; }
    std::size_t aw_fallback_alloc_count() const noexcept { return aw_fallback_alloc_count_; }
    std::size_t ar_idle_bypass_count() const noexcept { return ar_idle_bypass_count_; }
    std::size_t ar_same_dest_bypass_count() const noexcept { return ar_same_dest_bypass_count_; }
    std::size_t ar_fallback_alloc_count() const noexcept { return ar_fallback_alloc_count_; }

    // Deepest any single per-id order list got, over both directions -- the one
    // number max_txns_per_id_ bounds. Measurement-only; no behaviour effect.
    std::size_t order_list_hwm() const noexcept { return order_list_hwm_; }

    // Peak in-flight transaction count per direction. Nothing bounds it directly;
    // it is bounded transitively by max_txns_per_id_ x AXI_ID_SPACE.
    // Measurement-only; no behaviour effect.
    std::size_t write_txns_hwm() const noexcept { return write_txns_hwm_; }
    std::size_t read_txns_hwm() const noexcept { return read_txns_hwm_; }

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
    uint8_t port_id_;

    // In-flight transaction count, one per direction, summed over all AXI ids.
    // Incremented on an accepted request, decremented at response retire. Admits
    // nothing and refuses nothing: the per-id order list is the admission bound.
    // What these are for is the retire-side unmatched-response check and the
    // high-water marks below.
    std::size_t write_txns_ = 0;
    std::size_t read_txns_ = 0;

    // Per-AXI-ID single-outstanding flag for the R Disabled path. True while one AR
    // is in flight for that id; cleared by R(last) in retire_r.
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
        // Narrow-class byte lane for this beat (axi::narrow_lane, computed at
        // push_ar from the AR basis -- all n beats' addresses are known
        // upfront for a robbed burst). 0 / unused for Data class.
        uint8_t lane = 0;
    };
    std::array<WriteEntry, ORDERING_TAG_SPACE> write_entries_;
    std::array<ReadEntry, ORDERING_TAG_SPACE> read_entries_;

    // FlooNoC's rob_alloc_q (floo_rob.sv:146): one bit per allocated RANGE, set at
    // the range's top index. Free space is the leading-zero count above it, so the
    // pool behaves as a stack: space returns only from the top.
    std::bitset<ORDERING_TAG_SPACE> alloc_write_;
    std::bitset<ORDERING_TAG_SPACE> alloc_read_;

    // Highest set bit below `depth`, or -1 if none. std::bitset has no such query.
    // Scanning above `depth` would be wrong as well as wasteful: a stray bit there
    // makes `depth - 1 - msb` underflow, and std::size_t underflow is silent.
    // Allocation never sets one (base + n - 1 <= depth - 1), so the bound is also
    // the assertion.
    static int highest_set(const std::bitset<ORDERING_TAG_SPACE>& b, std::size_t depth) {
        for (std::size_t i = depth; i-- > 0;) {
            if (b.test(i)) return static_cast<int>(i);
        }
        return -1;
    }

    // Per-id ordered range list. AW = {base, 1}; AR = {base, len+1}.
    // ordering_req == false: bypassed, no slot reserved, base is meaningless.
    struct BeatRange {
        uint8_t base;
        uint16_t len_plus_1;  // up to 256
        bool ordering_req;
        // Write side only: marks an in-flight collective AW, which nothing may
        // follow until its merged B retires (R2, S4 design §2.3a).
        bool collective = false;
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

    // Class of the last accepted push for that id (axi::AxiClass, updated
    // alongside prev_dest_write_ / prev_dest_read_). The REQUEST side splits by
    // class (S3a T6): NarrowAw/NarrowAr ride REQ, DataAw/DataAr ride DAT. The
    // RESPONSE side is asymmetric -- B never splits (NarrowB and DataB both
    // ride RSP, spec §6 :348), R does (NarrowR rides RSP, DataR rides DAT) --
    // but that only changes which channel a same-ID pair races on, not whether
    // it can race: the two AWs (or ARs) still went out on independently
    // arbitrated networks, so nothing prevents the destinations from
    // completing them out of issue order, regardless of which network their
    // responses converge on. A class change is therefore treated like a dest
    // change on both write and read: falls into the needs_rob branch, so a
    // same-ID cross-class pair retires by ordering_tag in issue order
    // regardless of which network the response arrives on (AXI4 IHI 0022 §A5.3).
    std::array<axi::AxiClass, AXI_ID_SPACE> prev_cls_write_{};
    std::array<axi::AxiClass, AXI_ID_SPACE> prev_cls_read_{};

    // Per-base (keyed by ordering_tag base) arrival counter. NSU stamps every
    // R beat of a burst with ordering_tag=base; this counter positions beat i at base+i.
    // Reset when the range is popped from read_order_by_id_ (ties counter lifecycle
    // to slot-reuse eligibility). read_range_len_[base] = burst length n, set in
    // push_ar, used to bound the counter (beat past burst length is malformed).
    std::array<uint16_t, ORDERING_TAG_SPACE> read_arrival_offset_{};
    std::array<uint16_t, ORDERING_TAG_SPACE> read_range_len_{};

    // Beats of the head burst released so far, keyed by range base. FlooNoC keys the
    // same counter by ID (read_rob_idx_offset_q, floo_rob.sv:177-180); a range belongs
    // to exactly one ID, so keying by base carries the same information. Distinct from
    // read_arrival_offset_: arrival places incoming beats, release tracks how many left.
    std::array<uint16_t, ORDERING_TAG_SPACE> read_release_offset_{};

    // High-water mark backing read_slot_hwm(). See getter for details.
    std::size_t read_slot_hwm_ = 0;

    // Admission counters backing the count / hwm getters above. See those for
    // details. Written only on the accepted-push tail of push_aw / push_ar, read
    // by nothing inside this class: no admission, ordering or timing decision
    // may depend on them.
    std::size_t aw_idle_bypass_count_ = 0;
    std::size_t aw_same_dest_bypass_count_ = 0;
    std::size_t aw_fallback_alloc_count_ = 0;
    std::size_t ar_idle_bypass_count_ = 0;
    std::size_t ar_same_dest_bypass_count_ = 0;
    std::size_t ar_fallback_alloc_count_ = 0;
    std::size_t order_list_hwm_ = 0;
    std::size_t write_txns_hwm_ = 0;
    std::size_t read_txns_hwm_ = 0;

    // Narrow-class lane re-anchor for read beats that never touch
    // read_entries_ -- Enabled-mode bypass and Disabled/RoBless reads. Both
    // stream FIFO-order per id (AXI4 IHI 0022 §A5.3), so a per-id FIFO of the
    // AR basis (populated at push_ar, drained beat by beat, popped on last)
    // recovers each beat's address the same way read_entries_[].lane does for
    // robbed bursts. S2 design doc §2 site 4 (the risky one): the R flit
    // carries no address, only its AR does.
    struct ArLaneMeta {
        uint64_t local_addr;
        uint8_t len;
        uint8_t size;
        axi::Burst burst;
        uint16_t beat_counter = 0;
    };
    std::array<std::deque<ArLaneMeta>, AXI_ID_SPACE> ar_lane_meta_;
    void reanchor_from_fifo_(axi::RBeat& r);

    // Ready-to-emit beats drained by pop_b / pop_r.
    std::deque<CommittedBEntry> committed_b_queue_;
    std::deque<CommittedREntry> committed_r_queue_;
    std::array<uint8_t, ORDERING_TAG_SPACE> committed_b_pending_{};
    std::array<uint8_t, ORDERING_TAG_SPACE> committed_r_pending_{};
};

// ===== inline impl =====

inline bool Rob::push_aw(const axi::AwBeat& b) {
    // Every per-id array below is AXI_ID_SPACE deep, which is 8 and no longer the
    // full uint8_t range, so an over-range id is a silent out-of-bounds rather than
    // a structural impossibility. Both admission points check it once at entry;
    // this is a permanent input error, not backpressure.
    if (b.id >= AXI_ID_SPACE) {
        assert(false && "nmu::Rob::push_aw: AXI id outside AXI_ID_SPACE");
        std::abort();  // belt-and-braces for NDEBUG
    }
    // AWUSER collective validate + translate at push_aw entry (S4 design §2.1):
    // ahead of the outstanding pool, the per-id order list and all RoB slot math,
    // so a permanent illegal input can never present as backpressure. Fatal on
    // every reject row of design §2.3; returns the 8 b node mask.
    const uint8_t collective_mask = addr_trans::collective_translate(sam_, b, port_id_);
    const uint8_t collective_op = axi::awuser_collective_op(b.user);
    const bool collective = collective_op != axi::COLLECTIVE_OP_UNICAST;

    // R2 (design §2.3a): at most one outstanding collective per (NMU, AXI id).
    // Retryable backpressure, NOT an error -- the caller re-presents the AW.
    //   collective in, list non-empty -> refuse. A collective is admitted only
    //     into an idle id, so it always lands on the idle-ID bypass branch below
    //     (ordering_req=0, no RoB slot) -- ruling 4, NoRobReduction.
    //   anything in, front entry collective -> refuse. This closes the
    //     same-destination bypass hole: a later same-id AW whose dst equals the
    //     collective's dst_id would otherwise stream past it without a RoB slot.
    //     Testing the FRONT suffices -- a collective only ever enters an empty
    //     list, so while in flight it is the only entry.
    if (!write_order_by_id_[b.id].empty() &&
        (collective || write_order_by_id_[b.id].front().collective)) {
        return false;
    }

    // ax_gnt_o: the per-id order list is FlooNoC's status FIFO (floo_rob.sv:414).
    // Bypassed pushes allocate no RoB slot, so this list is their only limiter.
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
    } else if (!fallen_back_write_[b.id] && dst == prev_dest_write_[b.id] &&
               t.cls == prev_cls_write_[b.id]) {
        // Same-destination bypass: same dest AND same class as the previous same-id
        // push, not yet reordering. Ported from floo_rob.sv:427-428, with a class
        // term FlooNoC has no counterpart for (two AXI ports there, one here --
        // see the class comment on prev_cls_write_). A class change is treated like
        // a dest change: falls into the needs_rob branch below, so the pair retires
        // by ordering_tag in AW order regardless of which network (REQ vs DAT) the
        // request travels on.
        needs_rob = false;
        fallen_back = false;
    } else {
        // Ported from floo_rob.sv:430-432.
        needs_rob = true;
        fallen_back = true;  // sticky
    }
    // Guaranteed by the R2 gate above; ruling 4 (NoRobReduction) depends on it.
    assert(!(collective && needs_rob) &&
           "nmu::Rob::push_aw: collective AW must take the idle-ID bypass, never a RoB slot");
    std::size_t base = 0;
    if (needs_rob) {
        if (write_free_space() < 1) return false;
        base = b_rob_depth_ - write_free_space();
    }
    if (!next_pkt_.push_aw_with_meta(
            b, {t.dst_id, t.local_addr, static_cast<uint8_t>(needs_rob ? 1 : 0),
                static_cast<uint8_t>(needs_rob ? base : 0), t.cls, collective_op, collective_mask,
                t.port})) {
        return false;  // downstream backpressure: no state mutation
    }
    prev_dest_write_[b.id] = dst;  // updated on every accepted push (floo_rob.sv:417-420)
    prev_cls_write_[b.id] = t.cls;
    fallen_back_write_[b.id] = fallen_back;
    if (needs_rob) {
        alloc_write_.set(base);  // a 1-slot range: base is its own top
        write_entries_[base] = WriteEntry{/*occupied=*/true, /*ready=*/false, b.id, /*b_beat=*/{}};
    }
    write_order_by_id_[b.id].push_back({static_cast<uint8_t>(base), 1, needs_rob, collective});
    ++write_txns_;
    ++w_bursts_owed_;
    // Counter tail, past every early return: needs_rob / fallen_back above are
    // trial values and the push can still be refused after them, so counting
    // there would count pushes that never happened. (empty, needs_rob) names the
    // branch that ran -- empty: idle-ID bypass; !empty && !needs_rob:
    // same-destination bypass; needs_rob: fall-back allocate.
    if (empty) {
        ++aw_idle_bypass_count_;
    } else if (needs_rob) {
        ++aw_fallback_alloc_count_;
    } else {
        ++aw_same_dest_bypass_count_;
    }
    order_list_hwm_ = std::max(order_list_hwm_, write_order_by_id_[b.id].size());
    write_txns_hwm_ = std::max(write_txns_hwm_, write_txns_);
    return true;
}

inline bool Rob::push_w(const axi::WBeat& b) {
    if (w_bursts_owed_ == 0) return false;   // W cannot proceed before its AW
    if (!next_pkt_.push_w(b)) return false;  // downstream backpressure: NO change to owed count
    if (b.last) w_bursts_owed_--;
    return true;
}

inline bool Rob::push_ar(const axi::ArBeat& b) {
    // See push_aw: the per-id arrays are AXI_ID_SPACE deep, not 256.
    if (b.id >= AXI_ID_SPACE) {
        assert(false && "nmu::Rob::push_ar: AXI id outside AXI_ID_SPACE");
        std::abort();  // belt-and-braces for NDEBUG
    }
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
        } else if (!fallen_back_read_[b.id] && dst == prev_dest_read_[b.id] &&
                   t.cls == prev_cls_read_[b.id]) {
            // Same-destination bypass: same dest AND same class as the previous same-id
            // push, not yet reordering. Ported from floo_rob.sv:427-428, with a class
            // term FlooNoC has no counterpart for (two AXI ports there, one here --
            // see the class comment on prev_cls_read_). A class change is treated like
            // a dest change: falls into the needs_rob branch below, so the pair retires
            // by ordering_tag in AR order regardless of which network (RSP vs DAT) the
            // response arrives on.
            needs_rob = false;
            fallen_back = false;
        } else {
            // Ported from floo_rob.sv:430-432.
            needs_rob = true;
            fallen_back = true;  // sticky
        }
        std::size_t base = 0;
        if (needs_rob) {
            // n > r_rob_depth_ can never be admitted -- free space never reaches n, so the
            // free-space test below would refuse this AR forever, indistinguishable from
            // ordinary backpressure. That is a configuration/stimulus error (burst beats
            // exceed the RoB read depth), not backpressure: fail loud instead of wedging.
            if (n > r_rob_depth_) {
                assert(false &&
                       "nmu::Rob::push_ar: burst beats (len+1) exceed RoB read depth "
                       "(len+1 > ROB_R_DEPTH) -- permanent stimulus error, not backpressure");
                std::abort();  // belt-and-braces for NDEBUG
            }
            if (read_free_space() < n) return false;
            base = r_rob_depth_ - read_free_space();
        }
        // Hoisted into a named local so dst_port -- which sits behind the two
        // collective members AR never sets -- can be assigned instead of the
        // brace having to spell them out (nsu::Depacketize::pop_aw does the same).
        AwHeaderMeta meta{t.dst_id, t.local_addr, static_cast<uint8_t>(needs_rob ? 1 : 0),
                          static_cast<uint8_t>(needs_rob ? base : 0), t.cls};
        meta.dst_port = t.port;
        if (!next_pkt_.push_ar_with_meta(b, meta)) {
            return false;  // downstream backpressure: no state mutation
        }
        prev_dest_read_[b.id] = dst;  // updated on every accepted push (floo_rob.sv:417-420)
        prev_cls_read_[b.id] = t.cls;
        fallen_back_read_[b.id] = fallen_back;
        if (needs_rob) {
            for (std::size_t i = 0; i < n; ++i) {
                // Robbed: every beat's address (and so its narrow-class lane)
                // is known upfront, unlike the bypass/RoBless FIFO below.
                uint8_t lane = 0;
                if (t.cls == axi::AxiClass::Narrow) {
                    const uint64_t addr = axi::beat_addr(t.local_addr, b.len, b.size, b.burst, i);
                    lane = static_cast<uint8_t>(axi::narrow_lane(addr));
                }
                read_entries_[base + i] =
                    ReadEntry{/*occupied=*/true, /*ready=*/false, b.id, /*r_beat=*/{}, lane};
            }
            alloc_read_.set(base + n - 1);  // only the range TOP is marked
            read_slot_hwm_ =
                std::max<std::size_t>(read_slot_hwm_, r_rob_depth_ - read_free_space());
            read_range_len_[base] = static_cast<uint16_t>(n);
        } else if (t.cls == axi::AxiClass::Narrow) {
            // Bypass: no read_entries_ slot, so pop_r_staged's bypass branch
            // needs its own AR basis (see ar_lane_meta_'s comment).
            ar_lane_meta_[b.id].push_back({t.local_addr, b.len, b.size, b.burst, 0});
        }
        read_order_by_id_[b.id].push_back(
            {static_cast<uint8_t>(base), static_cast<uint16_t>(n), needs_rob});
        ++read_txns_;
        // Counter tail; same reasoning as push_aw's.
        if (empty) {
            ++ar_idle_bypass_count_;
        } else if (needs_rob) {
            ++ar_fallback_alloc_count_;
        } else {
            ++ar_same_dest_bypass_count_;
        }
        order_list_hwm_ = std::max(order_list_hwm_, read_order_by_id_[b.id].size());
        read_txns_hwm_ = std::max(read_txns_hwm_, read_txns_);
        return true;
    }
    auto t = sam_.translate(b.addr);
    if (read_outstanding_[b.id]) return false;  // single-outstanding per id
    AwHeaderMeta meta{t.dst_id, t.local_addr, 0, 0, t.cls};
    meta.dst_port = t.port;
    if (!next_pkt_.push_ar_with_meta(b, meta)) {
        return false;
    }
    if (t.cls == axi::AxiClass::Narrow) {
        ar_lane_meta_[b.id].push_back({t.local_addr, b.len, b.size, b.burst, 0});
    }
    read_outstanding_[b.id] = true;
    ++read_txns_;
    // Disabled reads run the single-outstanding interlock instead of the three
    // admission branches, so there is no clause to count here.
    read_txns_hwm_ = std::max(read_txns_hwm_, read_txns_);
    return true;
}

// Standalone pop: AxiSlavePort calls this only when its output queue has room
// (axi_slave_port.hpp:127-136), so the pop IS the AXI-side acceptance and retires here.
// The integrated Nmu never calls it -- it retires at push_rsp_b_to_axi_ instead.
inline std::optional<axi::BBeat> Rob::pop_b() {
    auto out = pop_b_staged();
    if (!out) return std::nullopt;
    retire_b(out->ordering_req, out->ordering_tag, out->axi_id);
    return out->beat;
}

inline std::optional<axi::RBeat> Rob::pop_r() {
    if (mode_r_ == RobMode::Enabled) {
        auto out = pop_r_staged();
        if (!out) return std::nullopt;
        retire_r(out->ordering_req, out->ordering_tag, out->axi_id, out->beat.last);
        return out->beat;
    }
    auto opt = pop_r_robless();
    if (!opt) return std::nullopt;
    retire_r(/*ordering_req=*/false, 0, opt->id, opt->last);
    return opt;
}

inline std::optional<axi::RBeat> Rob::pop_r_robless() {
    assert(mode_r_ != RobMode::Enabled &&
           "nmu::Rob::pop_r_robless: Enabled mode uses pop_r_staged");
    auto opt = next_depkt_.pop_r_with_meta();
    if (!opt) return std::nullopt;
    auto [r, meta] = *opt;
    if (meta.cls == axi::AxiClass::Narrow) reanchor_from_fifo_(r);
    return r;
}

// Shared by the Disabled/RoBless path and Enabled mode's bypass sub-path:
// neither owns a read_entries_ slot, so both recover the AR basis from the
// per-id ar_lane_meta_ FIFO pushed at push_ar.
inline void Rob::reanchor_from_fifo_(axi::RBeat& r) {
    auto& fifo = ar_lane_meta_[r.id];
    assert(!fifo.empty() && "nmu::Rob: narrow R beat with no staged AR basis");
    ArLaneMeta& am = fifo.front();
    const uint64_t addr = axi::beat_addr(am.local_addr, am.len, am.size, am.burst, am.beat_counter);
    axi::reanchor_narrow_lane(r.data, axi::narrow_lane(addr));
    ++am.beat_counter;
    if (r.last) fifo.pop_front();
}

inline void Rob::retire_b(bool ordering_req, uint8_t ordering_tag, uint8_t axi_id) {
    if (ordering_req) commit_b_exit(ordering_tag, axi_id);
    // Unconditional, so the bypassed path gets the unmatched-response check the robbed
    // path already has (floo_meta_buffer.sv:199-200; rob.hpp only checks the head is
    // bypassed, which a duplicate B would satisfy).
    assert(write_txns_ > 0 && "nmu::Rob: B response with no outstanding write transaction");
    if (write_txns_ == 0) std::abort();
    --write_txns_;
}

inline void Rob::retire_r(bool ordering_req, uint8_t ordering_tag, uint8_t axi_id, bool last) {
    // Per beat: a robbed burst holds one slot per beat.
    if (ordering_req) commit_r_exit(ordering_tag, axi_id);
    if (!last) return;  // per transaction: the burst retires on its last beat
    if (mode_r_ != RobMode::Enabled) {
        assert(read_outstanding_[axi_id] && "R(last) for id with no outstanding read");
        read_outstanding_[axi_id] = false;
    }
    assert(read_txns_ > 0 && "nmu::Rob: R response with no outstanding read transaction");
    if (read_txns_ == 0) std::abort();
    --read_txns_;
}

inline void Rob::drain_ready_write_heads_(uint8_t id) {
    while (!write_order_by_id_[id].empty()) {
        const BeatRange head = write_order_by_id_[id].front();
        if (!head.ordering_req) break;  // waiting on a bypassed response
        if (!write_entries_[head.base].ready) break;
        committed_b_queue_.push_back({write_entries_[head.base].b_beat, head.base, id, true});
        ++committed_b_pending_[head.base];
        write_order_by_id_[id].pop_front();
    }
}

inline void Rob::drain_ready_read_heads_(uint8_t id) {
    while (!read_order_by_id_[id].empty()) {
        const BeatRange head = read_order_by_id_[id].front();
        if (!head.ordering_req) break;  // waiting on a bypassed response
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
    if (meta.ordering_req == 0) {
        // Bypassed B: by the head invariant it is the head of its id's order list.
        const uint8_t id = b.id;
        if (write_order_by_id_[id].empty() || write_order_by_id_[id].front().ordering_req) {
            assert(false && "bypassed B does not match the head of its id's order list");
            std::abort();
        }
        write_order_by_id_[id].pop_front();
        drain_ready_write_heads_(id);  // the entry behind it may already be ready
        return CommittedBEntry{b, 0, id, /*ordering_req=*/false};
    }
    if (!(meta.ordering_tag < ORDERING_TAG_SPACE)) {
        assert(false && "ordering_tag out of range");
        std::abort();
    }
    auto& slot = write_entries_[meta.ordering_tag];
    if (!(slot.occupied && !slot.ready)) {
        assert(false && "B for unallocated or already-completed ordering_tag");
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
    if (meta.ordering_req == 0) {
        // Bypassed R streams beat by beat; the list entry pops on last, and only
        // then can a robbed entry behind it become releasable.
        const uint8_t id = r.id;
        if (read_order_by_id_[id].empty() || read_order_by_id_[id].front().ordering_req) {
            assert(false && "bypassed R does not match the head of its id's order list");
            std::abort();
        }
        if (meta.cls == axi::AxiClass::Narrow) reanchor_from_fifo_(r);
        if (r.last) {
            read_order_by_id_[id].pop_front();
            drain_ready_read_heads_(id);
        }
        return CommittedREntry{r, 0, id, /*ordering_req=*/false};
    }
    if (!(meta.ordering_tag < ORDERING_TAG_SPACE)) {
        assert(false && "ordering_tag out of range");
        std::abort();
    }
    uint8_t base = meta.ordering_tag;
    std::size_t arrival_offset = read_arrival_offset_[base];
    if (!(arrival_offset < read_range_len_[base])) {
        assert(false &&
               "nmu::Rob::pop_r_staged: R beat past the burst's reserved slot range "
               "-- malformed burst");
        std::abort();
    }
    std::size_t slot_idx = static_cast<std::size_t>(base) + arrival_offset;
    if (!(slot_idx < ORDERING_TAG_SPACE)) {
        assert(false && "computed read slot out of range");
        std::abort();
    }
    auto& slot = read_entries_[slot_idx];
    if (!(slot.occupied && !slot.ready)) {
        assert(false && "computed read slot unallocated or already filled");
        std::abort();
    }
    if (meta.cls == axi::AxiClass::Narrow) axi::reanchor_narrow_lane(r.data, slot.lane);
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

inline void Rob::commit_b_exit(uint8_t ordering_tag, uint8_t axi_id) {
    assert(ordering_tag < ORDERING_TAG_SPACE);
    assert(committed_b_pending_[ordering_tag] > 0);
    --committed_b_pending_[ordering_tag];
    if (committed_b_pending_[ordering_tag] == 0) {
        alloc_write_.reset(ordering_tag);  // no-op unless ordering_tag is a range top
        write_entries_[ordering_tag] = WriteEntry{};
    }
}

inline void Rob::commit_r_exit(uint8_t ordering_tag, uint8_t axi_id) {
    assert(ordering_tag < ORDERING_TAG_SPACE);
    assert(committed_r_pending_[ordering_tag] > 0);
    --committed_r_pending_[ordering_tag];
    if (committed_r_pending_[ordering_tag] == 0) {
        alloc_read_.reset(ordering_tag);  // no-op unless ordering_tag is a range top
        read_entries_[ordering_tag] = ReadEntry{};
    }
}

}  // namespace ni::cmodel::nmu
