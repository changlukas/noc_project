# VCS FSDB per-pattern waveform — design

Date: 2026-06-12
Status: Approved (pending spec review)
Scope: `cosim/vcs/Makefile`, `cosim/sv/tb_top_vcs.sv`, `cosim/sv/tb_genamba.sv`

## Goal

Enable Verdi FSDB waveform dumping for the VCS co-simulation flow on the Linux
workstation, such that each test pattern produces its own independent `.fsdb`
file. Both VCS testbenches (`tb_top` via its `tb_top_vcs` wrapper, and
`tb_genamba`) must support it.

Non-goals: Verilator tracing (VCD/FST) is explicitly out of scope. FSDB is a
Synopsys/Verdi format only VCS can emit; the Verilator flow is left untouched.

## Background (current state)

- No waveform dumping of any kind exists today — no `$dumpfile`/`$fsdbDumpfile`,
  no `--trace`/`VerilatedVcdC`, no `-debug_access`/`novas.tab`/`-lfsdb`.
- "Patterns" for `tb_top` are the scenario tree `tests/scenarios/AX4-*/scenario.yaml`
  (37 scenarios), selected at runtime via the `+scenario=<abs-path>` plusarg.
- `tb_genamba` runs a fixed in-line BFM task program in a single simulation; the
  scenario only seeds the C-model config. Its "pattern" granularity is therefore
  one whole run.
- VCS run recipes already create `cosim/vcs/output/<scenario>/` and tee `run.log`
  there. Simulator selection is by directory (`cd cosim/vcs`), not a `SIM=` var.
- The VCS Makefile has a `[WORKSTATION]` block (lines ~21-29) for site-specific
  knobs (`VCS`, `VCS_EXTRA`).

## Design

### Trigger and isolation

- New make variable `FSDB ?= 0` in `cosim/vcs/Makefile`. Default `0` = no dumping;
  regression / ctest behaviour is unchanged.
- `FSDB=1` adds, at compile time, the define `+define+FSDB_DUMP` plus the Verdi
  FSDB PLI link flags; at run time it adds the `+fsdb=<abs-path>` plusarg.
- All `$fsdbDump*` system tasks in SV are wrapped in `` `ifdef FSDB_DUMP ``. When
  `FSDB=0` the define is absent, so the un-linked Verdi PLI tasks are never
  referenced and the build/run is identical to today.

### SV dump block (both testbenches)

Add to `cosim/sv/tb_top_vcs.sv` (top module `tb_top_vcs`) and
`cosim/sv/tb_genamba.sv` (top module `tb_genamba`):

```systemverilog
`ifdef FSDB_DUMP
initial begin
  string fsdb_path;
  if (!$value$plusargs("fsdb=%s", fsdb_path)) fsdb_path = "dump.fsdb";
  $fsdbDumpfile(fsdb_path);
  $fsdbDumpvars(0, <top_module>);   // depth 0 = entire hierarchy below top
end
`endif
```

`<top_module>` is `tb_top_vcs` and `tb_genamba` respectively. `tb_top.sv` itself
needs no change — dumping from the `tb_top_vcs` wrapper recursively covers the
whole instantiated hierarchy (DPI wrappers and the `faxi` checkers included).

### Output naming and location

Reuse the existing `cosim/vcs/output/<scenario>/` convention. The filename is
prefixed with the testbench name so the two TBs running the same scenario do not
overwrite each other:

- `cosim/vcs/output/<scenario>/tb_top.fsdb`
- `cosim/vcs/output/<scenario>/tb_genamba.fsdb`

The run recipe passes this as an absolute path via `+fsdb=`.

### Makefile changes (`cosim/vcs/Makefile`)

1. `FSDB ?= 0`.
2. A conditional block, `ifeq ($(FSDB),1)`:
   - compile: append `+define+FSDB_DUMP` and the FSDB PLI link flags.
   - run: append `+fsdb=$(CURDIR)/output/$(SCENARIO)/<tb>.fsdb`.
3. FSDB PLI link flags live in the `[WORKSTATION]` block because they depend on a
   site-local `$VERDI_HOME`:
   ```
   FSDB_PLI ?= -P $(VERDI_HOME)/share/PLI/VCS/LINUX64/novas.tab \
                  $(VERDI_HOME)/share/PLI/VCS/LINUX64/pli.a
   ```
   Newer VCS+Verdi may additionally need `-kdb -lca`; left as a tunable in the
   same block. Exact flag set is `[TBD]` until first run on the real VCS+Verdi
   install (current tree is dry-run validated only).
4. New batch target `run-all-fsdb`:
   - enumerate scenarios via `$(notdir $(wildcard <repo>/tests/scenarios/AX4-*))`.
   - loop, invoking `$(MAKE) run-tb-top SCENARIO=$$s FSDB=1` per scenario.
   - fault-tolerant: a `$fatal` pattern (e.g. `AX4-INF-001`, or wb2axip-incompatible
     scenarios the checker stops) must not abort the batch; continue to the next
     and still leave that pattern's `.fsdb` for debugging.
   - print a PASS/FAIL summary at the end.
   - Also run `tb_genamba` once with `FSDB=1` so both TBs are covered.

## Testing / acceptance

Because the host has no VCS+Verdi, verification is staged:

1. `FSDB=0` (default) build+run of `run-tb-top` and `run-genamba` is byte-for-byte
   equivalent to current behaviour — no FSDB symbols compiled or linked. This is
   verifiable on the current host as a dry-run / flag-diff.
2. On the Linux workstation with VCS+Verdi:
   - `make run-tb-top SCENARIO=<id> FSDB=1` produces a non-empty
     `output/<id>/tb_top.fsdb` openable in Verdi.
   - `make run-genamba FSDB=1` produces `output/<scenario>/tb_genamba.fsdb`.
   - `make run-all-fsdb` produces one `.fsdb` per scenario, continues past
     failing patterns, and prints a PASS/FAIL summary.

## Open items

- Exact Verdi PLI link flag set (`novas.tab`/`pli.a` path, `-kdb`/`-lca`) is
  `[TBD]` until first run on the real install; encoded as `[WORKSTATION]` knobs.
- Whether to also dump SVA (`$fsdbDumpSVA`) for the `faxi` checkers — deferred;
  not needed for the initial per-pattern signal trace.
