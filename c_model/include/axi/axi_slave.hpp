// Algorithms ported from cocotbext-axi (MIT) — see axi/ATTRIBUTION.md
#pragma once
#include "axi/memory.hpp"
#include "axi/memory_port.hpp"
#include "axi/protocol_rules.hpp"
#include "axi/types.hpp"
#include <deque>
#include <map>
#include <optional>
#include <tuple>

namespace ni::cmodel::axi {

// Config struct for Stage 5b Wrap hermetic construction.
// AxiSlave(AxiSlaveConfig) owns its Memory internally; no IMemoryPort& ref needed.
struct AxiSlaveConfig {
    uint64_t memory_base_addr = 0;
    std::size_t memory_size = 65536;
    std::size_t write_latency = 1;
    std::size_t read_latency = 1;
    std::size_t channel_queue_depth = 32;
};

// AXI4 IHI 0022 §A7.2.4 exclusive access tag, recorded by the slave's
// exclusive monitor when an exclusive AR is admitted. A subsequent exclusive
// AW on the same ID matches against the recorded attributes (address range,
// length, size, burst, cache, protection); a mismatch — or any intervening
// normal AW that overlaps tag's address range — invalidates the tag.
struct ExclusiveTag {
    uint64_t addr_start = 0;  // inclusive
    uint64_t addr_end = 0;    // exclusive
    uint8_t len = 0;
    uint8_t size = 0;
    Burst burst = Burst::INCR;
    uint8_t cache = 0;
    uint8_t prot = 0;
    bool ready = false;  // set on RLAST; gates exclusive-write match
    // Number of RLASTs that must still arrive before this tag becomes ready.
    // Set at E1 (AR admit) to (active_reads_[id].size() + 1) so any RLASTs from
    // older same-id ARs still in flight do not prematurely promote a newly-set
    // tag — only the tag's own AR's RLAST takes it to ready.
    std::size_t pending_rlasts = 0;
};

class AxiSlave {
  public:
    // Stage 5b standalone ctor: owns Memory internally, no IMemoryPort& ref needed.
    // owned_memory_ is declared before memory_port_ so it is constructed first.
    explicit AxiSlave(AxiSlaveConfig cfg)
        : owned_memory_(std::in_place, cfg.memory_base_addr, cfg.memory_size, cfg.write_latency,
                        cfg.read_latency),
          memory_port_(*owned_memory_),
          depth_(cfg.channel_queue_depth) {}

    // Original ctor: caller supplies an external IMemoryPort& (preserved verbatim).
    explicit AxiSlave(IMemoryPort& memory_port, std::size_t channel_queue_depth = 32)
        : memory_port_(memory_port), depth_(channel_queue_depth) {}

    bool push_aw(const AwBeat& b) {
        if (aw_q_.size() >= depth_) return false;
        aw_q_.push_back(b);
        return true;
    }
    bool push_w(const WBeat& b) {
        if (w_q_.size() >= depth_) return false;
        w_q_.push_back(b);
        return true;
    }
    bool push_ar(const ArBeat& b) {
        if (ar_q_.size() >= depth_) return false;
        ar_q_.push_back(b);
        return true;
    }

    std::optional<BBeat> pop_b() {
        if (b_q_.empty()) return std::nullopt;
        auto r = b_q_.front();
        b_q_.pop_front();
        return r;
    }
    std::optional<RBeat> pop_r() {
        if (r_q_.empty()) return std::nullopt;
        auto r = r_q_.front();
        r_q_.pop_front();
        return r;
    }

    void tick();  // implemented in Task 3.2+; also advances owned_memory_ if present

    // Advance the owned Memory one tick (standalone-ctor path only).
    // External-ref ctor: caller is responsible for ticking the IMemoryPort
    // implementation (e.g. a Memory instance) separately, as the tests do.
    // Called at the top of tick() so memory responses are available in the
    // same cycle the slave drains them in step 1.
    void tick_memory() {
        if (owned_memory_) owned_memory_->tick();
    }
    void set_memory_bounds(uint64_t base, std::size_t size) {
        bounds_base_ = base;
        bounds_size_ = size;
        bounds_set_ = true;
    }

    std::size_t aw_q_size() const { return aw_q_.size(); }
    std::size_t w_q_size() const { return w_q_.size(); }
    std::size_t ar_q_size() const { return ar_q_.size(); }
    std::size_t b_q_size() const { return b_q_.size(); }
    std::size_t r_q_size() const { return r_q_.size(); }

    // Phase C: exclusive-monitor inspection helpers (unit-test surface).
    bool has_exclusive_tag(uint8_t id) const {
        return exclusive_tags_.find(id) != exclusive_tags_.end();
    }
    bool exclusive_tag_ready(uint8_t id) const {
        auto it = exclusive_tags_.find(id);
        return it != exclusive_tags_.end() && it->second.ready;
    }
    ExclusiveTag peek_exclusive_tag(uint8_t id) const { return exclusive_tags_.at(id); }
    // Phase C audit fix (D3-2): expose pending-RLAST counter so the race
    // regression test can verify the tag-promotion accounting end to end.
    std::size_t exclusive_tag_pending_rlasts(uint8_t id) const {
        return exclusive_tags_.at(id).pending_rlasts;
    }

  private:
    // tick() phase methods — one per numbered step in the AXI4 slave pipeline.
    // Each operates on member state and preserves the original step's semantics.
    void tick_drain_write_resp_();          // Step 1
    void tick_drain_read_resp_();           // Step 2
    void tick_admit_aw_();                  // Step 3 (incl. E2/E3 exclusive monitor)
    void tick_submit_w_();                  // Step 4 (incl. E4 exclusive-write suppress)
    void tick_drain_failed_exclusive_b_();  // Step 4b
    void tick_admit_ar_();                  // Step 5 (incl. E1 exclusive monitor)
    void tick_submit_ar_();                 // Step 6

    // Owned memory for standalone ctor (AxiSlaveConfig path).
    // Declared before memory_port_ so it is constructed first; the standalone
    // ctor's initializer list can then safely bind memory_port_ to *owned_memory_.
    // External-ref ctor leaves this nullopt.
    std::optional<Memory> owned_memory_;

    struct WriteBurstState {
        AwBeat aw;
        std::size_t beats_submitted = 0;
        std::size_t beats_completed = 0;
        Resp worst_resp = Resp::OKAY;  // accumulate worst across burst
        // Phase C: locked-in at AW admission. is_exclusive=true means W beats are
        // gated (E4: suppress memory submit when !exclusive_match), and the B
        // response carries EXOKAY / OKAY based on exclusive_match (E6).
        bool is_exclusive = false;
        bool exclusive_match = false;
    };
    struct ReadBurstState {
        ArBeat ar;
        std::size_t beats_submitted = 0;
        std::size_t beats_returned = 0;
    };

    IMemoryPort& memory_port_;
    std::size_t depth_;
    std::deque<AwBeat> aw_q_;
    std::deque<WBeat> w_q_;
    std::deque<ArBeat> ar_q_;
    std::deque<BBeat> b_q_;
    std::deque<RBeat> r_q_;
    std::deque<uint8_t> aw_issue_order_;  // oldest first; for W matching per AXI4
    // Per-ID FIFO: AXI4 requires same-ID burst responses to come back in issue
    // order, so each id maps to a deque of in-flight bursts (front = oldest).
    // Phase B-5a master can stack same-id sub-bursts (4KB cross auto-split), so
    // a single-slot map would block. Multi-id concurrency still works via the
    // map keying — each id has its own FIFO chain.
    std::map<uint8_t, std::deque<WriteBurstState>> active_writes_;
    std::map<uint8_t, std::deque<ReadBurstState>> active_reads_;
    // Phase C: per-ID exclusive monitor (IHI 0022 §A7.2). At most one tag per
    // ID; admitted on exclusive AR (E1), invalidated on overlapping normal AW
    // (E2) or on exclusive AW lookup (E3), turned ready on RLAST (E5).
    std::map<uint8_t, ExclusiveTag> exclusive_tags_;
    uint64_t bounds_base_ = 0;
    std::size_t bounds_size_ = 0;
    bool bounds_set_ = false;
};

inline void AxiSlave::tick() {
    // 0. Advance owned Memory one tick (standalone-ctor path only).
    //    Decrement latency counters BEFORE draining responses (step 1) so that
    //    a write submitted in this same tick can fire immediately on write_lat=0,
    //    or arrive in the next tick on write_lat=1 (the common case).
    tick_memory();
    tick_drain_write_resp_();          // 1
    tick_drain_read_resp_();           // 2
    tick_admit_aw_();                  // 3
    tick_submit_w_();                  // 4
    tick_drain_failed_exclusive_b_();  // 4b
    tick_admit_ar_();                  // 5
    tick_submit_ar_();                 // 6
}

inline void AxiSlave::tick_drain_write_resp_() {
    // 1. Drain memory write responses → match by id, advance OLDEST burst.
    //    Per-ID FIFO: same-id bursts complete in issue order (AXI4 IHI 0022
    //    A5.3 — ordering of transactions with the same AXI ID is preserved).
    while (auto resp = memory_port_.pop_write_resp()) {
        AXI_PROTOCOL_ASSERT(rules::check_resp_encoding(resp->resp),
                            "RESP_ENCODING: memory BRESP must be a legal AXI4 response");
        AXI_PROTOCOL_ASSERT(
            rules::check_b_id_match_outstanding(resp->id, active_writes_),
            "B_ID_MATCH_OUTSTANDING: memory BRESP id must match an in-flight write burst");
        auto it = active_writes_.find(resp->id);
        if (it == active_writes_.end() || it->second.empty()) continue;
        auto& st = it->second.front();
        ++st.beats_completed;
        if (static_cast<uint8_t>(resp->resp) > static_cast<uint8_t>(st.worst_resp))
            st.worst_resp = resp->resp;
        if (st.beats_completed == static_cast<std::size_t>(st.aw.len) + 1) {
            AXI_PROTOCOL_ASSERT(rules::check_w_before_b(st.beats_submitted ==
                                                        static_cast<std::size_t>(st.aw.len) + 1u),
                                "W_BEFORE_B: B response fired before all W beats submitted");
            // Phase C — E6: response priority. Memory error (SLVERR/DECERR) trumps
            // exclusive status. Otherwise an exclusive AW with matched tag returns
            // EXOKAY; everything else (normal AW, or exclusive with no-match — but
            // no-match is suppressed in step 4 / drained in step 4b) returns
            // worst_resp (defaults to OKAY).
            Resp final_resp;
            if (st.worst_resp == Resp::DECERR || st.worst_resp == Resp::SLVERR) {
                final_resp = st.worst_resp;
            } else if (st.is_exclusive && st.exclusive_match) {
                final_resp = Resp::EXOKAY;
            } else {
                final_resp = st.worst_resp;  // OKAY for normal writes
            }
            b_q_.push_back(BBeat{st.aw.id, final_resp, 0});
            it->second.pop_front();
            if (it->second.empty()) active_writes_.erase(it);
            // Note: aw_issue_order_.front() was popped when the burst's W beats were
            // fully submitted to memory (step 4), so the W routing for the NEXT
            // queued burst is correct even while this one waits on memory latency.
        }
    }
}

inline void AxiSlave::tick_drain_read_resp_() {
    // 2. Drain memory read responses → push R beats (advance OLDEST per id).
    while (auto rresp = memory_port_.pop_read_resp()) {
        AXI_PROTOCOL_ASSERT(rules::check_resp_encoding(rresp->resp),
                            "RESP_ENCODING: memory RRESP must be a legal AXI4 response");
        AXI_PROTOCOL_ASSERT(
            rules::check_r_id_match_outstanding(rresp->id, active_reads_),
            "R_ID_MATCH_OUTSTANDING: memory RRESP id must match an in-flight read burst");
        auto it = active_reads_.find(rresp->id);
        if (it == active_reads_.end() || it->second.empty()) continue;
        auto& st = it->second.front();
        AXI_PROTOCOL_ASSERT(rules::check_r_beat_count_within(st.beats_returned + 1, st.ar.len),
                            "R_BEAT_COUNT_WITHIN: R beats returned exceed burst len+1");
        RBeat rb{};
        rb.id = st.ar.id;
        rb.data = rresp->data;
        rb.resp = rresp->resp;
        rb.last = (st.beats_returned + 1 == static_cast<std::size_t>(st.ar.len) + 1);
        rb.user = 0;
        AXI_PROTOCOL_ASSERT(
            rules::check_r_last_timing(rb.last, st.beats_returned, st.ar.len),
            "R_LAST_TIMING: RLAST must be asserted on (and only on) the final R beat");
        r_q_.push_back(rb);
        ++st.beats_returned;
        if (rb.last) {
            // Phase C — E5: an exclusive AR's tag becomes ready on RLAST so a
            // following exclusive AW can match it (§A7.2.3). Only an existing tag
            // for this id is promoted; absent tag (normal AR) is left alone.
            //
            // Audit fix (D3-2): when a 2nd exclusive AR with the same ID overwrites
            // an in-flight tag, the in-flight AR's RLAST must not falsely promote
            // the NEW tag. pending_rlasts is set at E1 to (in-flight ARs + self) so
            // every same-id RLAST decrements once; only when the counter reaches 0
            // has the tag's own AR's RLAST arrived.
            auto tag_it = exclusive_tags_.find(rb.id);
            if (tag_it != exclusive_tags_.end() && tag_it->second.pending_rlasts > 0) {
                --tag_it->second.pending_rlasts;
                if (tag_it->second.pending_rlasts == 0) {
                    tag_it->second.ready = true;
                }
            }
            it->second.pop_front();
            if (it->second.empty()) active_reads_.erase(it);
        }
    }
}

inline void AxiSlave::tick_admit_aw_() {
    // 3. Start new AW (with burst-atomic OOB pre-check).
    //    Per-ID FIFO admits multi-outstanding same-id bursts: just append to
    //    the id's chain. AXI4 same-id ordering is preserved by FIFO discipline
    //    in steps 1/4 (B drain + W routing).
    while (!aw_q_.empty()) {
        auto& aw = aw_q_.front();
        AXI_PROTOCOL_ASSERT(rules::check_burst_encoding(aw.burst),
                            "BURST_ENCODING: AW.burst must be FIXED, INCR, or WRAP");
        AXI_PROTOCOL_ASSERT(rules::check_size_bound(aw.size),
                            "SIZE_BOUND: AW.size must be <= log2(DATA_BYTES)");
        AXI_PROTOCOL_ASSERT(rules::check_wrap_len(aw.burst, aw.len),
                            "WRAP_LEN: AW.len must be 1, 3, 7, or 15 for WRAP burst");
        AXI_PROTOCOL_ASSERT(rules::check_wrap_align(aw.burst, aw.addr, aw.size),
                            "WRAP_ALIGN: AW.addr must be aligned to (1<<size) for WRAP burst");
        AXI_PROTOCOL_ASSERT(rules::check_4kb_cross(aw.addr, aw.len, aw.size, aw.burst),
                            "CROSS_4KB: INCR burst at slave must not cross a 4KB boundary");
        if (bounds_set_) {
            std::size_t bpb = 1ull << aw.size;
            std::size_t total = bpb * (static_cast<std::size_t>(aw.len) + 1);
            // WRAP confines all beats to [wrap_lower, wrap_upper); check that
            // window rather than the linear [addr, addr+total). FIXED/INCR span
            // the linear range.
            bool oob = false;
            if (aw.burst == Burst::WRAP) {
                const uint64_t wrap_lower = aw.addr & ~(static_cast<uint64_t>(total) - 1u);
                const uint64_t wrap_upper = wrap_lower + total;
                oob = (wrap_lower < bounds_base_) || (wrap_upper > bounds_base_ + bounds_size_);
            } else {
                oob = (aw.addr < bounds_base_) || (aw.addr + total > bounds_base_ + bounds_size_);
            }
            if (oob) {
                b_q_.push_back(BBeat{aw.id, Resp::DECERR, 0});
                // Discard the W beats corresponding to this burst
                for (std::size_t i = 0; i < static_cast<std::size_t>(aw.len) + 1; ++i) {
                    if (w_q_.empty()) break;
                    w_q_.pop_front();
                }
                aw_q_.pop_front();
                continue;
            }
        }
        // Phase C — E2/E3: exclusive-monitor side-effects at AW admission.
        //   E2 normal AW (lock=0): erase every tag whose address window overlaps
        //                          this AW's effective address range (§A7.2.3:
        //                          any non-exclusive write that may affect the
        //                          monitored region invalidates the tag).
        //   E3 exclusive AW (lock=1): look up the per-ID tag; check attributes
        //                             match (delegated to protocol_rules helper);
        //                             record match-or-not and ERASE the tag
        //                             regardless of match — a single exclusive
        //                             AW consumes the reservation per §A7.2.3.
        // exclusive_match feeds W submit (E4: suppress memory_port on no-match)
        // and B drain (E6: EXOKAY on match, OKAY on no-match).
        AXI_PROTOCOL_ASSERT(rules::check_lock_encoding(aw.lock),
                            "LOCK_ENCODING: AW.lock must be 0 or 1");
        const auto aw_lock = static_cast<LockType>(aw.lock);
        bool is_exclusive_write = false;
        bool exclusive_match = false;
        if (aw_lock == LockType::Exclusive) {
            AXI_PROTOCOL_ASSERT(rules::check_exclusive_total_bytes_le_max(aw_lock, aw.len, aw.size),
                                "EXCLUSIVE_TOTAL_BYTES: AW exclusive total bytes must be <= 128");
            AXI_PROTOCOL_ASSERT(rules::check_exclusive_total_beats_le_max(aw_lock, aw.len),
                                "EXCLUSIVE_TOTAL_BEATS: AW exclusive total beats must be <= 16");
            AXI_PROTOCOL_ASSERT(
                rules::check_exclusive_total_pow2(aw_lock, aw.len),
                "EXCLUSIVE_POW2: AW exclusive total beats (len+1) must be a power of 2");
            AXI_PROTOCOL_ASSERT(
                rules::check_exclusive_addr_aligned_to_total(aw_lock, aw.addr, aw.len, aw.size),
                "EXCLUSIVE_ALIGN: AW exclusive addr must align to total burst bytes");
            AXI_PROTOCOL_ASSERT(rules::check_exclusive_burst_not_fixed(aw_lock, aw.burst),
                                "EXCLUSIVE_BURST_FIXED: AW exclusive burst must not be FIXED");
            is_exclusive_write = true;
            auto tag_it = exclusive_tags_.find(aw.id);
            if (tag_it != exclusive_tags_.end()) {
                exclusive_match = rules::check_exclusive_write_matches_read_tag(tag_it->second, aw);
                exclusive_tags_.erase(tag_it);  // consume regardless of match
            }
        } else {
            // E2: normal AW invalidates any tag whose monitored window overlaps.
            // FIXED reuses one bus word — span = single beat. INCR/WRAP delegate to
            // compute_tag_range, which mirrors the exclusive-tag window math.
            uint64_t aw_start, aw_end;
            if (aw.burst == Burst::FIXED) {
                const std::size_t aw_bpb = 1ull << aw.size;
                aw_start = aw.addr;
                aw_end = aw.addr + aw_bpb;
            } else {  // INCR or WRAP
                std::tie(aw_start, aw_end) = rules::compute_tag_range(aw);
            }
            for (auto it = exclusive_tags_.begin(); it != exclusive_tags_.end();) {
                const auto& tag = it->second;
                const bool overlap = aw_start < tag.addr_end && tag.addr_start < aw_end;
                if (overlap) {
                    it = exclusive_tags_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        WriteBurstState st{aw, 0, 0, Resp::OKAY};
        st.is_exclusive = is_exclusive_write;
        st.exclusive_match = exclusive_match;
        active_writes_[aw.id].push_back(st);
        aw_issue_order_.push_back(aw.id);
        aw_q_.pop_front();
    }
}

inline void AxiSlave::tick_submit_w_() {
    // 4. Submit W beats for oldest-issued active write (per-id FIFO).
    //    Multi-outstanding same-id: chain.front() may already have its W beats
    //    fully submitted (and be waiting on B drain). Find the FIRST burst in
    //    the chain that still has W beats remaining — that is the one currently
    //    receiving W beats. aw_issue_order_ orders these globally across ids.
    while (!w_q_.empty() && !aw_issue_order_.empty()) {
        uint8_t front_id = aw_issue_order_.front();
        auto& chain = active_writes_[front_id];
        WriteBurstState* stp = nullptr;
        for (auto& cand : chain) {
            if (cand.beats_submitted < static_cast<std::size_t>(cand.aw.len) + 1) {
                stp = &cand;
                break;
            }
        }
        if (!stp) break;  // should not happen — aw_issue_order_ tracks pending W
        auto& st = *stp;
        std::size_t beat_idx = st.beats_submitted;
        AXI_PROTOCOL_ASSERT(rules::check_w_beat_count_within(beat_idx + 1, st.aw.len),
                            "W_BEAT_COUNT_WITHIN: master submitted more W beats than burst len+1");
        AXI_PROTOCOL_ASSERT(
            rules::check_w_last_timing(w_q_.front().last, beat_idx, st.aw.len),
            "W_LAST_TIMING: WLAST must be asserted on (and only on) the final W beat");
        AXI_PROTOCOL_ASSERT(rules::check_strb_valid_bits(w_q_.front().strb),
                            "STRB_VALID_BITS: WSTRB bits above WSTRB_WIDTH must be 0");
        const uint64_t w_beat_addr_v =
            beat_addr(st.aw.addr, st.aw.len, st.aw.size, st.aw.burst, beat_idx);
        AXI_PROTOCOL_ASSERT(
            rules::check_strb_sparse_legal(w_q_.front().strb, st.aw.size, w_beat_addr_v),
            "STRB_SPARSE_LEGAL: WSTRB bits outside this beat's byte-lane window must be 0");
        MemWriteReq req{};
        req.addr = w_beat_addr_v;
        req.data = w_q_.front().data;
        req.strb = w_q_.front().strb;
        req.id = st.aw.id;
        req.last = w_q_.front().last;
        req.tag = (static_cast<uint64_t>(front_id) << 32) | beat_idx;
        // Phase C — E4: a failed exclusive write must NOT commit to memory
        // (§A7.2.3). Drop the W beat from w_q_ and advance both beats_submitted
        // and beats_completed (synthetic completion — no memory_port traffic).
        // The terminal B response is emitted later in step 4b respecting per-ID
        // FIFO order (so any preceding same-id burst still waiting on memory B
        // drains first).
        if (st.is_exclusive && !st.exclusive_match) {
            ++st.beats_submitted;
            ++st.beats_completed;
            w_q_.pop_front();
            if (st.beats_submitted == static_cast<std::size_t>(st.aw.len) + 1) {
                aw_issue_order_.pop_front();
            }
            continue;
        }
        if (!memory_port_.submit_write(req)) break;  // retry next tick
        ++st.beats_submitted;
        w_q_.pop_front();
        if (st.beats_submitted == static_cast<std::size_t>(st.aw.len) + 1) {
            // Burst's W beats fully forwarded to memory. Free up W routing so the
            // next queued AW (if any) can take ownership of subsequent W beats,
            // even while this burst's B response is still pending in memory.
            aw_issue_order_.pop_front();
            // Continue the loop: a subsequent burst may have W beats already queued.
        }
    }
}

inline void AxiSlave::tick_drain_failed_exclusive_b_() {
    // 4b. Phase C — emit B for failed exclusive writes (E6 partial: no-match).
    //     These bursts never reach memory_port_, so step 1 cannot drain them.
    //     Walk each id's chain front-to-back: drain any prefix of bursts whose
    //     synthetic completion is done. Stop at the first burst still in flight
    //     so per-id FIFO ordering is preserved (a normal burst ahead of a
    //     failed-exclusive in the chain still serializes its memory B first).
    for (auto it = active_writes_.begin(); it != active_writes_.end();) {
        auto& chain = it->second;
        while (!chain.empty()) {
            auto& head = chain.front();
            const bool head_complete =
                head.beats_completed == static_cast<std::size_t>(head.aw.len) + 1u;
            if (!head_complete) break;
            // Only failed-exclusive bursts get drained here; normal/EXOKAY paths
            // are handled in step 1 when memory's MemWriteResp arrives.
            if (!(head.is_exclusive && !head.exclusive_match)) break;
            AXI_PROTOCOL_ASSERT(rules::check_w_before_b(head.beats_submitted ==
                                                        static_cast<std::size_t>(head.aw.len) + 1u),
                                "W_BEFORE_B: failed exclusive must drain all W beats first");
            // Failed exclusive → OKAY (per §A7.2.3); no memory error possible since
            // memory was never accessed.
            b_q_.push_back(BBeat{head.aw.id, Resp::OKAY, 0});
            chain.pop_front();
        }
        if (chain.empty())
            it = active_writes_.erase(it);
        else
            ++it;
    }
}

inline void AxiSlave::tick_admit_ar_() {
    // 5. Start new AR (with burst-atomic OOB pre-check).
    //    Per-ID FIFO: append same-id ARs to the id's chain. Step 2 (R drain)
    //    advances FRONT — AXI4 preserves same-id response order.
    while (!ar_q_.empty()) {
        auto& ar = ar_q_.front();
        AXI_PROTOCOL_ASSERT(rules::check_burst_encoding(ar.burst),
                            "BURST_ENCODING: AR.burst must be FIXED, INCR, or WRAP");
        AXI_PROTOCOL_ASSERT(rules::check_size_bound(ar.size),
                            "SIZE_BOUND: AR.size must be <= log2(DATA_BYTES)");
        AXI_PROTOCOL_ASSERT(rules::check_wrap_len(ar.burst, ar.len),
                            "WRAP_LEN: AR.len must be 1, 3, 7, or 15 for WRAP burst");
        AXI_PROTOCOL_ASSERT(rules::check_wrap_align(ar.burst, ar.addr, ar.size),
                            "WRAP_ALIGN: AR.addr must be aligned to (1<<size) for WRAP burst");
        AXI_PROTOCOL_ASSERT(rules::check_4kb_cross(ar.addr, ar.len, ar.size, ar.burst),
                            "CROSS_4KB: INCR read burst at slave must not cross a 4KB boundary");
        if (bounds_set_) {
            std::size_t bpb = 1ull << ar.size;
            std::size_t total = bpb * (static_cast<std::size_t>(ar.len) + 1);
            bool oob = false;
            if (ar.burst == Burst::WRAP) {
                const uint64_t wrap_lower = ar.addr & ~(static_cast<uint64_t>(total) - 1u);
                const uint64_t wrap_upper = wrap_lower + total;
                oob = (wrap_lower < bounds_base_) || (wrap_upper > bounds_base_ + bounds_size_);
            } else {
                oob = (ar.addr < bounds_base_) || (ar.addr + total > bounds_base_ + bounds_size_);
            }
            if (oob) {
                for (uint8_t i = 0; i < ar.len + 1; ++i) {
                    RBeat rb{};
                    rb.id = ar.id;
                    rb.data.fill(0);
                    rb.resp = Resp::DECERR;
                    rb.last = (i == ar.len);
                    rb.user = 0;
                    r_q_.push_back(rb);
                }
                ar_q_.pop_front();
                continue;
            }
        }
        // Phase C — E1: admit exclusive AR (lock=1) → record tag (ready=false).
        // Per-ID; a same-id second exclusive AR overwrites the previous tag per
        // §A7.2.3 ("a master can only have one outstanding exclusive read per
        // ID"). The tag becomes ready on RLAST (E5) and is consumed/erased on the
        // matching exclusive AW (E3) or an overlapping normal AW (E2).
        AXI_PROTOCOL_ASSERT(rules::check_lock_encoding(ar.lock),
                            "LOCK_ENCODING: AR.lock must be 0 or 1");
        const auto ar_lock = static_cast<LockType>(ar.lock);
        if (ar_lock == LockType::Exclusive) {
            AXI_PROTOCOL_ASSERT(rules::check_exclusive_total_bytes_le_max(ar_lock, ar.len, ar.size),
                                "EXCLUSIVE_TOTAL_BYTES: AR exclusive total bytes must be <= 128");
            AXI_PROTOCOL_ASSERT(rules::check_exclusive_total_beats_le_max(ar_lock, ar.len),
                                "EXCLUSIVE_TOTAL_BEATS: AR exclusive total beats must be <= 16");
            AXI_PROTOCOL_ASSERT(
                rules::check_exclusive_total_pow2(ar_lock, ar.len),
                "EXCLUSIVE_POW2: AR exclusive total beats (len+1) must be a power of 2");
            AXI_PROTOCOL_ASSERT(
                rules::check_exclusive_addr_aligned_to_total(ar_lock, ar.addr, ar.len, ar.size),
                "EXCLUSIVE_ALIGN: AR exclusive addr must align to total burst bytes");
            AXI_PROTOCOL_ASSERT(rules::check_exclusive_burst_not_fixed(ar_lock, ar.burst),
                                "EXCLUSIVE_BURST_FIXED: AR exclusive burst must not be FIXED");
            auto [start, end] = rules::compute_tag_range(ar);
            ExclusiveTag tag{};
            tag.addr_start = start;
            tag.addr_end = end;
            tag.len = ar.len;
            tag.size = ar.size;
            tag.burst = ar.burst;
            tag.cache = ar.cache;
            tag.prot = ar.prot;
            tag.ready = false;
            // E1 runs BEFORE the AR is pushed to active_reads_[ar.id] below, so the
            // current chain size counts only OLDER same-id ARs still in flight; the
            // "+1" represents the just-being-admitted AR whose RLAST will close out
            // the new tag. If an old same-id tag was present (per §A7.2.3 it gets
            // silently overwritten), its pending counter is discarded with it —
            // each in-flight AR's RLAST decrements whatever tag is currently mapped.
            auto chain_it = active_reads_.find(ar.id);
            const std::size_t in_flight =
                (chain_it == active_reads_.end()) ? 0u : chain_it->second.size();
            tag.pending_rlasts = in_flight + 1u;
            exclusive_tags_[ar.id] = tag;
        }
        active_reads_[ar.id].push_back(ReadBurstState{ar, 0, 0});
        ar_q_.pop_front();
    }
}

inline void AxiSlave::tick_submit_ar_() {
    // 6. Submit AR beats to memory.
    //    For each id, walk the FIFO chain front-to-back: drain the front burst's
    //    remaining beats before issuing the next one. This preserves the order
    //    in which the memory side observes per-id reads (AXI4 same-id ordering).
    for (auto& [id, chain] : active_reads_) {
        bool backpressure = false;
        for (auto& st : chain) {
            if (backpressure) break;
            while (st.beats_submitted < static_cast<std::size_t>(st.ar.len) + 1) {
                MemReadReq req{};
                req.addr =
                    beat_addr(st.ar.addr, st.ar.len, st.ar.size, st.ar.burst, st.beats_submitted);
                req.size = st.ar.size;
                req.id = st.ar.id;
                req.last = (st.beats_submitted == static_cast<std::size_t>(st.ar.len));
                req.tag = (static_cast<uint64_t>(id) << 32) | st.beats_submitted;
                if (!memory_port_.submit_read(req)) {
                    backpressure = true;
                    break;
                }
                ++st.beats_submitted;
            }
        }
    }
}

}  // namespace ni::cmodel::axi
