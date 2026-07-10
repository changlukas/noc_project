# NoC backlog

Running action items and open bugs, maintained across iteration rounds. Each round adds what it
surfaces and strikes what it closes. Read it at session start. An item is not started unless a round
picks it up.

## SAM address remap — implemented on `feat/sam-remap` (2026-07-08, pending merge review)

Replaced the `addr[39:32]` bit-slice decode with a per-tile SAM table `{base, size, dst_id,
remove_offset}` loaded from the topology YAML `address_map` block; NMU rebases `local_addr = addr -
remove_offset` so subordinates see 0-based local addresses. Spec
`docs/superpowers/specs/2026-07-08-sam-remap-design.md`, plan `docs/superpowers/plans/2026-07-08-sam-remap.md`.
11 tasks, subagent-driven, each task + a final whole-branch review clean. Verified: ctest 397/397,
directed co-sim rebase-proven (slave sees local `0x1000`-range) on all 4 directed patterns
(`neighbor`/`transpose`/`uniform_random`/`hotspot`, `mesh_4x4_vc1`, scoreboard clean, 16 nodes non-vacuous),
constrained_random zero-%Error with the offset-normalized `axi_reorder_compare`, non-uniform-size tile-map
smoke clean, generator pytest 16/16.

**Follow-ups (deferred, none merge-blocking):**
- **Per-tile arbitrary `base` guard.** The c_model SAM honors an explicit `tiles:` `base`, but
  `gen_tb_top.py`/`gen_test_patterns.py` compute the region/address as `coord_id*tile_size` only. A
  future non-uniform YAML with `base != coord_id*tile_size` would silent-misroute (stimulus/REGION_BASE
  vs c_model SAM diverge, no assert). No shipped YAML triggers it (the smoke overrides size only, base=0).
  Add a fail-loud guard (generators assert `base == coord_id*tile_size`, or honor the override) before any
  arbitrary-base map ships. Co-sim non-uniform is SIZE-override-only today.
- **`translate` miss under `NDEBUG`.** Miss ⇒ `assert` (fail-loud), but a release/`NDEBUG` DPI build would
  null-deref instead. ctest/co-sim build with asserts on; only matters if a release DPI lib is produced.
- **`max_unique_ids` guard under `NDEBUG`.** Same class. The only validity check on `max_unique_ids` is an
  `assert` in the `Depacketize` ctor (`nsu/depacketize.hpp:31-33`). Under `NDEBUG` a misconfigured value
  (say 5) would silently take the identity remap instead of failing. Inert today: only 1 or 256 ever reach
  the ctor, and no release build exists. Promote to a runtime `throw` if an `NDEBUG` DPI build ever lands.
- **`+sam_config` unconditional.** Run recipes always pass it; a topology YAML lacking `address_map` would
  assert in `load_sam_table`. All 6 current YAMLs carry the block. A clearer error string is nice-to-have.
- ~~`make build-cmodel` ignores `local.mk` BUILD_ROOT~~ **FIXED** (`0981828`): `CMODEL_BUILD` changed from
  `:=` to `=` (deferred) so the `local.mk` BUILD_ROOT override (`$(HOME)/noc_build` on WSL) applies.
  `make build` / `make test` now target `$HOME/noc_build/cmodel` correctly; no cmake workaround needed.

## NMU RoB -- as-built spec written, direction reset (2026-07-10)

The 2026-07-10 item that stood here ("decouple the NMU RoB from the outstanding limit") rested on
three false premises. A FlooNoC source survey (me + Codex, both against `E:/05_NoC/FlooNoC`) killed
all three. The as-built microarchitecture is now written down: **`docs/nmu-rob-microarchitecture.md`**.

| the old claim | what the RTL says |
|---|---|
| "outstanding request limit: FlooNoC `MaxTxns`, here none" | `MaxTxns` is the **NSU** meta buffer depth (`floo_axi_chimney.sv:811-816`, guarded by `EnSbrPort`; `floo_meta_buffer.sv:21` "Maximum number of non-atomic outstanding requests"). We already model it as `meta_buffer.max_outstanding` = 32. `floo_axi_chimney.sv:872-873` asserts RoB and meta buffer sit on opposite faces. |
| "our slot pool doubles as the outstanding limit -- that is the coupling to fix" | It is the **deadlock guarantee**, and FlooNoC gates identically (`floo_rob.sv:345`). `docs/floonoc/chimneys.md:18`: "Stalling the network is not an option ... the NI also needs to track or allocate space in the RoB and only inject new requests into the network if it can guarantee that the responses can be handled." |
| "add an independent outstanding counter before slot allocation" | Pointless alone. An independent per-ID limit (`MaxTxnsPerId`) becomes **mandatory** only once a bypass exists, because a bypassed transaction still occupies an order-list entry while consuming no slot (`floo_rob.sv:443` pushes unconditionally). Bypass and `MaxTxnsPerId` are one feature. |

What FlooNoC actually does that we do not: `floo_rob_status_table` skips slot allocation for two
classes of transaction (`floo_rob.sv:422-433`) -- the first transaction of an ID, and a follow-on to
the same destination as the previous one. Only the `else` branch allocates. That is why a 64-entry
FlooNoC RoB is mostly idle.

**Clause 2 (same destination) does not hold in our fabric.** It assumes same destination implies
in-order arrival, which FlooNoC buys by hardwiring `NumVirtChannels(1)` into `floo_axi_router`
(`:100,132`). Our VC arbiter spreads one ID's packets across a VC pool ID-agnostically
(`nmu/vc_arbiter.hpp:10-13`) and the router round-robins VCs per output (`router/router.hpp:240-250`).
Restoring the clause needs per-ID VC pinning -- the mechanism deliberately deleted on 2026-06-30
([[project_vc_id_agnostic_landed]]) -- which surrenders the VC spread that a multi-VC fabric exists
to provide. FlooNoC has never shipped clause 2 together with multiple VCs; its VC router is in
`hw/deprecated/`, and it assigns VC by next-hop direction with free-VC overflow, never by AXI ID
(`hw/deprecated/vc_router_util/floo_vc_assignment.sv:70-88`, `floo_vc_selection.sv:36-46`).

### Measured, but NOT validated -- do not build on these numbers

A throwaway probe counted, per admitted transaction, whether each FlooNoC bypass clause *would* have
fired. Reverted after the run; the patch is not in the tree. The aggregation reproduced the published
sweep numbers exactly (`vc1 uniform_random` = 1227.7, `vc8` = 2127.8), which validates the harness but
not the counters.

Saturation, `INJECTION_MODE=1 INJECTION_RATE=1.0`, 200 txn/node x 16 nodes, seed 1:

| tb | pattern | ids/tile | clause 1 (AW) | clause 2 (AW) | agg BW |
|---|---|---|---|---|---|
| vc1 | neighbor | 1 | 0.5% | 99.5% | 2079.3 |
| vc1 | neighbor | 16 | 59.8% | 40.2% | 2079.3 |
| vc1 | uniform_random | 1 | 0.5% | 0.0% | 1227.7 |
| vc1 | uniform_random | 16 | 93.6% | 0.4% | 1228.3 |
| vc8 | neighbor | 1 | 0.5% | 99.5% | 2564.3 |
| vc8 | neighbor | 16 | 59.8% | 40.2% | 2564.3 |
| vc8 | uniform_random | 1 | 0.6% | 0.1% | 2127.8 |
| vc8 | uniform_random | 16 | 24.2% | 1.2% | 2219.2 |

- **clause 1 at the shipped stimulus is 0.5%**, i.e. exactly 16/3200 -- the first transaction of each
  node, once. Injection rate does not move it (0.2 / 0.5 / 1.0 all give 0.5%). Structural: `--ids-per-tile`
  defaults to 1 (`gen_test_patterns.py:483`, never passed by `sim/verilator/Makefile`), so each tile
  drives one AXI ID whose order list never drains while the master pipelines.
- **RoB occupancy does not govern throughput here.** Raising ids/tile 1 -> 16 swings clause-1 hit rate
  from 0.5% to 24-94% (so the RoB empties) while BW moves +0.05% (vc1) / +4.3% (vc8). This **refutes**
  the earlier backlog claim that "the per-NMU RoB ceiling (32 entries, `rob.hpp:80`) is the true limiter".
  Caveat: ids/tile also raises master-side concurrency, so the +4.3% is not cleanly attributable.
- **The probe was never fault-injected.** A sticky-flag clear bug would produce the identical 16/3200
  signature. Before any number above is used, delete the sticky flag and confirm the count changes
  ([[feedback_verification_ip_fault_injection]]).
- **`neighbor` BW is bit-identical across ids/tile** (2079.3 / 2564.3 to the decimal) while
  `uniform_random` moves. Unexplained.
- **Nothing here sees a burst.** All runs are `--len 0`, so one read slot = one transaction. The read
  pool's beat-granular allocation (`len+1` slots per read burst, since rewritten to the lzc allocator) and its
  fragmentation are the one place the RoB plausibly binds, and this matrix cannot see them.

### Next

Per user (2026-07-10): the c_model is where hardware design decisions get pinned, so area is now a
first-class concern. Do **not** build an area model inside the c_model -- FlooNoC does not. Expose the
parameters that determine area, sweep performance against them, compute area outside.
`docs/nmu-rob-microarchitecture.md` section 7 lists the five shapes C++ chose by accident; section 6
maps them onto FlooNoC's five named sizes. Ranked candidates:

**IN PROGRESS.** Spec `docs/superpowers/specs/2026-07-10-nmu-rob-bypass-and-depth-design.md` covers
items 1-3 below: `b_rob_depth` / `r_rob_depth` as independent runtime parameters, bypass clause 1,
`max_txns_per_id`. Codex-reviewed; two decisions were refuted and rewritten (`max_txns_per_id` is a
sizing parameter, not a safety requirement; bypass adds no backpressure class the shipped
`RobMode::Disabled` path does not already have).

1. Fault-inject the probe (blocking for every number above).
2. ~~`BRoBSize` / `RRoBSize` as independent runtime parameters~~ **DONE.** `b_rob_depth_` /
   `r_rob_depth_` are ctor parameters (default 32, `rob.hpp:47-48`), plumbed from a Makefile knob
   (`B_ROB_DEPTH` / `R_ROB_DEPTH`, `sim/verilator/Makefile:211-212`) end to end. See
   `docs/nmu-rob-microarchitecture.md` section 6.
3. ~~Burst co-sim (`--len > 0`)~~ **DONE.** `BURST_LEN` knob (`sim/verilator/Makefile`), verified
   with a 64-beat read burst -- see "FIXED 2026-07-11" in the Bugs section below.
3b. ~~**Per-beat R release.**~~ **DONE.** `drain_ready_read_heads_` releases one beat at a time via
   `read_release_offset_` (`rob.hpp:347-364`), matching FlooNoC (`floo_rob.sv:250-266,287-297`).
   See `docs/nmu-rob-microarchitecture.md` section 3, Release.
4. Per-ID order table structure and `NumIds`: 256 independent FIFOs (`floo_rob.sv:450-465`) vs a
   shared `id_queue` (`floo_meta_buffer.sv:146-151`, which FlooNoC uses for the same problem) differ
   by roughly an order of magnitude at 256 IDs. Third option: pulp `axi_id_remap`
   (`axi/v0.39.7/src/axi_id_remap.sv`, params `AxiSlvPortMaxUniqIds` / `AxiMaxTxnsPerId`) in front of
   the RoB, which is the standard way to give a RoB a small `NumIds` without narrowing the NI's
   external AXI ID width. `AWID_WIDTH = 8` is a spec value; changing it needs approval.
5. `RobMode` per direction. `NmuConfig` has `read_rob_mode` / `write_rob_mode`; `nmu_wrap.hpp:71-72`
   ties them. A B-only RoB is FlooNoC's cheap point (`BRoBType` != `RRoBType`) and is unreachable today.
6. `RobMode::Disabled` vs FlooNoC `NoRoB`: ours stalls same-ID regardless of destination, FlooNoC
   admits same-destination follow-ons up to a counter (`floo_rob_wrapper.sv:139`). **Not a gap we
   can close.** It rests on the same "same destination implies in-order arrival" premise as bypass
   clause 2, and fails for the same reason -- multiple VCs, ID-agnostic round-robin, no reorder
   storage in `NoRoB` to catch the overtake.
7. `max_txns_per_id` default (32) is still `[TBD]` -- needs the depth sweep before it is a
   considered value rather than a placeholder.
8. `r_rob_depth = 256` (8 KiB, the paper's design point) is now expressible
   (`R_ROB_DEPTH=256`, `sim/verilator/Makefile:212`) but unswept.
9. **VC allocation** -- new design round. Port `floo_vc_assignment.sv:70-88`: per-hop turn-model VC
   assignment with look-ahead routing, per-port VC counts `{2,4,2,4,4}`, `AllowVCOverflow = 0`. It
   would unlock bypass clause 2 and the `NoRoB` NI (25 kGE vs 281 kGE, paper §VI-C) at the cost of
   the `vc1/2/4/8` configuration axis. Lives in `hw/deprecated/`.

**Consequence worth stating once.** Under ID-agnostic VC round-robin, *every* cheap ordering path
FlooNoC offers -- bypass clause 2, `NoRoB`'s same-destination counter -- is closed to us. The only
way to hold more than one outstanding transaction per AXI ID is a RoB. `RobMode::Disabled`
(one per ID) and `RobMode::Enabled` (RoB) are therefore not two points on a spectrum with a middle;
they are the only two points. Bypass clause 1 remains available because it assumes nothing about the
network. This is the price the VC spread charges, and it has never been written down.

## Done -- injection-mode + rate sweep, VC comparison figures (2026-07-09, branch `feat/injection-mode-sweep`)

Spec `docs/superpowers/specs/2026-07-09-injection-mode-and-rate-sweep-design.md`, plan
`docs/superpowers/plans/2026-07-09-injection-mode-and-rate-sweep.md`. Folded continuous injection into
`make sim`, retired the `run-traffic` / `sim-saturation` / `collect_saturation.py` / `plot_saturation.py`
path, and produced the VC comparison figure at the shipped design point.

**Interface:** `make sim TB=<topo> PATTERN=<p> [INJECTION_MODE=1 INJECTION_RATE=<r> INJECTION_COUNT=<n>]
[MAX_UNIQUE_IDS=<n> MAX_OUTSTANDING=<n>] [SEED=<n>]`. `INJECTION_MODE=0` (default) is the directed/checked
run; `=1` is continuous injection for a throughput measurement, one `continuous_*/result.csv` per run.
`INJECTION_COUNT` default is mode-dependent (`4` in mode 0, `200` in mode 1). `make sim-injection-sweep
PATTERN=<p>` loops `vc1/vc2/vc4/vc8` at nine rates; `sim/tools/plot_injection_sweep.py <p>` renders the
throughput + latency figure (light `injection_sweep.png`, dark `injection_sweep_dark.png`, dpi=300).

**Sweep result** (`uniform_random`, `_rob`, `max_unique_ids=1`, `max_outstanding=32`, saturation at
rate 1.0, seed 99425787): vc1=1240, vc2=1642, vc4=2071, vc8=2179 bits/cyc. Clear knee at rate ~0.2-0.4,
then plateau. VC value vc1->vc8 = +75.7% at saturation.

**Bring-up numbers** (all eight, `uniform_random`, `_rob`, mode 1, rate 1.0, seed 1; fault injection
confirmed both knobs wired: `MAX_OUTSTANDING=1`->1105.1 vs `=32`->1227.7 differ; illegal
`MAX_UNIQUE_IDS=5` aborts at `depacketize.hpp:50`, rc=2):

| axis | value | vc1 | vc8 |
|---|---|---|---|
| `MAX_OUTSTANDING` (`MAX_UNIQUE_IDS=256`) | 32 (shipped) | 1227.7 | 2127.8 |
| | 512 (ideal sink) | 1227.7 | 2127.8 |
| `MAX_UNIQUE_IDS` (`MAX_OUTSTANDING=32`) | 1 (shipped) | 1227.7 | 2127.8 |
| | 256 | 1227.7 | 2127.8 |

- **VC value at the design point** `(vc8-vc1)/vc1` at `MAX_OUTSTANDING=32`: **+73.3%**. The headline
  figure shows this.
- **What the 32-entry NI buffer costs** `(mo512-mo32)/mo32`: **0.0% at both VC counts.** This is not a
  stuck knob (Step 1 fault injection moved the number, `=1`->`=32`: 1105.1->1227.7). Under
  `uniform_random` the per-NMU RoB ceiling (32 entries, `rob.hpp:80`) is the true limiter, and some
  resource upstream of the NSU pool binds first -- candidates are the `AxiMasterPort` per-channel queue
  (16, `wrap_defaults.hpp:12`), the router per-VC input depth (4), and the router inject credit. The
  shipped 32-entry pool already reaches the RoB-limited throughput, so 512 buys nothing. 0.0% means the
  pool is not the bottleneck at 32, not that the knob does nothing.
- **`max_unique_ids` did not move the curve.** `1` and `256` are byte-identical at each VC, to the
  decimal, as a prior survey predicted (it constrains ordering, not throughput: FlooNoC
  `docs/floonoc/chimneys.md:50`, and the co-sim subordinate is a zero-wait `MAPPED` pulp `axi_rand_slave`
  whose uniform latency makes in-order same-ID return free). The headline runs at the shipped default `1`.

New backlog item: **`max_outstanding` as its own sweep axis.** It is an NI metadata buffer depth, hence
area. Sweeping it against VC count would show how the two trade off. Not this round; at
`uniform_random`/`_rob` the RoB ceiling masks it, so the axis needs a pattern whose RoB ceiling is 512
(`hotspot`) to be informative.

## Done — checked-traffic-benchmark (Stages 1-5 complete, merged + pushed to `main` 2026-07-07)

Rebuild the regression/benchmark on the pulp VIP. Spec:
`docs/superpowers/specs/2026-07-04-checked-traffic-benchmark-design.md` (read it — all decisions + the
Stages table live there). Two-checker model: **directed(`axi_file_master`)→`axi_scoreboard`(data integrity)**,
**random(`axi_rand_master`)→`axi_reorder_compare`(transport)**. Per-stage, subagent-driven, spike-first.

| stage | status | action item (see spec Stages table for success criteria) |
|---|---|---|
| 1 scoreboard 2-state spike | **DONE** | scoreboard usable on Verilator directed axis (clean 0 warn / fault 8× warn); D6 resolved, no VCS fallback |
| 2 emitter | **DONE** (`fcdfbe4..75f1549`) | `gen_test_patterns --format file_master`; add-only; 55 test green; plan `2026-07-04-benchmark-stage2-emitter.md` |
| 3 file_master path | **DONE** (`334387f..bd884be`) | `TB_DIRECTED` endpoint flavor (file_master + in-endpoint `axi_scoreboard` on master face + per-node two-phase); `gen_tb_top` `ifdef TB_DIRECTED` guard (reorder_compare out, cmp_eos kept unconditional so watchdog compiles); `RUN_CLASS=directed` + `run-directed` recipe w/ fail-loud RUN_CLASS guard; new `mesh_1x1_vc1.yaml`. Verified: fault-injection fires `Unexpected RData`, then 1x1 + 4x4 all-4-pattern scoreboard clean, 16-node non-vacuous, 0 %Error. plan `2026-07-04-benchmark-stage3-file-master-path.md` |
| 4 rand conformance | **DONE** (`aead67e..ddc7ac7`) | 2-flavor rename (`directed`/`constrained_random`, retire `data_integrity`/`TB_TRANSPORT_RUN`); `run-constrained-random` (rand_master WRAP/EXC + tb-level `reorder_compare`, `%Error` gate, fault-injection verified first); root `make sim TB=tb_<topo> PATTERN=<pat> [SEED=]` unified launcher (recursive make to `run-directed`/`run-constrained-random`, SEED unset draws + records a random seed). Verified: 4x4 all 5 axes (4 directed patterns + constrained_random) + no-SEED random-seed run pass through the new entry point |
| 5 harness + deletes | **DONE (delete, not rewrite)** | Stage-5 matrix-harness rewrite dropped as YAGNI: `make sim TB= PATTERN= [SEED=]` already runs any single config; no live need for a batch driver. Retired the whole old regress subsystem instead — deleted `run_benchmark.py`, `sim/regress/` (`run_regress.py`, `matrix.yaml`, tests, README), and the `gen_test_patterns` legacy `--from`/yaml emitter path + its tests (`gen_test_patterns.py` is now a file_master-only generator; `sim/verilator/Makefile` drops `--format file_master`). Removed the `make check` target entirely (user: not needed — no CI enforces it anyway) + `sim-regress` + `make help`/`.PHONY` lines; pre-commit gate converged to `make test` (ctest; lint via `make lint_scenarios lint_docs`), reconciled in `docs/development.md` + `docs/architecture.md`. Verified: `pytest sim/tools/` green, `make sim TB=tb_mesh_4x4_vc1 PATTERN=neighbor` DIRECTED PASS (scoreboard clean, 16-node non-vacuous). KEPT `sim/test_patterns/AX4-*` (ctest consumes them; retirement tracked in Infra section). If a batch/matrix runner is ever wanted, build it fresh against `make sim`. |

Checked-traffic-benchmark work COMPLETE (Stages 1-5). Run interface: **`make sim TB=tb_<topo> PATTERN=<pat> [SEED=]`** — PATTERN picks the axis (`neighbor/transpose/uniform_random/hotspot`=directed, `constrained_random`=CR); SEED unset draws+records a random seed. Env on WSL/Linux (Verilator 5.048 + z3): gitignored repo-root `local.mk` with `BUILD_ROOT := $(HOME)/noc_build` + `PYTHON3 := python3` + `VERILATOR := verilator` (the `/mnt/e/.../build` tree is Windows-COFF, WSL `ld` rejects it). yaml-cpp built once under `$HOME/noc_build` (see [[feedback_run_on_wsl_linux]]). Emitter tests run on Windows `py -3` too. Merged + pushed to `main` 2026-07-07 (through commit `7e5d66d`); the Stage-5 cleanup retired the whole legacy regress/run_benchmark subsystem.

Same push (2026-07-07) also landed a **ctest audit + prune** (branch `chore/ctest-prune`, memory `project_ctest_audit_prune`): 5-agent audit of all 54 test files, then trimmed low-value / co-sim-redundant tests and root-caused a `-j` flakiness (fixed shared temp paths -> unique per pid+test via new `tests/common/tmp_path.hpp`). Result: ctest 497 -> 385, parallel-safe (`ctest -j8 --repeat until-fail:2` green). The multi-hour "hang" scare was `cmake --build -j`(all cores) thrashing WSL, not a test deadlock. Fine-grained in-file mirror cuts deferred (list in memory).

## Done — hygiene sweep (2026-07-07/08, merged + pushed to `main` through `5b0acbf`)

Between-rounds cleanup, no new feature:
- **Saturation sweep productized**: `make sim-saturation` runs the VC-value sweep (run-traffic on vc1/2/4/8 at `ids_per_tile=16`, `inj=1.0`) -> `collect_saturation.py` CSV + `plot_saturation.py` table. Recorded result: vc1=1248 vc2=1710 vc4=1916 vc8=1935 bits/cyc (**+53% vc1->vc4**, plateau by vc8).
- **ctest decoupled from `sim/test_patterns/`** (`35e5bdc`): deleted all 25 `AX4-*` dirs + `scenarios_list.hpp` manifest + `noc_axi4_scenarios` lib + `RequireKnownScenario` registry + `SCENARIO_TREE_ROOT`; the one surviving fixture (ORD-003) is inlined into its test. ctest now has zero external-scenario dependency (381/381).
- **Dead perf path removed**: `axi_slots`/`latency` (txn-perf DPI never wired) dropped; perf.json is noc-only (routers + links, which are live).
- **AI-ism removals** (user: no real project ships these): the doc-ASCII `lint_docs` linter + `lint_scenarios` + their make targets. Emptied root `tools/` -> `sim/tools/` is the only tools dir. See [[feedback_reject_ai_invented_mechanisms]].
- **Dead files + doc staleness**: dead headers (`ni_spec.hpp`, `master/slave_wrap_io.hpp`), stale `perf_baseline/*.json`, 17 stray root `master_wrap_read_dump*.txt`; `make clean` now sweeps generated stimulus; all live-doc refs to removed machinery (`make check`, `run-tb-top SCENARIO=`, `test_integration`, AX4 GLOB) reconciled.

## NEXT SESSION -> design round

Hygiene is done; the next action is a **design round** (brainstorm-first, per [[feedback_superpowers_entry]]). Pick ONE from the Design rounds table below:
- **SAM remap** — largest: reworks address gen + NMU/NSU, needs an AMBA System Address Map / FlooNoC survey.
- **NoC-layer QoS** — medium: QoS-aware VC arbitration + mapping (`NOC_QOS_WIDTH=0`, `noc_qos` zero-filled today).
- **non-4x4 topology** — smallest: a non-square YAML smoke to prove the generator path.

## Next round — ranked (set 2026-07-03, after the VIP cutover round)

0. ~~**Load-dependent DUT deadlock under random traffic**~~ **RESOLVED 2026-07-04 — NMU
   request-path HOL, not a fabric bug.** Root cause: `NmuReqS1Bridge::tick` (`nmu.hpp` old line 78,
   `if (s1_aw_.full()) return;`) head-of-line-blocked W and AR whenever the AW slot could not drain.
   Combined with the WormholeArbiter AW->W pairing lock (needs the W body to release, then drain the
   AW input), a full depth-4 AW input self-deadlocked: W blocked -> lock never releases -> AW input
   stays full. The read AR was collateral (HOL-blocked behind the same full AW), so R never returned.
   Load-dependent because the AW input only fills at >=4 outstanding writes. NOT a network wormhole
   deadlock: the forensic dump (`run_8r8w_s1.log`) showed 5 NMUs internally wedged with
   `req_credit_avail=1` (network had space). Diagnosed via a fabric-state dump + 4 parallel hypothesis
   analyses + Codex cross-check against FlooNoC. Fix (spec `docs/superpowers/specs/2026-07-04-nmu-request-hol-fix-design.md`,
   commit `0a50480`): independent per-channel bridge drain + `Packetize::push_w` backpressure on empty
   `w_meta_fifo_` (AW-before-W preserved by the meta FIFO, FlooNoC-aligned SelAw/SelW equivalent).
   Verified: deterministic ctest deadlock-repro (2/2), full ctest 497/497, and the exact prior repro
   `8R/8W seed 1` co-sim now PASS (all 16 nodes done, non-vacuous, zero %Error/dump).
0b. **Verilator+z3 wall-time budget** — measured ~34 sim-cycles/s under random stimulus (z3 89% CPU;
   every randomize round-trips the solver pipe). Matrix/smoke loads must budget for this: local WSL
   gate = 2R/2W-scale; heavy seeded runs → VCS (native solver). Affects Task 8 matrix sizing.
0c. **pulp `axi_scoreboard` data-integrity axis on VCS** — withdrawn from Verilator flow (2-state X
   collapse, spec `2a2db6c`); re-enable on the 4-state workstation to restore write→readback checking
   and all-to-all checked runs.
   **UPDATE 2026-07-04 (branch `feat/checked-traffic-benchmark`, spike):** the 2-state withdrawal is
   too broad. A standalone Verilator spike (file_master two-phase + rand_slave MAPPED + scoreboard on
   one bus) showed the scoreboard does NOT false-report on a **directed full-readback** (clean read of
   a written address = 0 warning; fault read of an unwritten `0x2000` = 8× `Unexpected RData`, so the
   checker is live and the X-collapse only bites unwritten-byte reads). → the **directed data axis runs
   on Verilator/WSL, no VCS needed**. Only all-to-all *random* data-integrity still needs VCS (out of
   the new benchmark's scope; random uses reorder_compare instead). See spec
   `docs/superpowers/specs/2026-07-04-checked-traffic-benchmark-design.md`.

## Previous round — ranked (set 2026-07-01, after the ultra dead-code sweep)

1. ~~**Triage `test_router_loopback` `BidirectionalZeroMismatch/vc4,vc8`**~~ **RESOLVED 2026-07-01 —
   stale build artifact, not a router VC bug.** The failing set was 24 ctest cases (not just
   router_loopback), all `bad file: .../sim/test_patterns/AX4-{STR-002,BND-003,BND-005,STR-003,
   BAS-001_single_write_no_read}/scenario.yaml` — scenario dirs the 2026-06-30 AX4 prune renamed. Test
   SOURCE already carries the new names (`STR-001`/`BND-002`/`BND-004`/`ORD-003`); only the `build/cmodel`
   `.exe`s were stale (compiled pre-prune, old id string baked in — which is why `RequireKnownScenario`
   did not fire: a stale binary's own known-list is stale too). A clean `ninja -k 0` rebuild → **ctest
   499/499 pass**, all router_loopback vc1/2/4/8 green. No source fix; the rename was already on `main`
   (git clean). The "reproduces on a fresh rebuild" claim did not hold up — a real fresh rebuild passes.
2. **GCC ICE on `test_pins_smoke.cpp`** — **did NOT recur 2026-07-01**: the `ninja -k 0` rebuild
   recompiled `test_pins_smoke.cpp.obj` cleanly (mtime today) and it ran inside the 499/499. Downgrade to
   intermittent (toolchain flakiness, likely memory pressure), not a hard `make check` block. Still worth
   a toolchain bump if it returns. (Infra section.)
3. ~~**Injection-rate / saturation sweep**~~ **DONE 2026-07-07** (branch `feat/saturation-throughput-sweep`,
   spec `docs/superpowers/specs/2026-07-07-injection-rate-sweep-design.md`, plan
   `docs/superpowers/plans/2026-07-07-saturation-throughput-sweep.md`). Directed path, z3-free. Ported
   FlooNoC's method (per-cycle injection at `+traffic_inj_ratio`, reuse the existing per-node
   `axi_bw_monitor`), booksim2-informed (saturation throughput, not a bus-tap latency curve which cannot
   see saturation). Two bring-up findings were the crux: (a) the `rand_slave` default AX_MAX_WAIT=100
   throttled responses (util 1.2%) -> set to zero-wait ideal sink; (b) single AXI id serialized -> new
   `gen_test_patterns --ids-per-tile` gives each tile a non-overlapping id block (concurrency knob, NOT
   VC spread -- VC is id-agnostic). Result (4x4 uniform, ids_per_tile=16, inj=1.0): saturation
   throughput vc1=1248 vc2=1710 vc4=1916 vc8=1935 bits/cyc -> +53% vc1->vc4, plateau by vc8; latency
   169->98. VC value is now measurable. `sim/tools/collect_saturation.py` + `plot_saturation.py` render
   the per-VC table. Note: c_model `perf.json` records 0 bytes in traffic mode (perf DPI not wired for
   it) -> bw_monitor is the sole readout, cross-check deferred (below).
4. **Cleanup leftovers** (low priority, own refactor round): the ultra-sweep SKIPPED items —
   `cpp_params`/`sv_params` dedup, `constants.py` eval-swap, `_load_topology` 4-way consolidation — plus
   the remaining Dead-code-prune candidates not yet touched. Deletion<addition; do only if a round wants it.

Design-round candidates (SAM remap, multi-id stimulus, NoC-layer QoS, non-4x4 topology) remain in the
Design rounds section; none is urgent.

## Bugs

### FIXED 2026-07-11 — `RobMode::Enabled` wedges the AR channel on a read burst longer than the RoB

`Rob::push_ar` (pre-bypass) returned `false` for any `len + 1 > ROB_IDX_SPACE` = 32 and had no other
path. `AxiSlavePort::forward_ar_to_packetizer_` (`axi_slave_port.hpp:153-158`) retries the same
`ar_q_.front()` and never pops it, so the oversized AR head-of-line-blocks every later AR, including
short ones on other AXI IDs. `arready` is driven from queue capacity alone (`nmu_wrap.hpp:188-190`),
so it drops once `ar_q_` fills behind the stuck head and never rises. AXI4 permits `len` up to 255
(`axi/protocol_rules.hpp:40`). Confirmed by inspection, Codex-verified; no upstream limit prevents
such an AR from being presented.

Recorded until now only as a matrix exclusion (`AX4-BUR-003`, `len 256`, "rob capacity") and as an
`EXPECT_FALSE` in `test_rob.cpp:278-293`. Neither names it a defect.

FlooNoC's 64-entry RoB accepts the same burst: `rob_free_space > ax_len_i` is consulted **only** on
the `ax_rob_req_o` path (`floo_rob.sv:336-355`), so a transaction that needs no reordering is admitted
at any length. Bypass clause 1 is the fix, not a bigger pool. Spec:
`docs/superpowers/specs/2026-07-10-nmu-rob-bypass-and-depth-design.md`.

**Verified on the wire (2026-07-11).** A 64-beat read burst (`BURST_LEN=63`, `--size 5` = 2048 B,
inside the 4 KB boundary) driven through Verilator co-sim, gated by the new `BURST_LEN` knob:

```
make sim TB=tb_mesh_4x4_vc1_rob PATTERN=neighbor SEED=1 BURST_LEN=63
```

- Pre-bypass (`rob.hpp` at the Task 6 "max_txns_per_id gates" commit): the run wedged. It loaded all
  16 nodes' stimulus, printed `[Config] max_unique_ids=1 max_outstanding=32`, then made no forward
  progress past time `[0]`; no `PASS` ever emitted and the 900 s `timeout` reaped it. The AR head of
  line never popped — exactly the wedge above.
- Post-bypass (HEAD): `PASS: all 16 nodes done, non-vacuous` →
  `DIRECTED PASS: directed_mesh_4x4_vc1_rob_neighbor_s1 scoreboard clean, non-vacuous` (rc=0). The
  64-beat AR drains. No regression at `BURST_LEN=0` across neighbor / transpose / uniform_random /
  hotspot.
- Timing evidence (independent hang confirmation): post-bypass burst completes in 24.2s; pre-bypass
  killed by a 300s `timeout` (>10x that) with no `PASS` line and no forward progress past sim time
  `[0]` — a hang, not slowness.

### Pre-existing fabric bugs (the matrix caught these, which is its purpose)

Each is excluded in `sim/regress/matrix.yaml` with a reason and re-included once the fabric bug is
fixed.

| id | symptom | suspected root cause | status |
|---|---|---|---|
| `AX4-ORD-002` | multi-id concurrent write hang (was flaky/clustered). | **FIXED 2026-07-01** — the residual "data mismatch" was the `WireSlavePort` AW-replay bug (below). Un-excluded; re-verified green (64 reads, 0 mismatch, 16-node concurrent). | FIXED, un-excluded |
| `AX4-BND-005` | 4KB-crossing burst at `0x0FE0` (`len:7`, `size:5`): read phase hung under 16-node load (4KB read-split). | **FIXED 2026-06-30** by the push_ar gate (same AR-drop class). Re-verified green under regression load; un-excluded. | FIXED, un-excluded |
| `AX4-BND-006` | same 4KB-boundary-edge class (2 writes + 2 reads spanning 0x1000). | **FIXED 2026-06-30** by the push_ar gate. Re-verified green under regression load; un-excluded. | FIXED, un-excluded |

The 4KB carriers renumbered in the 2026-06-30 prune: old `BND-006` (cross_4kb_auto_split) → `BND-005`, old `BND-007` (4kb_boundary_edges) → `BND-006` (see the prune entry below for the full old→new map).

### Root cause — WireSlavePort AW-replay (FIXED 2026-07-01)

Symptom: STR-001 / ORD-002 aborted on `B_FRONT_CAN_ACCEPT` (`axi_master.hpp` — a master drained a
B with an empty per-id deque). Localized by instrumented co-sim + a `[MST-ADMIT-W]` vs `[NMU-AW-IN]`
correlation: masters ADMIT all N distinct writes (128 = 16 nodes x 8 distinct id/addr), but each
NMU ingress saw only ONE of them (the highest id) replayed N times — ids 1..7 lost, id 8 replayed.

`detail::WireSlavePort::push_aw` latched `last_aw_ = b` BEFORE the `aw_delivered_this_tick_` gate.
`tick_push_aw_w_` walks the WHOLE `active_write_ops_` std::map (ascending id) every tick, so multiple
ids call push_aw in one tick. On the wait cycles (awready low, no delivery) the gate was never set,
so each later id overwrote `last_aw_` to the LAST id walked (id 8). The wire presented id 8 and
replayed it; the subordinate executed duplicate writes and returned extra B's → the assert. It is a
regression from the AR-drop fix (`9d218bb`), which added the delivered gate but left the latch on the
wrong side of it (the pre-gate code was also not beta-tick-correct — it marked all N delivered in one
tick — but did not create the replay).

FIX: gate at OFFER time — `if (X_offered_this_tick_) return false;` before latching, so the FIRST
(oldest) id's beat stays presented and later ids retry next tick. Applied to push_aw / push_w /
push_ar; flag renamed `*_delivered_this_tick_` → `*_offered_this_tick_`. Regression test
`c_model/tests/axi/test_wire_slave_port.cpp` (first offered wins while not ready; one delivery per
tick; progression to next id; AR channel). Verified: STR-001 green (128/128, 0 mismatch), ORD-002
green (un-excluded), BUR-003 fixed on 3/4 patterns (hotspot remains), `mesh_4x4_vc1` matrix
pass=49 fail=2 (was 48/3), existing unit tests green (test_axi_master 44, raw_order 2, integration 25).

The first full `make sim-regress` is a discovery run. Sweeping the curated set through the
concurrent 16-node fabric will surface more pre-existing co-sim bugs. Add each to `matrix.yaml`
exclusions with a reason as it is confirmed.

**Discovery run — `mesh_4x4_vc1`, 2026-06-30** (first end-to-end run after the build decouple; 74
cells executed, 64 pass / 10 fail). New fails beyond the excluded set:

| id | failing patterns | note | survives prune? |
|---|---|---|---|
| `AX4-BUR-003` | neighbor / uniform_random / transpose / hotspot (all 4, non-rob) | len 256; rob already excluded, non-rob fails on every pattern -> scenario-level fabric bug | yes (burst) |
| `AX4-HSH-001` | all 4 patterns | backpressure/retry, traffic-independent | deleted in prune (was ≡ ORD-002 stimulus) |
| `AX4-BUR-002` | hotspot only | other 3 pass -> hotspot congestion | yes (burst) |
| `AX4-STR-002`→`STR-001` | neighbor only | outstanding stress | yes (outstanding) |

After the prune the real-bug worklist was `BUR-003` (all patterns + rob exclusion), `BUR-002`@hotspot,
`STR-001`@neighbor, plus the then-excluded `ORD-002` hang. The `HSH-001` fails left with the HSH delete.
All of these are now RESOLVED (AW-replay + generator slot-overlap fixes; matrix `pass=400 fail=0`).

### Fabric-bug round — 2026-06-30. Worklist re-triaged into 3 distinct modes

Reproduced each item in isolation (single scenario replicated across 16 nodes). The four entries
collapse into three unrelated failure modes, not one fabric bug:

| id | actual mode | finding |
|---|---|---|
| `BUR-003`, `ORD-002` | **flaky cycle-deadlock — FIXED** | Was non-deterministic + CLUSTERED (isolated reruns pass 20-45x, then a regression-load burst fails the same binary). Root cause = the test master dropping read sub-burst ARs (below), not a fabric bug. Fixed; 0 hangs across repeated regressions + loops. |
| `STR-001` | gen-crash FIXED; residual data mismatch | The `gen_test_patterns alloc_unique_offset` overflow (8 unique addrs + `preserve_addr` over the `memory_size 0x1000` window) is fixed by enlarging the scenario `memory_size` to `0x4000`. With gen passing and the deadlock fixed, the cell no longer hangs but now fails a scoreboard DATA MISMATCH (8-outstanding multi-id) — folds into the residual data-mismatch worklist, not the deadlock. |
| `BUR-002`@hotspot | **scoreboard data mismatch, not a hang** | Fails fast with a readback mismatch under hotspot congestion. Independent of the deadlock. Now joined by `BUR-003`@hotspot (its hang was masking the same hotspot mismatch). Next round; may be nmu/nsu/router (apply FlooNoC cross-check). |

**ROOT CAUSE (FIXED) — test AXI master dropped read sub-burst ARs; NOT a fabric/router/uninit bug.**

Found by a timeout state dump (per-router FIFO/credit/wormhole-lock + per-master done/outstanding/
sub-burst progress). The dump is decisive:

- At every hang the ENTIRE router fabric is EMPTY: all input/output FIFOs empty, all per-VC credits
  full, zero wormhole locks, both REQ and RSP nets. So it is NOT a fabric wormhole/credit deadlock
  (refutes the first hypothesis). Nothing is in the network.
- Every master is stuck `done=0` with nothing in flight: `active_read=1`, and the stuck read shows
  `ar_sub=N/N r_sub=1` — the read split (4KB boundary) into N same-AXI-id sub-burst ARs, the master
  marked ALL N issued, but only 1 sub-burst's R-burst ever returned.

The drop site is `detail::WireSlavePort::push_ar` in `c_model/include/axi/axi_master.hpp`. `push_w`
has a `w_delivered_this_tick_` one-beat-per-tick gate (the registered SV wire transfers one beat per
clock); `push_ar` (and `push_aw`) lacked it. The read-side caller `push_reads_` loops over all
sub-burst ARs in one tick; when `arready` is high it consumes the whole loop — every sub-burst AR is
marked delivered while only ONE reached the wire. The other sub-bursts' ARs are silently dropped, so
their R-bursts never return and the read never completes. Flaky/clustered because it only fires on a
tick where `arready` is high while the loop runs; the write path was masked because `push_w`'s gate
serializes AWs indirectly.

The earlier `-ftrivial-auto-var-init` signal was a RED HERRING: zero/pattern fill only shifts the
arbitration timing (whether `arready` is high during the loop tick), modulating the trigger rate. It
was never an uninitialized-read bug. The `CMODEL_CXX_HARDENING` build flag was REMOVED once the real
fix landed (it fixed nothing).

**FIX:** add `ar_delivered_this_tick_` / `aw_delivered_this_tick_` gates to `push_ar` / `push_aw`,
reset in `set_arready` / `set_awready`, mirroring `push_w`. One-beat-per-tick on all three request
channels is the correct registered-wire model. Verified: 3× `make sim-regress BUILD=mesh_4x4_vc1`
with ZERO timeouts (was a reliable 5-hang cluster), plus 15× BUR-003 neighbor and 15× ORD-002 loops
with 0 hangs. Debugged with a timeout state dump (since removed) and a read-only Codex cross-check.

**RESIDUAL — data-mismatch worklist — ALL RESOLVED (see below).** After the AR-drop + AW-replay fixes
the hangs and the STR-001/ORD-002 B-assert are gone. The remaining scoreboard DATA MISMATCHES
(`BUR-002`/`BUR-003`@hotspot) were **NOT a fabric/nmu/nsu/router bug** — root cause below.

### Root cause — test-generator slot overlap (FIXED 2026-07-01)

`BUR-002`/`BUR-003`@hotspot readback was off by exactly one stride (0x40). Root cause = the test
pattern generator, not the fabric. `sim/tools/gen_test_patterns.py` `alloc_unique_offset` spaced slots
by a fixed `_SLOT_STRIDE=0x40` while a burst reserves `(len+1)*2**size` bytes (BUR-002=256B,
BUR-003=8192B). The offsets were distinct in VALUE but their FOOTPRINTS overlapped. Under many-to-one
(hotspot: many sources into one dst tile) neighbouring sources overwrote each other, so a readback
returned the neighbour slot's data — off by one stride. `neighbor` (bijection, one writer per tile)
passed because slots never shared a tile; hence VC-independent and pattern-specific.

Diagnosed from the captured `sim/regress/output/run_prune.log` mismatch report (`actual = expected -
0x40`, a clean monotonic whole-burst shift — overlap, not fabric corruption) plus the allocator code;
no sim re-run needed. The earlier "NSU MetaBuffer" suspicion was REFUTED: a read-only Codex pass showed
the C++ modeled path (depacketize/AxiMasterPort/AxiSlave) preserves same-id order, and a FlooNoC survey
placed our MetaBuffer at FlooNoC's `MaxUniqueIds==1` corner — a real LATENT hazard, but not this bug.

FIX (branch `fix/hotspot-slot-overlap`): `stride = max(stride, reserved)` in `alloc_unique_offset`;
both emit callers auto-grow `memory_size` to `n_nodes*n_seq*stride`. TDD (disjoint-footprint test);
the old fixed-window-overflow contract folded into a cross-node disjoint test. Verified: full
`make sim-regress` all 8 builds = **pass=400 fail=0** excluded=16 (only BUR-003 rob-capacity, legit)
skipped=32. BUR-002/003 hotspot + STR-001 green, zero regression. vc1 verilator rebuilt to confirm the
NSU `snapshot_*`→`allocate_*` rename compiles (GCC ICE still blocks only the ctest `test_pins_smoke`).

**Deferred (latent, not triggered):** the MetaBuffer per-id-FIFO src recovery (id-agnostic many-to-one)
was NOT unmasked by the fix (no new different-signature mismatch). FlooNoC `id_queue` / unique-slot
id-remap alignment stays a future hardening item, not needed for any current test. `run_regress.py`
`CAPACITY_SLOTS=4` cap is now loosenable (memory_size auto-grows).

**VERIFICATION SCOPE — full `make sim-regress` (all 8 builds) run 2026-07-01: pass=400 fail=0.**
vc1/2/4/8 × {disabled, enabled} all green (the `enabled` verilator exes exist and run; GCC ICE blocks
only the ctest build). The AR-drop / AW-replay / slot-overlap fixes are confirmed across every build.

### ~~Confirmed design bug — per-id VC binding~~ RESOLVED 2026-06-30

Fixed on branch `feat/vc-id-agnostic` (merged to main local, commits `253b744..e39d8bd`; spec
`docs/superpowers/specs/2026-06-30-vc-binding-removal-design.md`, plan `docs/superpowers/plans/2026-06-30-vc-id-agnostic.md`).
Root-cause confirmed: the cited "spec §8" does not exist (architecture.md ends at §7); the NMU pin was a
self-added cross-transaction mechanism. RTL-verified FlooNoC alignment (floo_vc_arbiter round-robin,
floo_rob arrival-offset, R bursts on one VC). Outcome: VC selection id-agnostic round-robin by class;
same-id ordering = Enabled RoB (rob_idx) / Disabled single-outstanding interlock; NSU R-burst follow
kept (renamed `r_burst_vc_`, burst coherence not an id pin); unit-test-only MultiCandidate mode deleted.
ctest 522/522. Codex final review: 0 correctness findings.

**NOT yet verified — moved to next round (pattern/co-sim):** the `ORD-002` hang hypothesis (binding was
the root cause) is UNPROVEN — this round ran unit tests only, no co-sim, no traffic pattern. Re-run
`ORD-002` co-sim is the first verification step of the next prune→regress round (sequence unchanged:
prune scenarios -> re-run regression -> triage). Fix H1 (below) before that run or every `*×hotspot`
cell errors.

**Follow-up (#5, pre-existing, not introduced this round):** NSU `VcArbiter` still carries `use_pools_`
+ scalar `write_rsp_vc_`/`read_rsp_vc_` + a `!use_pools_` branch; NMU collapsed scalar into a size-1
vector pool (no flag, no branch). Mirror NMU on the NSU side to remove the asymmetry. Shrink, ~-20 lines.

### Matrix harness bugs (codex-found)

| id | location | symptom | fix |
|---|---|---|---|
| ~~H1~~ DONE | `run_regress.py:149` | hotspot cells errored (no `--hotspot`). | Landed: `run_cell` passes `_interior_hotspot(topology)` when `pattern == hotspot`. |
| H2 | `sim/tools/gen_test_patterns.py:701-702` | `preserve_addr` is ignored on the `uniform_random` / `hotspot` paths (they call `_emit_base_driven_node` without the preserve option). The deterministic paths honor it at `:630` and `:657`. Dormant today because the AX4 group runs only on `neighbor`. | Honor `preserve_addr` on all paths, or keep the address-agnostic group on default reallocation (which needs no preserve) and document the constraint. |
| H3 | `sim/regress/run_regress.py:62-69` (`is_excluded`) | Exclusion matches only `from` / `pattern` / `topology` / `rob_mode`. A `from` scenario shared by a preserve group and a non-preserve group makes one exclusion hit both. | The full-cross split puts each `from` in one group, so this stays dormant. Add `preserve_addr` to the `when` key only if a scenario is ever shared. |

## AX4 scenario prune — DONE 2026-06-30

Spec `docs/superpowers/specs/2026-06-30-ax4-scenario-prune-design.md`, plan
`docs/superpowers/plans/2026-06-30-ax4-scenario-prune.md`, branch `feat/ax4-scenario-prune`. Supersedes
the earlier strict-15 plan and the full-cross plan (the full cross — `all_independent_ax4` ×4 patterns /
`all_dependent_ax4` ×neighbor preserve, classified by `metadata.address_mode` — already landed in
`matrix.yaml`; this prune shrinks the scenario set feeding it).

**Method: hybrid standard ∩ marginal value.** Surveyed standard AXI4 coverage (AMBA AXI4 spec +
tim_axi4_vip / cocotbext-axi / OSVVM). Kept every cited standard point that adds marginal stimulus
value; cut duplicates and non-AXI scenarios. Key correction over the strict-15 plan: BND
(narrow/unaligned/4KB) and EXC (exclusive) are textbook wire-verifiable coverage and were **kept**, not
cut.

**Deleted 12** (stimulus duplicates / non-AXI / within-family redundant): `BAS-001` (write-only),
`BAS-002` (default-fill), `BAS-004` (≡BAS-003), `BUR-007` (≡BUR-005), `BUR-008/009` (aligned WRAP never
wraps), `BND-002` (⊂BND-003), `EXC-004` (excl+WRAP marginal), `HSH-001` (≡ORD-002 stimulus), `HSH-002`
(≡BAS-003), `STR-001` (≡BAS-003), `INF-001` (non-AXI). HSH/STR-001 differed only in `write_latency` /
`read_latency` — a **slave-model knob, not AXI stimulus** (`scenario→SlaveWrap→axi::Memory` response
delay), so as patterns they are duplicates.

**Reclassified:** `STR-003` multi_dst_stress → `ORD-003_same_id_multi_dst` (`category: ordering`; only
same-id-different-dst ordering case).

**Renumber (gap-free) old→new:** `BAS-003`→`BAS-001`, `BAS-005`→`BAS-002`; `BND-003`→`BND-002`,
`BND-004`→`BND-003`, `BND-005`→`BND-004`, `BND-006`→`BND-005`, `BND-007`→`BND-006`; `STR-002`→`STR-001`.
`BND-001`, `BUR-001..006`, `EXC-001..003`, `ORD-001/002`, `QOS-001`, `RSP-001..003` unchanged.

**Result:** 25 dirs = 21 wire matrix + 4 Layer-2 (`QOS-001`, `RSP-001..003`, kept on disk, auto-skipped
by `is_self_checking`).

**Verification — `make sim-regress BUILD=mesh_4x4_vc1` (2026-06-30):** `pass=43 fail=6` (run=49). All 6
fails were pre-existing fabric/harness bugs already in the discovery table — `BUR-003` (all 4 patterns,
non-rob), `BUR-002`@hotspot, `STR-001`@neighbor — no new fail from the prune/renumber, and the
`HSH-001` noise is gone. All 6 were subsequently FIXED (STR-001 by the AW-replay fix; BUR-002/003 by
the generator slot-overlap fix, both above); the full 8-build matrix is now `pass=400 fail=0`.

**ORD-002 hang hypothesis — REFUTED 2026-06-30 (historical).** The VC-binding-removal round
([[project_vc_id_agnostic_landed]]) left open whether removing the per-id VC binding fixed the ORD-002
hang. Un-excluding ORD-002 and running one cell still hung — so the binding was NOT the cause. That
put a 4-item worklist (`BUR-003`, `BUR-002`@hotspot, `STR-001`@neighbor, `ORD-002`) on the next round.
**All 4 are now RESOLVED, and none was where this note guessed** (not VC binding, not a RAW-release /
NSU per-id path): the ORD-002 / STR-001 hangs were the `WireSlavePort` AR-drop + AW-replay bugs (test
master), and the BUR-002/003 mismatches were the generator slot overlap. See the Bugs section above.

## Verification methodology gaps

| item | summary |
|---|---|
| injection-rate / saturation sweep | **DONE 2026-07-07** (ranked #3 above): saturation throughput per VC on `make sim` run-traffic (`+traffic_inj_ratio`, `--ids-per-tile`), directed/z3-free. vc1->vc4 +53%. Deferred follow-ups: (1) c_model perf DPI not wired for traffic mode (`perf.json`=0 bytes) so the bw_monitor vs perf.json cross-check (spec Verify #2) is unrun -- wire it or validate bw_monitor against a hand-computed single-stream case; (2) full `+traffic_inj_ratio` low-to-high curve (only the saturation point at inj=1.0 was run; the concurrency axis `ids_per_tile` showed the plateau instead); (3) plot_saturation.py minors (cwd-relative path, vc1 baseline by insertion order). |
| coverage + CRV + wire-side SVA | The matrix gates on the scoreboard only and skips non-wire-verifiable response/write-only cases (`sim/regress/README.md:17-23`, `run_regress.py:80-89`). No covergroup, no constrained-random framework, no wire-side protocol assertions. Make it actionable: a coverage plan plus co-sim scenario-coverage accounting (how many AX4 actually run at co-sim), not a vague bucket. |
| slave-latency testbench axis | Slave-side backpressure coverage (subordinate not ready / response stall) belongs as a matrix axis sweeping a base scenario's `write_latency`/`read_latency` (analogous to `rob_modes`), not duplicate scenario files. The 2026-06-30 prune deleted HSH-001/002 + STR-001, which encoded backpressure only via this slave-model knob. Add the axis if slave-backpressure coverage is wanted; do not reintroduce duplicate scenarios. |

## Design rounds (broader)

| item | summary |
|---|---|
| SAM remap | The NI has a second memory-mapping mechanism (dst via SAM table lookup, `local_addr = addr - base`) per `addr_trans.hpp:7-8,25,28-30` and spec sec 4.3. Today `local_addr = addr` (decode, `:25`). Under remap, stimulus address generation changes from "synthesize dst into the high bits" to "produce flat SAM-mapped addresses, dst by lookup". Reworks `gen_test_patterns`, preserve_addr, and NMU/NSU. Own survey (AMBA System Address Map / FlooNoC) plus brainstorm round. |
| per-id VC binding re-eval | RESOLVED 2026-06-30: confirmed a design bug, promoted to Bugs (per-id VC binding, fix first next round). Survey (IHI 0022 + FlooNoC) basis kept there. |
| ~~multi-id stimulus~~ **DONE 2026-07-07** | `gen_test_patterns --ids-per-tile` gives each tile a non-overlapping AXI id block (added in the injection-rate sweep round, commit `93ff917`). CORRECTION to this item's old rationale: the purpose is **injection concurrency** (a single id + same-id ordering caps outstanding, so the fabric never loads), **NOT VC-spread** -- VC allocation is id-agnostic (by VC id only), per user (memory `reference_vc_alloc_not_axi_id`). The prior "vc4/vc8 VC-spread under-exercised" framing was wrong. |
| NoC-layer QoS | AXI QoS passthrough is done (`packetize.hpp:117,165`, `depacketize.hpp:94,119`) and the NSU has response VC pools plus per-id binding (`nsu/vc_arbiter.hpp:50-56,113-196`). What remains is NoC-layer QoS arbitration and mapping: `NOC_QOS_WIDTH=0` (`ni_flit_pkg.sv:23-24`), `noc_qos` is zero-filled (`docs/architecture.md:66,133`). |
| non-4x4 topology | The generator path already builds `x_dim*y_dim` nodes from YAML (`gen_tb_top.py:21-28,108-122,214-242`) and `router_wrap` exposes 5 link ports (`router_wrap.sv:43-76`). Only 4x4 YAMLs exist (`sim/topologies/mesh_4x4_vc*.yaml`). Add a non-4x4 YAML smoke to prove the generator path on a different shape. |

## Infra / portability

The VCS regression path is documented as Linux-workstation and dry-run pending a real run
(`docs/development.md:227-234`, `sim/vcs/Makefile:8-15`). The matrix is Verilator-only by design
(`docs/superpowers/specs/2026-06-27-regression-matrix-design.md:174`).

- ~~**Decouple C++ ctest from `sim/test_patterns/*/scenario.yaml`**~~ **DONE 2026-07-07** (commit `35e5bdc`).
  The ctest-prune round had already deleted the integration/loopback tests that replayed AX4 YAMLs; only
  `test_request_response_loopback` still read one scenario (`AX4-ORD-003`). Codex + GoogleTest-primer
  survey confirmed self-contained fixtures are idiomatic (a globbed manifest + runtime known-id registry
  is not). Removed all 25 `sim/test_patterns/AX4-*/` dirs (~4.5 MB), the generated `scenarios_list.hpp`
  manifest, the `noc_axi4_scenarios` INTERFACE lib, `scenario_helpers.hpp` (RequireKnownScenario), and the
  `SCENARIO_TREE_ROOT` compile-def; inlined ORD-003 into its test (temp scenario.yaml + data.txt via
  `common/tmp_path.hpp`, loaded through the real `axi::load_scenario`). ctest now has zero
  `sim/test_patterns/` dependency; the dir stays only as the co-sim generated-stimulus location. Verified
  ctest 381/381.

- **GCC ICE on `test_pins_smoke.cpp`** (pre-existing, Windows host): GCC internal compiler error
  (segfault) when compiling the `build-cmodel` CMake target on this toolchain. Breaks any path
  running `make build` or `make test` (ctest gate). The co-sim Verilator binary is unaffected
  (c_model is header-only; build directly via `make -C sim/verilator`). Investigate toolchain upgrade
  or workaround.

- **`test_router_loopback` triage — RESOLVED 2026-07-01: stale build artifact.** The real failing set
  was 24 ctest cases (Packetize/PortPair/VcArb/DpiLifecycle loopbacks + router_loopback), all `bad file`
  on pruned scenario paths (`AX4-STR-002`/`BND-003`/`BND-005`/`STR-003`/`BAS-001_single_write_no_read`).
  Source already had the post-prune names; only `build/cmodel` binaries were pre-prune. Clean `ninja -k 0`
  rebuild → ctest 499/499. Not a router VC bug. See ranked-list #1 above.

## Dead-code prune candidates (ponytail-audit, 2026-06-30)

Audit ran read-only, no working-tree change. Net ~-1606 lines, 0 dependencies removable. Zero-caller
claims NOT independently verified — confirm each against existing code before deleting (CLAUDE.md
verify-before-change). Lowest-risk first batch = orphans that die with their own self-test (zero
behavior change). Pick a batch and open a round; do not bulk-delete.

**Re-audit + first execution — 2026-07-01.** Re-ran `/ponytail-audit` (4 parallel read-only agents,
one per tree) to grep-verify every zero-caller claim against current code. Corrected estimate: ~-1367
lines (not -1606), 0 deps. Two claims REFUTED: the `gen_test_patterns.py` synthetic path is LIVE
(`test_gen_test_patterns.py` calls it 5× without `--from`, ~-75 was wrong), and `report.py` is -40 not
-190. `exceptions.py` is also NOT over-built (all 5 classes raised/caught) — struck from the specgen row.
**Executed this round — tests batch + docs batch (~-540 lines):** deleted `router_path`/`isolated_scenario`/
`component_dwell_observer` (+tests), `test_two_node_fabric_at.cpp` + the `req_router_at`/`rsp_router_at`
accessors, `test_ni_stage.cpp` (EnumValuesAreDistinct trivial), the two `*StructsAreConstructible`
blocks in `test_scaffold.cpp`, `perf-probe-report.md`, `build_perf_probe_slides.py`. CMake registrations
+ `docs/issue/ARCHITECTURE.md` external-doc tree updated.

**Second execution (ponytail ultra) — 2026-07-01, sim + specgen + c_model batches (~-330 more lines).**
Deleted: `check_perf_parity.py` + its test; `gen_tb_top.py --check` drift gate (+ dead `difflib`
import, docstring, stale `check_perf_parity` comment); specgen `report.py` (+ `__init__` re-exports);
3 zero-caller invariant checkers (`check_signals_pin_uniqueness` + its `_check_pin_unique` helper,
`check_blocks_related_features_symmetric`, `check_protocol_rules_id_uniqueness`); c_model 10 `peek_aw/w/
ar/b/r` methods (nmu `axi_slave_port` + nsu `axi_master_port`, intended `AxiDpiAdapter` never built),
`detail::NullSlavePort` (AxiMasterStandalone owns WireSlavePort — 3 stale comments fixed),
`check_b_one_response_per_write` (+ its death test; the wired `check_b_front_can_accept_response`
subsumes it), dead config fields `NmuConfig::ni_req_extra_depth` + `NsuConfig::{ni_req,ni_rsp}_extra_depth`
(`NmuConfig::ni_rsp_extra_depth` is LIVE — kept). Merged `is_excluded`/`is_xfail` → one `_match`
(4 call sites); inlined `cpp_signals._to_pascal`. Fixed a **pre-existing** stale test
(`test_bnd007_excluded` asserted BND-006 excluded, but it was un-excluded 2026-06-30 → retargeted to the
live BUR-003 rob exclusion). **Verified:** specgen pytest 158 pass (1 pre-existing `jsonschema`-missing
env fail); run_regress 12 pass; 10 affected ctest targets pass (111 tests); co-sim `mesh_4x4_vc1`
build + run = scoreboard clean. Generated-file timestamp churn restored (confirms `_to_pascal` inline is
byte-identical output).

**Deliberately SKIPPED in the ultra sweep (deletion < addition, or risk > reward):**
`cpp_params.py`/`sv_params.py` shared-emitter dedup (-26) and `constants.py` `_eval_ast`→`eval` (-9) both
touch codegen output — drift risk over the specgen gate outweighs the lines, and the `eval` swap trades
an ast-walk for a flimsier `eval`. `_load_topology` 4-way consolidation (-30) is a new shared abstraction
across 4 files with 4 different return shapes — addition, not deletion. Left for a dedicated refactor
round if wanted. Remaining `check_b`/peek were the last c_model batch; the deferred latent MetaBuffer
id-remap hardening (Bugs section) is untouched.

| area | candidate | kind | ~lines |
|---|---|---|---|
| tests | ~~`router_path.hpp`/`isolated_scenario.hpp`/`component_dwell_observer.hpp` + tests, `test_two_node_fabric_at.cpp` + `req_router_at`/`rsp_router_at`, `test_ni_stage.cpp`, 2 `*StructsAreConstructible` blocks~~ | DONE 2026-07-01 | -470 |
| docs | ~~`perf-probe-report.md` (dup of `performance-probe.md`), `build_perf_probe_slides.py` (unrunnable)~~ | DONE 2026-07-01 | -535 |
| sim | ~~`check_perf_parity.py` + test (no Makefile/CI ref; no golden JSON existed)~~ | DONE 2026-07-01 | -170 |
| sim | ~~`gen_test_patterns.py` synthetic-scenario dead path~~ REFUTED 2026-07-01: LIVE, `test_gen_test_patterns.py` calls it 5× without `--from` | keep | 0 |
| sim | ~~`gen_tb_top.py --check` drift gate~~ | DONE 2026-07-01 | -24 |
| sim | ~~`run_regress.py` `is_excluded`/`is_xfail` -> one `_match`~~ | DONE 2026-07-01 | -18 |
| sim | `_load_topology` 4-way consolidation (4 files, 4 return shapes) | SKIPPED (addition>deletion; own refactor round) | ~-30 |
| specgen | ~~`ni_spec/report.py` (0 caller)~~ | DONE 2026-07-01 | -40 |
| specgen | ~~3 zero-caller invariant checkers (pin-uniqueness / related-features-symmetric / id-uniqueness)~~ + `_check_pin_unique` helper | DONE 2026-07-01 | -59 |
| specgen | ~~`cpp_signals._to_pascal` inline~~ DONE 2026-07-01 (-6). `cpp_params`/`sv_params` dedup + `constants.py` eval-swap SKIPPED (codegen drift risk; `eval` flimsier than ast). `exceptions.py` REFUTED (keep) | shrink | -6 |
| c_model | ~~VcArbiter `MultiCandidate`~~ DONE (deleted on `feat/vc-id-agnostic`, commit `253b744`) | yagni | ~-130 (headers) |
| c_model | ~~`peek_aw/w/ar/b/r` (10 methods; AxiDpiAdapter never built)~~ | DONE 2026-07-01 | -46 |
| c_model | ~~`detail::NullSlavePort`, `check_b_one_response_per_write` (+test), dead config `ni_req_extra_depth`×2 + NSU `ni_rsp_extra_depth`~~ | DONE 2026-07-01 | -35 |

Correctly excluded as live (do not touch): `RobMode::Enabled`, `IMemoryPort` (2 impl), `PerfCollector`,
fault injection. Cross-repo theme: dead code kept alive only by its own self-test.

## Cosmetic / cheap (defer)

M-items from the matrix final review: `preserve_addr=True` still runs the ignored `pair_offset` loop,
test inline imports, `gen_tb_top.emit_tb_top(requested_name="")` default is a silent-failure trap,
`is_excluded` KeyError on an unknown `when` key, prebuild `check=True` aborts the whole run instead of
failing one cell. JUnit XML reporting waits until a CI consumer exists.
(Closed in the coverage round: `run_regress` `PASS_MARKER` and `_ax4_curated` orphans deleted, commit
`aa235f5`.)

M-items from the coverage-round final review (none gate merge): ~~`is_xfail`/`is_excluded` verbatim
duplicates -> one `_match`~~ DONE 2026-07-01 (ultra sweep); `gen_test_patterns._rewrite_ids` does `t["addr"]` unguarded while
`unique_addr_count` guards with `if "addr" in t` -> mirror the guard (a base scenario with an addr-less
transaction would `KeyError`); `test_run_regress.py` `import pathlib as _pl` sits after the test
functions and is a redundant alias of the top-level `pathlib` import -> hoist and drop the alias;
`_ax4_by_address_mode` parses `unique_addr_count` twice on the raise path -> use a local var; the
`effective_topology()` `_rob`-append branch is only indirectly covered after `test_rob_topology_suffix`
was dropped -> one assert restores it; BND-007 wording differs between the `docs/backlog.md` row
("excluded (matrix.yaml)") and the `matrix.yaml` reason ("re-check on first full run") -> align if the
file is touched.
