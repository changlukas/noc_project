# Retire the constrained_random co-sim axis; directed data-integrity as the sole check — design

## Problem

Two things converge this round. (1) The response VC binding (branch `docs/clause2-vc-safe-bypass`,
implemented and proven necessary) needs its verification stage committed and the branch closed. (2)
The constrained_random co-sim axis is built on `axi_reorder_compare`, a RoB-isolation unit-test
checker misapplied in the full-mesh tb: it cannot attribute multi-id concurrent traffic (so reads
are pinned single-outstanding), and it was hand-edited in the vendored tree. Converting it to a
data-integrity scoreboard drags in a memory-mapped slave swap, an INCR-only burst restriction, a
same-address write hazard, and a scoreboard non-vacuity blind spot — a large, tangled change for an
axis whose sound design is not yet settled.

Decision: **remove the constrained_random axis now**; defer a proper random-traffic verification
method to its own round. The directed axis (`axi_file_master` + `axi_scoreboard` + `MAPPED=1` slave)
remains as the sole co-sim data-integrity check.

## What the directed axis already gives us

- `axi_file_master.run_aw()` / `run_ar()` fire every queued AW/AR back to back; `send_*` blocks only
  on the address handshake (`axi_test.sv:2540-2565`). The directed axis is already multi-outstanding,
  no cap.
- `axi_scoreboard` is trusted DV IP: it compares readback data against written data per address, so a
  printed PASS is a real pass. Directed writes-then-reads distinct addresses, so the checker is
  non-vacuous by construction and the same-address write hazard cannot arise.
- Running the RoB-Enabled tb (`*_rob`) on a same-dest-heavy pattern at depth (hotspot, high count,
  burst — the config the binding necessity proof used) drives the same-dest bypass path; the
  scoreboard catches any read-data corruption, from the binding or anything else. The necessity proof
  already showed this scoreboard fires on an unbound build, so its detection ability is established —
  a one-time fault-injection, not repeated per run.

## Design — remove, do not rewrite

### `sim/tb/user_node_endpoint.sv`
- Delete the `else` (non-`TB_DIRECTED`) branch: the `axi_rand_master` typedef + initial blocks, the
  `RAND_RESP`/`MAPPED=0` `axi_rand_slave`, the permutation-pairing `add_memory_region` (`:346-352`).
- Delete the `MAX_READ_TXNS_IN_FLIGHT` / `MAX_WRITE_TXNS_IN_FLIGHT` params (`:49-50`, fed only the
  rand_master).
- Collapse the `TB_DIRECTED` ifdef so the directed flavor is the unconditional module body.

### `sim/tools/gen_tb_top.py`
- Delete the `ifndef TB_DIRECTED` `reorder_compare` emission block (`:648-707`) and the `cmp_eos`
  declaration (`:656`); the exit guard (`:764-768`) collapses to the directed (`end_of_sim` only)
  form. **Also update the watchdog** (`:504-505`), which references `cmp_eos[i]` — remove or rewire
  it so directed-only generation leaves no undeclared `cmp_eos`.

### Run recipes (`Makefile`, `sim/verilator/Makefile`, `sim/vcs/Makefile`)
- Remove `PATTERN=constrained_random`, `RUN_CLASS=constrained_random`, and the
  `run-constrained-random` recipe. `sim/vcs/Makefile` also carries `RUN_CLASS ?= constrained_random`
  (`:6`) as its default and only defines `TB_DIRECTED` for the directed class (`:143`) — flip it to
  directed-only there too. `make sim` runs the directed axis only.
- Add explicit pattern validation: today an unknown `PATTERN` falls through to `_CLASS=directed` and
  reaches `gen_test_patterns.py` (four directed patterns, `:457`). With `constrained_random` gone,
  fail loud on an unknown pattern instead of silently routing it to directed (`Makefile:180`).

### Vendored floonoc-test
- `axi_reorder_compare.sv` is no longer instantiated: drop it from **both** the source filelist
  template (`sim/build_config.mk:100`) and the checked-in generated filelists (`sim/filelist_*.f:17`).
  The file can be removed from the vendored import since it is now unused. This clears the DV-IP
  hand-edit debt (`Rebase`/`RegionBase`) at the same time. Keep `axi_bw_monitor.sv` (perf, all flavors).

### Docs
- Reconcile the two-checker description to the single directed scoreboard axis: `docs/architecture.md`,
  `docs/verification-environment.md`, `docs/development.md`, the checked-traffic-benchmark spec, and
  the ones Codex flagged — `docs/cosim-log.md:36` (`CR PASS`), `docs/2026-07-02-pulp-axi-vip-node-design.md:33`
  (unified `axi_reorder_compare`), `docs/backlog.md:374` (`PATTERN=constrained_random` run interface),
  and `sim/dv/README.md:8` (drop `axi_reorder_compare.sv` from the vendored inventory). State that the
  constrained_random axis is retired pending a proper random-traffic verification method.

## Deferred — its own round

A sound random-traffic co-sim check. A data-integrity scoreboard on random traffic needs a
memory-mapped slave, INCR-only bursts, single-outstanding writes (same-address hazard), and a
non-vacuity mechanism — or a different checker design. Revisit deliberately. The prior two-checker
model (`docs/superpowers/specs/2026-07-04-checked-traffic-benchmark-design.md`) is the context.

## Unchanged

- Directed axis, `axi_file_master`, `axi_scoreboard`, `MAPPED=1` slave, RoB-Enabled tbs.
- `r_rob_depth` (32, already a ctor parameter + `R_ROB_DEPTH` knob).
- ctest (C++ unit tests) — the AXI-protocol conformance the random axis loosely touched lives here.

## Stages

### Stage 1 — remove the constrained_random axis
Edit `user_node_endpoint.sv` + `gen_tb_top.py` (incl. watchdog `cmp_eos`) + the three Makefiles; drop
`axi_reorder_compare.sv` from `build_config.mk` + generated `filelist_*.f`. Verify: a grep for
`constrained_random` / `reorder_compare` / `rand_master` / `MAX_READ_TXNS_IN_FLIGHT` / `cmp_eos` /
`RUN_CLASS` across `sim/`, the Makefiles, and filelists is clean; ctest green;
`make sim TB=tb_mesh_4x4_vc1 PATTERN=neighbor` directed PASS (scoreboard clean, 16-node non-vacuous);
a same-dest-heavy `_rob` run (hotspot, depth) PASS on HEAD; unknown `PATTERN` fails loud.

### Stage 2 — reconcile docs
Update the four docs above to the single-axis description.

### Stage 3 — commit parked binding verification + close branch
Commit the branch's parked verification, land, close `docs/clause2-vc-safe-bypass`.

## Verification summary

| property | check |
|---|---|
| directed axis sound after removal | `make sim` directed PASS on representative patterns, scoreboard clean, non-vacuous |
| binding path exercised | `_rob` same-dest deep run PASS on HEAD (scoreboard = the data-integrity check) |
| no dangling refs | no `PATTERN`/`RUN_CLASS=constrained_random`, no `reorder_compare` instantiation; docs reconciled |
| ctest unaffected | full ctest green |

## References

- Removed: the `axi_reorder_compare` full-mesh misuse and its DV-IP hand-edit debt (backlog
  "ARCHITECTURAL MISUSE" note + DV-IP audit `cross-review/dvip-audit.md`).
- `axi_test.sv`: `axi_file_master` `run_ar` `:2558`, `axi_scoreboard` `:1929`.
- Binding branch `docs/clause2-vc-safe-bypass`; spec
  `docs/superpowers/specs/2026-07-12-clause2-vc-safe-bypass-design.md`.
- Deferred random-axis context: `docs/superpowers/specs/2026-07-04-checked-traffic-benchmark-design.md`.
