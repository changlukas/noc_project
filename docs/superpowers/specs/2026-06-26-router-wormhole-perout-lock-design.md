# Router per-output wormhole lock (Family B fix) + read-ROB guard (Family C)

Status: Draft (brainstorm approved 2026-06-26)
Scope: c_model fabric router wormhole semantics. Family B = full fix this round. Family C = guard + defer.

## Problem

**Family B (write corruption, live).** `router.hpp` holds a wormhole lock per `(output, vc)`
(`wormhole_[out][vc]`, `:161`) and stage-2 round-robins VCs on each output (`:208-247`, advances
`vc_rr_[out]` after every grant). Two wormhole packets on different VCs routed to the same output
therefore interleave on that output link. The downstream NSU assumes (per FlooNoC) that an AW+W
packet arrives non-interleaved and forwards W to the AXI slave by AW issue order
(`axi_slave.hpp:397`). Interleaved W beats from different sources are assigned to the wrong AW.

Hard-signal: `BUR-002 incr 8-beat + hotspot + vc2/4/8` → 64 scoreboard mismatches (data off by
0x40), identical on base code (pre-existing, not the recent arbiter fix).

**Family C (read reassembly, latent/unreachable).** R flits are stamped `last=1` per beat
(`nsu/packetize.hpp:97-108`) so the router wormhole lock never engages for reads. Reads order via the
NMU ROB, not wormhole. Every R beat of a burst carries the same base `rob_idx` because NSU
packetization reuses the read meta until `rlast` (`nsu/packetize.hpp:138-146`, stamp at `:102-104`).
Enabled-mode fill then writes `read_entries_[meta.rob_idx]` for every beat (`rob.hpp:337-356`) while
commit waits for the whole `base..base+len` range (`:364-378`). The per-ID arrival offset is
missing, so beats overwrite slot `base`. Not reachable today: ROB defaults Disabled
(`nmu.hpp:122-128`), DPI/wrap force `ReadWriteSplit` (R pinned to one VC), the only Enabled
integration scenario is single-beat.

## FlooNoC grounding (reference)

- Wormhole lock is **per-output across VCs**: `floo_vc_router.sv:308-342`. A `hdr.last==0` flit starts
  wormhole routing, "not allowed to be interleaved"; `outport_valid = vc_valid & (~wh_valid |
  wh_correct_sel)` holds the output to the same source until `last`.
- Destination unpacks W directly to AXI, no receive-side W reassembly: `floo_axi_chimney.sv:705-738`.
- Reads use a RoB with per-ID offset, not wormhole: `floo_rob.sv:246-328` (`base + offset_q[id]`).

→ req = wormhole packet integrity, rsp = RoB. The c_model must mirror both, kept separate.

## Design B: per-output wormhole lock

**Data.** Replace per-`(output, vc)` lock with one lock per output.

```
struct WormholeState {
    std::optional<std::size_t> locked_input;
    std::optional<uint8_t>     locked_vc;   // NEW: VC the in-flight packet rides
    std::size_t                rr = 0;       // input round-robin (unlocked scan)
};
std::array<WormholeState, ROUTER_PORT_COUNT> wormhole_;   // was [out][vc]
// vc_rr_[out] (per-output VC round-robin) unchanged, used only when unlocked.
```

**Stage-2 grant (per output, one grant/output/cycle).**

- **INPUT**: output not full (`output_fifo_[out].size() < output_fifo_depth`).
- **COMPUTE**:
  - **Locked** (`ws.locked_input && ws.locked_vc`): consider ONLY `(ws.locked_input, ws.locked_vc)`.
    Eligible if that input-FIFO non-empty and `credit_[out][*ws.locked_vc] > 0`. No other VC is
    examined while locked. This is the per-output hold.
  - **Unlocked**: scan VCs from `vc_rr_[out]`; for each VC scan inputs from `ws.rr` for a front flit
    whose `route_compute(dst) == out` with `credit_[out][vc] > 0`. First match wins.
- **OUTPUT** (single atomic grant): pop flit, `--credit_[out][vc]`, push to `output_fifo_[out]`,
  emit credit pulse. Then:
  - `last == 0` → `ws.locked_input = in; ws.locked_vc = vc` (start/continue hold).
  - `last == 1` → clear `locked_input` / `locked_vc`; `ws.rr = (in+1) % ports`;
    `vc_rr_[out] = (vc+1) % num_vc`.

**Invariant.** Between a `last==0` grant and the matching `last==1` grant on an output, every grant on
that output is from the same `(input, vc)`. No cross-VC interleave on a single output link. Matches
`floo_vc_router.sv` wormhole semantics.

**Stall and isolation (confirmed vs `floo_sa_global.sv:41-48`).** While locked, if the locked
`(input, vc)` FIFO is empty or `credit_[out][vc] == 0`, the output grants nothing this cycle and
resumes the same `(input, vc)` when the flit or credit arrives. This is head-of-line blocking local
to that output. Other outputs grant independently, so no router-wide deadlock. VC scope is per
output. The same VC number on a different output is a separate credit domain and proceeds
independently (no global VC reservation).

**Malformed-packet lifetime (named risk).** A packet that starts with `last==0` and never delivers a
matching `last==1` locks its output forever. This is inherent to wormhole locking. Upstream already
guarantees well-formed packets (NMU `WormholeArbiter` A2 guards plus `Packetize` last stamping). The
router treats a violation as a held output (debuggable via the per-output lock state), not silent
corruption.

**Behavior change (policy, accepted by user).** An output now serves one wormhole packet at a time.
Multi-VC no longer interleaves packets onto the same output. This lowers per-output multi-VC
concurrency versus the current (incorrect) model and matches FlooNoC.

## Design C: guard (defer full fix)

Full C fix (per-ID arrival offset in ROB fill, mirroring `floo_rob.sv` `offset_q[id]`) is out of
scope this round (unreachable today, larger than B).

**Guard.** Not at `push_ar`. Allocating a consecutive slot range for a multi-beat read is correct, and
existing Enabled tests rely on it (they inject distinct per-beat `rob_idx`, `test_rob.cpp:225-246`,
`:416-430`). The hazard is the NSU stamping every R beat with the same base `rob_idx`, so the second
beat lands on an already-filled slot. Guard the read fill in `Rob::pop_r_staged` (`rob.hpp:355-360`):
if the target read slot is already `ready` when a new R beat arrives, fail fast (assert + `std::abort`)
with a message naming the gap (Family C: per-ID arrival offset `base + offset_q[id]` from
`floo_rob.sv` not implemented). This converts silent overwrite into a loud, located failure and leaves
the legitimate distinct-`rob_idx` path untouched.

## Test plan

**B (TDD, router unit).**
- RED (core): two wormhole packets from two different inputs on two different VCs, both routed to the
  same output. Assert the output drains the first packet fully before any beat of the second (no
  interleave).
- Multi-beat W: AW `last=0`, intermediate W `last=0`, final W `last=1`, with a competing packet on
  another VC trying to interleave. The competitor waits until `last=1` releases.
- Locked FIFO empty mid-packet: the locked input stops mid-burst → output stalls (grants nothing,
  does not switch VC/input), resumes the same `(input, vc)` when the next flit arrives.
- Locked VC credit exhaustion: `credit_[out][vc]==0` mid-packet → output stalls, resumes same
  `(input, vc)` when credit returns.
- Same VC number on different outputs proceeds independently (no global VC reservation).
- Different-output independence: a locked output does not block grants on other outputs.
- Malformed (policy): packet with `last=0` and no `last=1` holds the output (documented as held, not
  corrupting); covered by a doc/assert test consistent with the upstream A2 guarantee.

**B (co-sim, hard-signal).** The three previously failing runs must pass:
`make sim TB=mesh_4x4_vc{2,4,8} PATTERN=hotspot HOTSPOT=0 BASE=.../AX4-BUR-002_incr_8beat/scenario.yaml`
→ scoreboard 0 mismatches. Full 4-pattern × 4-TB and 48-run multibeat matrices stay green.

**B (regression).** Existing `test_router.cpp` cases that assumed per-VC interleave are reviewed and
updated to the per-output semantics (do not delete coverage, re-express it).

**C (guard).** Death test: an Enabled read ROB fed two R beats with the same base `rob_idx` aborts on
the second fill. Existing Enabled tests that inject distinct `rob_idx` still pass. Doc test: NSU-built
multi-beat R flits all carry the same base `rob_idx` (records the source of the hazard).

## Out of scope

- Full Family C fix (per-ID ROB arrival offset).
- NSU receive-side W reassembly (rejected: FlooNoC has none, would mask the fabric bug).
- Read-path wormhole packetization (rejected: FlooNoC reads use RoB, not wormhole).

## Risks

| Risk | Handling |
|------|----------|
| Per-output lock changes router timing → other tests assert old behavior | Review `test_router.cpp` + co-sim under TDD; update to new semantics |
| Throughput regression in multi-VC benchmarks | Accepted policy change. Latency re-baselined, correctness is the gate |
| Fairness reduced: a stalled locked packet starves other packets to that output | Inherent to wormhole. Starvation stays output-local (other outputs unaffected), covered by the different-output independence test |
| Malformed packet (no final `last=1`) locks an output forever | Inherent to wormhole. Upstream A2 guards guarantee well-formed packets. Held output is debuggable via lock state, not silent corruption |
| Guard abort surprises a future ROB user | Message names Family C + the offset fix. Spec + doc test record the path |
