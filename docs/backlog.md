# NoC backlog

Running action items and open bugs, maintained across iteration rounds. Each round adds what it
surfaces and strikes what it closes. Read it at session start. An item is not started unless a round
picks it up.

## Bugs

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
| injection-rate / saturation sweep | The benchmark runner measures one operating point and labels itself `greedy-finite-trace-stress`, "single operating point, no injection-rate sweep" (`sim/tools/run_benchmark.py:16-18,84-86,118`). A latency-vs-offered-load sweep is the measurement that exposes VC-count differences. Today vc1..vc8 latency reads flat because no sweep applies congestion. Needs an AxiMaster injection schedule driving the c_model interface. Recurs across the benchmark-generator, struct-refactor, and congestion-bugfix rounds. |
| coverage + CRV + wire-side SVA | The matrix gates on the scoreboard only and skips non-wire-verifiable response/write-only cases (`sim/regress/README.md:17-23`, `run_regress.py:80-89`). No covergroup, no constrained-random framework, no wire-side protocol assertions. Make it actionable: a coverage plan plus co-sim scenario-coverage accounting (how many AX4 actually run at co-sim), not a vague bucket. |
| slave-latency testbench axis | Slave-side backpressure coverage (subordinate not ready / response stall) belongs as a matrix axis sweeping a base scenario's `write_latency`/`read_latency` (analogous to `rob_modes`), not duplicate scenario files. The 2026-06-30 prune deleted HSH-001/002 + STR-001, which encoded backpressure only via this slave-model knob. Add the axis if slave-backpressure coverage is wanted; do not reintroduce duplicate scenarios. |

## Design rounds (broader)

| item | summary |
|---|---|
| SAM remap | The NI has a second memory-mapping mechanism (dst via SAM table lookup, `local_addr = addr - base`) per `addr_trans.hpp:7-8,25,28-30` and spec sec 4.3. Today `local_addr = addr` (decode, `:25`). Under remap, stimulus address generation changes from "synthesize dst into the high bits" to "produce flat SAM-mapped addresses, dst by lookup". Reworks `gen_test_patterns`, preserve_addr, and NMU/NSU. Own survey (AMBA System Address Map / FlooNoC) plus brainstorm round. |
| per-id VC binding re-eval | RESOLVED 2026-06-30: confirmed a design bug, promoted to Bugs (per-id VC binding, fix first next round). Survey (IHI 0022 + FlooNoC) basis kept there. |
| multi-id stimulus (`--id-policy`) | Traffic-pattern cells inject a single AXI id (`_emit_synthetic_node` id:0, `--from` copies the base single id), so vc4/vc8 VC-spread is under-exercised for the traffic-pattern group. The AX4 multi-id scenarios cover part of it. |
| NoC-layer QoS | AXI QoS passthrough is done (`packetize.hpp:117,165`, `depacketize.hpp:94,119`) and the NSU has response VC pools plus per-id binding (`nsu/vc_arbiter.hpp:50-56,113-196`). What remains is NoC-layer QoS arbitration and mapping: `NOC_QOS_WIDTH=0` (`ni_flit_pkg.sv:23-24`), `noc_qos` is zero-filled (`docs/architecture.md:66,133`). |
| non-4x4 topology | The generator path already builds `x_dim*y_dim` nodes from YAML (`gen_tb_top.py:21-28,108-122,214-242`) and `router_wrap` exposes 5 link ports (`router_wrap.sv:43-76`). Only 4x4 YAMLs exist (`sim/topologies/mesh_4x4_vc*.yaml`). Add a non-4x4 YAML smoke to prove the generator path on a different shape. |

## Infra / portability

The VCS regression path is documented as Linux-workstation and dry-run pending a real run
(`docs/development.md:227-234`, `sim/vcs/Makefile:8-15`). The matrix is Verilator-only by design
(`docs/superpowers/specs/2026-06-27-regression-matrix-design.md:174`).

- **GCC ICE on `test_pins_smoke.cpp`** (pre-existing, Windows host): GCC internal compiler error
  (segfault) when compiling the `build-cmodel` CMake target on this toolchain. Breaks any CI path
  running `make build` or `make check` (ctest gate). The co-sim Verilator binary is unaffected
  (c_model is header-only; build directly via `make -C sim/verilator`). Investigate toolchain upgrade
  or workaround.

## Dead-code prune candidates (ponytail-audit, 2026-06-30)

Audit ran read-only, no working-tree change. Net ~-1606 lines, 0 dependencies removable. Zero-caller
claims NOT independently verified — confirm each against existing code before deleting (CLAUDE.md
verify-before-change). Lowest-risk first batch = orphans that die with their own self-test (zero
behavior change). Pick a batch and open a round; do not bulk-delete.

| area | candidate | kind | ~lines |
|---|---|---|---|
| tests | `router_path.hpp` + `test_router_path.cpp` (XY predictor, self-test only) | delete | ~-470 (whole tests batch) |
| tests | `isolated_scenario.hpp` + test (self-test only) | delete | |
| tests | `component_dwell_observer.hpp` (SegmentDwell/OccupancyPeak) + test (no harness) | delete | |
| tests | `req_router_at`/`rsp_router_at` + `test_two_node_fabric_at.cpp` (alias assert only) | delete | |
| tests | `test_scaffold.cpp` `*StructsAreConstructible`, `test_ni_stage.cpp` `EnumValuesAreDistinct` (compile-trivial) | delete | |
| docs | `perf-probe-report.md` vs `performance-probe.md` near-duplicate (keep one) | delete | -265 |
| docs | `build_perf_probe_slides.py` (input .pptx + output both gitignored, unrunnable on clone) | delete | -271 |
| sim | `check_perf_parity.py` + test + 4 golden JSON (no Makefile/CI ref) | yagni | -135 |
| sim | `gen_test_patterns.py` synthetic-scenario dead path (`--from` always passed) | delete | -75 |
| sim | `gen_tb_top.py --check` drift gate (no CI, diffs gitignored file) | yagni | -17 |
| sim | `run_regress.py` `is_excluded`/`is_xfail` -> one `_match` (mirrors coverage-round M-item) | shrink | |
| specgen | `ni_spec/report.py` (0 caller) | delete | ~-190 (specgen area) |
| specgen | 3 zero-caller invariant checkers (pin-uniqueness / related-features-symmetric / id-uniqueness) | delete | |
| specgen | `cpp_params.py`/`sv_params.py` duplicate emitter; `constants.py` re-rolled safe-eval; `exceptions.py` 5-class tree | shrink | |
| c_model | ~~VcArbiter `MultiCandidate`~~ DONE (deleted on `feat/vc-id-agnostic`, commit `253b744`) | yagni | ~-130 (headers) |
| c_model | `peek_aw/w/ar/b/r` (10 methods; AxiDpiAdapter caller does not exist) | delete | |
| c_model | `detail::NullSlavePort` (real owner is WireSlavePort), `check_b_one_response_per_write`, dead config `ni_req_extra_depth` | delete | |

Correctly excluded as live (do not touch): `RobMode::Enabled`, `IMemoryPort` (2 impl), `PerfCollector`,
fault injection. Cross-repo theme: dead code kept alive only by its own self-test.

## Cosmetic / cheap (defer)

M-items from the matrix final review: `preserve_addr=True` still runs the ignored `pair_offset` loop,
test inline imports, `gen_tb_top.emit_tb_top(requested_name="")` default is a silent-failure trap,
`is_excluded` KeyError on an unknown `when` key, prebuild `check=True` aborts the whole run instead of
failing one cell. JUnit XML reporting waits until a CI consumer exists.
(Closed in the coverage round: `run_regress` `PASS_MARKER` and `_ax4_curated` orphans deleted, commit
`aa235f5`.)

M-items from the coverage-round final review (none gate merge): `is_xfail`/`is_excluded` are verbatim
duplicates -> factor a private `_match_when(cell, rules)` both delegate to (removes a second place the
`when`-key map can drift); `gen_test_patterns._rewrite_ids` does `t["addr"]` unguarded while
`unique_addr_count` guards with `if "addr" in t` -> mirror the guard (a base scenario with an addr-less
transaction would `KeyError`); `test_run_regress.py` `import pathlib as _pl` sits after the test
functions and is a redundant alias of the top-level `pathlib` import -> hoist and drop the alias;
`_ax4_by_address_mode` parses `unique_addr_count` twice on the raise path -> use a local var; the
`effective_topology()` `_rob`-append branch is only indirectly covered after `test_rob_topology_suffix`
was dropped -> one assert restores it; BND-007 wording differs between the `docs/backlog.md` row
("excluded (matrix.yaml)") and the `matrix.yaml` reason ("re-check on first full run") -> align if the
file is touched.
