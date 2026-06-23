# gen_amba role-1 testbench -- Phase 1 findings

This document records the Phase 1 outcome of the gen_amba role-1
single-master testbench. It captures per-task results, observed bridge
behaviour, the limitations of the present cosim configuration, and the
Phase 2 prerequisites that the data uncovered.

For the corresponding design spec, see
`docs/superpowers/specs/2026-06-08-genamba-role1-testbench-design.md`
(rev 6). For the per-task execution plan with amendments, see
`docs/superpowers/plans/2026-06-08-genamba-role1-testbench.md`.

Run date: 2026-06-10. Commit range: `6d29be3..7e00f32` (T1..T10).

## Table of contents

1. [Per-task outcome](#1-per-task-outcome)
2. [Stderr observations](#2-stderr-observations)
3. [Bridge-level findings](#3-bridge-level-findings)
4. [What the testbench does NOT exercise](#4-what-the-testbench-does-not-exercise)
5. [Phase 2 prerequisites](#5-phase-2-prerequisites)
6. [Verdict](#6-verdict)
7. [References](#7-references)

---

## 1. Per-task outcome

All twelve scenarios (A; B blen 4/8/16; C N 4/8; D N=4 blen 4/8; E
same-ID; F mixed R+W; G N 8/16) PASS in a single run of
`cosim/verilator/obj_genamba/Vtb_genamba.exe` against
`sim/test_patterns/AX4-BAS-001_single_write_no_read/scenario.yaml`.
Sim finish at 16.3 us, walltime 0.007 s on Verilator 5.036 + MSYS2.

| Task | Outcome | Sim time at PASS | Notes |
|---|---|---|---|
| A baseline mem_test | PASS | 3.34 us | gen_amba self-check (blen=1) |
| B burst blen=4 | PASS | 4.4 us | -- |
| B burst blen=8 | PASS | 5.7 us | -- |
| B burst blen=16 | PASS | 7.7 us | -- |
| C outstanding N=4 | PASS | 8.2 us | distinct IDs 1..4 |
| C outstanding N=8 | PASS | 9.0 us | distinct IDs 1..8 |
| D outstanding burst N=4 blen=4 | PASS | 9.7 us | -- |
| D outstanding burst N=4 blen=8 | PASS | 10.8 us | -- |
| E same-ID outstanding (id=7) | PASS | 11.2 us | same-ID order preserved |
| F mixed R+W concurrent | PASS | 13.4 us | top-level fork, distinct ID ranges |
| G deep pressure N=8 | PASS | 14.2 us | 84 cycles (840 ns) |
| G deep pressure N=16 | PASS | 15.8 us | 156 cycles (1560 ns) |

The G N=8 measurement gives an analytical budget for N=16 per spec
§3.7: `measured_N8 * 2 * 4 = 672` cycles. Because 672 is well under the
default `WATCHDOG_CYCLES = 2000`, the code keeps the 2000-cycle
localparam unchanged (no calibration edit was needed); N=16 actually
completed in 156 cycles.

## 2. Stderr observations

| Source | Observed |
|---|---|
| `faxi_slave` violations | N/A -- checker was removed during T5 (commit `d40525d`) because its non-pipelined-write model false-fires on AXI4-legal multi-beat writes; the bind comes out per `[[dont-silence-the-checker]]`, replaced by a handshake-progress watchdog plus per-task data compare |
| DPI error pump fires | None |
| `$fatal` fires | None |
| `tb_genamba` 1 us handshake-progress watchdog | Did not fire |
| Task G bounded watchdog (`fork join_none` + `wait fork`) | Did not fire (N=8: 84 cy; N=16: 156 cy; cap 2000 cy) |

## 3. Bridge-level findings

### 3.1 ROB same-ID ordering (Task E)

Same-ID R returns observed in AR-issue order across four outstanding
reads sharing `ARID = 8'd7`. No reorder. The c_model NMU ROB in its
current Disabled mode acts as a per-ID transaction-ordering filter only
(`c_model/include/nmu/rob.hpp`, class-doc lines 18-40); this is
sufficient for the AXI4 §A5.3 invariant exercised here.

### 3.2 Cross-ID R-return ordering observed (Tasks C, D)

For Tasks C and D the bridge returns R bursts at the BFM in AR-issue
order across distinct IDs as well. AXI4 §A5.3 does NOT require this
across distinct IDs; the bridge happens to preserve it because there is
no router and the c_model NMU/NSU response path is a single-stage
passthrough for distinct-ID returns. The project-owned `bfm_drain_r`
relies on this empirical property (the shadow array is indexed by a
global RVALID-RREADY counter, not by RID); if a future bridge variant
reorders cross-ID returns, the drain would need per-ID FIFOs. This
is annotated in the `bfm_drain_r` task comment in `genamba_master_bfm.sv`.

### 3.3 ROB MetaBuffer pressure (Tasks D, G)

The combination of multi-beat W bursts and deep outstanding pressure
(D N=4 blen=8 -> 32 in-flight W beats; G N=16 -> 16 ARs and 16 W bursts)
runs through the NMU AXI port adapter, packetize stage, and the
RoB Disabled-mode FIFOs without backpressure visible at the BFM
AW/AR/W handshake. Bridge handled cleanly.

### 3.4 Valid-ready backpressure under deep outstanding (Task G)

G N=16 completed in 156 cycles versus G N=8's 84 cycles -- a roughly 2x
scaling consistent with sequential per-transaction overhead at the
valid-ready handshake. No stall, no deadlock. **This result attests
only to the valid-ready backpressure path** -- credit-based flow control
is not exercised (see §4).

## 4. What the testbench does NOT exercise

The current cosim configuration intentionally stubs or omits the
following. These are out of scope per spec §4; they are recorded here so
that Phase 2 starts with the right inventory.

| Feature | State | Where it is stubbed |
|---|---|---|
| Multiple VCs | Not exercised; PoC guard fails elaboration if `NUM_VC > 1` | `cosim/sv/nmu_wrap.sv:58`, `cosim/sv/nsu_wrap.sv:55` |
| Credit-based flow control | Wired, fixed at 0 | `cosim/sv/nmu_wrap.sv:277-278`, `cosim/sv/nsu_wrap.sv:309-310` ("PoC always 0"); c_model `NullNocReqOut::credit_avail()` returns `true` unconditionally (`c_model/include/nmu/nmu.hpp:176`) |
| Router / fabric | Not present | `tb_genamba.sv` connects `noc_mosi_o` -> `noc_miso_i` directly via a single `noc_intf`; XY routing computes `dst_id` at packetize time but no downstream consumer |
| ROB Enabled mode | Implemented, not exercised here | `c_model/include/nmu/rob.hpp` implements `RobMode::Enabled` (slot pool + per-ID ranges); this testbench constructs the bridge in Disabled mode |
| ChannelModel tick | Not ticked | chandle created so DPI state-machine assertions pass; BFM and `mem_axi` are the only active drivers |
| AXI REGION transport | Not marshalled by DPI | spec §3.2; tied to 0 at boundary |
| AXI exclusive access | Not exercised | spec §4; `mem_axi` has no exclusive monitor |
| AXI FIXED / WRAP burst | Not exercised | spec §4 |
| Slave-side B/R backpressure | Not exercised | gen_amba drains complete handshakes without delay loops; vendored `delay=0` everywhere |

## 5. Phase 2 prerequisites

The Phase 1 result tells us what Phase 2 needs to gain before role 2
(crossbar + multi-master) is meaningful:

| Item | Rationale |
|---|---|
| Real credit-based flow control | Phase 2's multi-master load will create routing contention that valid-ready alone cannot characterise without false stalls or deadlock blind spots |
| ROB Enabled mode | Multi-master + non-trivial router will return R out-of-order across distinct IDs at higher rates; per-ID reorder must be active |
| AXI REGION DPI extension | If role 2's xbar routes by region, DPI must marshal `AWREGION` / `ARREGION` |
| gen_amba crossbar generation | the gen_amba_2021 crossbar generator (`gen_amba_axi`) plus a topology choice (e.g. 2 master x 2 slave) and ID widening to `WIDTH_SID = 8 + ceil(log2(N_master))` |
| Multi-instance NMU/NSU wiring | The chandle ABI already supports per-instance create; the testbench needs N NMU + N NSU + per-slot `cm_ctx` |

## 6. Verdict

**GO** for Phase 2 startup.

The single-master point-to-point bridge passes all twelve AXI4
patterns. The bridge handled `N=16` deep outstanding pressure without
stall propagation collapsing into deadlock; same-ID ordering invariant
holds at the boundary; data integrity was verified per-address across
six tasks of accumulating pressure. Limitations (no credit, no router,
no multi-VC, ROB Disabled mode only) are catalogued in §4 and become
the Phase 2 input set.

## 7. References

- Spec: `docs/superpowers/specs/2026-06-08-genamba-role1-testbench-design.md` (rev 6)
- Plan: `docs/superpowers/plans/2026-06-08-genamba-role1-testbench.md`
- Architecture: `docs/architecture.md`
- Vendored attribution: `cosim/sv/genamba/ATTRIBUTION.md`
- AMBA AXI / ACE Protocol Specification, ARM IHI 0022H, §A3, §A5
