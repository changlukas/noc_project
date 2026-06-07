# VcArb: Multi-Mode VC Arbiter + Header.last Wormhole Fix — design

**Status**: design (Codex round-1 NEEDS-REVISION → fixes folded; round-2 NEEDS-ROUND-3 → 1 new issue + header.last bug deferral question folded; pending Codex round-3 verify on the consolidated spec)
**Date**: 2026-06-03
**Owner**: c_model NoC behavior model (`noc::` namespace)
**Branch**: `stage3/packetize-depacketize` (accumulating; no push until full feature set merged)

**Prior rounds shipped on this branch**:
- ROB Disabled + AXI4 conformity + addr_trans
- ROB Enabled + multi-NSU testbench
- Test logger (SCENARIO + AxiMasterObserver)

**Reference architecture (FlooNoC pulp-platform)**:
- `hw/floo_wormhole_arbiter.sv:34-49, :62-70` (LockIn=1, last_out = data_o.hdr.last & valid_o)
- `hw/floo_vc_arbiter.sv:96-116` (LockIn=0, no `last` usage)
- `hw/floo_output_arbiter.sv:64-75` (wormhole arbiter at output port)
- `hw/floo_axi_chimney.sv:568-577` (AW header.last=0), `:584-591` (W header.last=axi_req_in.w.last), `:608-616` (B last=1), `:624-633` (R last=1 always; ROB-based not wormhole)
- `hw/include/floo_noc/typedef.svh:53-63` (flit header `last` field, 1 bit, packet-boundary marker)
- `hw/floo_router.sv:130-143` (per-VC FIFO, no `last` use)
- gem5 Garnet: `OutputUnit::has_credit(vc)`, `OutVcState::m_credit_count` (sender mirror, query API)
- BookSim 2: `BufferState::IsFullFor/AvailableFor(vc)` + `Credit` backchannel (similar sender-mirror)
- ni_signals.json: `noc_req_credit_i` has `width_param=NUM_VC` (per-VC credit return signal)

---

## 1. Motivation

Per main plan `docs/noc_cmodel_rtl_plan.md` §3.1 + §3 file structure (line 167):
- `nmu/vc_mapping.hpp` — decide which VC a flit goes into (per axi_ch policy)
- `nmu/vc_arb.hpp` — round-robin + per-VC credit arbitration to NoC
- Same for NSU side (no vc_mapping, just vc_arb)

This round delivers a **merged VcArb class** (vc_mapping + vc_arb in one), supporting **2 modes**:
- **Mode A (ReadWriteSplit)**: static split — NMU AW/W → write_vc, AR → read_vc; NSU B → write_rsp_vc, R → read_rsp_vc. Default mode.
- **Mode B (MultiCandidate)**: per-axi_ch candidate VC list, push picks first VC with pending queue space. Avoids head-of-line (HoL) blocking under credit pressure.

Plus a critical **W-follows-AW invariant** (both modes): all W beats of a write burst use the same VC as the AW (matches AXI4 §A5.3 W stream ordering; aligns with FlooNoC's AW+W single-wormhole-packet design).

Also folds in a **pre-existing Packetize header.last bug fix** (architectural alignment with FlooNoC wormhole semantic), discovered during VC arbitration design and confirmed against FlooNoC source.

## 2. Scope

**In scope**:
- `noc::NocReqOut` / `noc::NocRspOut` interface extension: add `credit_avail(uint8_t vc_id) const` virtual with default `return true` impl (preserves all existing mocks)
- `tests/common/loopback_noc.hpp`: per-VC FIFO + `credit_avail` impl
- `nmu/vc_arb.hpp` (new) — `VcArb` class with 2 modes + per-VC pending queue + round-robin + credit-gated tick
- `nsu/vc_arb.hpp` (new) — mirror class for response side
- Integration testbench: wire VcArb into PacketizeLoopback fixture
- `nmu/packetize.hpp` header.last fix: AW → 0, W → wlast (per FlooNoC wormhole semantic)
- Tests: parameterized matrix NUM_VC × Mode (~15 new tests)

**Out of scope** (deferred / future round):
- `floo_wormhole_arbiter` equivalent at NoC fabric level (router internal; this round is NI side only)
- VC starvation / weighted round-robin policies (this round = simple round-robin)
- Multi-NSU testbench routing through VcArb (current multi-NSU testbench bypasses VcArb)
- Dynamic per-cycle VC remapping (Mode B does at push-time only, not in tick)
- CDC / clock-domain crossing (Stage 4+)

## 3. Anchored decisions

| Decision | Choice | Source |
|---|---|---|
| Round scope | 3-in-1: NMU vc_mapping (merged) + NMU vc_arb + NSU vc_arb | User Q1 |
| Parameterization | All sizes derived from codegen (`NUM_VC_MAX = 1u << ni::header::VC_ID_WIDTH = 8`) | User Q2 + memory rule |
| Test matrix | NUM_VC = 1, 2, 4, 8 parameterized | User Q2 |
| vc_mapping policy | Per-AXI-channel table; merged with vc_arb | User Q3 + Q4 |
| Credit API | Query-style `credit_avail(vc) -> bool` (gem5/BookSim pattern) | User Q5 + Codex survey |
| Pipeline placement | After Packetize, decorator pattern (Packetize unchanged) | User Q6 |
| Mode A | ReadWriteSplit (NMU: AW/W → write_vc, AR → read_vc; NSU symmetric) | User Q7 |
| Mode B | MultiCandidate (per-axi_ch candidate list, first-with-space wins) | User Q7 |
| W-follows-AW | Both modes enforce; pending_w_routes_ deque mirrors Packetize w_meta_fifo_ | User Q7 + Codex round-1 |
| NUM_VC=1 degenerate | Both modes equivalent (all flits → VC=0); behavior identical to prior round | User Q7 |
| header.last bug fix | Include in this round (FlooNoC-aligned wormhole semantic) | User chose A |
| `wlast` source | `payload.W.wlast` (NOT header.last, since header.last is buggy + about to be fixed) | Codex round-1 |
| W-route state | `std::deque<uint8_t>` not single value (mirrors Packetize w_meta_fifo_ serialization) | Codex round-1 |
| Sentinel removal | `std::optional<uint8_t>` instead of magic `0xFF` | Codex round-1 |
| Ctor disambiguation | Named factories `read_write_split()` / `multi_candidate()` | Codex round-1 |
| Round-robin scan | All NUM_VC VCs from `round_robin_ptr_`, not single-shot check | Codex round-1 |
| Per-VC test introspection | LoopbackNoc adds `nmu_req_vc_q_size(vc)` etc. | Codex round-1 |
| Release-mode safety | `assert(false) + std::abort()` pattern on invariant violation | Codex round-2 |

## 4. Architecture overview

### 4.1 Pipeline placement (decorator pattern)

```
Request side (NMU):
  AxiSlavePort
    → Rob (admission + rob_idx alloc)
    → Packetize (Beat → Flit, stamp axi_ch + header fields)
    → VcArb (decorator: implements NocReqOut interface)
       │ decides vc_id from axi_ch (Mode A or B)
       │ enqueues flit into per-VC pending queue
       │ tick() round-robins through VCs, gated by credit_avail
    → NocReqOut (real downstream, e.g., LoopbackNoc per-VC FIFO)

Response side (NSU):
  AxiSlave/Memory
    → AxiMasterPort
    → nsu::Packetize (Beat → Flit, stamp axi_ch via MetaBuffer)
    → nsu::VcArb (same decorator pattern, NocRspOut interface)
    → NocRspOut
```

Packetize is **unchanged in API** — it still calls `noc_req_out.push_flit(flit)`. The `NocReqOut&` reference Packetize receives is now a `VcArb` (which derives from `NocReqOut`). VcArb extracts `axi_ch` from `flit.get_header_field("axi_ch")` to make routing decisions.

### 4.2 NMU vs NSU duplication

Two separate classes (`nmu::VcArb` and `nsu::VcArb`) rather than shared base/template. Rationale:
- Different default axi_ch grouping (NMU sees AW/W/AR; NSU sees B/R)
- Different downstream interface type (NocReqOut vs NocRspOut)
- 化繁為簡: shared base would force abstraction over minor differences; cost-benefit favors duplication at < 250 LOC per class

Future: if logic drifts toward common implementation, refactor to shared helper in `ni/vc_arb_base.hpp`. For this round, accept controlled duplication.

### 4.3 Files affected

| Path | Change |
|---|---|
| `c_model/include/noc/noc_req_out.hpp` | Add virtual `credit_avail(uint8_t) const` with default `return true` |
| `c_model/include/noc/noc_rsp_out.hpp` | Same |
| `c_model/include/nmu/vc_arb.hpp` (new) | VcArb class for request side |
| `c_model/include/nsu/vc_arb.hpp` (new) | VcArb class for response side (mirror) |
| `c_model/include/nmu/packetize.hpp` | Header.last fix: AW → 0, W → wlast (2 lines) |
| `c_model/tests/common/loopback_noc.hpp` | Per-VC FIFO + credit_avail impl + per-VC introspection getters |
| `c_model/tests/nmu/test_vc_arb.cpp` (new) | ~10 unit tests for NMU VcArb |
| `c_model/tests/nsu/test_vc_arb.cpp` (new) | ~5 unit tests for NSU VcArb |
| `c_model/tests/nmu/test_packetize.cpp` | Line 50: update AW header.last expectation 1→0; +1 new test for W header.last per-beat |
| `c_model/tests/integration/test_request_response_loopback.cpp` | Wire VcArb between Packetize and LoopbackNoc |
| `c_model/tests/common/CMakeLists.txt` + `tests/nmu` + `tests/nsu` | Register new tests |
| `NEXT_STEPS.md` | Flip pointer |

**Not touched** in production code: Rob, AxiSlavePort, addr_trans, all axi/*, nsu/packetize.hpp (B/R header.last already correct per FlooNoC pattern), MetaBuffer.

## 5. `NocReqOut` / `NocRspOut` interface extension

### 5.1 New API

```cpp
// c_model/include/noc/noc_req_out.hpp
namespace ni::cmodel::noc {

class NocReqOut {
public:
    virtual bool push_flit(const Flit& flit) = 0;  // existing

    // NEW: per-VC credit availability query (gem5/BookSim pattern).
    // Caller MUST check credit_avail(vc) before push_flit(flit with vc_id=vc).
    // Default impl returns true (preserves existing mocks that ignore credit;
    // overrides should track per-VC FIFO depth or equivalent).
    virtual bool credit_avail(uint8_t /*vc_id*/) const { return true; }
};

}  // namespace
```

Same shape for `NocRspOut`. Default `return true` preserves all existing mocks (LoopbackNoc-old, test_test_logger AxiMasterObserver paths, integration test_request_response_loopback's response shuttle).

### 5.2 Caller contract

Upstream (VcArb in this design) must:
1. Call `credit_avail(vc)` to check
2. Only call `push_flit(f)` if check returned true AND f.vc_id == vc
3. On `push_flit` returning false (despite credit_avail true), treat as protocol violation → assert+abort

`push_flit` return is defensive double-check: in a correctly designed system with credit_avail tracking, push_flit always succeeds after a true credit_avail.

## 6. LoopbackNoc per-VC FIFO

### 6.1 Per-VC queue + credit_avail impl

```cpp
namespace ni::cmodel::testing {

class LoopbackNoc {
private:
    static constexpr std::size_t NUM_VC_MAX = 1u << ni::header::VC_ID_WIDTH;  // 8
    std::array<std::deque<Flit>, NUM_VC_MAX> nmu_req_vc_q_;  // per-VC pending
    std::size_t                              per_vc_depth_;  // configurable, default 4
    // Same per-VC structures for nsu_rsp side
    // ...

public:
    // Overridden NocReqOut methods on the NMU req side
    bool push_flit_req(const Flit& f) {
        uint8_t vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
        if (nmu_req_vc_q_[vc].size() >= per_vc_depth_) return false;
        nmu_req_vc_q_[vc].push_back(f);
        return true;
    }
    bool credit_avail_req(uint8_t vc_id) const {
        return nmu_req_vc_q_[vc_id].size() < per_vc_depth_;
    }

    // Per-VC introspection for tests (NEW)
    std::size_t nmu_req_vc_q_size(uint8_t vc_id) const {
        return nmu_req_vc_q_[vc_id].size();
    }
    std::size_t per_vc_depth() const { return per_vc_depth_; }

    // Configurable per-VC depth via ctor or setter
    void set_per_vc_depth(std::size_t depth);
};

}  // namespace
```

### 6.2 Backward compat (270 prior tests)

Prior tests use single-FIFO aggregate behavior with `req_q_size()` etc. LoopbackNoc's per-VC FIFO **aggregates** the same way (all flits have vc_id=0 in prior tests; aggregate behavior == VC=0 FIFO behavior). Legacy `req_q_size()` returns `sum(nmu_req_vc_q_[i].size() for i in 0..NUM_VC)` for compatibility.

Critical invariant: no prior test should change behavior. Verify via full ctest sweep after Commit 2.

## 7. `VcArb` class structure

### 7.1 Common shape (NMU + NSU)

```cpp
namespace ni::cmodel::nmu {  // same pattern in nsu::

enum class VcMode {
    ReadWriteSplit,   // Mode A: NMU {AW,W} → write_vc, {AR} → read_vc; NSU {B} → write_rsp_vc, {R} → read_rsp_vc
    MultiCandidate,   // Mode B: per-axi_ch candidate list; pick first with pending queue space
};

class VcArb : public noc::NocReqOut {  // or NocRspOut in nsu::
public:
    static constexpr std::size_t NUM_VC_MAX  = 1u << ni::header::VC_ID_WIDTH;   // 8
    static constexpr std::size_t AXI_CH_COUNT = 5;                              // AW, W, AR, B, R

    // Named factory — Mode A (default for production use)
    static VcArb read_write_split(
        noc::NocReqOut& downstream,
        std::size_t num_vc,
        uint8_t write_vc,
        uint8_t read_vc,
        std::size_t pending_depth = 4);

    // Named factory — Mode B
    static VcArb multi_candidate(
        noc::NocReqOut& downstream,
        std::size_t num_vc,
        std::array<std::vector<uint8_t>, AXI_CH_COUNT> candidate_vcs,
        std::size_t pending_depth = 4);

    // NocReqOut decorator methods
    bool push_flit(const Flit& f) override;
    bool credit_avail(uint8_t vc_id) const override;

    void tick();  // drain pending → downstream

    // Test introspection
    std::size_t pending_size(uint8_t vc_id) const { return pending_[vc_id].size(); }
    uint8_t     round_robin_ptr() const { return round_robin_ptr_; }

private:
    // Unified private ctor; public factories construct via this.
    VcArb(noc::NocReqOut& downstream,
        std::size_t num_vc,
        VcMode mode,
        uint8_t write_vc,
        uint8_t read_vc,
        std::array<std::vector<uint8_t>, AXI_CH_COUNT> candidate_vcs,
        std::size_t pending_depth);

    std::optional<uint8_t> select_vc_for_axi_ch(uint8_t axi_ch);

    noc::NocReqOut&                                 downstream_;
    std::size_t                                     num_vc_;
    VcMode                                          mode_;
    // Mode A
    uint8_t                                         write_vc_, read_vc_;
    // Mode B
    std::array<std::vector<uint8_t>, AXI_CH_COUNT>  candidate_vcs_;
    // Common
    std::array<std::deque<Flit>, NUM_VC_MAX>        pending_;
    std::size_t                                     pending_depth_;
    std::size_t                                     round_robin_ptr_ = 0;

    // W-follows-AW invariant (§10)
    std::deque<uint8_t>                             pending_w_routes_;
};

}  // namespace
```

### 7.2 NSU variant

Identical shape, except:
- Inherits `noc::NocRspOut`
- `axi_ch` ∈ {B, R} (different from NMU's {AW, W, AR})
- `write_vc` semantic = "response VC for write completions" (B), `read_vc` = R
- `pending_w_routes_` not needed (NSU produces single-flit B; multi-flit R is ROB-tracked, no wormhole on response side per FlooNoC `hw/floo_axi_chimney.sv:624-633`)

NSU's VcArb is slightly simpler — no W-route tracking needed.

## 8. Push algorithm (Mode A + B)

### 8.1 `push_flit(f)` pseudocode (NMU)

```
push_flit(f):
    axi_ch = f.get_header_field("axi_ch")
    vc_opt = select_vc_for_axi_ch(axi_ch)
    if !vc_opt.has_value():
        return false                          // no candidate VC available
    vc_id = *vc_opt
    if pending_[vc_id].size() >= pending_depth_:
        return false                          // queue full backpressure

    // W-follows-AW state update (§10)
    if axi_ch == AW:
        pending_w_routes_.push_back(vc_id)
    if axi_ch == W:
        // vc_id already correctly forced by select_vc_for_axi_ch (uses front of deque)
        if f.get_payload_field("W", "wlast") != 0:
            pending_w_routes_.pop_front()

    f.set_header_field("vc_id", vc_id)
    pending_[vc_id].push_back(f)
    return true
```

### 8.2 `select_vc_for_axi_ch(axi_ch)` pseudocode

```
std::optional<uint8_t> select_vc_for_axi_ch(uint8_t axi_ch):
    // Degenerate single-VC case
    if num_vc_ == 1:
        return 0

    // W-follows-AW enforcement (both modes); see §10
    if axi_ch == W:
        if pending_w_routes_.empty():
            assert(false &&
                "VcArb::push_flit: W arrived with empty pending_w_routes_ — "
                "Packetize w_meta_fifo invariant violated (W before AW); "
                "check upstream Rob credit gate or AxiSlavePort routing");
            std::abort();   // belt-and-braces: NDEBUG strips assert
        return pending_w_routes_.front();   // force W onto AW's VC; NO mode lookup for W

    // Mode-dependent VC selection
    if mode_ == ReadWriteSplit:
        if axi_ch == AW: return write_vc_
        if axi_ch == AR: return read_vc_
        // NSU side variant:
        // if axi_ch == B: return write_rsp_vc_
        // if axi_ch == R: return read_rsp_vc_
        return std::nullopt   // unknown axi_ch (defensive)

    if mode_ == MultiCandidate:
        for vc in candidate_vcs_[axi_ch]:
            if pending_[vc].size() < pending_depth_ AND downstream_.credit_avail(vc):
                return vc
        return std::nullopt   // all candidates full or credit-blocked → upstream backpressure

    return std::nullopt   // unreachable
```

### 8.3 Atomicity

push_flit mutation order:
1. Check all conditions (vc_opt, pending depth) — no state change
2. Update pending_w_routes_ (push or pop) — only after pass conditions
3. Stamp f.vc_id, push to pending_[vc_id]

If push_flit returns false (condition fail), no state mutation. Idempotent retry safe.

## 9. `tick()` round-robin + credit gate

```
void tick():
    // Scan ALL num_vc_ VCs from round_robin_ptr_; pick first eligible
    for k in 0..num_vc_:
        vc = (round_robin_ptr_ + k) % num_vc_
        if !pending_[vc].empty() AND downstream_.credit_avail(vc):
            f = pending_[vc].front()
            bool ok = downstream_.push_flit(f);
            assert(ok && "VcArb::tick: credit_avail returned true but push_flit refused — protocol violation");
            pending_[vc].pop_front()
            round_robin_ptr_ = (vc + 1) % num_vc_  // advance fairness pointer past served VC
            return  // 1 flit/cycle (matches noc_req_valid_o single-bit semantic)

    // All VCs either empty or credit-blocked → idle cycle
    // No round_robin_ptr_ advance on idle
```

Per cycle: at most 1 push to downstream (single valid_o per ni_signals contract).

## 10. W-follows-AW invariant

### 10.1 Why

AXI4 §A5.3 + FlooNoC `hw/floo_axi_chimney.sv:640-651` (SelW state machine): W beats MUST follow AW issue order, no interleaving. If W beats of two different AWs split across different VCs, downstream NoC + receiver cannot reassemble the burst correctly (data corruption).

### 10.2 Implementation: pending_w_routes_ deque mirrors Packetize w_meta_fifo_

Packetize already serializes multi-outstanding AWs into `w_meta_fifo_` (push on push_aw, pop on push_w with wlast). VcArb mirrors this with `std::deque<uint8_t> pending_w_routes_`:

| Event | VcArb action |
|---|---|
| `push_flit(AW)` | Choose vc_id per mode; `pending_w_routes_.push_back(vc_id)` |
| `push_flit(W)` | `vc_id = pending_w_routes_.front()` (force W to AW's VC, ignore mode lookup) |
| `push_flit(W)` with `payload.W.wlast == 1` | After stamp, `pending_w_routes_.pop_front()` |

### 10.3 wlast source: `payload.W.wlast` (NOT `header.last`)

Critical: VcArb reads `f.get_payload_field("W", "wlast")` not `f.get_header_field("last")`. Reasons:
- `payload.W.wlast` is the AXI semantic, unambiguous
- `header.last` is the wormhole packet boundary marker (FlooNoC semantic — see §12), which AFTER the §12 fix will equal wlast for W flits, but the design is decoupled from that timing
- VcArb works correctly regardless of when §12 fix lands

### 10.4 Defensive abort on invariant violation

```cpp
if axi_ch == W AND pending_w_routes_.empty():
    assert(false && "VcArb: W before AW — Packetize w_meta_fifo invariant violated");
    std::abort();
```

Pattern matches Rob's cross-side abort messages from prior round: `assert(false) + std::abort()` so NDEBUG release builds also fail-fast (no UB from `front()` on empty deque).

## 11. NUM_VC=1 degenerate behavior

When `num_vc_ == 1`:
- Both modes equivalent: `select_vc_for_axi_ch` returns 0 unconditionally (early return at top)
- `pending_[0]` is the only used queue
- `tick()` round-robin trivially picks VC=0 every cycle
- Behavior identical to pre-VcArb pipeline (Packetize directly to NocReqOut, vc_id always 0)
- **Zero behavioral difference** from prior 270 tests

For NUM_VC=1, both Mode A factory and Mode B factory accept and degrade gracefully:
- Mode A: `write_vc=0, read_vc=0` (constraints validated in ctor)
- Mode B: caller passes `candidate_vcs={{0}, {0}, {0}, {0}, {0}}` (all axi_ch → VC=0)

## 12. Pre-existing Packetize header.last fix (FlooNoC wormhole alignment)

### 12.1 Bug description

Current `c_model/include/nmu/packetize.hpp`:
- Line 109 (`push_aw_with_meta`): `f.set_header_field("last", 1)` — INCORRECT
- Line 138 (`push_w`): `f.set_header_field("last", 1)` — INCORRECT (doesn't follow wlast)

Per FlooNoC reference (citations in §16):
- AW flit: `header.last = 0` — AW is the START of a wormhole packet, not the end
- W flit: `header.last = axi_req_in.w.last` (= wlast) — W's wlast marks packet boundary
- AR / B: `header.last = 1` — single-flit packets (CORRECT in c_model)
- R: `header.last = 1` ALWAYS — R doesn't use wormhole; uses ROB for reorder. rlast stays in payload (CORRECT in c_model)

### 12.2 Why this is architectural, not cosmetic

The `floo_wormhole_arbiter.sv:62-70` releases its VC lock on `last_out = data_o.hdr.last & valid_o`. If c_model's broken `header.last = 1` on every W flit feeds into a wormhole arbiter:
- Arbiter releases the VC after first W beat
- Other AW's W beats can interleave on the same VC
- Receiver gets corrupt write burst (AXI4 violation)

Even though VcArb itself decouples (uses payload.W.wlast), any future wormhole arbiter or NoC fabric component that consults header.last will be broken without this fix.

### 12.3 Fix scope (minimal)

**`c_model/include/nmu/packetize.hpp`**:
```cpp
// Line 109 (push_aw_with_meta): 1 → 0
f.set_header_field("last", 0);  // AW starts wormhole packet (FlooNoC pattern)

// Line 138 (push_w): 1 → b.last
f.set_header_field("last", b.last ? 1u : 0u);  // W's wlast ends wormhole packet
```

**Tests affected**:
- `c_model/tests/nmu/test_packetize.cpp:50`: `EXPECT_EQ(f.get_header_field("last"), 1u)` for AW → change to `EXPECT_EQ(..., 0u)`
- Add 1 new test: `WHeaderLastMatchesWlast` validating per-W-beat: intermediate beats `header.last=0`, last beat `header.last=1`

**Not touched**:
- `nsu/packetize.hpp:71` (B header.last=1) — already CORRECT per FlooNoC
- `nsu/packetize.hpp:92` (R header.last=1) — already CORRECT per FlooNoC

## 13. Test plan (parameterized matrix)

### 13.1 New unit tests (~15 tests, NMU + NSU)

`c_model/tests/nmu/test_vc_arb.cpp` (~10):

| Test | Invariant tested | Without VcArb feature, what fails? |
|---|---|---|
| `Degenerate_NumVc1_AllModesPassthrough` | NUM_VC=1: both modes → VC=0; behavior identical to prior round | NUM_VC parameterization broken: hardcoded value would fail one of NUM_VC=1/2/4/8 |
| `ReadWriteSplit_AW_AR_GoSeparateVcs` | Mode A NUM_VC=2: AW → write_vc, AR → read_vc | Without ReadWriteSplit, all flits go to VC=0 → no concurrency |
| `MultiCandidate_HoLAvoidance` | Mode B NUM_VC=4: AW candidates {0,1}, AR candidates {2,3}; VC=0 full → AW picks VC=1 | Without dynamic candidate, AW would HoL-block on VC=0 even though VC=1 has space |
| `WFollowsAW_InvariantEnforced` | Push AW1 (vc=0) + AW2 (vc=1) (via Mode B); push W1's beats + W2's beats; verify all W1 beats → vc=0, all W2 beats → vc=1 | Without pending_w_routes_ deque, W2 beats would be routed by current mode (potentially vc=0), causing burst corruption at receiver |
| `WlastFromPayloadNotHeader` | Push AW + 2 W beats (1 with payload.wlast=1); verify pending_w_routes_ pops only on payload.wlast=1, ignoring header.last (which is buggy until §12 fix) | Without explicit payload.wlast read, pending_w_routes_ would pop on every W (due to header.last=1 bug) → next AW's W beats route wrongly |
| `RoundRobinFairness_AllVcsServiced_NoStarvation` | NUM_VC=4, push 4 flits to 4 different VCs, tick 4 times → all 4 sent in RR order | Without scan-all-VCs round_robin advance, some VC could be starved indefinitely |
| `CreditGating_TickIdleWhenAllVcsBlocked` | LoopbackNoc per_vc_depth=1; push 4 flits; tick → 1 sent, downstream full; subsequent tick → idle (no spurious push) | Without credit_avail check, push_flit would return false in tick → assert fires |
| `BackpressureChain_VcArbToUpstream` | LoopbackNoc per_vc_depth=1; push fills VC=0 + VcArb pending_[0]=4 → next push_flit returns false → Packetize returns false → backpressure propagates | Without pending queue depth check, VcArb would unbounded-grow → memory leak / hidden backpressure |
| `EnabledModeMixedWith_PriorRoundTests` | Sanity: existing single-VC tests (e.g., NmuRob.Enabled_PushAw_AllocatesSlotAndStampsRobIdx) still pass with VcArb in pipeline (decorator transparent) | Without proper decorator interface adoption, Packetize would not see VcArb as NocReqOut → compile fail |
| `WHeaderLastMatchesWlast` (§12 fix verify) | After §12 fix, W flit's header.last == wlast (per-beat) | Without §12 fix, all W flits have header.last=1 → future wormhole arbiter would prematurely release VC |
| `WFollowsAW_WBeforeAW_DeathTest` (§10.4) | EXPECT_DEATH on `push_flit(W)` when `pending_w_routes_` is empty (no preceding AW); regex match on FQN class::method abort message | Without §10.4 abort, push_flit would call `front()` on empty deque → UB in NDEBUG; release builds would silently corrupt routing |
| `ProtocolViolation_LyingDownstream_DeathTest` (§5.2/§9) | Custom mock downstream where `credit_avail(vc)` returns true and `push_flit(f)` returns false; tick() pulls a flit, calls push_flit, hits assert+abort | Without the protocol-violation guard, a misbehaving downstream would silently drop flits and pending_[] would never drain → hidden hang in production tests |

`c_model/tests/nsu/test_vc_arb.cpp` (~5):

| Test | Invariant tested |
|---|---|
| `Nsu_Degenerate_NumVc1_Passthrough` | NSU NUM_VC=1: B + R → VC=0 |
| `Nsu_ReadWriteSplit_B_R_GoSeparateVcs` | NSU Mode A NUM_VC=2: B → write_rsp_vc, R → read_rsp_vc |
| `Nsu_MultiCandidate_HoLAvoidance` | NSU Mode B variant of NMU's HoL test |
| `Nsu_RoundRobinFairness` | NSU NUM_VC=4 RR fairness |
| `Nsu_CreditGating` | NSU credit gating mirror of NMU test |

### 13.2 Integration test wire (1 fixture update)

`c_model/tests/integration/test_request_response_loopback.cpp`: insert `VcArb` between `Packetize` and `LoopbackNoc.req_out()` / `rsp_out()`. Default mode = ReadWriteSplit, NUM_VC=1 (preserves existing fixture behavior). Optional: 1 new fixture variant exercising NUM_VC=2 with multi_dst_stress for HoL coverage (deferred to future round; not required this round).

### 13.3 Parameterized matrix

Parameterized via TEST_P:
- NUM_VC: 1, 2, 4, 8 (4 instantiations per test)
- Mode: A, B (2 instantiations where applicable)

Not every test runs full matrix:
- Mode A tests only run NUM_VC ≥ 2 (NUM_VC=1 covered by Degenerate test)
- Mode B HoL test only runs NUM_VC ≥ 4 (need multiple candidates)
- Round-robin test only NUM_VC=4 (representative)
- Single-VC sanity tests use NUM_VC=1

### 13.4 Test count summary

| Source | Count |
|---|---|
| `test_vc_arb.cpp` (NMU) | +12 (10 functional + 2 death tests: §10.4 W-before-AW, §5.2/§9 lying-downstream) |
| `test_vc_arb.cpp` (NSU) | +5 |
| `test_packetize.cpp` (1 expectation update + 1 new test for W header.last) | +1 (net: 1 new) |
| **Total new tests** | **18** |
| Prior round total | 302 |
| **Final ctest target** | **320/320** |

### 13.5 Drift gates (per commit)

```bash
cd specgen && py -3 -m pytest -q && py -3 tools/codegen.py --check && py -3 tools/gen_inventory.py --check
cd ../c_model && cmake --build build && ctest --test-dir build -j 1
```

Expected at HEAD:
- specgen pytest: 163 passed
- codegen --check: clean
- gen_inventory --check: clean
- ctest: 320/320

## 14. Commit boundary plan (7 commits)

### Commit 1: `feat(noc): add credit_avail to NocReqOut/NocRspOut interface`
- Modify `c_model/include/noc/noc_req_out.hpp`, `noc_rsp_out.hpp`
- Add virtual method with default `return true` impl
- Acceptance: 302/302 ctest unchanged (default impl preserves all existing mocks)

### Commit 2: `feat(tests/common/loopback_noc): per-VC FIFO + credit_avail impl`
- Modify `c_model/tests/common/loopback_noc.hpp`
- Per-VC `std::deque<Flit>` arrays (NMU req + NSU rsp sides)
- Override `credit_avail` returning per-VC FIFO not full
- Add `nmu_req_vc_q_size(vc)`, `nsu_rsp_vc_q_size(vc)`, `per_vc_depth()` introspection
- Backward compat: legacy `req_q_size()` aggregates all VC sizes
- Acceptance: 302/302 ctest unchanged

### Commit 3: `fix(nmu/packetize): correct AW + W header.last per FlooNoC wormhole semantic`
- Modify `c_model/include/nmu/packetize.hpp` lines 109 + 138
- Modify `c_model/tests/nmu/test_packetize.cpp` line 50 (AW expectation 1→0) + add `WHeaderLastMatchesWlast` test
- Acceptance: 302 → 303 ctest (1 new test added; 1 existing test expectation updated)

### Commit 4: `feat(nmu/vc_arb): VcArb class (Mode A + B) + 10 unit tests`
- Create `c_model/include/nmu/vc_arb.hpp`
- Create `c_model/tests/nmu/test_vc_arb.cpp` (10 functional + 2 EXPECT_DEATH tests)
- Register in `c_model/tests/nmu/CMakeLists.txt`
- Acceptance: 303 → 315 ctest

### Commit 5: `feat(nsu/vc_arb): VcArb class (NSU mirror) + 5 unit tests`
- Create `c_model/include/nsu/vc_arb.hpp`
- Create `c_model/tests/nsu/test_vc_arb.cpp`
- Register in `c_model/tests/nsu/CMakeLists.txt`
- Acceptance: 315 → 320 ctest

### Commit 6: `feat(tests/integration): wire VcArb into PacketizeLoopback testbench`
- Modify `c_model/tests/integration/test_request_response_loopback.cpp`
- Insert NMU VcArb + NSU VcArb in pipelines (Mode A, NUM_VC=1, default depth)
- Acceptance: 320/320 ctest unchanged (decorator transparent in NUM_VC=1 mode)

### Commit 7: `docs(NEXT_STEPS): vc_arb done; next is route_par / nmu.hpp top-level`
- Modify `NEXT_STEPS.md`
- Karpathy 4-lens summary + flip pointer
- Acceptance: 320/320 ctest

### Commit dependency / parallelization

| Commit | Depends on | Can parallelize with |
|---|---|---|
| 1 (NocReqOut interface) | — | 2, 3 |
| 2 (LoopbackNoc per-VC FIFO) | 1 | 3 |
| 3 (header.last fix) | — | 1, 2 |
| 4 (NMU VcArb) | 1, 2, 3 | 5 |
| 5 (NSU VcArb) | 1, 2 | 4 |
| 6 (integration wire) | 4, 5 | — |
| 7 (NEXT_STEPS) | 6 | — |

Parallel waves:
- Wave 1: 1, 2, 3
- Wave 2: 4, 5
- Wave 3: 6
- Wave 4: 7

## 15. Open follow-ups (deferred)

1. **NoC fabric wormhole_arbiter** equivalent (FlooNoC `floo_wormhole_arbiter.sv`). c_model NoC fabric is just LoopbackNoc today; per-link wormhole arbitration belongs in production NoC class
2. **Multi-NSU testbench routing through VcArb**: current multi-NSU testbench (from prior round) bypasses VcArb at integration level. Wire VcArb into multi-NSU paths once basic wiring is verified
3. **Weighted / priority round-robin policies**: current = simple modulo round-robin. Future may need QoS-aware arbitration
4. **VC starvation detection**: instrumentation to alert when a VC is unfairly starved over N cycles
5. **Dynamic per-cycle VC remapping**: Mode B picks VC at push time; if downstream credit drops between push and tick, flit stuck in original VC. Future could re-evaluate at tick time
6. **vc_mapping policy from external config / runtime**: current candidate_vcs_ is ctor-time fixed; could support YAML-driven config for fixture-specific policies

## 16. References

- Prior round design: `docs/superpowers/specs/2026-06-03-test-logger-scenario-observer-design.md`
- Main plan: `docs/noc_cmodel_rtl_plan.md` §3, §3.1 (vc_mapping + vc_arb + credit-based NoC boundary)
- FlooNoC paper: arXiv:2305.08562 (Fischer et al., 2023)
- FlooNoC `floo_pkg.sv` / `typedef.svh:53-63` (flit header `last` field)
- FlooNoC `hw/floo_wormhole_arbiter.sv:34-49, :62-70` (LockIn=1, last_out gates release)
- FlooNoC `hw/floo_vc_arbiter.sv:96-116` (LockIn=0, no last use)
- FlooNoC `hw/floo_output_arbiter.sv:64-75` (wormhole arbiter at output port)
- FlooNoC `hw/floo_axi_chimney.sv:568-577, :584-591, :596-604, :608-616, :624-633, :640-651, :452-457` (AW/W/AR/B/R header.last per-channel stamping + SelW state machine)
- FlooNoC `hw/floo_router.sv:130-143` (per-VC FIFO, no last)
- gem5 Garnet: `OutputUnit.hh:65-72`, `OutVcState.hh:49-55`, `SwitchAllocator.cc:150-220` (has_credit / m_credit_count / sender mirror pattern)
- BookSim 2: `buffer_state.cpp:25-28`, `buffer_state.hpp:174-191`, `credit.hpp:39-45` (IsFullFor / Credit object)
- AXI4 spec: ARM IHI 0022 §A5.3 (same-ID response ordering); §A4.3 (burst length encoding)
- ni_signals.json: `noc_req_credit_i` with `width_param=NUM_VC`, `noc_req_valid_o` 1 bit, NUM_VC default 1, constraint "1 ≤ x ≤ 8; when x > 1, both R_ROB_TYPE and B_ROB_TYPE MUST be != NoRoB"
- ni_packet.json: `VC_ID_WIDTH = 3`
- c_model AxiMaster callback API: `c_model/include/axi/axi_master.hpp:291-292`
- c_model Packetize current state (pre-fix): `c_model/include/nmu/packetize.hpp:109, :138`
