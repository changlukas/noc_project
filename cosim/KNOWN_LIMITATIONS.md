# Cosim PoC — Known Limitations

This PoC verifies c_model NMU/NSU AXI4 boundary conformity via wb2axip SVA
checkers run in Verilator simulation. The following are explicit scope limits,
documented for the planned Stage 5 (b) round where RTL `nmu.sv` / `nsu.sv`
replaces the c_model DUT.

---

## 1. faxi_wstrb.v — permissive stub

`cosim/sv/wb2axip/faxi_wstrb.v` was created during the build-fix pass (commit
822a780) because the upstream wb2axip formal version of `faxi_wstrb` is inlined
into the parent modules; Verilator requires a separate module to satisfy the
instantiation.

**Current behavior**: `o_valid` is hardwired to `1'b1`. WSTRB alignment
checking is disabled — the checker never rejects a write strobe regardless of
address, transfer size, or strobe pattern.

**Stage 5 (b) action**: Pull a proper upstream `faxi_wstrb.v` (available in
the wb2axip repository as `bench/formal/faxi_wstrb.v`) or implement the
alignment check directly. The c_model AXI master does generate correct strobes
per `protocol_rules::check_strb_*`, so this is likely a non-issue for the
c_model but must be verified for RTL.

---

## 2. Multi-beat W burst and multi-outstanding AW — invisible to checker

The `AxiDpiAdapter::tick()` loop captures a single pin snapshot per simulation
cycle using `peek_aw()` / `peek_w()` (non-consuming peek at queue front).
Within the same tick, the NMU `AxiSlavePort::tick()` drains the entire AW
and W queues via `while (!q.empty())` loops.

**Effect on wb2axip**: For a burst with `AWLEN=N`, `N+1` W beats are queued
in the NMU port in one tick, but only the first beat is visible to the SV
side. The checker tracks `f_axi_wr_pending` decremented once per visible W
handshake; the remaining `N` beats are never presented. Similarly, when
`max_outstanding_write > 1`, multiple AW transactions queue up but only the
first is visible per tick — the checker's `f_axi_awr_nbursts` counter
underflows when B responses arrive for the invisible AW transactions.

**Specific wb2axip properties affected**:
- `f_axi_wr_pending <= f_axi_wr_len + 1` (induction invariant, line 889)
- `WLAST → f_axi_wr_pending == 1` (beat-count alignment, line 563)
- `f_axi_awr_nbursts < {(F_LGDEPTH){1'b1}}` (overflow guard, line 649)

**Ctest smoke set**: The three ctest scenarios (`conformity_write_read.yaml`,
`conformity_seq_writes.yaml`, `conformity_backpressure.yaml`) are scoped to
`max_outstanding_write=1` (implicit default) and `len=0` (single-beat writes).
The original multi-beat scenarios (`burst_incr_2beat.yaml`,
`burst_incr_8beat.yaml`, `backpressure_retry.yaml` with `max_outstanding_write=4`)
are retained in `c_model/tests/axi/fixtures/` for the c_model AXI unit tests
but are excluded from the cosim smoke set.

**Stage 5 (b) action**: RTL nmu.sv / nsu.sv drives AXI wires directly cycle-
by-cycle; the snapshot model does not apply. Multi-beat and multi-outstanding
scenarios can be re-added to the ctest suite without modification.

---

## 3. cmodel_finalize() not called on C++ timeout path

`cosim/verilator/main.cpp` returns before `$finish` when the cycle timeout
fires. `cmodel_finalize()` is never called on this path; the `g_adapter`
destructor runs at process exit (no UB), but `axi_dpi_adapter_read_dump_*.tmp`
files are not unlinked.

**Effect**: Cosmetic. Temp files accumulate in the binary working directory
after timeout runs. Normal (non-timeout) runs are unaffected.

**Stage 5 (b) action**: Call `cmodel_finalize()` and delete the temp file
before the timeout return path, or use RAII around the adapter instance.

---

## 4. axi_dpi_adapter_read_dump_\<ptr\>.tmp accumulation

`AxiDpiAdapter::init()` constructs a per-instance temp file path using the
`this` pointer as a unique suffix. The destructor does not unlink the file.
After many ctest runs the build directory accumulates pointer-named temp files.
Pointer-based names make manual cleanup awkward (`ls *.tmp | xargs rm`).

**Effect**: Disk accumulation. No functional impact.

**Stage 5 (b) action**: Unlink in the destructor, or use a deterministic name
derived from the scenario path instead of the pointer.

---

## 5. Timing master direction differs from spec §3

The design spec anchor point was "(b) SV master — HDL kernel owns clock."
The implementation puts C++ as the timing master: `clk` and `rst_n` are
top-level input ports of `tb_axi_conformity`, driven from `main.cpp` via
`top->clk = 1/0`.

**Why**: Verilator `--no-timing` mode requires the C++ harness to drive all
`$time` advancement; SV `#`-delay constructs are not supported.

**Stage 5 (b) action**: None required. The Verilator `--no-timing` harness
is unchanged between PoC and RTL-replacement rounds. The clock topology
is identical.

---

## 6. Smoke scenario set differs from original spec §7.2

The spec listed `4kb_cross.yaml` and `multi_outstanding_stress.yaml` as the
primary smoke scenarios. These files do not exist in `c_model/tests/axi/fixtures/`.
The PoC uses `conformity_write_read.yaml`, `conformity_seq_writes.yaml`, and
`conformity_backpressure.yaml` instead (added in commit 391f13d).

**Stage 5 (b) action**: Implement `4kb_cross.yaml` (cross-4KB-boundary burst)
and `multi_outstanding_stress.yaml` (multi-ID, multi-outstanding). Both are
valid and useful conformity tests for RTL but require resolving limitation §2
first (or verifying the RTL harness is immune to the snapshot issue by
construction).
