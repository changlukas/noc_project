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
| `AX4-ORD-002` | multi-id concurrent write (ids 0x1-0x4, `max_outstanding_write: 4`) hangs to the 100k-cycle co-sim timeout. Reproduces without preserve_addr. | RAW-release / NSU per-id response path | excluded |
| `AX4-BND-006` | 4KB-crossing burst at `0x0FE0` (`len:7`, `size:5`): write OKAY, read phase hangs under 16-node load. NMU 4KB auto-split works for write, read-split does not. | NMU 4KB read-split under concurrent load | excluded |
| `AX4-BND-007` | same boundary-edge class. Manual single-cell check was inconclusive (data-file relpath artifact); excluded preemptively until first full run confirms. | same as BND-006 (unconfirmed) | excluded (matrix.yaml) |

The first full `make sim-regress` is a discovery run. Sweeping the curated set through the
concurrent 16-node fabric will surface more pre-existing co-sim bugs. Add each to `matrix.yaml`
exclusions with a reason as it is confirmed.

### Matrix harness bugs (codex-found)

| id | location | symptom | fix |
|---|---|---|---|
| H1 | `sim/regress/run_regress.py:91-98` (`run_cell`) + `sim/tools/gen_test_patterns.py:691` | `pattern=hotspot` cells error out. `run_cell` passes `--preserve-addr` but never `--hotspot`, and `gen_test_patterns` calls `ap.error("--hotspot is required")` when it is absent. The current nightly group-1 `BAS-003 x hotspot` cell hits this on the first complete run. | `run_cell` passes a default `--hotspot` target (e.g. the center tile) when `pattern == hotspot`. Hotspot stays in the matrix (user decision). |
| H2 | `sim/tools/gen_test_patterns.py:701-702` | `preserve_addr` is ignored on the `uniform_random` / `hotspot` paths (they call `_emit_base_driven_node` without the preserve option). The deterministic paths honor it at `:630` and `:657`. Dormant today because the AX4 group runs only on `neighbor`. | Honor `preserve_addr` on all paths, or keep the address-agnostic group on default reallocation (which needs no preserve) and document the constraint. |
| H3 | `sim/regress/run_regress.py:62-69` (`is_excluded`) | Exclusion matches only `from` / `pattern` / `topology` / `rob_mode`. A `from` scenario shared by a preserve group and a non-preserve group makes one exclusion hit both. | The full-cross split puts each `from` in one group, so this stays dormant. Add `preserve_addr` to the `when` key only if a scenario is ever shared. |

## Next-session plan: full cross (orthogonal shape x spatial)

Traffic pattern decides the destination tile (spatial). AX4 transaction pattern decides the read/write
shape. The two axes are independent. The current matrix runs all 36 curated AX4 scenarios on `neighbor`
only because it applies `preserve_addr` to the whole AX4 set. The runner imposes no such limit
(`expand` cross-products the declared groups). Address-agnostic scenarios can run all 4 patterns
through default offset reallocation: `_emit_base_driven_node` copies the base shape and moves only the
address, grouping transactions by original local address so paired and ordered accesses stay
consistent.

`transpose` is a bijection on a square mesh (`gen_test_patterns.py:320-336` requires square power-of-two),
so it does not converge. Only `uniform_random` and `hotspot` converge.

Classify each AX4 scenario by whether its conformity depends on the absolute or low address bits.

| group | scenarios | patterns | address mode |
|---|---|---|---|
| address-agnostic | `BAS-002..005`, `BUR-001..004` (INCR / FIXED), `EXC-001..003`, `HSH-*`, `ORD-001`, `STR-001/002` | neighbor, uniform_random, transpose, hotspot | default reallocation |
| address-sensitive | `BND-*` (narrow / unaligned / sparse / 4KB), `BUR-005..009` (WRAP, wrap phase depends on start addr), `EXC-004` (WRAP pair), `RSP-*` (OOB vs memory window), `STR-003` (addr encodes dst) | neighbor | preserve_addr |

`BAS-001` and `QOS-001` are write-only and `RSP-*` is `category: response`, so the wire-verifiable
filter skips them in either group. `ORD-002` and `BND-006` stay excluded (fabric bugs above).

**Cell count.** 16 agnostic scenarios x 4 patterns + 13 sensitive x 1 = 77 stimulus combinations x 8
builds = 616 raw runnable cells, against 264 today (2.3x). The gain is the transaction-shape x
spatial-pattern cross that the current single-carrier design omits.

**Prerequisites (land with the cross):**
- Fix H1 (hotspot default target), or every `agnostic x hotspot` cell errors.
- Confirm EXC and ORD survive default reallocation under `uniform_random` / `transpose` / `hotspot`
  (paired addresses stay paired, ids are copied unchanged, so likely fine).

**Open decisions (ask at start of next session):**
- Classification mechanism: hardcoded family/id lists in `run_regress._ax4_curated`, or a per-scenario
  `metadata.address_sensitive: true` tag. The tag is self-documenting and survives new scenarios. The
  lists need no per-scenario edits.
- Fold the `BAS-003` carrier group into the agnostic group, or keep it explicit.
- Accept the 2.3x cell-count, or start with a 1-2-per-family sentinel and grow by evidence.

## Verification methodology gaps

| item | summary |
|---|---|
| injection-rate / saturation sweep | The benchmark runner measures one operating point and labels itself `greedy-finite-trace-stress`, "single operating point, no injection-rate sweep" (`sim/tools/run_benchmark.py:16-18,84-86,118`). A latency-vs-offered-load sweep is the measurement that exposes VC-count differences. Today vc1..vc8 latency reads flat because no sweep applies congestion. Needs an AxiMaster injection schedule driving the c_model interface. Recurs across the benchmark-generator, struct-refactor, and congestion-bugfix rounds. |
| coverage + CRV + wire-side SVA | The matrix gates on the scoreboard only and skips non-wire-verifiable response/write-only cases (`sim/regress/README.md:17-23`, `run_regress.py:80-89`). No covergroup, no constrained-random framework, no wire-side protocol assertions. Make it actionable: a coverage plan plus co-sim scenario-coverage accounting (how many of the 37 AX4 actually run at co-sim), not a vague bucket. |

## Design rounds (broader)

| item | summary |
|---|---|
| SAM remap | The NI has a second memory-mapping mechanism (dst via SAM table lookup, `local_addr = addr - base`) per `addr_trans.hpp:7-8,25,28-30` and spec sec 4.3. Today `local_addr = addr` (decode, `:25`). Under remap, stimulus address generation changes from "synthesize dst into the high bits" to "produce flat SAM-mapped addresses, dst by lookup". Reworks `gen_test_patterns`, preserve_addr, and NMU/NSU. Own survey (AMBA System Address Map / FlooNoC) plus brainstorm round. |
| per-id VC binding re-eval | c_model binds each AXI id to one VC within its read/write class (`nmu/nsu vc_arbiter.hpp`). Survey (IHI 0022 + FlooNoC) found this deviates from FlooNoC (id-agnostic VC by message class plus a RoB for same-id order) and is redundant when the response ROB is enabled (load-bearing only robless). Decide keep vs restructure. |
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
