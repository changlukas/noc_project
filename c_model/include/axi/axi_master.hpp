// Algorithms ported from cocotbext-axi (MIT) — see axi/ATTRIBUTION.md
#pragma once
#include "axi/types.hpp"
#include "axi/protocol_rules.hpp"
#include "axi/scenario_parser.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ni::cmodel::axi {

// Sub-burst descriptor produced by split_into_sub_bursts(). Each describes one
// AXI4 AW/AR (with its own len/addr) carved out of a single scenario_txn.
struct BurstSpec {
    uint64_t addr;
    uint8_t len;
    uint8_t size;
    Burst burst;
};

// AXI4 (IHI 0022 A3.4.1) requires INCR bursts not to cross a 4KB boundary, and
// caps a single burst at 256 beats. A scenario_txn that violates either is
// auto-segmented here into a chain of legal sub-bursts. WRAP/FIXED never need
// splitting — WRAP wraps inside [wrap_lower, wrap_upper) by construction;
// FIXED reuses one address.
inline std::vector<BurstSpec> split_into_sub_bursts(const ScenarioTransaction& txn) {
    std::vector<BurstSpec> out;
    if (txn.burst != Burst::INCR) {
        out.push_back(BurstSpec{txn.addr, txn.len, txn.size, txn.burst});
        return out;
    }
    const std::size_t bpb = 1ull << txn.size;
    uint64_t addr = txn.addr;
    std::size_t beats_remaining = static_cast<std::size_t>(txn.len) + 1u;
    while (beats_remaining > 0) {
        const std::size_t bytes_to_4kb = rules::kAxi4PageBytes - (addr & rules::kAxi4PageMask);
        std::size_t beats_to_4kb = bytes_to_4kb / bpb;
        if (beats_to_4kb == 0) beats_to_4kb = beats_remaining;  // already aligned
        const std::size_t beats_this = std::min(
            {beats_to_4kb, beats_remaining, static_cast<std::size_t>(rules::kAxi4MaxBurstBeats)});
        out.push_back(BurstSpec{addr, static_cast<uint8_t>(beats_this - 1), txn.size, Burst::INCR});
        addr += beats_this * bpb;
        beats_remaining -= beats_this;
    }
    return out;
}

// WriteResult / ReadResult carry the ORIGINAL user txn.addr (the address the
// scenario asked the master to access), plus enough AXI4 burst geometry to let
// the scoreboard re-derive per-beat lane offsets under lane-positioned bus
// semantics. Per-beat byte_lane = (txn.addr + beat*(1<<size)) mod DATA_BYTES;
// the AW.addr on the wire is still aligned DOWN by the master.
struct WriteResult {
    uint64_t addr;  // original user txn.addr
    uint8_t size;   // log2(bytes_per_beat)
    uint8_t len;    // beats - 1
    Burst burst;
    // Phase C: mirrors the originating scenario_txn.lock so the scoreboard can
    // distinguish a failed exclusive write (lock=Exclusive + resp=OKAY → no
    // memory commit) from a normal write (resp=OKAY → commit) without peeking
    // at the slave's exclusive monitor state.
    LockType lock = LockType::Normal;
    std::vector<uint8_t> data;            // packed user bytes, (len+1)*(1<<size)
    std::vector<uint32_t> strb_per_beat;  // bus-level WSTRB per beat (lane-positioned)
    Resp resp;
    uint8_t id;
    std::size_t scenario_line;
};

struct ReadResult {
    uint64_t addr;  // original user txn.addr
    uint8_t size;
    uint8_t len;
    Burst burst;
    std::vector<uint8_t> data;  // packed user bytes, (len+1)*(1<<size)
    Resp resp;
    uint8_t id;
    std::size_t scenario_line;
};

// SFINAE helper: call slave.force_aw_not_pending() only if the slave type
// provides that method (WireSlavePort does; NullSlavePort, AxiSlave do not).
// Used by the fault-injection path to clear stale AW-pending state so that
// AWVALID drops on the registered SV wire (beta-tick discipline).
template <typename SlaveT>
auto clear_aw_pending_if_supported(SlaveT& s) -> decltype(s.force_aw_not_pending(), void()) {
    s.force_aw_not_pending();
}
inline void clear_aw_pending_if_supported(...) noexcept {}

template <typename SlaveT>
class AxiMasterT {
  public:
    AxiMasterT(const std::string& scenario_yaml, SlaveT& slave, const std::string& read_dump_path,
               std::size_t max_outstanding_write = 1, std::size_t max_outstanding_read = 1)
        : slave_(slave), max_out_w_(max_outstanding_write), max_out_r_(max_outstanding_read) {
        sc_ = load_scenario(scenario_yaml);
        read_dump_.open(read_dump_path);
        if (!read_dump_.is_open())
            throw std::runtime_error("AxiMaster: cannot open read_dump_path: " + read_dump_path);
    }

    void tick() {
        // ===== Fault injection arm =====
        if (inject_.mode == InjectConfig::Mode::AwUnstable && cycle_count_ == inject_.cycle) {
            force_awvalid_low_one_cycle_ = true;
        }
        ++cycle_count_;

        // ===== Drain B responses =====
        //
        // OperationContext model: one scenario_txn → one OperationContext → N
        // sub-bursts (split at 4KB / 256-beat). Each sub-burst gets its own AW;
        // each AW returns its own B. Operation completes (single WriteResult
        // fires) when all sub-bursts' B responses are in. The slave's per-ID FIFO
        // delivers B responses in submission order, so we advance the operation's
        // sub-burst-complete counter regardless of which sub-burst the B id maps
        // to — order is implied by submission.
        while (auto b = slave_.pop_b()) {
            AXI_PROTOCOL_ASSERT(rules::check_resp_encoding(b->resp),
                                "RESP_ENCODING: B response must be a legal AXI4 response");
            // AXI4 IHI 0022 §A5.3: response routes to oldest outstanding for that id.
            AXI_PROTOCOL_ASSERT(
                rules::check_b_front_can_accept_response(b->id, active_write_ops_),
                "B_FRONT_CAN_ACCEPT: id deque empty or front op already fully responded");
            // .at() (not operator[]) so a malformed bid throws in release builds —
            // NDEBUG strips the AXI_PROTOCOL_ASSERT above, and operator[] would
            // silently insert an empty deque → deque.front() UB.
            auto& deq = active_write_ops_.at(b->id);
            auto& op = deq.front();
            ++op.b_count_;
            if (static_cast<uint8_t>(b->resp) > static_cast<uint8_t>(op.worst_resp_))
                op.worst_resp_ = b->resp;
            if (op.b_count_ == op.sub_bursts.size()) {
                // WriteResult carries the ORIGINAL user txn (addr, size, len, burst)
                // — NOT sub-burst geometry. The scoreboard re-derives per-beat addr
                // from the original txn.
                if (wcb_)
                    wcb_(WriteResult{op.src_txn.addr, op.src_txn.size, op.src_txn.len,
                                     op.src_txn.burst, op.src_txn.lock, op.data, op.strb_per_beat,
                                     op.worst_resp_, b->id, op.src_txn.scenario_line});
                deq.pop_front();
                --active_write_count_;
                if (deq.empty()) active_write_ops_.erase(b->id);
            }
        }

        // ===== Drain R responses =====
        //
        // R beats arrive in sub-burst order (per-id FIFO at the slave). Track
        // which sub-burst the current beat belongs to via cur_r_sub_idx_ +
        // r_beats_in_cur_; advance to the next sub-burst on r->last. Operation
        // completes when cur_r_sub_idx_ reaches sub_bursts.size().
        while (auto r = slave_.pop_r()) {
            AXI_PROTOCOL_ASSERT(
                rules::check_r_front_can_accept_beat(r->id, r->last, active_read_ops_),
                "R_FRONT_CAN_ACCEPT: bad beat timing or rlast mismatch with sub-burst length");
            // .at() (not operator[]) — see B-drain rationale above.
            auto& deq = active_read_ops_.at(r->id);
            auto& op = deq.front();
            const auto& sub = op.sub_bursts[op.cur_r_sub_idx_];
            const std::size_t bpb = 1ull << sub.size;
            // Lane-positioned bus: byte j on the bus is at lane (byte_lane + j),
            // where byte_lane = (per-beat addr) mod DATA_BYTES. Per-beat addr from
            // the CURRENT sub-burst (FIXED stays; INCR advances; WRAP wraps).
            // Lane room caps the per-beat payload at DATA_BYTES - byte_lane; any
            // trailing user bytes that would have fallen off the bus are zero-
            // padded so downstream packed-buffer offsets stay aligned at the
            // operation-level (beat * bpb) layout.
            const uint64_t beat_addr_v =
                beat_addr(sub.addr, sub.len, sub.size, sub.burst, op.r_beats_in_cur_);
            const std::size_t byte_lane = static_cast<std::size_t>(beat_addr_v & (DATA_BYTES - 1));
            const std::size_t lane_room = (byte_lane < DATA_BYTES) ? (DATA_BYTES - byte_lane) : 0;
            const std::size_t copy_bytes = std::min(bpb, lane_room);
            for (std::size_t i = 0; i < copy_bytes; ++i)
                op.read_accumulator.push_back(r->data[byte_lane + i]);
            for (std::size_t i = copy_bytes; i < bpb; ++i) op.read_accumulator.push_back(0);
            ++op.r_beats_in_cur_;
            if (static_cast<uint8_t>(r->resp) > static_cast<uint8_t>(op.worst_resp_))
                op.worst_resp_ = r->resp;
            if (r->last) {
                ++op.cur_r_sub_idx_;
                op.r_beats_in_cur_ = 0;
            }
            if (op.cur_r_sub_idx_ == op.sub_bursts.size()) {
                // All sub-bursts drained → fire single ReadResult for the original
                // scenario_txn, dumping the packed user-byte buffer.
                const std::size_t bpb_orig = 1ull << op.src_txn.size;
                const std::size_t total = op.read_accumulator.size();
                const std::size_t lines = (bpb_orig > 0) ? (total / bpb_orig) : 0;
                for (std::size_t line = 0; line < lines; ++line) {
                    for (std::size_t j = 0; j < bpb_orig; ++j) {
                        if (j > 0) read_dump_ << ' ';
                        char buf[4];
                        std::snprintf(buf, sizeof(buf), "%02X",
                                      op.read_accumulator[line * bpb_orig + j]);
                        read_dump_ << buf;
                    }
                    read_dump_ << '\n';
                }
                read_dump_.flush();
                if (rcb_)
                    rcb_(ReadResult{op.src_txn.addr, op.src_txn.size, op.src_txn.len,
                                    op.src_txn.burst, op.read_accumulator, op.worst_resp_, r->id,
                                    op.src_txn.scenario_line});
                deq.pop_front();
                --active_read_count_;
                if (deq.empty()) active_read_ops_.erase(r->id);
            }
        }

        // ===== Admission =====
        //
        // One OperationContext per scenario_txn. AXI4 IHI 0022 §A5.3 permits
        // multiple in-flight ops with the same AXI ID; per-id deque preserves
        // submission order so B/R routing can take .front() (oldest outstanding).
        // active_*_count_ tracks TOTAL ops across all ids (not per-id) — gated
        // against max_outstanding_{write,read} below.
        while (next_txn_idx_ < sc_.transactions.size()) {
            const auto& txn = sc_.transactions[next_txn_idx_];
            if (txn.op == ScenarioTransaction::Op::Write) {
                if (active_write_count_ >= max_out_w_) break;
                OperationContext op;
                op.src_txn = txn;
                op.sub_bursts = split_into_sub_bursts(txn);
                op.data = load_write_data_(
                    txn.data_file, static_cast<std::size_t>(txn.len + 1u) * (1ull << txn.size));
                op.strb_per_beat =
                    load_strb_file_(txn.strb_file, static_cast<std::size_t>(txn.len + 1u));
                active_write_ops_[txn.id].push_back(std::move(op));
                ++active_write_count_;
            } else {
                if (active_read_count_ >= max_out_r_) break;
                OperationContext op;
                op.src_txn = txn;
                op.sub_bursts = split_into_sub_bursts(txn);
                active_read_ops_[txn.id].push_back(std::move(op));
                ++active_read_count_;
            }
            ++next_txn_idx_;
        }

        // ===== Push AW + W beats per operation =====
        //
        // Walk each id's per-id FIFO in submission order. Within one op, walk its
        // sub-bursts in order. For the current sub-burst:
        //   - Push its AW if not yet pushed.
        //   - Push its W beats up to its len+1; the W payload is a slice of the
        //     operation-level packed user buffer starting at op-level beat index
        //     (which == sum of preceding sub-bursts' beat counts + cur_w_in_sub_).
        // Break on the first op whose request phase is incomplete — AXI4 W has no
        // WID, so same-id ops MUST emit W in AW issue order (IHI 0022 §A5.3).
        for (auto& [id, deq] : active_write_ops_) {
            for (auto& op : deq) {
                if (!push_writes_(id, op)) break;
            }
        }

        // ===== Push AR per operation =====
        //
        // Same-id FIFO order: AR for op[0] must be fully pushed before op[1]'s AR.
        for (auto& [id, deq] : active_read_ops_) {
            for (auto& op : deq) {
                if (!push_reads_(id, op)) break;
            }
        }
    }

    bool done() const {
        return next_txn_idx_ >= sc_.transactions.size() && active_write_count_ == 0 &&
               active_read_count_ == 0;
    }

    // Diagnostic accessors for tests: visibility into in-flight op counters so
    // a regression test can assert N ops are concurrently admitted mid-run.
    std::size_t active_write_count() const { return active_write_count_; }
    std::size_t active_read_count() const { return active_read_count_; }

    void on_write_completed(std::function<void(const WriteResult&)> cb) { wcb_ = std::move(cb); }
    void on_read_observed(std::function<void(const ReadResult&)> cb) { rcb_ = std::move(cb); }

    // Fault injection: call once after construction to arm a specific violation.
    // When inject.mode == None (default), tick() adds one bool check — no other
    // behavioral change.
    void configure_inject(const InjectConfig& inj) noexcept {
        inject_ = inj;
        cycle_count_ = 0;
    }

  private:
    static std::vector<uint8_t> load_write_data_(const std::string& path,
                                                 std::size_t expected_bytes) {
        std::ifstream f(path);
        if (!f.is_open()) throw std::runtime_error("AxiMaster: cannot open data_file: " + path);
        std::vector<uint8_t> bytes;
        std::string tok;
        while (f >> tok) bytes.push_back(static_cast<uint8_t>(std::stoul(tok, nullptr, 16)));
        if (bytes.size() < expected_bytes)
            throw std::runtime_error("AxiMaster: data_file too short (" +
                                     std::to_string(bytes.size()) + " < " +
                                     std::to_string(expected_bytes) + "): " + path);
        return bytes;
    }

    static std::vector<uint32_t> load_strb_file_(const std::string& path,
                                                 std::size_t expected_beats,
                                                 uint32_t default_full = 0xFFFF'FFFFu) {
        if (path.empty()) {
            return std::vector<uint32_t>(expected_beats, default_full);
        }
        std::ifstream f(path);
        if (!f.is_open()) throw std::runtime_error("AxiMaster: cannot open strb_file: " + path);
        std::vector<uint32_t> strbs;
        std::string tok;
        while (f >> tok) {
            unsigned long long v = std::stoull(tok, nullptr, 16);
            if (v > 0xFFFFFFFFull)
                throw std::runtime_error("AxiMaster: strb_file token out of uint32_t range: " +
                                         tok);
            strbs.push_back(static_cast<uint32_t>(v));
        }
        if (strbs.size() != expected_beats)
            throw std::runtime_error("AxiMaster: strb_file line count " +
                                     std::to_string(strbs.size()) + " != expected beats " +
                                     std::to_string(expected_beats) + ": " + path);
        return strbs;
    }

    // OperationContext aggregates one scenario_txn's full lifecycle. Splitting
    // INCR at 4KB / 256-beat boundaries produces N sub-bursts; the operation
    // emits N AWs (or ARs) with the same id, each carrying its own len/addr.
    // Per-op state tracks BOTH write and read pipeline progress; only the
    // relevant half is exercised depending on src_txn.op.
    struct OperationContext {
        ScenarioTransaction src_txn;
        std::vector<BurstSpec> sub_bursts;
        // Write-side state
        std::vector<uint8_t> data;            // packed user bytes, full operation
        std::vector<uint32_t> strb_per_beat;  // operation-level, 1 entry per user beat
        std::size_t next_aw_sub_idx_ = 0;     // index of next sub-burst's AW to push
        std::size_t cur_w_sub_idx_ = 0;       // sub-burst currently emitting W beats
        std::size_t w_pushed_in_cur_ = 0;     // W beats pushed for current sub-burst
        std::size_t b_count_ = 0;             // B responses received (per sub-burst)
        Resp worst_resp_ = Resp::OKAY;
        // Read-side state
        std::size_t next_ar_sub_idx_ = 0;  // index of next sub-burst's AR to push
        std::size_t cur_r_sub_idx_ = 0;    // sub-burst currently absorbing R beats
        std::size_t r_beats_in_cur_ = 0;
        std::vector<uint8_t> read_accumulator;  // packed user bytes, full operation

        // Phase A: request-phase completion predicates. The outer FIFO loop in
        // tick() breaks on the first op whose request phase is incomplete so
        // same-id ops emit their AW + W stream in submission order (AXI4 W has
        // no WID; W beats follow AW issue order — IHI 0022 §A5.3).
        bool write_request_done() const {
            return next_aw_sub_idx_ == sub_bursts.size() && cur_w_sub_idx_ == sub_bursts.size();
        }
        bool read_request_done() const { return next_ar_sub_idx_ == sub_bursts.size(); }
    };

    // Per-operation W push: walk sub-bursts in order, pushing AW + W beats.
    // The operation-level beat index (used to slice op.data / op.strb_per_beat)
    // is the running sum of preceding sub-bursts' beat counts plus the current
    // sub-burst's in-progress index.
    //
    // Returns true iff op's write request phase (all AWs + all W beats across
    // all sub-bursts) is fully pushed downstream. The outer FIFO loop breaks
    // on the first false to preserve AXI4 W stream ordering: same-id ops MUST
    // emit their W phase in AW issue order (AXI4 W has no WID — IHI 0022 §A5.3).
    bool push_writes_(uint8_t id, OperationContext& op) {
        // Pre-compute prefix sum of sub-burst beat counts so the operation-level
        // beat offset for sub-burst k is op_beat_base[k].
        std::size_t op_beat_base = 0;
        for (std::size_t k = 0; k < op.cur_w_sub_idx_; ++k) {
            op_beat_base += static_cast<std::size_t>(op.sub_bursts[k].len) + 1u;
        }
        while (op.cur_w_sub_idx_ < op.sub_bursts.size()) {
            const auto& sub = op.sub_bursts[op.cur_w_sub_idx_];
            // AXI4 INCR unaligned start (IHI 0022): AW.addr is aligned DOWN to the
            // (1<<size) transfer boundary; the unaligned-prefix bytes are skipped
            // via WSTRB on the first beat. The byte_lane used to position bytes on
            // the bus is derived from the bus DATA_BYTES granularity.
            const uint64_t aligned_addr = sub.addr & ~((1ull << sub.size) - 1);
            if (op.next_aw_sub_idx_ == op.cur_w_sub_idx_) {
                AwBeat aw{};
                aw.id = id;
                aw.addr = aligned_addr;
                aw.len = sub.len;
                aw.size = sub.size;
                aw.burst = sub.burst;
                // Phase C: pure wire-through of scenario_txn.lock onto AW.lock. AXI4
                // AxLOCK is 1-bit; LockType::Exclusive maps to 1, Normal to 0. Every
                // sub-burst of one operation carries the same lock value.
                aw.lock = (op.src_txn.lock == LockType::Exclusive) ? 1u : 0u;
                // Fault injection: force awvalid low for one cycle by treating
                // push_aw as rejected. Auto-clear the flag after this cycle.
                if (force_awvalid_low_one_cycle_) {
                    force_awvalid_low_one_cycle_ = false;
                    // Beta-tick: clear any pending AW beat so AWVALID drops on
                    // the SV wire. detail::clear_aw_pending is a helper that calls
                    // force_aw_not_pending() if the slave type exposes it (SFINAE).
                    clear_aw_pending_if_supported(slave_);
                    return op.write_request_done();
                }
                if (!slave_.push_aw(aw)) return op.write_request_done();
                ++op.next_aw_sub_idx_;
            }
            const std::size_t bpb = 1ull << sub.size;
            while (op.w_pushed_in_cur_ <= sub.len) {
                WBeat w{};
                // Lane-positioned bus: byte j of the user payload lands at bus lane
                // (byte_lane + j); byte_lane = per-beat-addr mod DATA_BYTES.
                const uint64_t beat_addr_v =
                    beat_addr(sub.addr, sub.len, sub.size, sub.burst, op.w_pushed_in_cur_);
                const std::size_t byte_lane =
                    static_cast<std::size_t>(beat_addr_v & (DATA_BYTES - 1));
                w.data.fill(0);
                const std::size_t writable =
                    (byte_lane < DATA_BYTES) ? std::min<std::size_t>(bpb, DATA_BYTES - byte_lane)
                                             : 0;
                const std::size_t op_beat = op_beat_base + op.w_pushed_in_cur_;
                for (std::size_t j = 0; j < writable; ++j) {
                    const std::size_t off = op_beat * bpb + j;
                    w.data[byte_lane + j] = (off < op.data.size()) ? op.data[off] : 0;
                }
                const uint32_t lane_mask = static_cast<uint32_t>(((1ull << bpb) - 1) << byte_lane);
                w.strb = op.strb_per_beat[op_beat] & lane_mask;
                w.last = (op.w_pushed_in_cur_ == sub.len);
                if (!slave_.push_w(w)) return op.write_request_done();
                ++op.w_pushed_in_cur_;
            }
            // Current sub-burst's W beats fully pushed; advance to the next.
            op_beat_base += static_cast<std::size_t>(sub.len) + 1u;
            ++op.cur_w_sub_idx_;
            op.w_pushed_in_cur_ = 0;
        }
        return op.write_request_done();
    }

    // Per-operation AR push: walk sub-bursts in order, pushing one AR per
    // sub-burst. Returns true iff all sub-bursts' ARs have been pushed
    // downstream. The outer FIFO loop breaks on the first false to preserve
    // submission order for same-id reads (R responses route to per-id .front()).
    bool push_reads_(uint8_t id, OperationContext& op) {
        while (op.next_ar_sub_idx_ < op.sub_bursts.size()) {
            const auto& sub = op.sub_bursts[op.next_ar_sub_idx_];
            ArBeat ar{};
            ar.id = id;
            // Align AR addr DOWN to (1<<size) boundary, symmetric with AW.
            ar.addr = sub.addr & ~((1ull << sub.size) - 1);
            ar.len = sub.len;
            ar.size = sub.size;
            ar.burst = sub.burst;
            // Phase C: wire-through scenario_txn.lock onto AR.lock (1-bit).
            ar.lock = (op.src_txn.lock == LockType::Exclusive) ? 1u : 0u;
            if (!slave_.push_ar(ar)) return op.read_request_done();
            ++op.next_ar_sub_idx_;
        }
        return op.read_request_done();
    }

    Scenario sc_;
    SlaveT& slave_;
    std::size_t max_out_w_, max_out_r_;
    std::size_t next_txn_idx_ = 0;
    std::ofstream read_dump_;
    std::function<void(const WriteResult&)> wcb_;
    std::function<void(const ReadResult&)> rcb_;
    // Per-AXI-ID FIFO of outstanding ops (post AXI4 conformity fix — same-id
    // concurrent allowed). AXI4 IHI 0022 §A5.3: responses for same id complete
    // in submission order; per-id deque preserves submission order; B/R routing
    // uses .front().
    // NOTE: std::map is used as a stable per-id container; its ordered iteration
    // is NOT relied upon for correctness (AXI4 has no cross-id ordering).
    std::map<uint8_t, std::deque<OperationContext>> active_write_ops_;
    std::map<uint8_t, std::deque<OperationContext>> active_read_ops_;

    // Total active op counters (map.size() now counts active *ids*, not ops).
    std::size_t active_write_count_ = 0;
    std::size_t active_read_count_ = 0;

    // Fault injection state (armed via configure_inject()).
    InjectConfig inject_{};
    std::size_t cycle_count_ = 0;
    bool force_awvalid_low_one_cycle_ = false;
};

class AxiSlave;
using AxiMaster = AxiMasterT<AxiSlave>;

// -------------------------------------------------------------------------
// Stage 5b: AxiMasterStandalone — hermetic, no external SlaveT& ref.
//
// ShellAdapters that drive AxiMaster without a concrete AxiSlave (e.g.,
// DPI-wired cosim) use this class. It owns a NullSlavePort (all push_*
// calls return false; pop_* return nullopt) so AxiMasterT construction
// succeeds without a real slave. The ShellAdapter overrides the
// tick-level slave interaction via the DPI layer instead of calling
// slave_.push_aw etc. directly.
// -------------------------------------------------------------------------

namespace detail {

// NullSlavePort satisfies the AxiMasterT<SlaveT> interface.
// push_* always returns false (no backpressure modeling needed at this layer
// when the real channel is wired externally by the ShellAdapter).
// pop_* return nullopt (responses injected via external call by ShellAdapter).
struct NullSlavePort {
    bool push_aw(const AwBeat&) { return false; }
    bool push_w(const WBeat&) { return false; }
    bool push_ar(const ArBeat&) { return false; }
    std::optional<BBeat> pop_b() { return std::nullopt; }
    std::optional<RBeat> pop_r() { return std::nullopt; }
};

// WireSlavePort — handshake-aware intermediary for Stage 5b ShellAdapter.
//
// Models the AXI4 wire handshake from the master's perspective:
//   - push_aw/push_w/push_ar model the master PRESENTING a beat (driving valid+
//     beat info onto the channel). The beat is accepted by WireSlavePort only
//     when the corresponding ready signal (set by ShellAdapter from the incoming
//     SV wire) is true that cycle — matching the "both valid and ready asserted"
//     transfer condition (IHI 0022 §A3.2.1).
//   - When ready is false: push_* returns false so AxiMasterT retries next
//     cycle. The ShellAdapter reads the pending beat directly from last_aw_/
//     last_w_/last_ar_ to drive awvalid/wvalid/arvalid onto the output wire.
//   - When ready is true: push_* returns true (handshake complete); the beat is
//     consumed and ShellAdapter clears the pending latch.
//
// pop_b / pop_r supply B/R response beats injected by ShellAdapter from the
// incoming SV wire. inject_b / inject_r are the injection points.
struct WireSlavePort {
    // Backpressure controls: ShellAdapter sets these from MasterInputs before
    // each call to AxiMasterT::tick().
    // Beta-tick semantics: set_wready resets the per-tick delivery gate.
    // Only ONE W beat is accepted per tick (modelling the 1-beat-per-clock-edge
    // constraint of the registered SV wire in the co-sim beta-tick discipline).
    void set_awready(bool v) noexcept { awready_ = v; }
    void set_wready(bool v) noexcept {
        wready_ = v;
        w_delivered_this_tick_ = false;  // reset gate each tick
    }
    void set_arready(bool v) noexcept { arready_ = v; }

    // Called by AxiMasterT to present a request beat. Returns true (handshake
    // complete) only when the corresponding ready signal is high.
    bool push_aw(const AwBeat& b) {
        last_aw_ = b;
        aw_pending_ = true;
        if (!awready_) return false;
        aw_pending_ = false;
        return true;
    }
    bool push_w(const WBeat& b) {
        last_w_ = b;
        w_pending_ = true;
        if (!wready_ || w_delivered_this_tick_) return false;
        w_pending_ = false;
        w_delivered_this_tick_ = true;
        return true;
    }
    bool push_ar(const ArBeat& b) {
        last_ar_ = b;
        ar_pending_ = true;
        if (!arready_) return false;
        ar_pending_ = false;
        return true;
    }

    // Called by AxiMasterT to consume response beats injected by ShellAdapter.
    std::optional<BBeat> pop_b() {
        if (b_queue_.empty()) return std::nullopt;
        auto front = b_queue_.front();
        b_queue_.pop_front();
        return front;
    }
    std::optional<RBeat> pop_r() {
        if (r_queue_.empty()) return std::nullopt;
        auto front = r_queue_.front();
        r_queue_.pop_front();
        return front;
    }

    // ShellAdapter reads the pending beat (if any) to populate MasterOutputs.
    // pending_aw() returns the beat the master is currently presenting on the
    // AW channel (awvalid = true). Returns nullopt when no AW is pending.
    std::optional<AwBeat> pending_aw() const {
        if (!aw_pending_) return std::nullopt;
        return last_aw_;
    }
    std::optional<WBeat> pending_w() const {
        if (!w_pending_) return std::nullopt;
        return last_w_;
    }
    std::optional<ArBeat> pending_ar() const {
        if (!ar_pending_) return std::nullopt;
        return last_ar_;
    }

    // ShellAdapter injects B/R response beats from the SV wire into the port.
    void inject_b(const BBeat& b) { b_queue_.push_back(b); }
    void inject_r(const RBeat& r) { r_queue_.push_back(r); }

    // Beta-tick inject support: force-clear AW pending state so that AWVALID
    // drops to 0 on the wire. Called by MasterShellAdapter when fault injection
    // suppresses the AW push for the current cycle.
    void force_aw_not_pending() noexcept { aw_pending_ = false; }

  private:
    bool awready_ = false;
    bool wready_  = false;
    bool arready_ = false;
    bool w_delivered_this_tick_ = false;  // beta-tick: max 1 W beat per tick

    bool    aw_pending_ = false;
    AwBeat  last_aw_{};
    bool    w_pending_  = false;
    WBeat   last_w_{};
    bool    ar_pending_ = false;
    ArBeat  last_ar_{};

    std::deque<BBeat>  b_queue_;
    std::deque<RBeat>  r_queue_;
};

}  // namespace detail

// Config struct for Stage 5b ShellAdapter hermetic construction.
struct AxiMasterConfig {
    std::string scenario_yaml;
    std::string read_dump_path;
    std::size_t max_outstanding_write = 1;
    std::size_t max_outstanding_read = 1;
};

// AxiMasterStandalone owns a WireSlavePort and wraps AxiMasterT<WireSlavePort>.
// Exposes the same public API as AxiMasterT: tick(), done(), on_write_completed(),
// on_read_observed(), active_write_count(), active_read_count().
// wire_port() gives ShellAdapter direct access to drain request beats and
// inject response beats on each DPI tick.
class AxiMasterStandalone {
  public:
    explicit AxiMasterStandalone(AxiMasterConfig cfg)
        : wire_slave_(),
          inner_(cfg.scenario_yaml, wire_slave_, cfg.read_dump_path,
                 cfg.max_outstanding_write, cfg.max_outstanding_read) {}

    void tick() { inner_.tick(); }
    bool done() const { return inner_.done(); }
    std::size_t active_write_count() const { return inner_.active_write_count(); }
    std::size_t active_read_count() const { return inner_.active_read_count(); }
    void on_write_completed(std::function<void(const WriteResult&)> cb) {
        inner_.on_write_completed(std::move(cb));
    }
    void on_read_observed(std::function<void(const ReadResult&)> cb) {
        inner_.on_read_observed(std::move(cb));
    }
    void configure_inject(const InjectConfig& inj) noexcept { inner_.configure_inject(inj); }

    // ShellAdapter access to the wire port for request drain + response inject.
    detail::WireSlavePort& wire_port() { return wire_slave_; }

  private:
    detail::WireSlavePort wire_slave_;
    AxiMasterT<detail::WireSlavePort> inner_;
};

}  // namespace ni::cmodel::axi
