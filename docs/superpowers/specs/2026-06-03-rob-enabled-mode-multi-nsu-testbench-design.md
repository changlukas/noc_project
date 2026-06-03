# Stage 3: ROB Enabled mode + multi-NSU testbench — design

**Status**: design approved (Sections 1-5 all Codex-reviewed APPROVED)
**Date**: 2026-06-03
**Owner**: c_model NoC behavior model (`noc::` namespace)
**Branch**: `stage3/packetize-depacketize` (accumulating with prior round, no push until full feature set merged)

**Prior round**: `2026-06-02-addr-trans-rob-disabled-axi4-conformity-design.md` — shipped ROB Disabled mode (per-AXI-ID ordering filter), `addr_trans` XYRouting helper, Stage 2 AxiMaster AXI4 conformity fix, `multi_dst_stress.yaml` smoke fixture.

**Reference for this round**: FlooNoC `tb_floo_rob.sv` (ETH Zurich, arXiv:2305.08562), `floo_rob.sv`, `floo_simple_rob.sv` from `pulp-platform/FlooNoC` GitHub.

---

## 1. Motivation

Prior round's ROB Disabled mode handles same-id-different-dst by **stalling** at admission. This works as long as responses for same-id different-dst transactions never need to be reordered — guaranteed by the dst-agnostic FIFO `LoopbackNoc`. Once per-dst latency variance enters the picture, B/R can arrive out of order and the master sees AXI4 §A5.3 violation.

This round delivers:

1. **ROB Enabled mode** in `nmu::Rob`: per-AXI-ID reorder buffer (per-beat slot model, dynamic free-list pool), in-order commit logic with In-order Path bypass.
2. **Multi-NSU testbench refactor**: 4 NSU instances (matching FlooNoC `tb_floo_rob.sv` `{Fast, Fast, Slow, Mixed}` pattern), each with own `src_id` and per-NSU response latency. Routing via `dst_id → nsu_idx` table inside `LoopbackNoc`.
3. **`multi_dst_stress.yaml` graduation**: prior round's smoke fixture becomes a true regression gate — Scoreboard catches AXI4 ordering violations when ROB is disabled.

These two changes are paired: Enabled mode logic is meaningless without latency variance to exercise reordering, and per-dst latency in single-NSU testbench (where response `src_id` is fixed) cannot differentiate destinations.

## 2. Scope

**In scope**:
- `nmu::Rob` Enabled mode bodies (push_aw/push_w/push_ar Enabled paths, pop_b/pop_r Enabled paths with in-order Path + reorder buffer)
- `ni::Depacketizer` interface: add `pop_*_with_meta()` virtual with default impl
- `nmu::Depacketize` override: extract `rob_idx` / `rob_req` from flit header
- `c_model/tests/common/loopback_noc.hpp` rewrite: multi-NSU ctor, routing table, per-NSU response latency (static + random hybrid), backward-compat single-NSU ctor
- Integration testbench refactor: 4 NSU stacks for `multi_dst_stress` fixture path; other 6 fixtures stay single-NSU
- `multi_dst_stress.yaml` fixture graduation (YAML unchanged, testbench setup change)
- Unit + death tests for all new behavior (17 new tests)

**Out of scope** (deferred to later rounds):
- `vc_arb` / `vc_mapping` (virtual channel arbitration, per main plan §3.1)
- `route_par` / `flit_ecc` / `nmu.hpp` top-level assembly
- Per-NSU separate memory backend (shared `axi::Memory` is sufficient for this round)
- Multi-NMU topology (1 NMU + 4 NSU is the round target)
- ROB capacity expansion beyond 32 (would require `ROB_IDX_WIDTH` change in `ni_packet.json`, deferred)
- `nsu::MetaBuffer` redesign (stays AXI-ID-keyed as-is)

## 3. Anchored design decisions (Section 1-5 settled)

| Decision | Choice | Rationale |
|---|---|---|
| Round scope | ROB Enabled + multi-NSU testbench paired | LoopbackNoc latency is the testability companion; can't validate Enabled without it |
| Reorder buffer allocator | Dynamic pool, FlooNoC paradigm | Better SRAM utilization vs static per-id partition; aligns with future RTL |
| `ROB_CAPACITY` | 32 (from `ni::header::ROB_IDX_WIDTH = 5`) | Hard cap from existing `ni_packet.json`; parameterized via codegen |
| Slot model | Per-beat (1 slot = 1 R beat) | RTL fidelity; matches FlooNoC `floo_rob.sv` SRAM word semantics; AR allocates `len+1` consecutive slots |
| Mode coexistence | Disabled + Enabled, mixable via `mode_w/mode_r` | Backward-compat with prior round; tests can mix per direction |
| LoopbackNoc latency injection | Per-NSU instance (static + random hybrid) | Each NSU has own `src_id`; FlooNoC-aligned multi-slave testbench pattern |
| `rob_idx` delivery to Rob | `pop_*_with_meta()` virtual on Depacketizer interface, default impl returns `meta={0,0}` | Clean layer separation: AXI Beat stays clean, metadata flows via parallel return; Disabled passthrough is no-op |
| Per-id quota in Rob | None (only pool free-list + AxiMaster's `max_outstanding` cap) | Avoid over-engineering; AXI4 doesn't require per-id quota at master |
| Naming | Independent (no FlooNoC copies), snake_case for vars/methods, PascalCase for types | User preference; clearer than literal SV name translation |
| Per-NSU memory | Shared `axi::Memory` for this round | Simpler; can split per-NSU memory in future round if testing memory-distinct semantics |

## 4. Component flow

### 4.1 Pipeline (after this round)

```
Request side:
  AxiMaster
    → AxiSlavePort (NMU)
    → Rob (Packetizer; admission + rob_idx allocate when Enabled)
    → Packetize (Beat → Flit, stamps header from AwHeaderMeta)
    → LoopbackNoc.nmu_req_out()
    → routing by dst_id → LoopbackNoc.nsu_req_in(nsu_idx)
    → Depacketize (NSU)
    → AxiMasterPort (NSU)
    → axi::AxiSlave + axi::Memory (shared)

Response side:
  axi::AxiSlave (B/R beats)
    → AxiMasterPort.push_b / push_r (NSU)
    → Packetize (NSU; stamps src_id = NSU's src_id, dst_id = original NMU src_id from MetaBuffer)
    → LoopbackNoc.nsu_rsp_out(nsu_idx)
    → per-NSU response delay queue (static or random latency)
    → merge into LoopbackNoc.nmu_rsp_in()
    → Depacketize (NMU; pop_*_with_meta extracts rob_idx)
    → Rob (Depacketizer; reorder by rob_idx, in-order commit per axi_id)
    → AxiSlavePort (NMU)
    → AxiMaster
```

### 4.2 Tick semantics

Unchanged from prior round (spec §4.6, frozen):
- `AxiSlavePort::tick()` drains response side first (`pop_b/pop_r`), then forwards request side (`push_aw/push_w/push_ar`)
- Allows same-cycle ID free: response pop releases a slot → request push in same tick can reuse it
- `Rob` mutates `read_entries_` / `write_entries_` / `committed_*_queue_` only within its own pop_b/pop_r and push_aw/push_w/push_ar calls (no background thread)

### 4.3 Files affected

| Path | Change |
|---|---|
| `c_model/include/ni/depacketizer.hpp` | Add `struct ResponseMeta` + virtual `pop_b_with_meta()` / `pop_r_with_meta()` with default impl forwarding to pop_b/pop_r with `meta={0,0}` |
| `c_model/include/nmu/depacketize.hpp` | Override `pop_*_with_meta()` to extract `rob_idx` / `rob_req` from flit header before decode |
| `c_model/include/nmu/rob.hpp` | Add Enabled-mode state members + push/pop bodies + `find_consecutive_free` helper; preserve all Disabled-mode logic unchanged |
| `c_model/tests/common/loopback_noc.hpp` | Rewrite: multi-NSU ctor + backward-compat single-NSU ctor + per-NSU queues + routing table + per-NSU latency |
| `c_model/tests/common/test_loopback_latency.cpp` | New: 5 tests for multi-NSU + per-NSU latency |
| `c_model/tests/common/CMakeLists.txt` | Register `test_loopback_latency` |
| `c_model/tests/nmu/test_depacketize.cpp` | +2 tests for `pop_*_with_meta` |
| `c_model/tests/nmu/test_rob.cpp` | +10 tests for Enabled mode |
| `c_model/tests/integration/test_request_response_loopback.cpp` | Multi-NSU testbench wiring for `multi_dst_stress` fixture path; positive ordering assertion |
| `NEXT_STEPS.md` | Karpathy 4-lens summary + pointer to next round |

**Unchanged**: all Stage 2 `axi/*`, `nmu/packetize.hpp`, `nmu/addr_trans.hpp`, `nsu/*` (other than instance count), `ni_packet.json`, generated headers under `specgen/generated/`.

## 5. `ni::Depacketizer` interface extension

### 5.1 New API

```cpp
namespace ni::cmodel {

struct ResponseMeta {
    uint8_t rob_idx;
    uint8_t rob_req;   // 1 = Enabled-mode flit; 0 = Disabled passthrough
};

class Depacketizer {
public:
    // Existing API — unchanged
    virtual std::optional<axi::BBeat> pop_b() = 0;
    virtual std::optional<axi::RBeat> pop_r() = 0;
    virtual std::optional<axi::AwBeat> pop_aw() = 0;
    virtual std::optional<axi::WBeat>  pop_w()  = 0;
    virtual std::optional<axi::ArBeat> pop_ar() = 0;

    // New — default impl forwards to pop_b/pop_r and returns meta={0,0}
    // (preserves backward compat for non-Rob-aware callers)
    virtual std::optional<std::pair<axi::BBeat, ResponseMeta>> pop_b_with_meta() {
        auto b = pop_b();
        if (!b) return std::nullopt;
        return std::make_pair(*b, ResponseMeta{0, 0});
    }
    virtual std::optional<std::pair<axi::RBeat, ResponseMeta>> pop_r_with_meta() {
        auto r = pop_r();
        if (!r) return std::nullopt;
        return std::make_pair(*r, ResponseMeta{0, 0});
    }
};

}  // namespace ni::cmodel
```

### 5.2 `nmu::Depacketize` override

```cpp
inline std::optional<std::pair<axi::BBeat, ResponseMeta>>
Depacketize::pop_b_with_meta() {
    if (b_q_.empty()) return std::nullopt;
    auto entry = b_q_.front();
    b_q_.pop_front();
    return std::make_pair(entry.beat, entry.meta);
}
```

`Depacketize` internal queue changes from `std::deque<axi::BBeat>` to `std::deque<BWithMeta>` where `BWithMeta { axi::BBeat beat; ResponseMeta meta; }`. The `decode_b` / `decode_r` helpers extract both AXI payload and `rob_idx` / `rob_req` from flit header in one pass. `pop_b()` (legacy) returns only the beat, discarding meta — preserves prior API.

## 6. `nmu::Rob` Enabled mode design

### 6.1 Storage model (per-beat)

Each AR transaction allocates `len+1` **consecutive** rob_idx slots from a 32-slot free pool. Each R beat owns its own slot. NSU enumerates `r.rob_idx = ar.rob_idx + beat_idx` when generating R flits — this is the wire protocol contract.

Each AW transaction allocates 1 rob_idx slot. B response carries that single rob_idx.

### 6.2 Data members

```cpp
class Rob : public Packetizer, public Depacketizer {
private:
    // === Disabled mode (prior round, unchanged) ===
    Packetize&    next_pkt_;
    Depacketizer& next_depkt_;
    RobMode       mode_w_, mode_r_;
    struct OutstandingEntry { uint8_t dst_id; };
    std::array<WriteState, AXI_ID_SPACE> write_;
    std::array<ReadState,  AXI_ID_SPACE> read_;
    uint32_t      w_burst_credit_ = 0;

    // === Enabled mode (NEW) ===
    static constexpr std::size_t ROB_CAPACITY      = 1u << ni::header::ROB_IDX_WIDTH;
    static constexpr std::size_t AXI_ID_SPACE      = 1u << ni::width::AXI_ID_WIDTH;
    static constexpr std::size_t MAX_AXI_LEN_PLUS1 = 1u << ni::width::AXI_LEN_WIDTH;

    // 1 slot = 1 beat. AR consumes len+1 consecutive slots.
    struct WriteEntry {
        bool        occupied = false;
        bool        ready    = false;     // B arrived
        uint8_t     axi_id   = 0;
        axi::BBeat  b_beat   = {};
    };
    struct ReadEntry {
        bool        occupied = false;
        bool        ready    = false;     // this R beat arrived
        uint8_t     axi_id   = 0;
        axi::RBeat  r_beat   = {};
    };
    std::array<WriteEntry, ROB_CAPACITY> write_entries_;
    std::array<ReadEntry,  ROB_CAPACITY> read_entries_;
    std::bitset<ROB_CAPACITY>            free_write_entries_;
    std::bitset<ROB_CAPACITY>            free_read_entries_;

    // Per-id ordered range list. AW = {base, 1}; AR = {base, len+1}.
    struct BeatRange {
        uint8_t base;
        uint8_t len_plus_1;
    };
    std::array<std::deque<BeatRange>, AXI_ID_SPACE> write_order_by_id_;
    std::array<std::deque<BeatRange>, AXI_ID_SPACE> read_order_by_id_;

    // Ready-to-emit beats drained by pop_b / pop_r
    std::deque<axi::BBeat> committed_b_queue_;
    std::deque<axi::RBeat> committed_r_queue_;
};
```

All sizes derive from codegen-generated `ni_flit_constants.h` — no hardcoded magic numbers.

### 6.3 Algorithms

**`push_aw` Enabled** (atomic on downstream failure):
```
1. if free_write_entries_.none()                    → return false
2. base = find_first_free(free_write_entries_)       // single 1 bit
3. ok = next_pkt_.push_aw_with_meta(beat,
            {dst, local_addr, rob_req=1, rob_idx=base})
4. if !ok                                            → return false (no state mutation)
5. free_write_entries_.reset(base)
6. write_entries_[base] = {occupied=true, ready=false, axi_id=id}
7. write_order_by_id_[id].push_back({base, 1})
8. ++w_burst_credit_
9. return true
```

**`push_ar` Enabled** (per-beat allocation key):
```
1. n = beat.len + 1
2. base = find_consecutive_free(free_read_entries_, n)
3. if base < 0                                       → return false (no consecutive run)
4. ok = next_pkt_.push_ar_with_meta(beat,
            {dst, local_addr, rob_req=1, rob_idx=base})
5. if !ok                                            → return false (state unchanged)
6. for i in [0, n):
       free_read_entries_.reset(base+i)
       read_entries_[base+i] = {occupied=true, ready=false, axi_id=id}
7. read_order_by_id_[id].push_back({base, n})
8. return true
```

**`push_w` Enabled**: identical to Disabled mode (uses `w_burst_credit_` gate; Rob doesn't per-slot track W).

**`pop_b` Enabled**:
```
1. if !committed_b_queue_.empty()                    → pop_front, return
2. opt = next_depkt_.pop_b_with_meta()
3. if !opt                                            → return nullopt
4. (b, meta) = *opt
5. if meta.rob_req == 0                              → return b (Disabled passthrough)
6. assert(meta.rob_idx < ROB_CAPACITY)               // defensive
7. slot = write_entries_[meta.rob_idx]
   assert(slot.occupied && !slot.ready)              // programmer error if duplicate
   slot.b_beat = b; slot.ready = true
8. id = slot.axi_id
   // In-order Path check: head of write_order_by_id_[id]?
   while !write_order_by_id_[id].empty():
       head = write_order_by_id_[id].front()
       if !write_entries_[head.base].ready: break
       committed_b_queue_.push_back(write_entries_[head.base].b_beat)
       free_write_entries_.set(head.base)
       write_entries_[head.base].occupied = false
       write_order_by_id_[id].pop_front()
9. if !committed_b_queue_.empty()                    → pop_front, return
   else                                              → return nullopt
```

**`pop_r` Enabled** (per-beat multi-slot range):
```
1. if !committed_r_queue_.empty()                    → pop_front, return
2. opt = next_depkt_.pop_r_with_meta()
3. if !opt                                            → return nullopt
4. (r, meta) = *opt
5. if meta.rob_req == 0                              → return r
6. assert(meta.rob_idx < ROB_CAPACITY)
7. slot = read_entries_[meta.rob_idx]
   assert(slot.occupied)
   slot.r_beat = r; slot.ready = true
8. id = slot.axi_id
   while !read_order_by_id_[id].empty():
       range = read_order_by_id_[id].front()
       // Check all slots in range ready
       all_ready = true
       for i in [0, range.len_plus_1):
           if !read_entries_[range.base + i].ready: all_ready = false; break
       if !all_ready: break
       // Flush all beats in range to committed queue (in burst order)
       for i in [0, range.len_plus_1):
           committed_r_queue_.push_back(read_entries_[range.base + i].r_beat)
           free_read_entries_.set(range.base + i)
           read_entries_[range.base + i].occupied = false
       read_order_by_id_[id].pop_front()
9. if !committed_r_queue_.empty()                    → pop_front, return
   else                                              → return nullopt
```

### 6.4 `find_consecutive_free` helper

```cpp
// Linear scan for first run of n consecutive 1s in bitset<ROB_CAPACITY>.
// Returns base index (0..ROB_CAPACITY-1), or -1 if no such run exists.
// O(ROB_CAPACITY) worst case.
static int find_consecutive_free(const std::bitset<ROB_CAPACITY>& free, std::size_t n);
```

First-fit (lowest base). Acceptable fragmentation at `ROB_CAPACITY = 32`. If a large AR fails despite enough total free slots, returns false → upstream stalls until reorder commit frees a contiguous run.

### 6.5 Mixed-mode contract

`mode_w` and `mode_r` are independent; either can be Disabled or Enabled. Mixed-mode rules:

- **`mode_*` = Disabled**: all flits in that direction must have `rob_req = 0`. Rob touches only Disabled-mode state members.
- **`mode_*` = Enabled**: all flits in that direction must have `rob_req = 1`. Rob touches only Enabled-mode state members.
- **Mid-stream switching is not supported**: mode is fixed at ctor; runtime change would corrupt state.
- **Contract violation**: Enabled-mode `pop_b/r` with `rob_req=0` is passthrough (no assert); Disabled-mode `pop_b/r` receiving `rob_req=1` would still treat it as Disabled (no rob_idx use), potentially mis-routing — caller's responsibility to ensure mode consistency.

## 7. `LoopbackNoc` multi-NSU + per-NSU latency

### 7.1 API

```cpp
namespace ni::cmodel::testing {

class LoopbackNoc {
public:
    // Backward-compat: single-NSU ctor. All dst_id default-route to NSU_0.
    LoopbackNoc(std::size_t req_depth, std::size_t rsp_depth);

    // Multi-NSU ctor. dst_to_nsu_ defaults to -1 (unmapped → assert on push).
    LoopbackNoc(std::size_t num_nsu,
                std::size_t req_q_depth_per_nsu,
                std::size_t rsp_q_depth_total);

    // NMU-side accessors (single)
    noc::NocReqOut& nmu_req_out();
    noc::NocRspIn&  nmu_rsp_in();

    // NSU-side accessors (per-NSU)
    noc::NocReqIn&  nsu_req_in(std::size_t nsu_idx);
    noc::NocRspOut& nsu_rsp_out(std::size_t nsu_idx);

    // Legacy aliases (single-NSU compat)
    noc::NocReqOut& req_out() { return nmu_req_out(); }
    noc::NocReqIn&  req_in()  { return nsu_req_in(0); }
    noc::NocRspOut& rsp_out() { return nsu_rsp_out(0); }
    noc::NocRspIn&  rsp_in()  { return nmu_rsp_in(); }

    // Routing
    void set_dst_route(uint8_t dst_id, std::size_t nsu_idx);

    // Per-NSU response latency
    void set_nsu_latency(std::size_t nsu_idx, std::size_t cycles);
    void set_nsu_latency_range(std::size_t nsu_idx,
                               std::size_t min, std::size_t max);
    void set_random_seed(uint64_t seed);

    // Legacy global delay (still works for non-Enabled-mode fixtures)
    void set_req_delay(unsigned cycles);
    void set_rsp_delay(unsigned cycles);

    void tick();
};

}  // namespace ni::cmodel::testing
```

### 7.2 Internal state

```cpp
private:
    std::size_t num_nsu_;
    std::size_t req_q_depth_per_nsu_;
    std::size_t rsp_q_depth_total_;

    // Per-NSU request queues
    std::vector<std::deque<Flit>> nsu_req_q_;

    // Merged response queue (single NMU)
    std::deque<Flit> rsp_q_;

    // Per-NSU response delay queues
    struct DelayedFlit { Flit flit; std::size_t cycles_remaining; };
    std::vector<std::deque<DelayedFlit>> nsu_rsp_delay_q_;
    std::size_t total_delayed_rsp_count_ = 0;

    // Routing table: dst_id → nsu_idx, -1 = unmapped
    static constexpr std::size_t DST_ID_SPACE = 1u << ni::header::DST_ID_WIDTH;
    std::array<int8_t, DST_ID_SPACE> dst_to_nsu_;

    // Per-NSU latency config
    struct NsuLatencyConfig {
        bool        is_random = false;
        std::size_t value     = 0;
        std::size_t min       = 0, max = 0;
    };
    std::vector<NsuLatencyConfig> nsu_latency_;

    // Legacy global delay (kept for backward compat)
    unsigned         req_delay_ = 0, rsp_delay_ = 0;
    std::deque<std::pair<Flit, unsigned>> req_pipe_, rsp_pipe_;
    std::deque<Flit>                       req_q_;   // legacy single req queue

    std::mt19937_64 rng_;
```

### 7.3 Algorithm

**Request push** (`NmuReqOutAdapter::push_flit`):
```
1. nsu_idx = dst_to_nsu_[flit.dst_id]
2. if nsu_idx < 0                                    → assert(false) "unmapped dst"
3. if nsu_req_q_[nsu_idx].size() >= req_q_depth_per_nsu_
                                                     → return false
4. nsu_req_q_[nsu_idx].push_back(flit); return true
```

**Response push** (`NsuRspOutAdapter[i]::push_flit`):
```
1. cfg = nsu_latency_[i]
2. latency = cfg.is_random
             ? uniform_int(cfg.min, cfg.max, rng_)
             : cfg.value
3. if latency == 0:
       if rsp_q_.size() >= rsp_q_depth_total_       → return false
       rsp_q_.push_back(flit)
4. else:
       if total_delayed_rsp_count_ + rsp_pipe_.size() + rsp_q_.size()
              >= rsp_q_depth_total_                 → return false
       nsu_rsp_delay_q_[i].push_back({flit, latency})
       ++total_delayed_rsp_count_
5. return true
```

**`tick()`**:
```
- For each i in [0, num_nsu_):
      for each entry in nsu_rsp_delay_q_[i]: --cycles_remaining
      while nsu_rsp_delay_q_[i].front().cycles_remaining == 0
             AND rsp_q_.size() < rsp_q_depth_total_:
          rsp_q_.push_back(front.flit)
          pop_front
          --total_delayed_rsp_count_
- Legacy req_pipe_ / rsp_pipe_ aging (for backward compat with set_req_delay/set_rsp_delay)
```

### 7.4 Backward-compat invariants (Commit 4 critical gate)

Single-NSU ctor (`LoopbackNoc(req_depth, rsp_depth)`) MUST satisfy:

1. **All 256 `dst_id` slots default-route to `NSU_0`** (`dst_to_nsu_` initialized to `0` everywhere, not `-1`). Legacy fixtures don't call `set_dst_route()`, so unmapped-assert would break them.
2. **Legacy aliases (`req_in() / req_out() / rsp_in() / rsp_out()`) are byte-identical to** `nsu_req_in(0) / nmu_req_out() / nmu_rsp_in() / nsu_rsp_out(0)`.
3. **Legacy `set_req_delay(N) / set_rsp_delay(N)`** continue as global delay API; apply to ALL flits regardless of NSU. Per-NSU latency is additional, not replacement.

Multi-NSU ctor (`LoopbackNoc(num_nsu, ...)`) MUST require explicit `set_dst_route()` (default `-1`); unmapped push asserts.

### 7.5 Latency semantics (off-by-one resolution)

> `set_nsu_latency(i, N)` means: when NSU i pushes a response flit, the flit is sampled with `latency = N` at push time; after N subsequent `tick()` aging operations (not including the push tick), the flit becomes visible in `rsp_q_`. `N = 0` means immediate visibility (fast path, identical to current `rsp_delay_=0` behavior).

Random latency samples once at push time and stays fixed for that flit's lifetime in the delay queue (avoids non-convergent behavior).

## 8. Test plan

### 8.1 Unit tests

**`c_model/tests/nmu/test_depacketize.cpp`** (+2 tests):

| Test | Invariant |
|---|---|
| `PopBWithMeta_ExtractsRobIdxAndRobReq` | B flit with `rob_idx=5, rob_req=1` → `pop_b_with_meta()` returns `(BBeat, {5, 1})` |
| `PopRWithMeta_ExtractsPerBeatRobIdx` | Multi-beat R: flits with `rob_idx=5,6,7,8` (consecutive enumeration) → each `pop_r_with_meta()` returns matching meta |

**`c_model/tests/nmu/test_rob.cpp`** (+10 tests):

| Test | Invariant |
|---|---|
| `Enabled_PushAw_AllocatesSlotAndStampsRobIdx` | 1 AW → 1 free_write_entries_ bit reset; downstream Packetize receives stamped `rob_idx` |
| `Enabled_PushAr_AllocatesConsecutiveSlotsForBurst` | AR `len=3` (4 beats) → 4 consecutive `free_read_entries_` bits reset; first slot's `rob_idx` stamped to AR flit |
| `Enabled_PushAr_PoolFragmented_FailWhenNoConsecutiveRun` | Pool has 4 free slots but at indices {0, 2, 4, 6} (fragmented) → AR `len=3` returns false |
| `Enabled_PushAw_PoolFull_ReturnFalseAtomic` | All 32 slots occupied → push_aw returns false; `free_write_entries_` unchanged |
| `Enabled_PushAw_DownstreamBackpressure_AtomicRollback` | Packetize push fails → `free_write_entries_` and `write_order_by_id_` unchanged |
| `Enabled_PopB_InOrder_ImmediateCommit` | B for head rob_idx arrives → committed_b_queue gains beat; subsequent `pop_b` returns it |
| `Enabled_PopB_OutOfOrder_HeldUntilHeadReady` | id=5 has AW1+AW2 in flight; B2 arrives first → committed_b_queue empty; B1 arrives → both committed in order |
| `Enabled_PopR_MultiBeatBurstCommitInOrder` | id=5 has AR1 (4 beats) + AR2 (2 beats); beats arrive {slot 4, 5, 0, 1, 2, 3} → committed_r_queue order is `r_beat[0..3]` then `r_beat[4..5]` |
| `Enabled_DifferentIdsInterleaveAtTransactionBoundary` | id=5 AR + id=6 AR both in flight, beats arrive interleaved → each id commits as its range completes; different ids interleave at AR boundary |
| `EnabledDeath_PopBWithUnallocatedRobIdx_Abort` | Inject B with `rob_idx` whose slot is not occupied → assert+abort |

**`c_model/tests/common/test_loopback_latency.cpp`** (new, +5 tests):

| Test | Invariant |
|---|---|
| `MultiNsu_RouteByDstId` | `set_dst_route(0, 0) + set_dst_route(1, 1)`; flit `dst_id=0` → `nsu_req_in(0)` queue; `dst_id=1` → `nsu_req_in(1)` queue |
| `MultiNsu_UnmappedDst_Assert` | `dst_id=99` not routed → push_flit assert+abort |
| `PerNsuLatency_StaticDelay` | `set_nsu_latency(1, 5)`; response from NSU_1 pushed at tick T → visible in `nmu_rsp_in` at tick T+5 |
| `PerNsuLatency_RandomBounded` | `set_nsu_latency_range(1, 2, 8) + set_random_seed(42)`; 100 responses → all latencies ∈ [2, 8]; re-seed reproduces same sequence |
| `PerNsuQueueFull_DoesNotBlockOtherNsu` | NSU_0 req queue full, push to NSU_1 succeeds |

### 8.2 Integration test fixture

`multi_dst_stress.yaml` (YAML unchanged from prior round) — graduated to real regression gate:

- testbench branch detects fixture name, switches to multi-NSU mode (4 NSU instances `{0x10..0x13}`)
- Routing: `set_dst_route(0, 0)` (dst=0 from `addr=0x100`), `set_dst_route(1, 1)` (dst=1 from `addr=0x10100`)
- Per-NSU latency: NSU_0 high (e.g. 10 cycles), NSU_1 low (1-3 random)
- Rob set to Enabled mode (`mode_w = mode_r = Enabled`)
- Expected: AW2 (dst=1) B beat arrives before AW1 (dst=0) B; Rob reorders; Scoreboard sees correct submission order; data integrity passes

**Positive ordering assertion**: testbench adds a per-id B/R order tracker that confirms B/R beats reach AxiMaster in original submission order. This is the formal regression gate (better than expected-failure-on-Disabled).

Other 6 fixtures stay single-NSU mode (testbench conditionally builds single-NSU or multi-NSU based on fixture name).

### 8.3 Test count summary

| Source | Count |
|---|---|
| `test_depacketize.cpp` | +2 |
| `test_rob.cpp` | +10 (5 push + 4 pop + 1 death) |
| `test_loopback_latency.cpp` | +5 (new file) |
| Integration fixture variants | 0 new, 1 graduated |
| **Total new tests** | **17** |
| Prior round total | 276 |
| **Final ctest target** | **293/293 pass** |

### 8.4 Drift gates (per commit)

```bash
cd specgen && py -3 -m pytest -q && py -3 tools/codegen.py --check && py -3 tools/gen_inventory.py --check
cd ../c_model && cmake --build build && ctest --test-dir build -j 1
```

Expected at HEAD:
- specgen pytest: 163 passed (untouched)
- codegen --check: clean
- gen_inventory --check: clean
- ctest: 293/293

## 9. Commit boundary plan

### 9.1 Six commits (atomic, each compiles + passes tests + ships new tests)

#### Commit 1: `feat(ni/depacketizer): add pop_*_with_meta virtual + ResponseMeta`

**Files**: `ni/depacketizer.hpp`, `nmu/depacketize.hpp`, `tests/nmu/test_depacketize.cpp`

**Content**: Add `ResponseMeta` struct + virtual `pop_*_with_meta()` with default impl forwarding to pop_b/pop_r + `meta={0,0}`. Override in `nmu/depacketize.hpp` to extract from flit header. Add 2 unit tests.

**Acceptance**: 276 → 278 ctest pass. Backward compat: existing callers (Rob Disabled, AxiSlavePort direct) unaffected (default impl is no-op).

**Parallelization note**: independent of Commits 4 (LoopbackNoc multi-NSU). Could ship in parallel.

#### Commit 2: `feat(nmu/rob): add Enabled mode push paths + state`

**Files**: `nmu/rob.hpp`, `tests/nmu/test_rob.cpp`

**Content**: Add Enabled state members (write_entries_/read_entries_/free_*_entries_/write_order_by_id_/read_order_by_id_/committed_*_queue_/`find_consecutive_free` helper). Implement `push_aw`/`push_w`/`push_ar` Enabled paths. Preserve all Disabled-mode logic unchanged. `pop_b`/`pop_r` Enabled paths still assert+abort (next commit). Add 5 push-side tests.

**⚠ Caveat**: After commit 2, Enabled mode is **intentionally push-only**. Do NOT wire any e2e fixture to Enabled mode until commit 3. Unit tests only.

**Acceptance**: 278 → 283 ctest pass.

#### Commit 3: `feat(nmu/rob): add Enabled mode pop paths + commit logic`

**Files**: `nmu/rob.hpp`, `tests/nmu/test_rob.cpp`

**Content**: Implement `pop_b`/`pop_r` Enabled paths with In-order Path bypass + chain-flush reorder commit logic. Add 5 pop-side tests (4 behavior + 1 death).

**Acceptance**: 283 → 288 ctest pass. Enabled mode complete in Rob; e2e regression gate still pending (LoopbackNoc still single-NSU).

#### Commit 4: `feat(tests/common/loopback_noc): multi-NSU refactor + per-NSU latency`

**Files**: `tests/common/loopback_noc.hpp`, `tests/common/test_loopback_latency.cpp` (new), `tests/common/CMakeLists.txt`

**Content**: Rewrite LoopbackNoc with multi-NSU ctor + per-NSU req/rsp queues + routing table + per-NSU latency. Backward-compat single-NSU ctor forwards to multi-NSU with `num_nsu=1` + defaults all `dst_to_nsu_` to 0. Legacy aliases (`req_in/req_out/rsp_in/rsp_out`) preserved as wrappers around new accessors. Add 5 unit tests.

**Critical**: Backward compat MUST satisfy the three invariants in §7.4 (default route to NSU_0; aliases byte-identical; legacy global delay preserved).

**Acceptance**: 288 → 293 ctest pass. 270 prior tests stay green via single-NSU compatibility path. Multi_dst_stress still in single-NSU mode (smoke test, graduates in commit 5).

**Parallelization note**: Independent of Commits 1-3.

#### Commit 5: `feat(tests/integration): multi-NSU testbench + multi_dst_stress real regression gate`

**Files**: `tests/integration/test_request_response_loopback.cpp` only

**Content**: testbench branch on fixture name. For `multi_dst_stress`, build multi-NSU mode (4 NSU stacks, src_ids `{0x10..0x13}`, routing setup, per-NSU latency config), Rob in Enabled mode. Add positive per-id B/R ordering assertion (formal regression gate). Other 6 fixtures stay single-NSU.

**Acceptance**: 293/293 ctest pass. Sanity-check (reviewer manual validation, NOT committed): temp swap Rob to Disabled, run multi_dst_stress, confirm Scoreboard or ordering assertion catches violation, revert.

#### Commit 6: `docs(NEXT_STEPS): ROB Enabled + multi-NSU testbench done; next is vc_arb`

**Files**: `NEXT_STEPS.md`

**Content**: Karpathy 4-lens summary + flip pointer to next round (main plan §3.1 successors: `vc_arb`, `vc_mapping`, `route_par`, `flit_ecc`, `nmu.hpp` top-level assembly).

**Acceptance**: drift gates all clean, ctest 293/293.

### 9.2 Commit ordering / parallelization

| Commit | Depends on | Notes |
|---|---|---|
| 1 (Depacketizer interface) | — | Independent — adds new virtual API; default impl backward compat |
| 2 (Rob Enabled push) | — | Uses prior round's `push_aw_with_meta`; does NOT use new `pop_*_with_meta`; independent of Commit 1 |
| 3 (Rob Enabled pop) | 1, 2 | Needs `pop_*_with_meta` from Commit 1 + Enabled push state from Commit 2 |
| 4 (LoopbackNoc multi-NSU) | — | Independent — touches `tests/common/` only |
| 5 (Integration testbench) | 3, 4 | Needs full Rob Enabled (1+2+3) + multi-NSU LoopbackNoc (4) |
| 6 (NEXT_STEPS) | 5 | Final sweep |

Parallel waves for subagent-driven-development:
- **Wave 1** (parallel): Commits 1, 2, 4
- **Wave 2**: Commit 3 (gates on 1 + 2)
- **Wave 3**: Commit 5 (gates on 3 + 4)
- **Wave 4**: Commit 6

### 9.3 Risk register

| Risk | Mitigation |
|---|---|
| Commit 4 breaks legacy 270 tests (compatibility regression) | §7.4 invariants explicit; build + full ctest immediately after Commit 4 |
| Commit 5 testbench refactor affects existing fixture wiring | testbench branches on fixture name; single-NSU branch unchanged from prior round; blast radius limited to `multi_dst_stress` path |
| Commit 3 brings up Enabled pop but multi_dst_stress can't yet exercise it | Accepted — unit tests cover correctness; e2e validation in Commit 5 |
| `find_consecutive_free` fragmentation at 32 slots | First-fit + 32 cap is documented limitation; large AR can fail despite total free; acceptable for c_model behavior model |
| Per-NSU rng seed determinism across test runs | Default seed=0; `set_random_seed` available; mt19937_64 portable |

## 10. Open follow-ups (deferred)

1. **`vc_arb` + `vc_mapping`** — virtual channel arbitration (per main plan §3.1)
2. **`route_par`** — routing parity
3. **`flit_ecc`** — flit-level ECC
4. **`nmu.hpp` top-level assembly** — single class composing addr_trans + Rob + Packetize + Depacketize
5. **Per-NSU separate memory backend** — enables tests with NSU-distinct address ranges
6. **Multi-NMU topology** — N NMUs + M NSUs cross-routing
7. **ROB capacity expansion beyond 32** — requires `ROB_IDX_WIDTH` change in `ni_packet.json` + spec cascade

## 11. References

- Prior round design: `docs/superpowers/specs/2026-06-02-addr-trans-rob-disabled-axi4-conformity-design.md`
- Main plan: `docs/noc_cmodel_rtl_plan.md` §3.1
- FlooNoC paper: arXiv:2305.08562 (Fischer et al., "FlooNoC: A Multi-Tbps Wide NoC for Heterogeneous AXI4 Traffic", 2023)
- FlooNoC RoB module: `pulp-platform/FlooNoC/hw/floo_rob.sv` (RoBSize=64, MaxRoTxnsPerId=32, per-beat slot SRAM)
- FlooNoC RoB testbench: `pulp-platform/FlooNoC/hw/tb/tb_floo_rob.sv` (4 slave instances with `{Fast, Fast, Slow, Mixed}` latency profiles)
- AXI4 spec: ARM IHI 0022 §A5.3 (same-ID response ordering)
- ni_packet.json: `specgen/generated/json/ni_packet.json` (`ROB_IDX_WIDTH = 5`, `AXI_ID_WIDTH = 8`)
