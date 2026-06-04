# WormholeArbiter + Packetize multi-output refactor — design

**Status**: design (Codex round-1 NEEDS-REVISION → fixes folded; pending round-2 verify)
**Date**: 2026-06-04
**Branch**: `stage3/packetize-depacketize` (commits accumulate)
**Prior rounds shipped on this branch**: ROB Disabled+Enabled, addr_trans, Test logger, vc_arb multi-mode

**Reference architecture (FlooNoC pulp-platform)**:
- `hw/floo_axi_chimney.sv:744` — req-side wormhole_arbiter NumRoutes=2 (aw_w_path + ar_path)
- `hw/floo_axi_chimney.sv:758` — rsp-side wormhole_arbiter NumRoutes=2 (b_path + r_path)
- `hw/floo_axi_chimney.sv:709-720` — SelW state machine (AW→W serialization, separate from arbiter)
- `hw/floo_wormhole_arbiter.sv:34-49, :62-70` — LockIn=1, release on `last_out & ready_i`
- Channel header.last semantics (vc_arb round commit `1f82ba8`): AW=0, W=wlast, AR/B=1, R=1

---

## 1. Motivation

5→2 channel mapping (AXI AW/W/AR/B/R → NoC req/rsp) currently happens implicitly in `nmu::Packetize` / `nsu::Packetize` single-output stream. FlooNoC delegates this merging to wormhole_arbiter inside the NI/chimney; the arbiter also enforces wormhole packet locking (AW→W must not be interleaved with AR; B/R each form their own packet).

This round delivers:
- `noc/wormhole_arbiter.hpp` — generic N-to-1 lock arbiter with optional channel pairing for AW→W lock semantic
- Packetize multi-output refactor (NMU 3 outputs aw/w/ar, NSU 2 outputs b/r)
- VcArbiter simplification (`pending_w_routes_` deque → `current_aw_vc_` single optional, enabled by wormhole serialization upstream)
- Mechanical `VcArb` → `VcArbiter` rename (full-word naming consistency)
- Integration testbench wiring update

Architectural intent matches FlooNoC chimney; collapses FlooNoC's separate SelW state machine + wormhole_arbiter into one c_model arbiter that natively handles 3 inputs with AW→W pairing.

## 2. Scope

**In scope**:
- New module `noc/wormhole_arbiter.hpp` + 9 unit tests (6 functional incl. lock-leak/idle-stall + 3 EXPECT_DEATH)
- NMU `Packetize` ctor multi-output (3 NocReqOut refs); NSU 2 outputs
- NMU `VcArbiter` simplification (deque → optional + assert)
- `VcArb` → `VcArbiter` rename (file + class + CMake target)
- Integration testbench wormhole_arbiter wiring
- Test fixture helper `PerChannelCapture<Interface>` mock

**Out of scope** (deferred):
- `nmu.hpp` / `nsu.hpp` top-level assembly (next round)
- NoC fabric router (`noc/router.hpp`) using wormhole_arbiter at output ports (Stage 4)
- `num_inputs=1` pass-through degenerate mode (avoiding dead code)
- Typed accessors `aw_input() / w_input() / ar_input()` helpers
- Header field stubs (`route_par`, `commtype`, `multicast`, `noc_qos`, `flit_ecc`)
- Weighted RR / starvation detection / dynamic VC remap / YAML candidate config

## 3. Anchored decisions

| Decision | Choice | Source |
|---|---|---|
| Round scope | NMU + NSU both sides simultaneously | User Q1 |
| Pipeline order | wormhole_arbiter BEFORE vc_arbiter | User Q2 |
| Packetize output count | 3 (NMU) / 2 (NSU) per AXI channel | User Q3 |
| Path serializer module | Cancelled — wormhole_arbiter absorbs SelW logic via pairing | User Q4 |
| Naming convention | Full-word ("arbiter" not "Arb"); rename VcArb → VcArbiter | User Q5 |
| `w_meta_fifo_` location | Stays in Packetize (metadata inheritance ≠ channel locking) | Section 3 review |
| VcArbiter deque → optional | In this round (architectural cleanup enabled by wormhole) | Section 3 review |
| Test simplicity | 9 unit tests, no parameterized matrix, rely on e2e (324 integration) | User Section 5 |
| Commit 3 indivisible | Keep big commit, accept review risk | User Section 6 + I3 rejected |
| Template -T suffix | Drop; write `WormholeArbiter<NocReqOut>` directly at use sites | Codex I4 |

## 4. Architecture

### 4.1 Pipeline

```
NMU req:  AxiSlavePort → Rob → Packetize{aw_out, w_out, ar_out}
                             → WormholeArbiter<NocReqOut>(3 inputs, pairing {{0,1}})
                             → VcArbiter
                             → NocReqOut (LoopbackNoc)

NSU rsp:  AxiMasterPort → Depacketize+MetaBuffer → Packetize{b_out, r_out}
                       → WormholeArbiter<NocRspOut>(2 inputs, no pairing)
                       → VcArbiter
                       → NocRspOut (LoopbackNoc)
```

Per-cycle tick order (upstream-first, sequel to vc_arb round):
```
master.tick → port.tick → wh_arb.tick → vc_arbiter.tick → loopback.tick
```

Same-cycle propagation: 1 flit can traverse Packetize → wormhole → vc_arbiter → LoopbackNoc in one cycle. Behavior model; not cycle-accurate per stage.

### 4.2 Layered responsibilities

| Module | Manages |
|---|---|
| Packetize | Per-AXI-channel beat→flit conversion; `w_meta_fifo_` (W inherits AW's dst_id/rob_idx) |
| WormholeArbiter | 5→2 channel merge; wormhole packet locking (AW lock until W wlast) |
| VcArbiter | Per-VC pending + round-robin + credit gating + VC assignment per axi_ch |

## 5. WormholeArbiter API

### 5.1 Class shape

```cpp
namespace ni::cmodel::noc {

struct ChannelPairing {
    std::size_t from;   // master port (e.g., aw input port index)
    std::size_t to;     // slave port (e.g., w input port index)
};

// Lifetime: heap-allocate via std::unique_ptr OR construct as a stable named
// member of an owning class. Do NOT push_back into std::vector<WormholeArbiter>
// (deleted move/copy makes that a compile error anyway). InputAdapter holds a
// raw parent pointer; pointer must remain stable for the arbiter's lifetime.
template <typename Downstream>   // Downstream = NocReqOut or NocRspOut
class WormholeArbiter {
public:
    static constexpr std::size_t MAX_INPUTS = 8;

    WormholeArbiter(Downstream& downstream,
                    std::size_t num_inputs,
                    std::vector<ChannelPairing> pairings = {},
                    std::size_t per_input_depth = 4);

    WormholeArbiter(const WormholeArbiter&) = delete;
    WormholeArbiter(WormholeArbiter&&)      = delete;

    Downstream& input(std::size_t idx);  // upstream wires into this
    void tick();                          // drain one flit per cycle

    // Test introspection
    std::size_t pending_size(std::size_t input_idx) const;
    bool is_locked() const;
    std::optional<std::size_t> locked_to() const;

private:
    // InputAdapter inherits Downstream; provides per-port push_flit/credit_avail
    // backed by parent->pending_[idx].
    struct InputAdapter : Downstream { /* ... */ };
    // ... data members
};

}  // namespace
```

Use sites write `WormholeArbiter<NocReqOut>` / `WormholeArbiter<NocRspOut>` directly; no `-T` suffix or alias.

### 5.2 tick() algorithm

```
state: locked_to_ : std::optional<std::size_t>
       round_robin_ptr_ : std::size_t

tick():
    # Select target port
    if locked_to_:
        target = *locked_to_
        if pending_[target].empty(): return       # waiting for paired-port flit
    else:
        target = first non-empty pending starting at round_robin_ptr_
        if none: return

    flit = pending_[target].front()
    if !downstream_.credit_avail(flit.vc_id): return    # downstream blocked

    last = flit.get_header_field("last")

    # Defensive guards
    if target is in any pairing.from and last == 1:
        assert(false && "wormhole_arbiter: from-port flit with last=1 (malformed AW)")
    if target is in any pairing.to and not locked_to_:
        assert(false && "wormhole_arbiter: to-port flit before paired AW (W-before-AW)")

    ok = downstream_.push_flit(flit)
    assert(ok && "wormhole_arbiter: lying downstream (credit_avail=true but push_flit refused)")

    pending_[target].pop_front()
    round_robin_ptr_ = (target + 1) % num_inputs_    # drain-then-advance, matches VcArbiter

    # Lock/unlock state transition
    if last == 0 and not locked_to_:                 # fresh packet start
        for p in pairings_:
            if p.from == target:                     # flit drained from p.from
                locked_to_ = p.to; break
    elif last == 1 and locked_to_:
        assert(*locked_to_ == target)                # unlock target must equal locked target
        locked_to_ = std::nullopt
```

### 5.3 Pairing validation in ctor

- `pairing.from < num_inputs && pairing.to < num_inputs`
- `pairing.from != pairing.to`
- No duplicate `from` across pairings
- No nested chain: a `to` cannot also appear as a `from` in any pairing

### 5.4 InputAdapter

Each `input(idx)` returns a `Downstream&` (an `InputAdapter` instance owned by the arbiter). `push_flit` buffers into `pending_[idx]`; `credit_avail` returns whether pending has slot.

## 6. Packetize multi-output refactor

### 6.1 NMU `Packetize`

Ctor change:
```cpp
// Before (vc_arb round):
Packetize(noc::NocReqOut& req_out, uint8_t src_id);
// After:
Packetize(noc::NocReqOut& aw_out,
          noc::NocReqOut& w_out,
          noc::NocReqOut& ar_out,
          uint8_t src_id);
```

Internal routing:
- `push_aw` / `push_aw_with_meta` → `aw_out_`
- `push_w` → `w_out_` (reads `w_meta_fifo_` for dst_id/rob_idx inheritance, pops on b.last)
- `push_ar` / `push_ar_with_meta` → `ar_out_`
- `push_b` / `push_r` → assert+abort (NMU does not emit responses; existing behavior)

`w_meta_fifo_` retained — orthogonal to wormhole locking (manages metadata inheritance, not channel time slot).

### 6.2 NSU `Packetize`

```cpp
// Before:
Packetize(noc::NocRspOut& rsp_out, MetaBuffer& meta, uint8_t src_id);
// After:
Packetize(noc::NocRspOut& b_out,
          noc::NocRspOut& r_out,
          MetaBuffer& meta,
          uint8_t src_id);
```

`push_b` → `b_out_`; `push_r` → `r_out_`.

### 6.3 `Packetizer` base interface — unchanged

Abstract base in `c_model/include/ni/packetizer.hpp` defines 5 push methods. Doesn't reference NocReqOut. Rob/AxiSlavePort/AxiMasterPort callers are unaffected.

## 7. VcArbiter simplification

### 7.1 Change

NMU `VcArbiter` replaces:
```cpp
std::deque<uint8_t> pending_w_routes_;   // multi-outstanding AW support
```
with:
```cpp
std::optional<uint8_t> current_aw_vc_;   // single outstanding AW (wormhole-serialized upstream)
```

### 7.2 Behavior change

- On `push_flit(AW)`: assert `!current_aw_vc_.has_value() && "must be downstream of WormholeArbiter (multi-outstanding AW detected; upstream serialization broken)"`; set `current_aw_vc_ = chosen_vc`.
- On `push_flit(W)`: assert `current_aw_vc_.has_value() && "W before AW (must be downstream of WormholeArbiter)"`; return `*current_aw_vc_` as VC.
- On `push_flit(W)` with `payload.wlast == 1` after successful enqueue: `current_aw_vc_.reset()`.

NSU `VcArbiter` — no change (no AW/W concept).

### 7.3 Constraint A1 (architectural)

> `VcArbiter` REQUIRES upstream serialization of AW+W bursts (one AW emitted at a time, all its W beats flush before next AW). `WormholeArbiter` placed immediately upstream satisfies this contract. Standalone `VcArbiter` use without such upstream guarantee will trigger assert on multi-AW push.

This constraint propagates to all VcArbiter use-sites. Stated explicitly in assert messages.

## 8. Test plan

### 8.1 New WormholeArbiter tests (9 total in `c_model/tests/noc/test_wormhole_arbiter.cpp`)

Functional (6):
1. NSU pass-through (2 inputs, no pairing, alternating push, RR drain)
2. AW triggers lock (NMU mode, push AW, tick → is_locked, locked_to == w_in)
3. AR cannot interleave during lock (locked state, push AR, tick → AR remains pending)
4. Multi-beat W burst flows through with unlock on wlast (AW + W₁ + W₂ + W₃_last, tick 4 → ordered emit + unlock)
5. Backpressure: input pending full → push_flit false; downstream credit_avail false → tick idle
6. Lock leak / idle stall: push AW, lock fires, no W arrives, tick N → arbiter stays idle (no spurious emit, no deadlock)

Death (3):
7. EXPECT_DEATH W-before-AW (to-port flit when unlocked)
8. EXPECT_DEATH malformed AW (from-port flit with last=1)
9. EXPECT_DEATH lying downstream (mock returns credit_avail=true, push_flit=false)

Ctor pairing validation (bad pairings: duplicate from, nested chain, from==to, out-of-range) — folded into a single death test or smoke test in #7-9 vicinity.

### 8.2 Existing test impact

| File | Change |
|---|---|
| `nmu/test_packetize.cpp` | Fixture: replace `LoopbackNoc` with 3× `PerChannelCapture<NocReqOut>`; test logic unchanged |
| `nsu/test_nsu_packetize.cpp` | Fixture: 2× `PerChannelCapture<NocRspOut>` |
| `nmu/test_rob.cpp` | Fixture wiring update (Rob constructs Packetize internally); Rob test logic unchanged |
| `nmu/test_axi_slave_port.cpp` | Same |
| `nmu/test_vc_arbiter.cpp` (renamed) | `WFollowsAW_InvariantEnforced` restructure to single-AW + multi-W scenario; `WFollowsAW_WBeforeAW_DeathTest` kept |
| `nsu/test_nsu_vc_arbiter.cpp` (renamed) | Pure rename, no logic change |
| `tests/integration/test_request_response_loopback.cpp` | Wire WormholeArbiter into NMU + NSU pipelines; per-cycle tick loop adds wh_arb.tick() before vc_arbiter.tick() |

### 8.3 E2E validation

324 existing integration tests = e2e. WormholeArbiter is transparent (lock semantic preserves order + throughput); all 324 should pass unchanged after wiring.

### 8.4 Test count

Estimated: 359 (vc_arb final) + 9 (new wormhole) = **368 ctest target** (test impact may shift by ±2 depending on VcArbiter restructure).

## 9. Commit boundary plan (5 commits)

| # | Subject | Touches | Ctest |
|---|---|---|---|
| 1 | `refactor: rename VcArb → VcArbiter (full-word naming)` | vc_arb.hpp → vc_arbiter.hpp (NMU+NSU), class rename, CMake targets, all references | 359/359 |
| 2 | `feat(noc): add WormholeArbiter module + 9 unit tests` | noc/wormhole_arbiter.hpp (new), tests/noc/* (new dir), common/per_channel_capture.hpp (new) | 368/368 |
| 3 | `refactor(packetize): multi-output ctor + wire WormholeArbiter into integration` | Packetize NMU/NSU ctor; all Packetize callers (Rob, AxiSlavePort, AxiMasterPort fixtures, integration testbench, VcArbiter test fixtures) | 368/368 |
| 4 | `refactor(vc_arbiter): simplify pending_w_routes deque → optional (Constraint A1)` | nmu/vc_arbiter.hpp, test_vc_arbiter.cpp restructure | 368/368 |
| 5 | `docs(NEXT_STEPS): wormhole_arbiter round done; next is nmu/nsu top-level assembly` | NEXT_STEPS.md, optional 1-line main plan §3 update | 368/368 |

Commit 3 is large (10+ files) but indivisible — Packetize ctor change + caller updates + integration wire are coupled, no compile-clean intermediate state. Mitigation: each affected file reviewed individually for "wiring only, no logic change".

## 10. Constraints (explicit)

**Constraint A1**: `VcArbiter` REQUIRES upstream serialization. `WormholeArbiter` satisfies this when placed immediately upstream. Standalone use → assert on multi-outstanding-AW push.

**Constraint A2**: `WormholeArbiter` REQUIRES Packetize stamps `header.last` per FlooNoC pattern (AW=0, W=wlast, AR/B/R=1; vc_arb round commit `1f82ba8`). Malformed-AW guard catches regression at runtime.

## 11. References

- vc_arb round spec: `docs/superpowers/specs/2026-06-03-vc-arb-multi-mode-design.md`
- Main plan: `docs/noc_cmodel_rtl_plan.md` §3 (Stage 3 NI) + §4 (Stage 4 NoC fabric)
- FlooNoC source: see citations in section header
- Naming rule: `feedback-naming-full-word-no-abbreviation` memory
- Ceremony rule: `feedback-match-ceremony-to-feature-complexity` memory
