// Independent design; AXI4 IHI 0022 rule extraction inspired by cocotbext-axi.
//
// Runtime AXI4 protocol validation. Debug builds compile each rule check into
// an assert that fires on violation; release builds (NDEBUG) compile to a
// no-op. The intent is to catch model-internal bugs during c_model testing
// without paying any cost in release builds (release = inline-empty).
//
// Naming: helpers are pure bool predicates returning true on COMPLIANCE
// (false → violation). The AXI_PROTOCOL_ASSERT macro wraps them.
//
// References: AXI4 IHI 0022, sections A3 (Single Interface Requirements),
// A5 (Transaction Identifiers), B1.4 (Address structure of bursts).
#pragma once
#include "axi/types.hpp"
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <utility>

#ifdef NDEBUG
  #define AXI_PROTOCOL_ASSERT(cond, msg) ((void)0)
#else
  #include <cassert>
  // Use && form so assert() prints the message together with the predicate.
  #define AXI_PROTOCOL_ASSERT(cond, msg) assert((cond) && (msg))
#endif

namespace ni::cmodel::axi::rules {

// ============================================================================
// Stateless field checks
// ============================================================================

// BURST_ENCODING — only FIXED, INCR, WRAP are legal (IHI 0022 A3.4.1).
inline bool check_burst_encoding(Burst b) {
  return b == Burst::FIXED || b == Burst::INCR || b == Burst::WRAP;
}

// SIZE_BOUND — AxSIZE encodes log2(bytes-per-beat); must not exceed
// log2(DATA_BYTES) so a beat fits the bus (IHI 0022 A3.4.1).
inline bool check_size_bound(uint8_t size) {
  // DATA_BYTES = 32 → max size = 5 (32 = 1 << 5).
  constexpr int kMaxSize = 5;  // log2(DATA_BYTES) for DATA_BYTES = 32
  static_assert(DATA_BYTES == (1 << kMaxSize),
                "kMaxSize must equal log2(DATA_BYTES); update if bus widens");
  return size <= kMaxSize;
}

// WRAP_LEN — WRAP bursts require len ∈ {1, 3, 7, 15} so the wrap window
// (len+1)*bytes_per_beat is a power of 2 (IHI 0022 B1.4.3).
inline bool check_wrap_len(Burst b, uint8_t len) {
  if (b != Burst::WRAP) return true;
  return len == 1 || len == 3 || len == 7 || len == 15;
}

// WRAP_ALIGN — WRAP base address must be aligned to bytes-per-beat so each
// beat sits on a (1<<size) grid (IHI 0022 B1.4.3).
inline bool check_wrap_align(Burst b, uint64_t addr, uint8_t size) {
  if (b != Burst::WRAP) return true;
  const uint64_t mask = (1ull << size) - 1u;
  return (addr & mask) == 0;
}

// RESP_ENCODING — only OKAY/EXOKAY/SLVERR/DECERR are legal (IHI 0022 A3.4.4).
inline bool check_resp_encoding(Resp r) {
  return r == Resp::OKAY || r == Resp::EXOKAY ||
         r == Resp::SLVERR || r == Resp::DECERR;
}

// ============================================================================
// Stateful intra-burst checks
// ============================================================================

// W_BEAT_COUNT_WITHIN — the master must not submit more W beats than the
// burst's len+1. (Implicit from IHI 0022 A3.4.1 burst length.)
inline bool check_w_beat_count_within(std::size_t submitted, uint8_t len) {
  return submitted <= static_cast<std::size_t>(len) + 1u;
}

// W_LAST_TIMING — WLAST must be asserted on (and only on) the final beat
// of a write burst (IHI 0022 A3.4.1).
inline bool check_w_last_timing(bool last, std::size_t beat_idx, uint8_t len) {
  return last == (beat_idx == static_cast<std::size_t>(len));
}

// R_BEAT_COUNT_WITHIN — symmetric to W.
inline bool check_r_beat_count_within(std::size_t returned, uint8_t len) {
  return returned <= static_cast<std::size_t>(len) + 1u;
}

// R_LAST_TIMING — RLAST must be on the final R beat (IHI 0022 A3.4.1).
inline bool check_r_last_timing(bool last, std::size_t beat_idx, uint8_t len) {
  return last == (beat_idx == static_cast<std::size_t>(len));
}

// B_ONE_RESPONSE_PER_WRITE — exactly one B per write transaction.
// `b_count` is the number of B responses observed so far for an operation
// whose sub-burst count is `expected`; before completion, b_count <= expected.
inline bool check_b_one_response_per_write(std::size_t b_count,
                                           std::size_t expected) {
  return b_count <= expected;
}

// STRB_VALID_BITS — only the lower WSTRB_WIDTH (= DATA_BYTES) bits of strb
// are defined; higher bits must be 0 (IHI 0022 A3.4.3).
inline bool check_strb_valid_bits(uint32_t strb) {
  if constexpr (DATA_BYTES >= 32) {
    return true;  // full uint32_t range is valid
  } else {
    const uint32_t mask = static_cast<uint32_t>((1ull << DATA_BYTES) - 1u);
    return (strb & ~mask) == 0;
  }
}

// STRB_SPARSE_LEGAL — at any beat, strb may enable only byte lanes inside
// that beat's transfer window: a contiguous (1<<size)-byte window starting
// at byte_lane = beat_addr & (DATA_BYTES-1). Bits outside the window must
// be 0 (IHI 0022 A3.4.3 + lane-positioning rules).
inline bool check_strb_sparse_legal(uint32_t strb, uint8_t size,
                                    uint64_t beat_addr_v) {
  const std::size_t bpb = 1ull << size;
  const std::size_t byte_lane =
      static_cast<std::size_t>(beat_addr_v & (DATA_BYTES - 1));
  const uint64_t window =
      ((1ull << bpb) - 1ull) << byte_lane;
  // strb bits set outside the window are illegal.
  return (static_cast<uint64_t>(strb) & ~window) == 0;
}

// CROSS_4KB — INCR bursts must not cross a 4KB address boundary
// (IHI 0022 A3.4.1). FIXED reuses one address; WRAP is bounded by its window.
// This rule catches model-internal segmentation bugs at the slave (a single
// AW arriving at the slave should already be 4KB-safe).
inline bool check_4kb_cross(uint64_t base_addr, uint8_t len,
                            uint8_t size, Burst burst) {
  if (burst != Burst::INCR) return true;
  const uint64_t bpb = 1ull << size;
  const uint64_t total = (static_cast<uint64_t>(len) + 1u) * bpb;
  if (total == 0) return true;
  const uint64_t last_byte = base_addr + total - 1u;
  return (base_addr >> 12) == (last_byte >> 12);
}

// ============================================================================
// Cross-channel ordering checks
// ============================================================================

// Helper: a per-id "outstanding entry" is non-empty (for slave per-id FIFO
// the value is a std::deque; for master operation-context map it is the
// operation itself — always non-empty when the key exists).
namespace detail {
template <typename T>
inline bool entry_non_empty(const std::deque<T>& d) { return !d.empty(); }
template <typename T>
inline bool entry_non_empty(const T&) { return true; }
}  // namespace detail

// B_ID_MATCH_OUTSTANDING — every B id must match an in-flight write burst
// (IHI 0022 A5.3 + A3.4.4: responses match an outstanding AW.id). Works for
// per-id FIFO maps (slave) and per-id operation maps (master).
template <typename V>
inline bool check_b_id_match_outstanding(
    uint8_t b_id, const std::map<uint8_t, V>& outstanding) {
  auto it = outstanding.find(b_id);
  return it != outstanding.end() && detail::entry_non_empty(it->second);
}

// R_ID_MATCH_OUTSTANDING — symmetric to B.
template <typename V>
inline bool check_r_id_match_outstanding(
    uint8_t r_id, const std::map<uint8_t, V>& outstanding) {
  auto it = outstanding.find(r_id);
  return it != outstanding.end() && detail::entry_non_empty(it->second);
}

// SAME_ID_W_ORDER — same-ID write responses must come back in issue order.
// Structurally enforced by the slave's per-ID FIFO (front = oldest); the rule
// is preserved as a tautology to document the invariant.
inline bool check_same_id_w_order() { return true; }

// SAME_ID_R_ORDER — symmetric to W (per-ID FIFO at the slave + master).
inline bool check_same_id_r_order() { return true; }

// DIFF_ID_INTERLEAVE_ALLOWED — AXI4 permits interleaving of R beats across
// different IDs; W beats follow AW issue order (structurally enforced by the
// master's per-operation push). Tautology, kept for documentation.
inline bool check_diff_id_interleave_allowed() { return true; }

// W_BEFORE_B — the slave must not send a B response before all W beats for
// that burst have been forwarded to memory.
inline bool check_w_before_b(bool all_w_done) { return all_w_done; }

// AW_W_INDEPENDENCE — AW and W are independent channels; AW may arrive
// before, after, or interleaved with W (IHI 0022 A3.3.1). The model already
// queues both; tautology, kept for documentation.
inline bool check_aw_w_independence() { return true; }

// ============================================================================
// Extras
// ============================================================================

// W_NO_INTERLEAVE — AXI4 removed write data interleaving (no WID); W beats
// follow AW issue order. The model carries no WID in WBeat, so this is
// structurally guaranteed. Tautology, kept for documentation.
inline bool check_w_no_interleave() { return true; }

// ============================================================================
// Phase C: AXI4 Exclusive Access (IHI 0022 §A7.2.4)
// ============================================================================

// LOCK_ENCODING — AXI4 AxLOCK is 1-bit; only 0 (Normal) and 1 (Exclusive)
// are legal. AXI3 LOCKED encoding (raw=2) is deprecated and not modeled.
inline bool check_lock_encoding(uint8_t raw) {
  return raw <= 1;
}

// EXCLUSIVE_TOTAL_BYTES — exclusive transfer total bytes ≤ 128
// (IHI 0022 §A7.2.4). Normal transfers are exempt.
inline bool check_exclusive_total_bytes_le_max(LockType lock,
                                                uint8_t len, uint8_t size) {
  if (lock == LockType::Normal) return true;
  const std::size_t total =
      (static_cast<std::size_t>(len) + 1u) * (1ull << size);
  return total <= 128u;
}

// EXCLUSIVE_TOTAL_BEATS — exclusive transfer total beats ≤ 16
// (IHI 0022 §A7.2.4). Normal exempt.
inline bool check_exclusive_total_beats_le_max(LockType lock, uint8_t len) {
  if (lock == LockType::Normal) return true;
  return len <= 15u;
}

// EXCLUSIVE_POW2 — exclusive total beats (len+1) must be a power of 2.
// Allowed len: 0, 1, 3, 7, 15 (IHI 0022 §A7.2.4). Normal exempt.
inline bool check_exclusive_total_pow2(LockType lock, uint8_t len) {
  if (lock == LockType::Normal) return true;
  const std::size_t beats = static_cast<std::size_t>(len) + 1u;
  return (beats > 0u) && ((beats & (beats - 1u)) == 0u);
}

// EXCLUSIVE_ALIGN — exclusive base address must be aligned to total burst
// bytes (IHI 0022 §A7.2.4). Combined with EXCLUSIVE_POW2 this ensures the
// (total-1) mask is well-defined. Normal exempt.
//
// Precondition: callers should also invoke check_exclusive_total_pow2()
// before relying on the (total-1) mask in downstream WRAP arithmetic; for a
// non-power-of-2 total the mask underflow-wraps and the alignment check
// loses meaning.
inline bool check_exclusive_addr_aligned_to_total(LockType lock, uint64_t addr,
                                                    uint8_t len, uint8_t size) {
  if (lock == LockType::Normal) return true;
  const std::size_t total =
      (static_cast<std::size_t>(len) + 1u) * (1ull << size);
  return (addr & (static_cast<uint64_t>(total) - 1u)) == 0u;
}

// EXCLUSIVE_BURST_FIXED — FIXED burst is not allowed for exclusive
// (IHI 0022 §A7.2.4). Normal exempt.
inline bool check_exclusive_burst_not_fixed(LockType lock, Burst burst) {
  if (lock == LockType::Normal) return true;
  return burst != Burst::FIXED;
}

// EXCLUSIVE_WRITE_MATCHES_READ — the slave-side exclusive monitor compares
// the incoming exclusive AW against the recorded exclusive AR tag (ID +
// address window + size + length + burst + cache + protection per
// IHI 0022 §A7.2.4). Template-driven so the tag struct can live in
// axi_slave.hpp without a circular include here.
//
// Helper: compute the exclusive-monitor tag's [addr_start, addr_end) range
// from an AW/AR beat. INCR → linear [addr, addr + total). WRAP → aligned
// [wrap_lower, wrap_upper) window. FIXED is rejected upstream by
// check_exclusive_burst_not_fixed (IHI 0022 §A7.2.4 bars exclusive FIXED), so
// this helper need not handle it; the E2 normal-AW invalidation path that
// also consults FIXED uses a separate single-beat span.
//
// Precondition: callers MUST have already passed the beat through
// check_exclusive_total_pow2() and check_exclusive_addr_aligned_to_total();
// otherwise the WRAP-arm (total-1) mask is undefined.
template <typename Beat>
inline std::pair<uint64_t, uint64_t> compute_tag_range(const Beat& b) {
  const std::size_t bpb = 1ull << b.size;
  const std::size_t total = (static_cast<std::size_t>(b.len) + 1u) * bpb;
  if (b.burst == Burst::WRAP) {
    const uint64_t wrap_lower =
        b.addr & ~(static_cast<uint64_t>(total) - 1u);
    return {wrap_lower, wrap_lower + total};
  }
  return {b.addr, b.addr + total};
}

// Precondition: the caller MUST have already passed the incoming AW through
// check_exclusive_addr_aligned_to_total() and check_exclusive_total_pow2();
// otherwise the WRAP-arm (total-1) mask is undefined and the computed
// [aw_start, aw_end) window is meaningless.
template <typename ExclusiveTagT>
inline bool check_exclusive_write_matches_read_tag(const ExclusiveTagT& tag,
                                                    const AwBeat& aw) {
  if (!tag.ready) return false;
  const auto [aw_start, aw_end] = compute_tag_range(aw);
  return tag.addr_start == aw_start
      && tag.addr_end   == aw_end
      && tag.len        == aw.len
      && tag.size       == aw.size
      && tag.burst      == aw.burst
      && tag.cache      == aw.cache
      && tag.prot       == aw.prot;
}

}  // namespace ni::cmodel::axi::rules
