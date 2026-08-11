# Stage 5 — Ordering depth

Last stage of the tile-architecture campaign. Stages 0-4 landed; the plan deferred this one's
gate with "defined with the stage", because it was to be scoped from the `HWM` numbers the
earlier stages produced. Those numbers are now in, and they point somewhere the original sketch
did not.

## What the earlier stages actually measured

`mesh_4x4_vc4_rob uniform_random` reports `read_slot_hwm = 3` against `NMU_ROB_R_DEPTH = 128`, and
the number is identical under both Stage 4 latency profiles. Two conclusions:

- **Sweeping the depths upward has no information in it.** At 2% occupancy nothing is near a
  limit. The sweep has to go *down*, until a depth is the thing that stalls.
- **The load is far below the standard bar.** Directed runs default to `INJECTION_COUNT = 4`
  transactions per node. FlooNoC's chimney testbench runs `MAX_READ_TXNS = 20` /
  `MAX_WRITE_TXNS = 20` *in flight* (`floo_axi_test_node.sv:59-60`).

## The stimulus gap

Every directed run to date has issued **one AXI ID per node** (`--ids-per-tile` defaults to 1).
The upstream bar is materially wider:

| | FlooNoC `floo_axi_test_node.sv:50-64` | ours today |
|---|---|---|
| distinct IDs per initiator | 8 — the random master takes `IW = AxiCfg.OutIdWidth = 3` (`:54`, `floo_test_pkg.sv:48`) | 1 |
| ID selection | uniform random over `N_AXI_IDS = 2**IW` | `seq % ids_per_tile`, round-robin |
| ID reuse | yes, `UNIQUE_IDS = 1'b0` | n/a at one ID |
| in flight | 20 read + 20 write | bounded by 4 total |

Their other two configs are narrower, not wider: `AxiCfgN.OutIdWidth = 2` (4 IDs) and
`AxiCfgW.OutIdWidth = 1` (2 IDs), `floo_test_pkg.sv:56,65`. So 8 is the ceiling upstream actually
runs, not a floor — worth knowing before treating 16 as the target.

`UNIQUE_IDS = 1'b0` is the load-bearing part: it puts same-ID pairs (which **must** stay ordered)
and cross-ID pairs (which **may** reorder) in the same run at the same time. That combination is
what the NMU's per-ID structures exist for, and it has never been generated.

What this does **not** mean: that response reordering is untested. `uniform_random` already draws
a fresh destination per transaction, so one ID already spreads across destinations and the RoB
already engages. The untested part is narrower and specific:

| mechanism | exercised today |
|---|---|
| RoB reorder of same-ID responses arriving out of order | yes |
| several per-ID order lists live at once | no |
| ID-to-slot allocation under contention | no |
| one ID's backlog not blocking another's | no |

`NMU_MAX_TXNS_PER_ID = 32` is a **per-ID** bound. At one ID it is indistinguishable from a global
bound, so the parameter has never meant what it says.

## Cross-tile ID uniqueness has to go

`gen_test_patterns.py:191-193` states the invariant "each tile owns an independent,
non-overlapping block of `ids_per_tile` ids ... so no two tiles share an id". It cannot survive
this stage, and it does not need to.

**It cannot.** `num_axi_ids = 1 << INITIATOR_ID_WIDTH = 16` since the Stage 2c ID split. On a 4x4,
16 nodes at one ID each already consume the whole space, so there is no room for a second ID per
tile. The generator does not wrap silently — `gen_test_patterns.py:979` already rejects
`n_nodes * ids_per_tile > id_space` with a hard `ap.error`. **That guard is what blocks this
stage**, and removing it is the change, not a docstring edit.

**It does not need to.** Responses route by the flit's `src_id`, not by AXI ID:
`meta_buffer.hpp:21` (`src_id; // requesting tile; becomes the response flit dst_id`) and
`packetize.hpp:97,123` (`f.set_header_field("dst_id", m.src_id)`). The scoreboard is per node on
`master_dv`. And upstream does not hold the invariant either — each FlooNoC test node draws from
the full ID space independently, so their nodes share IDs by construction.

Action: remove the guard, delete the claim, keep the block layout, and document that blocks
overlap once `ids_per_initiator * n_nodes > 2**INITIATOR_ID_WIDTH`.

**Rename with it.** `ids_per_tile` names a block size, which is only meaningful while blocks are
disjoint. Once overlap is legal the honest name is how many distinct IDs one initiator draws
from: `IDS_PER_INITIATOR` (`--ids-per-initiator`), matching `AXI_INITIATOR_ID_WIDTH` from
Stage 2c. Renaming now costs one Makefile row, one argparse name and a README row; leaving it
costs a knob whose name states an invariant the code no longer holds.

---

## Stage 5a — Random IDs in the stimulus

Goal: the directed stimulus carries FlooNoC-shaped IDs.

The IDs live in the stimulus file, not in the SV master — `axi_file_master` parses `ax_id` per
transaction (`axi_test.sv:2438`). So "random ID" means the Python generator draws them, which is
the right place: the run stays replayable from the file, which is the whole point of a file
master.

| change | file |
|---|---|
| remove the `n_nodes * ids_per_tile > id_space` guard | `gen_test_patterns.py:979` |
| `axid = (id_base + (seq % ids_per_tile))` → draw from the tile's block with `rng` | `:209` |
| docstring: drop the cross-tile uniqueness claim, state the overlap condition | `:191-196` |
| rename `ids_per_tile` → `ids_per_initiator` | `gen_test_patterns.py`, `sim/Makefile`, `sim/verilator/Makefile`, `README.md` |

The generator already seeds `random.Random(a.seed)` at `:991`, so replay costs nothing.

**Success criteria**
- `--ids-per-tile 1` emits byte-identical stimulus to today (`id = src_idx`), so nothing that
  passes now changes.
- `--ids-per-tile N > 1` emits, for a fixed seed, a reproducible ID stream that contains both a
  repeated ID and at least two distinct IDs per node.
- A pytest asserts both, plus that every emitted ID fits `INITIATOR_ID_WIDTH` — the endpoint's
  `a_mst_id_fits` assertion is the runtime backstop and it should never be what catches this.

**Status**: Complete

## Stage 5b — Raise the load

Goal: enough transactions in flight for a depth to be reachable.

`INJECTION_COUNT` is transactions *total* per node, not *in flight*, so matching FlooNoC's 20 in
flight needs a total well above it. Proposal: **64** for the directed axis, chosen as the smallest
power-of-two step that clears the current 4 by an order of magnitude while staying inside one
4 KB region.

**The allocator already scales.** `region_bytes = n_nodes * transactions_per_node * stride` with
`stride = max(_SLOT_STRIDE, burst_footprint)` (`gen_test_patterns.py:984-990`), so the window
grows with the count by construction, and
`test_injection_mode_burst_hotspot_no_overflow_and_disjoint` already covers 200 transactions with
a burst footprint — the shape that produced the previous round's off-by-`0x40` failure. 64 is
inside ground already held. Nothing in the allocator needs to move.

**What must NOT move**: `gen_tb_top.py`'s `REGION_BYTES` (`:137,:588`). It is the DV-side
tb constant, a different value that happens to share the name; the watchdog reads it and already
scales with the count through the `+num_reads` / `+num_writes` plusargs
(`sim/verilator/Makefile:243`, `gen_tb_top.py:659`).

**Success criteria**
- `mesh_4x4_vc1 neighbor` and `mesh_4x4_vc4_rob uniform_random` PASS at the raised count.
- `read_slot_hwm` is materially above 3 — if it is not, the load still is not the limiter and
  the number goes up again before 5c starts.

**Measured**, `mesh_4x4_vc4_rob uniform_random`, `read_slot_hwm` of 128:

| `INJECTION_COUNT` | 4 | 16 | 64 | 128 |
|---|---|---|---|---|
| at 4 ids | 3 | 12 | **60** | 75 |
| at 1 id | — | — | 63 | 58 |

64 was taken rather than 128: the mark rises almost linearly to 64 and then saturates, so past
that the fabric sets the load and the count no longer does. 60 of 128 is 47% occupancy against
the 2% the stage started from.

**The id axis does not move the mark in one direction, and 5c has to explain why.** At 128 four
ids give 75 against one id's 58; at 64 the order reverses, 60 against 63. Destinations are
identical across those pairs — Stage 5a put ids on their own rng stream precisely so this
comparison would be clean — so the difference is the ids alone.

A candidate mechanism, to be tested rather than assumed: a fresh id's first transaction takes the
idle-ID bypass and allocates no slot (`rob.hpp`, "Idle-ID bypass"), so spreading traffic over
more ids buys more bypasses while the lists are short, and stops buying them once every list is
busy. If that is right the crossover is a property of count-per-id, not of count. One
measurement each side of it is not enough to claim it.

**Status**: Complete

## Stage 5c — Run the multi-ID axis and triage

Goal: find out what breaks. This is the stage with real risk, and it is why 5a and 5b come first.

Every previously-unrun axis in this project has yielded a defect on first contact — per-ID VC
binding, hotspot slot overlap, and the `MAPPED` `axi_rand_slave` race in Stage 4. Budget for
triage rather than assuming a clean pass.

### The named question: why the RoB mark moves both ways with id count

5b measured 75 at four ids against 58 at one at `INJECTION_COUNT = 128`, and 60 against 63 at 64.
Destinations are identical inside each pair, so it is the ids. Admission classifies every AW and
AR into exactly one of three branches (`nmu-spec.md:364`, priority order):

| | branch | slot | what raising the id count does to it |
|---|---|---|---|
| 1 | idle-ID bypass — nothing in flight for this id | none | **more often**: more ids means more first-of-streak pushes |
| 2 | same-destination bypass — dst and class match this id's previous push, `fixed_vc` pins the streak to one VC so the fabric cannot split it (`vc_allocator.hpp:11-20`, `trade-off.md:89-96`) | none | **less often**: spreading a node's traffic over more ids shortens each id's same-dest run, and under `uniform_random` the chance the next same-id push repeats a destination falls as ids rise |
| 3 | sticky fall-back | one | **more often**, as the mirror of 2 |

### Measured — and clause 2 is not part of it

The telemetry landed and the first numbers refute the paragraph above. Per-node AW split:

| run | idle | same_dest | alloc | `read_slot_hwm` | `order_list_hwm` | `write_txns_hwm` |
|---|---|---|---|---|---|---|
| `4x4_vc1 neighbor`, 1 id | 1 | **63** | **0** | **0** | 26 | 27 |
| `4x4_vc4_rob uniform_random`, 1 id | 6 | **0** | 50 | 25 | 22 | 23 |
| same, 4 ids | 15 | **1** | 40 | 22 | 11 | 22 |
| same at count 128, 1 id | 13 | **0** | 100 | 26 | 22 | 22 |
| same at count 128, 4 ids | 33 | **3** | 77 | 26 | 11 | 22 |

**Clause 2 cannot be falling as ids rise, because under `uniform_random` it is already zero.** A
fresh destination is drawn per transaction out of 16, so the chance the next same-id push repeats
one is 1/16. The live trade is clause 1 against clause 3 alone: idle bypasses rise with id count
(6 -> 15, 13 -> 33) and allocations fall to match (50 -> 40, 100 -> 77).

That leaves the original question open rather than answered. Fewer allocations should mean a
lower mark, and at count 64 it is (25 -> 22), but at 128 it is not (26 -> 26 here, 58 -> 75 taken
as the max across nodes). Node-level and max-across-nodes disagree, so the next measurement is
the clause split **for the node that owns the maximum**, not for node 0 or node 15.

`neighbor` is the mirror image and explains why every neighbor run has reported a zero mark: 63
of 64 AWs take clause 2, nothing allocates, and the RoB is never used.

### What the pools say, which was not the question but matters more

`4x4_vc1 neighbor` reaches `write_txns_hwm = 27` against `NMU_OUTSTANDING_DEPTH = 32` and
`order_list_hwm = 26` against `NMU_MAX_TXNS_PER_ID = 32` — 84% and 81% — while its RoB sits at
zero. The two pools are close to binding on the plainest pattern in the suite, and the RoB, which
this stage was scoped around, is idle there.

5d's sweep order follows from that: `OUTSTANDING_DEPTH` first.

**Success criteria**
- `mesh_4x4_vc4_rob uniform_random` at `--ids-per-initiator 4` and at the full 16 reaches a
  non-vacuous PASS, or the failure is root-caused and either fixed or recorded in
  `docs/known-limitations.md` with the evidence that separates fabric from stimulus.
- Same for `mesh_2x2_vc1`, where 4 nodes x 4 IDs fits the space exactly with no block overlap —
  this is the control that tells overlap-induced failures apart from multi-ID-induced ones.
- The clause split is reported per run, and it accounts for the direction the high-water mark
  moved at both counts. "The mark went up" without the split is not an explanation.

**Status**: Not Started

## Stage 5d — Sweep the depths down

Goal: the four NMU depths get a measured basis instead of a placeholder value.

| parameter | default | plusarg |
|---|---|---|
| `NMU_ROB_B_DEPTH` | 128 | `B_ROB_DEPTH` |
| `NMU_ROB_R_DEPTH` | 128 | `R_ROB_DEPTH` |
| `NMU_MAX_TXNS_PER_ID` | 32 | `MAX_TXNS_PER_ID` |
| `NMU_OUTSTANDING_DEPTH` | 32 | `OUTSTANDING_DEPTH` |

All four already have plusargs, so the sweep needs no new command surface. One axis at a time,
downward, from the loaded multi-ID run 5b and 5c establish.

**`read_slot_hwm` only speaks for the RoB.** It is RoB slot telemetry (`rob.hpp:154`), so the
"depth 128, HWM 3" observation justifies sweeping the two RoB depths downward and nothing else.

The other two bind on traffic the RoB never sees. `MAX_TXNS_PER_ID` bounds the per-ID order list,
and a push enters that list whether or not it takes a RoB slot — `rob.hpp:346` tests the list
length, `rob.hpp:394` pushes unconditionally with `needs_rob` as a field. `OUTSTANDING_DEPTH` is
the shared pool, checked first at `rob.hpp:344`.

**Whether `MAX_TXNS_PER_ID` can bind at all is an open question, and 5d must answer it before
sweeping it.** Both defaults are 32, and every accepted push takes one pool slot and one list
entry, so on a naive reading the pool always fills first and the per-ID test is unreachable. It
is not that simple: the two release at different moments. `write_txns_` drops in `retire_b`
(`rob.hpp:554`) when the fabric delivers the response into the RoB; the list entry pops in
`pop_b_staged` (`rob.hpp:616`) when the master actually takes the B. Under B-channel
backpressure the list outlives the pool slot and can exceed it.

So the honest position is: the per-ID limit is reachable only with response-side backpressure,
which the ideal slave profile never produced and Stage 4's `random` profile now does. Whether it
is ever reached in practice cannot be argued from the source — it has to be measured.

**5d therefore starts with telemetry, not with a sweep.** `read_slot_hwm` (`rob.hpp:154`) is the
only high-water mark that exists, and it covers RoB slots alone. Add one for the per-ID order
list depth and one for each shared pool (`write_txns_`, `read_txns_`), exposed the same way. Then
each of the four parameters has a number attached to it before anything is swept, and the sweep
tests a limiter already known to be live.

**Success criteria**
- For each parameter, the value at which it becomes the limiter is recorded.
- At that value the run **stalls and still passes** — the spec's claim is that overflow stalls
  rather than erroring or dropping. A failure here is a real defect, not a sweep result.
- Each default is then either re-justified against the measurement or changed, with the reasoning
  written where the value lives (`specgen/source/constants.yaml`) and the
  `docs/known-limitations.md` "never-swept placeholder" row retired.

**Measured** on `mesh_4x4_vc4_rob all_to_all`, one id per initiator, count 64. Every point
reaches a non-vacuous PASS — no depth produced an error or a lost transaction, which is what the
spec claims (overflow stalls).

| `OUTSTANDING_DEPTH` | 32 | 16 | 8 | 4 | 2 | 1 |
|---|---|---|---|---|---|---|
| `write_txns_hwm` | 32 | 16 | 8 | 4 | 2 | 1 |
| `order_list_hwm` | 32 | 16 | 8 | 4 | 2 | 1 |

| `MAX_TXNS_PER_ID` (pool 32) | 32 | 16 | 8 | 4 | 2 | 1 |
|---|---|---|---|---|---|---|
| `order_list_hwm` | 32 | 16 | 8 | 4 | 2 | 1 |
| `write_txns_hwm` | 32 | **17** | **9** | **5** | **3** | **2** |

Two results.

**The pool holds exactly one more than the per-ID list, at every point.** That is the release
asymmetry written into 5d above, now measured rather than argued: `retire_b` frees the pool slot
when the fabric delivers the response, `pop_b_staged` frees the list entry when the master takes
it, so one transaction sits in the pool without a list entry. `MAX_TXNS_PER_ID` is therefore
reachable at any pool depth, and the "it can never bind" reading was wrong for this reason.

**At one id the two parameters are the same knob.** The upper table shows the list following the
pool exactly, because a single id's list is the pool. They separate only with several ids, which
is the case the sweep still has to cover.

### `read_slot_hwm` does not mean what its name suggests

`read_free_space` (`rob.hpp:141-144`) returns `r_rob_depth_ - 1 - highest_set(alloc_read_)`, so
`read_slot_hwm` is the peak **allocation frontier** — the highest live slot index — not the count
of simultaneously occupied slots. It reads 63 on a run whose read outstanding never exceeds 26,
which is not a contradiction: allocation bumps forward and only reclaims from the top, so a freed
slot below the frontier does not pull it back.

For sizing the RoB that is the number that matters, since the frontier is the depth that has to
exist. For "how full did it get" it is an overestimate, and every earlier statement in this plan
that read it as occupancy — including the Stage 5b direction analysis the whole of 5c was scoped
around — was reading it as something it is not.

### Multi-id: the pool binds, the per-ID list does not

Four ids, 64 transactions, so 16 per id. Only this case separates the two parameters.

| | default | per_id 16 | per_id 8 | per_id 4 | pool 16 | pool 8 | pool 4 |
|---|---|---|---|---|---|---|---|
| `order_list_hwm` (limit 32) | **12** | 12 | 8 | 4 | 9 | 6 | 4 |
| `write_txns_hwm` (limit 32) | **30** | 30 | 25 | 14 | 16 | 8 | 4 |

At the shipped settings the shared pool reaches 30 of 32 while the deepest per-ID list reaches 12
of 32. Dropping `MAX_TXNS_PER_ID` to 16 changes nothing, because 16 is still above the 12 the
traffic produces. **`NMU_MAX_TXNS_PER_ID = 32` does not bind at four ids; the pool binds first**,
and the per-ID limit only becomes the limiter below about 12, which is under the spread the
traffic naturally has.

That is the answer the parameter was waiting for. At one id the two are the same knob; at several
ids the per-ID limit is slack by roughly 2.5x. It earns its place only as a fairness cap against
one id monopolising the pool, and no stimulus in the suite produces that shape.

### RoB depths: both stall cleanly, and both are sized well above need

One id, the worst case for the read frontier.

| `NMU_ROB_R_DEPTH` | 128 | 64 | 32 | 16 | 8 |
|---|---|---|---|---|---|
| `read_slot_hwm` | 63 | 63 | 32 | 16 | 8 |
| result | PASS | PASS | PASS | PASS | PASS |

| `NMU_ROB_B_DEPTH` | 128 | 64 | 32 | 16 | 8 |
|---|---|---|---|---|---|
| `write_txns_hwm` | 32 | 32 | 28 | 17 | 9 |
| result | PASS | PASS | PASS | PASS | PASS |

The frontier clamps to whatever depth it is given and the run still completes, down to 8 of the
128 shipped. So the 63 measured at depth 128 is not a requirement — it is how far a bump
allocator wanders when nothing forces it to reuse. Read `read_slot_hwm` at a generous depth as an
upper bound on wandering, not as a floor on what the design needs.

`NMU_ROB_R_DEPTH` is the one that costs area: one slot per read **beat**, each holding a full data
word, so 128 x 512 b is 8 KB. The design completes at 8.

**What this sweep does not establish**: the throughput cost of a smaller depth. Every point
passes, so correctness is settled and the stall-not-error claim holds, but a shallower buffer
stalls more often and nothing here measures that. Changing a shipped default needs that number
first.

**Status**: Complete.

---

## Verification

Tier 2 per the standing table in `docs/backlog.md` after 5a and 5b; the Stage 3 gate set
(`mesh_4x4_vc1` plus the permutation patterns) is unaffected by an ID change and does not need
re-running. 5c and 5d carry their own criteria above.

`make pytest` after 5a and 5b, since both touch `sim/tools/`.

No c_model change is planned in 5a-5c, so ctest is Tier 1 only. 5d may produce one if a depth
turns out to be a real limiter with a bug behind it.

## Follows this stage, not part of it — NI ID architecture

Decided while planning 5d, scoped out because it changes the NI's interface rather than its
verification. Recorded here so the numbers are not lost; it gets its own survey and review round.

| | now | decided |
|---|---|---|
| NI-facing AXI ID width | 8 b (`AXI_ID_WIDTH`) | **3 b**, matching every FlooNoC config (`floo_test_pkg.sv:48,56,65`) |
| per-ID structures | `2**8 = 256` lists (`rob.hpp:241`) | 8 |
| transactions per ID | 32 | 32, unchanged |
| total outstanding | 32, capped by the shared pool | **256**, the per-ID capacity |

Three consequences to settle in that round, not here:

- **The shared pool may have to go.** FlooNoC has no outstanding counter on this path —
  `axi_aw_queue_ready_in = aw_rob_ready_out` (`floo_axi_chimney.sv:376`) is the only gate, so the
  per-ID FIFOs are the whole limit. Keeping `OUTSTANDING_DEPTH = 32` in front of an 8x32 structure
  puts the total straight back to 32.
- **`axi_id_remap` becomes required, not optional.** `AXI_INITIATOR_ID_WIDTH = 4` gives one
  initiator 16 IDs and a tile has two initiators, so up to 32 distinct IDs must fold into 8.
  pulp's `axi_iw_converter` selects `axi_id_remap` for exactly this case
  (`axi_iw_converter.sv:127-146`); using `axi_id_remap` directly is the same thing with the
  branch spelled out.
- **`AxiSlvPortMaxUniqIds` is a distinct mechanism** — how many upstream IDs the remap table
  tracks at once — and it sits outside the NMU. Not an NI parameter.

This is why Stage 5 runs first: `--ids-per-initiator` is bounded by what the remap can track, and
the count of distinct IDs actually seen (the third telemetry point in 5d) is the input that round
needs.

## Review

Reviewed by Codex against the repo and the upstream copies on disk. Three errors it found are
corrected above:

| claim as first written | what the code says |
|---|---|
| tiles collide "silently, because the `%` hides it" | `gen_test_patterns.py:979` raises a hard `ap.error`. Removing that guard is the change |
| FlooNoC drives "8 or 16" IDs per initiator, from `InIdWidth` | the random master takes `OutIdWidth`: 8, 4 and 2 across their three configs |
| raising the count means the allocator's derivation must be checked | it already derives from the count, and a 200-transaction burst case is already a regression test |

It also added the `read_slot_hwm` caveat now in 5d, and recommended the `ids_per_tile` rename
that the plan previously left open. The stage order, the downward sweep direction, and the
finding that no checker depends on an AXI ID identifying its source tile were all confirmed
independently.

Separately, review raised that a per-ID limit equal to the shared pool depth looks inert — the
usual spec-level knob is one outstanding depth, not a per-ID one. Chasing it produced the
release-timing asymmetry now written into 5d, and the telemetry step that has to come before any
sweep. The parameter is not inert, but nothing in the repo can currently say how close it gets.
