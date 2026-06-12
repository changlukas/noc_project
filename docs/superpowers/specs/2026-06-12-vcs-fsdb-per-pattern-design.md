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
- **Separate build artifacts per mode.** Make does not track command-line flag
  changes, so toggling `FSDB` must not silently reuse a binary compiled in the
  other mode (an FSDB-compiled simv re-run under `FSDB=0` would still dump).
  FSDB builds therefore get their own outputs:
  - `build/vcs/simv_tb_top_fsdb` + `-Mdir=build/vcs/csrc_tb_top_fsdb`
  - `build/vcs/simv_genamba_fsdb` + `-Mdir=build/vcs/csrc_genamba_fsdb`
  The non-FSDB `simv_tb_top` / `simv_genamba` and their csrc dirs are untouched;
  both modes coexist without cross-contamination.

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

Reuse the existing output-directory conventions of each run recipe (the fsdb
lands next to that run's `run.log`):

- `tb_top`:     `cosim/vcs/output/$(SCENARIO)/tb_top.fsdb`
- `tb_genamba`: `cosim/vcs/output/genamba_$(GENAMBA_SCENARIO)/tb_genamba.fsdb`
  (the existing genamba recipe uses the `genamba_` prefixed directory and the
  `GENAMBA_SCENARIO` variable — not `SCENARIO`)

The run recipe passes this as an absolute path via `+fsdb=`, creates the output
directory first, and deletes any stale `.fsdb` of the same name before launching
`simv`, so a failed run can never leave a misleading old waveform behind.

### Makefile changes (`cosim/vcs/Makefile`)

1. `FSDB ?= 0`.
2. A conditional block, `ifeq ($(FSDB),1)`:
   - compile: append `+define+FSDB_DUMP` and `$(FSDB_PLI)`; redirect outputs to
     the `_fsdb`-suffixed simv/csrc names (see Trigger and isolation).
   - run: append the `+fsdb=` plusarg with the per-recipe absolute path (see
     Output naming).
3. FSDB PLI link flags live in the `[WORKSTATION]` block because they depend on a
   site-local `$VERDI_HOME`, and are user-overridable (`?=`):
   ```
   FSDB_PLI ?= -P $(VERDI_HOME)/share/PLI/VCS/LINUX64/novas.tab \
                  $(VERDI_HOME)/share/PLI/VCS/LINUX64/pli.a
   ```
   Notes for first bring-up on the workstation (all `[TBD]` until validated on
   the real install — current tree is dry-run validated only):
   - Procedural `$fsdbDump*` needs only this PLI registration. `-debug_access+all`
     / `-kdb` serve interactive-Verdi / KDB source debug and are NOT added by
     default; add to `VCS_EXTRA` only if that workflow is wanted. `-lca` is
     version-dependent.
   - The Verdi FSDB runtime libraries may require `LD_LIBRARY_PATH` (e.g.
     `$VERDI_HOME/share/PLI/lib/LINUX64`); verify at first run and record the
     result in the `[WORKSTATION]` block.
   - `FSDB=1` errors out early with a clear message if `VERDI_HOME` is unset.
4. New batch target `run-all-fsdb`:
   - enumerate scenarios via `$(notdir $(wildcard <repo>/tests/scenarios/AX4-*))`.
   - loop with explicit exit-code capture (`if $(MAKE) run-tb-top SCENARIO=$$s
     FSDB=1; then ...; else ...; fi` — NOT `|| true`, which destroys the status),
     collecting pass/fail lists.
   - fault-tolerant: a failing pattern (e.g. `AX4-INF-001` which fails by design,
     or wb2axip-incompatible scenarios the checker stops) must not abort the
     batch; continue to the next and still leave that pattern's `.fsdb` for
     debugging.
   - final summary lists PASS/FAIL per scenario plus each produced `.fsdb` and
     its size; known-by-design failures (`AX4-INF-001`) are annotated as such.
   - the target itself always exits 0 — its purpose is waveform production, not
     a regression gate (ctest remains the gate).
   - Also run `tb_genamba` once with `FSDB=1` so both TBs are covered.

## Testing / acceptance

Because the host has no VCS+Verdi, verification is staged:

1. `FSDB=0` (default) build+run of `run-tb-top` and `run-genamba` is byte-for-byte
   equivalent to current behaviour — no FSDB symbols compiled or linked. This is
   verifiable on the current host as a dry-run / flag-diff.
2. On the Linux workstation with VCS+Verdi:
   - `make run-tb-top SCENARIO=<id> FSDB=1` produces a non-empty, freshly
     written `output/<id>/tb_top.fsdb`. Opening it in Verdi must show the
     representative signals — top-level AXI interfaces, the DPI wrapper
     boundaries, and `faxi` checker state — not merely a loadable file.
   - `make run-genamba FSDB=1` produces
     `output/genamba_<scenario>/tb_genamba.fsdb` with the BFM/NMU/NSU/mem
     interfaces visible.
   - `make run-all-fsdb` produces one `.fsdb` per scenario, continues past
     failing patterns, prints the PASS/FAIL + fsdb-size summary, and exits 0.
   - Toggling `FSDB=0` after an FSDB build runs the original `simv_tb_top`
     (separate artifact) and produces no `.fsdb`.

## Open items

- Exact Verdi PLI link flag set (`novas.tab`/`pli.a` path, `LD_LIBRARY_PATH`,
  `-lca`) is `[TBD]` until first run on the real install; encoded as
  overridable `[WORKSTATION]` knobs.
- Whether to also dump SVA (`$fsdbDumpSVA`) for the `faxi` checkers — deferred;
  not needed for the initial per-pattern signal trace.
- Dumping of memories / multidimensional arrays may need version-specific
  `$fsdbDumpvars` options (e.g. `+all`); decide at first bring-up if memory
  contents are needed in the waveform.

## Review history

- 2026-06-12 codex independent review: REQUEST CHANGES, 8 findings. Adopted:
  separate `_fsdb` build artifacts (High); genamba output-dir correction
  (`output/genamba_$(GENAMBA_SCENARIO)/`); exit-code-safe batch loop; stale-fsdb
  deletion; overridable `FSDB_PLI` + `LD_LIBRARY_PATH` note; signal-level
  acceptance criteria; fsdb-size summary. Simplified vs codex proposal: batch
  always exits 0 with annotated known failures instead of a three-state
  PASS/EXPECTED-FAIL/SKIP classification (avoids duplicating the wb2axip skip
  list maintained in `test_cosim_integration.cpp`); no `FSDB_TIMEOUT` / disk
  preflight (YAGNI until proven needed).
