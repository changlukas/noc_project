# ROB Family C — Enabled-mode multi-beat read offset

Status: Design approved, pre-implementation
Scope: NMU ROB Enabled-mode read fill correctness + co-sim Enabled validation path

## Problem

NSU `build_r_flit` (`c_model/include/nsu/packetize.hpp:96-104`) stamps every R beat of a
read burst with the same `rob_idx = base`. It reads one MetaBuffer entry keyed by `b.id`,
which carries no per-beat offset.

NMU `pop_r_staged` (`c_model/include/nmu/rob.hpp:355-368`) locates the slot by
`read_entries_[meta.rob_idx]` directly. For a multi-beat read in Enabled mode, beat 2
targets `base` again, finds `slot.ready == true`, and the guard (commit `557a526`) aborts.

`push_ar` (`rob.hpp:232-251`) already reserves `n = len+1` consecutive slots
`base..base+n-1` and records `read_order_by_id_[id].push_back({base, n})`. The slot pool is
ready. The gap is per-beat positioning: beat i must land at `base+i`, not all at `base`.

## Why existing tests missed it

`test_rob.cpp:568 FiresOncePerIdDrain_Enabled` drives a 4-beat read but feeds a hand-crafted
ideal sequence `push_r(0); push_r(1); push_r(2); push_r(3)`. It assumes a `base+offset`
stamping the real NSU never produces, so the fill path looks correct and the bug stays hidden.
Only `557a526`'s `ReadFillSameBaseRobIdxOverwriteGuarded` feeds the real all-base stamping
(`push_r(0); push_r(0)`) and pins the guard with `EXPECT_DEATH`.

## Reachability

`NmuConfig` (`nmu.hpp:124-125`) defaults `read_rob_mode = write_rob_mode = Disabled`.
`NmuWrap::init` (`nmu_wrap.hpp:46`) builds default `NmuConfig{}`. Co-sim runs Disabled, so
the guard is unreachable outside unit tests. This fix also opens an Enabled co-sim path
(read + write) to validate end to end.

## Fix mechanism — per-base arrival counter

Counter keyed by `base` (= `meta.rob_idx`, the burst's own identity, NMU-allocated and
round-tripped via the AR/R flit), not by AXI ID. Rejected the per-id variant: Enabled
`push_ar` has no same-id/different-dst stall (unlike Disabled at `rob.hpp:252-260`), so two
same-id reads to different NSUs can interleave at the receiver and make a per-id counter
ambiguous. Per-base isolates each burst.

**New state (`rob.hpp`)**
- `std::array<uint8_t, ROB_CAPACITY> read_arrival_offset_{}` — per-base beat counter
- `std::array<uint8_t, ROB_CAPACITY> read_range_len_{}` — per-base burst length `n`, set in `push_ar`

**pop_r_staged rewrite (`rob.hpp:355-368`)**

**INPUT** R flit with `meta.rob_idx = base`.

**COMPUTE**
1. `base = meta.rob_idx`
2. `slot_idx = base + read_arrival_offset_[base]`
3. Validate (replaces the old base-slot guard, does not remove fail-fast):
   - `read_arrival_offset_[base] < read_range_len_[base]` (beat within burst length)
   - `slot_idx < ROB_CAPACITY`
   - `read_entries_[slot_idx].occupied && !read_entries_[slot_idx].ready`
   - any violation aborts (malformed beat count / stale rob_idx / bad rlast must not corrupt an adjacent burst)
4. Fill `read_entries_[slot_idx]`, set `ready = true`
5. `++read_arrival_offset_[base]`

**OUTPUT** chain-flush loop (`rob.hpp:372-388`) unchanged: scans `read_entries_[head.base+i].ready`,
emits the range when fully ready. When a range is popped from `read_order_by_id_`, reset
`read_arrival_offset_[head.base] = 0`.

Reset on range-pop (not on rlast). Reason (Codex): if `rlast` is absent but all beats arrive,
the range still chain-flushes by readiness, slots free, and an rlast-reset would leave the
offset nonzero for base reuse. Range-pop reset ties the counter lifecycle to the same event
that frees the range for reuse. `push_ar` cannot reuse `base` until `commit_r_exit`
(`rob.hpp:422-428`) frees all its slots, which happens after the range is popped, so reuse
always starts at offset 0.

**Dependency assumption**: R beats of one burst (one base) arrive in order. Justified —
router single-VC queues are FIFO (`router.hpp:251-258`, confirmed by Codex), so one VC on one
route delays but never reorders. ReadWriteSplit locks all reads onto one VC.

**Write path unchanged**: `pop_b_staged` handles single-beat B (each AW reserves 1 slot),
no multi-beat positioning. Enabled write is already correct, only never exercised in co-sim.

## Unit tests (`test_rob.cpp`)

| Test | Change |
|------|--------|
| `FiresOncePerIdDrain_Enabled` | `push_r(0,1,2,3)` -> `push_r(0,0,0,0)` (real all-base); expect counter lands beats in slots 0..3 |
| `ReadFillSameBaseRobIdxOverwriteGuarded` | invert `EXPECT_DEATH` -> no abort, 4 beats land in order, data markers correct |
| NEW: same-id two bursts, different dst, interleaved R beats | per-base counter files each burst correctly |
| NEW: extra R beat past burst length | aborts (offset >= len), does not write adjacent slot |
| NEW: full burst missing final rlast | aborts or leaves no stale offset after reuse |
| NEW: sequential reuse of same base after full commit | offset starts at 0 |
| NEW: early rlast before reserved slots ready | caught / documented fatal |

## Co-sim Enabled path

Thread `read_rob_mode = write_rob_mode = Enabled` into co-sim without touching existing
Disabled regression TBs.

- Add `cmodel_nmu_create_ex(name, src_id, num_vc, rob_mode)` and the NSU peer. Keep
  `cmodel_nmu_create` as a Disabled-compatible wrapper. Do not change the existing DPI
  signature (every SV import site must match exactly).
- `NmuWrap::init` gains an optional `rob_mode` arg, default Disabled (preserves current path).
- Dedicated Enabled topology/TB selects `_ex`. Existing TBs keep the plain create.
- Stimulus: existing `BUR-002` (8-beat read) plus hotspot for cross-read interleave.
- Pass condition: scoreboard write->readback compare clean, no abort.

## Validation gate

- ctest green including modified and new rob tests.
- Enabled co-sim TB passes multi-beat read patterns (clean rebuild).
- Existing Disabled co-sim regression unchanged.

## Review trail

Two Codex read-only reviews. First rejected the per-id counter (same-id multi-dst interleave),
chose per-base. Second (this design) verdict SOUND-WITH-CAVEATS, must-fix folded in: keep a
computed-slot guard instead of removing it, validate `offset < len`, reset on range-pop not
rlast, add defensive tests, use `_ex` DPI instead of changing the signature.

## Code-level items (resolve with Codex during implementation)

- Exact placement of `read_range_len_` write and counter reset relative to chain-flush.
- `_ex` DPI plumbing through `gen_tb_top.py` codegen and `build_config.mk` topology selection.
