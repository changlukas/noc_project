# Verilator VCD per-pattern waveform — design

Date: 2026-06-12
Status: Approved (user-confirmed, twin of the VCS FSDB design)
Scope: `cosim/verilator/Makefile`, `cosim/verilator/main.cpp`,
`cosim/verilator/main_genamba.cpp`
Twin spec: `2026-06-12-vcs-fsdb-per-pattern-design.md` — decisions mirror it
1:1 unless stated; rationale lives there.

## Goal

Opt-in per-pattern VCD waveform dumping for the Verilator flow on the Windows
dev host, so waveforms are available before the Linux workstation (VCS/Verdi)
is usable. VCD is chosen over FST because Verdi opens VCD directly and GTKWave
(present on the dev host) gives an immediate local view.

## Design (deltas vs the FSDB twin)

- Trigger: `TRACE ?= 0` make variable; `TRACE=1` adds `--trace --trace-structs`
  at verilate time. No SV changes — under Verilator the dump lives in the C++
  harnesses, guarded by `#if VM_TRACE` (defined by Verilator iff `--trace`),
  so a non-trace build contains zero trace code.
- Harnesses: `main.cpp` (tb_top, two-phase clock loop — dump after each eval
  at `contextp->time()`, close on both the timeout and normal exit paths) and
  `main_genamba.cpp` (timing mode — dump after each `eval()` in the event
  loop). Path from plusarg `+vcd=<abs-path>` via `commandArgsPlusMatch`;
  fallbacks `dump.vcd` / `tb_genamba.vcd` (mirrors the FSDB per-TB fallbacks).
- Artifact isolation: trace builds verilate into `obj_dir_trace` /
  `obj_genamba_trace` (`TRACE_SUFFIX`), because `--trace` changes the
  generated C++ — same reasoning as the `_fsdb` simv split. `clean` covers
  both. Flag injection via guarded `+=` and plusarg attachment via the
  `$(if ...)` idiom, for the same TRACE=0 byte-identity requirement.
- Output: `cosim/verilator/output/$(SCENARIO)/tb_top.vcd` and
  `output/genamba_$(GENAMBA_SCENARIO)/tb_genamba.vcd`, stale file `rm -f`'d in
  the run recipes (both modes).
- Batch: `run-all-trace` mirrors the hardened `run-all-fsdb`: pre-build once,
  per-scenario `test -s` on the produced vcd, fault-tolerant loop, PASS/FAIL +
  size summary from collected paths, always exit 0.

## Acceptance (all locally verifiable — Verilator runs on this host)

1. `TRACE=0` build/run byte-identical to pre-change except the intentional
   `rm -f` lines; existing obj_dir binaries not rebuilt.
2. `make run-tb-top SCENARIO=<id> TRACE=1` produces a non-empty
   `output/<id>/tb_top.vcd` with a valid VCD header, openable in GTKWave.
3. `make run-genamba TRACE=1` produces `output/genamba_<id>/tb_genamba.vcd`.
4. `run-all-trace` produces one vcd per passing scenario and a summary.

## Notes

- VCD is uncompressed text; gzip before transferring to the workstation
  (Verdi reads .vcd; ~10-20x compression for transport).
- FST explicitly rejected: Verdi cannot open it.
