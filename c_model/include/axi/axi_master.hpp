// Algorithms ported from cocotbext-axi (MIT) — see axi/ATTRIBUTION.md
#pragma once
#include "axi/types.hpp"
#include "axi/protocol_rules.hpp"
#include "axi/scenario_parser.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ni::cmodel::axi {

// Sub-burst descriptor produced by split_into_sub_bursts(). Each describes one
// AXI4 AW/AR (with its own len/addr) carved out of a single scenario_txn.
struct BurstSpec {
  uint64_t addr;
  uint8_t  len;
  uint8_t  size;
  Burst    burst;
};

// AXI4 (IHI 0022 A3.4.1) requires INCR bursts not to cross a 4KB boundary, and
// caps a single burst at 256 beats. A scenario_txn that violates either is
// auto-segmented here into a chain of legal sub-bursts. WRAP/FIXED never need
// splitting — WRAP wraps inside [wrap_lower, wrap_upper) by construction;
// FIXED reuses one address.
inline std::vector<BurstSpec>
split_into_sub_bursts(const ScenarioTransaction& txn) {
  std::vector<BurstSpec> out;
  if (txn.burst != Burst::INCR) {
    out.push_back(BurstSpec{txn.addr, txn.len, txn.size, txn.burst});
    return out;
  }
  const std::size_t bpb = 1ull << txn.size;
  uint64_t addr = txn.addr;
  std::size_t beats_remaining = static_cast<std::size_t>(txn.len) + 1u;
  while (beats_remaining > 0) {
    const std::size_t bytes_to_4kb = 0x1000u - (addr & 0xFFFu);
    std::size_t beats_to_4kb = bytes_to_4kb / bpb;
    if (beats_to_4kb == 0) beats_to_4kb = beats_remaining;  // already aligned
    const std::size_t beats_this =
        std::min({beats_to_4kb, beats_remaining, std::size_t{256}});
    out.push_back(BurstSpec{
        addr, static_cast<uint8_t>(beats_this - 1),
        txn.size, Burst::INCR});
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
  uint64_t addr;                       // original user txn.addr
  uint8_t  size;                       // log2(bytes_per_beat)
  uint8_t  len;                        // beats - 1
  Burst    burst;
  // Phase C: mirrors the originating scenario_txn.lock so the scoreboard can
  // distinguish a failed exclusive write (lock=Exclusive + resp=OKAY → no
  // memory commit) from a normal write (resp=OKAY → commit) without peeking
  // at the slave's exclusive monitor state.
  LockType lock = LockType::Normal;
  std::vector<uint8_t> data;           // packed user bytes, (len+1)*(1<<size)
  std::vector<uint32_t> strb_per_beat; // bus-level WSTRB per beat (lane-positioned)
  Resp resp;
  uint8_t id;
  std::size_t scenario_line;
};

struct ReadResult {
  uint64_t addr;                       // original user txn.addr
  uint8_t  size;
  uint8_t  len;
  Burst    burst;
  std::vector<uint8_t> data;           // packed user bytes, (len+1)*(1<<size)
  Resp resp;
  uint8_t id;
  std::size_t scenario_line;
};

template<typename SlaveT>
class AxiMasterT {
public:
  AxiMasterT(const std::string& scenario_yaml,
             SlaveT& slave,
             const std::string& read_dump_path,
             std::size_t max_outstanding_write = 1,
             std::size_t max_outstanding_read  = 1)
      : slave_(slave),
        max_out_w_(max_outstanding_write),
        max_out_r_(max_outstanding_read) {
    sc_ = load_scenario(scenario_yaml);
    read_dump_.open(read_dump_path);
    if (!read_dump_.is_open())
      throw std::runtime_error("AxiMaster: cannot open read_dump_path: " + read_dump_path);
  }

  void tick() {
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
      AXI_PROTOCOL_ASSERT(rules::check_b_id_match_outstanding(b->id, active_write_ops_),
                          "B_ID_MATCH_OUTSTANDING: B id must match an in-flight write operation");
      auto it = active_write_ops_.find(b->id);
      if (it == active_write_ops_.end()) continue;
      auto& op = it->second;
      AXI_PROTOCOL_ASSERT(
          rules::check_b_one_response_per_write(op.b_count_ + 1, op.sub_bursts.size()),
          "B_ONE_RESPONSE_PER_WRITE: B responses exceed the operation's expected count");
      ++op.b_count_;
      if (static_cast<uint8_t>(b->resp) > static_cast<uint8_t>(op.worst_resp_))
        op.worst_resp_ = b->resp;
      if (op.b_count_ == op.sub_bursts.size()) {
        // WriteResult carries the ORIGINAL user txn (addr, size, len, burst)
        // — NOT sub-burst geometry. The scoreboard re-derives per-beat addr
        // from the original txn.
        if (wcb_) wcb_(WriteResult{op.src_txn.addr,
                                    op.src_txn.size,
                                    op.src_txn.len,
                                    op.src_txn.burst,
                                    op.src_txn.lock,
                                    op.data,
                                    op.strb_per_beat,
                                    op.worst_resp_,
                                    b->id,
                                    op.src_txn.scenario_line});
        active_write_ops_.erase(it);
      }
    }

    // ===== Drain R responses =====
    //
    // R beats arrive in sub-burst order (per-id FIFO at the slave). Track
    // which sub-burst the current beat belongs to via cur_r_sub_idx_ +
    // r_beats_in_cur_; advance to the next sub-burst on r->last. Operation
    // completes when cur_r_sub_idx_ reaches sub_bursts.size().
    while (auto r = slave_.pop_r()) {
      auto it = active_read_ops_.find(r->id);
      if (it == active_read_ops_.end()) continue;
      auto& op = it->second;
      const auto& sub = op.sub_bursts[op.cur_r_sub_idx_];
      const std::size_t bpb = 1ull << sub.size;
      // Lane-positioned bus: byte j on the bus is at lane (byte_lane + j),
      // where byte_lane = (per-beat addr) mod DATA_BYTES. Per-beat addr from
      // the CURRENT sub-burst (FIXED stays; INCR advances; WRAP wraps).
      // Lane room caps the per-beat payload at DATA_BYTES - byte_lane; any
      // trailing user bytes that would have fallen off the bus are zero-
      // padded so downstream packed-buffer offsets stay aligned at the
      // operation-level (beat * bpb) layout.
      const uint64_t beat_addr_v = beat_addr(sub.addr, sub.len, sub.size,
                                             sub.burst, op.r_beats_in_cur_);
      const std::size_t byte_lane =
          static_cast<std::size_t>(beat_addr_v & (DATA_BYTES - 1));
      const std::size_t lane_room =
          (byte_lane < DATA_BYTES) ? (DATA_BYTES - byte_lane) : 0;
      const std::size_t copy_bytes = std::min(bpb, lane_room);
      for (std::size_t i = 0; i < copy_bytes; ++i)
        op.read_accumulator.push_back(r->data[byte_lane + i]);
      for (std::size_t i = copy_bytes; i < bpb; ++i)
        op.read_accumulator.push_back(0);
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
        if (rcb_) rcb_(ReadResult{op.src_txn.addr,
                                    op.src_txn.size,
                                    op.src_txn.len,
                                    op.src_txn.burst,
                                    op.read_accumulator,
                                    op.worst_resp_, r->id,
                                    op.src_txn.scenario_line});
        active_read_ops_.erase(it);
      }
    }

    // ===== Admission =====
    //
    // One OperationContext per scenario_txn. Same-id concurrent operations
    // remain disallowed (operation-level ordering) — sub-burst stacking
    // happens within ONE operation via slave's per-id FIFO.
    while (next_txn_idx_ < sc_.transactions.size()) {
      const auto& txn = sc_.transactions[next_txn_idx_];
      if (txn.op == ScenarioTransaction::Op::Write) {
        if (active_write_ops_.size() >= max_out_w_) break;
        if (active_write_ops_.count(txn.id)) break;
        OperationContext op;
        op.src_txn = txn;
        op.sub_bursts = split_into_sub_bursts(txn);
        op.data = load_write_data_(txn.data_file,
                                    static_cast<std::size_t>(txn.len + 1u) * (1ull << txn.size));
        op.strb_per_beat = load_strb_file_(txn.strb_file,
                                            static_cast<std::size_t>(txn.len + 1u));
        active_write_ops_.emplace(txn.id, std::move(op));
      } else {
        if (active_read_ops_.size() >= max_out_r_) break;
        if (active_read_ops_.count(txn.id)) break;
        OperationContext op;
        op.src_txn = txn;
        op.sub_bursts = split_into_sub_bursts(txn);
        active_read_ops_.emplace(txn.id, std::move(op));
      }
      ++next_txn_idx_;
    }

    // ===== Push AW + W beats per operation =====
    //
    // Walk each operation's sub-bursts in order. For the current sub-burst:
    //   - Push its AW if not yet pushed.
    //   - Push its W beats up to its len+1; the W payload is a slice of the
    //     operation-level packed user buffer starting at op-level beat index
    //     (which == sum of preceding sub-bursts' beat counts + cur_w_in_sub_).
    // Advance to the next sub-burst when the current sub-burst's W beats are
    // fully pushed.
    for (auto& [id, op] : active_write_ops_) {
      push_writes_(id, op);
    }

    // ===== Push AR per operation =====
    //
    // Walk sub-bursts in order. Push each sub-burst's AR — slave's per-id
    // FIFO accepts multi-outstanding same-id ARs.
    for (auto& [id, op] : active_read_ops_) {
      while (op.next_ar_sub_idx_ < op.sub_bursts.size()) {
        const auto& sub = op.sub_bursts[op.next_ar_sub_idx_];
        ArBeat ar{};
        ar.id = id;
        // Align AR addr DOWN to (1<<size) boundary, symmetric with AW.
        ar.addr = sub.addr & ~((1ull << sub.size) - 1);
        ar.len = sub.len; ar.size = sub.size; ar.burst = sub.burst;
        // Phase C: wire-through scenario_txn.lock onto AR.lock (1-bit).
        ar.lock = (op.src_txn.lock == LockType::Exclusive) ? 1u : 0u;
        if (!slave_.push_ar(ar)) break;
        ++op.next_ar_sub_idx_;
      }
    }
  }

  bool done() const {
    return next_txn_idx_ >= sc_.transactions.size()
        && active_write_ops_.empty() && active_read_ops_.empty();
  }

  void on_write_completed(std::function<void(const WriteResult&)> cb) { wcb_ = std::move(cb); }
  void on_read_observed  (std::function<void(const ReadResult&)>  cb) { rcb_ = std::move(cb); }

private:
  static std::vector<uint8_t> load_write_data_(const std::string& path, std::size_t expected_bytes) {
    std::ifstream f(path);
    if (!f.is_open())
      throw std::runtime_error("AxiMaster: cannot open data_file: " + path);
    std::vector<uint8_t> bytes;
    std::string tok;
    while (f >> tok) bytes.push_back(static_cast<uint8_t>(std::stoul(tok, nullptr, 16)));
    if (bytes.size() < expected_bytes)
      throw std::runtime_error("AxiMaster: data_file too short (" + std::to_string(bytes.size())
                               + " < " + std::to_string(expected_bytes) + "): " + path);
    return bytes;
  }

  static std::vector<uint32_t> load_strb_file_(const std::string& path,
                                                std::size_t expected_beats,
                                                uint32_t default_full = 0xFFFF'FFFFu) {
    if (path.empty()) {
      return std::vector<uint32_t>(expected_beats, default_full);
    }
    std::ifstream f(path);
    if (!f.is_open())
      throw std::runtime_error("AxiMaster: cannot open strb_file: " + path);
    std::vector<uint32_t> strbs;
    std::string tok;
    while (f >> tok) {
      unsigned long long v = std::stoull(tok, nullptr, 16);
      if (v > 0xFFFFFFFFull)
        throw std::runtime_error("AxiMaster: strb_file token out of uint32_t range: " + tok);
      strbs.push_back(static_cast<uint32_t>(v));
    }
    if (strbs.size() != expected_beats)
      throw std::runtime_error("AxiMaster: strb_file line count " + std::to_string(strbs.size())
                                + " != expected beats " + std::to_string(expected_beats)
                                + ": " + path);
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
    std::vector<uint8_t>  data;            // packed user bytes, full operation
    std::vector<uint32_t> strb_per_beat;   // operation-level, 1 entry per user beat
    std::size_t next_aw_sub_idx_ = 0;      // index of next sub-burst's AW to push
    std::size_t cur_w_sub_idx_   = 0;      // sub-burst currently emitting W beats
    std::size_t w_pushed_in_cur_ = 0;      // W beats pushed for current sub-burst
    std::size_t b_count_         = 0;      // B responses received (per sub-burst)
    Resp worst_resp_             = Resp::OKAY;
    // Read-side state
    std::size_t next_ar_sub_idx_ = 0;      // index of next sub-burst's AR to push
    std::size_t cur_r_sub_idx_   = 0;      // sub-burst currently absorbing R beats
    std::size_t r_beats_in_cur_  = 0;
    std::vector<uint8_t> read_accumulator; // packed user bytes, full operation
  };

  // Per-operation W push: walk sub-bursts in order, pushing AW + W beats.
  // The operation-level beat index (used to slice op.data / op.strb_per_beat)
  // is the running sum of preceding sub-bursts' beat counts plus the current
  // sub-burst's in-progress index.
  void push_writes_(uint8_t id, OperationContext& op) {
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
        aw.id = id; aw.addr = aligned_addr;
        aw.len = sub.len; aw.size = sub.size; aw.burst = sub.burst;
        // Phase C: pure wire-through of scenario_txn.lock onto AW.lock. AXI4
        // AxLOCK is 1-bit; LockType::Exclusive maps to 1, Normal to 0. Every
        // sub-burst of one operation carries the same lock value.
        aw.lock = (op.src_txn.lock == LockType::Exclusive) ? 1u : 0u;
        if (!slave_.push_aw(aw)) return;  // backpressure: retry next tick
        ++op.next_aw_sub_idx_;
      }
      const std::size_t bpb = 1ull << sub.size;
      while (op.w_pushed_in_cur_ <= sub.len) {
        WBeat w{};
        // Lane-positioned bus: byte j of the user payload lands at bus lane
        // (byte_lane + j); byte_lane = per-beat-addr mod DATA_BYTES.
        const uint64_t beat_addr_v = beat_addr(sub.addr, sub.len, sub.size,
                                               sub.burst,
                                               op.w_pushed_in_cur_);
        const std::size_t byte_lane =
            static_cast<std::size_t>(beat_addr_v & (DATA_BYTES - 1));
        w.data.fill(0);
        const std::size_t writable = (byte_lane < DATA_BYTES)
            ? std::min<std::size_t>(bpb, DATA_BYTES - byte_lane)
            : 0;
        const std::size_t op_beat = op_beat_base + op.w_pushed_in_cur_;
        for (std::size_t j = 0; j < writable; ++j) {
          const std::size_t off = op_beat * bpb + j;
          w.data[byte_lane + j] = (off < op.data.size()) ? op.data[off] : 0;
        }
        const uint32_t lane_mask =
            static_cast<uint32_t>(((1ull << bpb) - 1) << byte_lane);
        w.strb = op.strb_per_beat[op_beat] & lane_mask;
        w.last = (op.w_pushed_in_cur_ == sub.len);
        if (!slave_.push_w(w)) return;  // backpressure
        ++op.w_pushed_in_cur_;
      }
      // Current sub-burst's W beats fully pushed; advance to the next.
      op_beat_base += static_cast<std::size_t>(sub.len) + 1u;
      ++op.cur_w_sub_idx_;
      op.w_pushed_in_cur_ = 0;
    }
  }

  Scenario   sc_;
  SlaveT&    slave_;
  std::size_t max_out_w_, max_out_r_;
  std::size_t next_txn_idx_ = 0;
  std::ofstream read_dump_;
  std::function<void(const WriteResult&)> wcb_;
  std::function<void(const ReadResult&)>  rcb_;
  std::map<uint8_t, OperationContext> active_write_ops_;
  std::map<uint8_t, OperationContext> active_read_ops_;
};

class AxiSlave;
using AxiMaster = AxiMasterT<AxiSlave>;

}
