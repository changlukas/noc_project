# gen_amba role-1 single-master testbench — design spec

Status: draft rev 6
Date: 2026-06-08 (rev 6 revised 2026-06-10)
Supersedes: rev 5 "gen_amba integration feasibility spike" at commit `07fff1f`.

## 1. Problem

The NoC behaviour model (AXI Master → NMU → NoC → NSU → AXI Slave) is AI-authored and currently exercised only by AI-authored stimulus + AI-authored expected-response checkers (the cosim suite checks our own model against our own harness). We want independent cross-tool evidence that NMU/NSU's AXI master/slave interfaces are spec-compliant by exercising them with `gen_amba_2021`'s externally AXI-compliant golden VIP.

This spec covers **role 1**: the NMU/NSU bridge sits between two golden gen_amba endpoints (single AXI master + single slave). The BFM drives a representative subset of gen_amba's single-master AXI4 surface — INCR single beat, INCR burst (multi-beat), outstanding (multi-ID), mixed R+W, same-ID ordering, deep-outstanding stall. **Phase 2** (NoC bridge as a node in a real AXI fabric, multi-master through gen_amba's generated crossbar) is the next spec.

**Phase gating**: Phase 2 begins only after Phase 1 ships its findings document and the discovered risks are dispositioned (fix in DUT, fix in wrapper, accept + document, or defer to Phase 2).

Phase split rationale: Phase 2 requires gen_amba crossbar generation, ID widening (`WIDTH_SID`), topology / address-map decisions, and multi-instance NMU/NSU wiring through the chandle DPI ABI — all genuinely new design surface. Phase 1 consumes only vendored gen_amba assets (BFM tasks + mem_axi) and only what the multi-instance DPI ABI already supports (single-instance per shell, exactly the `tb_top.sv` pattern).

## 2. Ground truth

- DUT AXI `axi4_intf` (`specgen/generated/sv/ni_signals_pkg.sv`): AXI4, ID=8, ADDR=64, DATA=256, carrying `aw/ar` `cache/prot/qos/region/lock`. AXI4 carries `AxLOCK` as a 1-bit signal (only `lock[0]` of the 2-bit AXI3 field).
- DUT NoC link (post `noc-layer-cleanup` refactor): unified `interface noc_intf` at `ni_signals_pkg.sv:136` with `mosi`/`miso` modports — replaces the old `noc_req_intf`/`noc_rsp_intf` split. NMU exposes `noc_intf.mosi noc_mosi_o` (`nmu_wrap.sv:48`); NSU exposes `noc_intf.miso noc_miso_i` (`nsu_wrap.sv:44`); mated point-to-point in tb.
- DUT DPI (post `multi-instance-dpi` refactor): chandle ABI per instance, no `g_*` singletons. Pattern at `tb_top.sv:47-56` (imports) + `tb_top.sv:70-76` (lifecycle):
  ```sv
  cmodel_init(scenario_path);
  cm_ctx     = cmodel_channel_model_create("channel_model_0");
  master_ctx = cmodel_master_create("master_0");
  slave_ctx  = cmodel_slave_create("slave_0");
  nmu_ctx    = cmodel_nmu_create("nmu_0");
  nsu_ctx    = cmodel_nsu_create("nsu_0");
  ```
  Per-cycle error pump `cmodel_check_error(output string msg) → int` at `tb_top.sv:374-388`. Exit polling via `cmodel_done()` at `tb_top.sv:350`. Single `cmodel_finalize()` at end.
- Cosim NoC flow control **STUBBED**: `c_model/include/cosim/nmu_shell_adapter.hpp:187` and `nsu_shell_adapter.hpp:186` hard-code `noc_*_credit_return = false`. Real credit-based backpressure is not modelled. Backpressure instead arrives through NMU/NSU internal FIFO full → channel `ready=0` propagating back to the BFM (BFM task waits in `@(posedge ACLK)` until `ready==1`). Deep outstanding can stall the BFM but **must not deadlock**.
- gen_amba VIP vendored at `cosim/sv/genamba/` (commit `07fff1f`, ATTRIBUTION 2-clause BSD): `axi_master_tasks.v` (single + channel-level + multi-outstanding primitives), `mem_axi.v` (golden slave with `AXI_WIDTH_{CID,ID,AD,DA}` params), `mem_test_tasks.v` (`mem_test` golden self-check + buggy `mem_test_burst` — do not use the latter). Build defines required: `AMBA_AXI4` + `AMBA_QOS` + `AMBA_AXI_CACHE` + `AMBA_AXI_PROT` (NOT `AMBA_AXI_QOS`).
- **Vendored primitive caveats discovered during spec review** (see §3.6 for workarounds):
  - `axi_master_{write,read}_multiple_outstanding` has internal arrays sized 16 (`axi_master_tasks.v:146` / `:323`) — **N≤16 only**.
  - `axi_master_write_multiple_outstanding` writes data based on `reg_addr[idx]` after the AW issue loop has left `idx==awnum` (`axi_master_tasks.v:344`), so per-address data correspondence is **broken** for self-check; we wrap channel-level primitives instead.
  - `axi_master_write` / `_read` randomise `awid`/`arid` internally and do not expose an ID parameter (`axi_master_tasks.v:186-210`) — fork-ing them creates multi-driver conflicts on shared AW/W/B signal arrays. Same-ID stress (task E) requires a single sequencer over channel-level tasks.
  - `axi_master_rmw` writes a 2-bit lock value `2'b10` (`axi_master_tasks.v:366-372`), but AXI4 carries only `lock[0]` → exclusive bit is 0 in practice; the task does not exercise exclusive access on AXI4.
- Simulator: Verilator 5.036+ `--cc --exe --timing` (two-phase build: `verilator` generates C++ in `obj_genamba/`, then `$(MAKE) -C obj_genamba -f Vtb_genamba.mk` compiles). **Not `--binary --timing`** — Verilator 5.036 on MSYS2/Windows can't spawn the nested `make` subprocess via Windows `CreateProcess()` (can't see MSYS2's `make`). Build invoked from **Git Bash on Windows** (MSYS2 Bash): `make -C cosim/verilator genamba PYTHON3=python3`.

### 2.1 Already established (rev 5 spike artefacts)

- Verilator 5.040 `--timing` builds gen_amba's full VIP into a runnable exe (verified prior; central "can Verilator host the VIP?" risk retired).
- gen_amba's native full self-test is impractically slow under `--timing` → use targeted per-task patterns.
- Build + run from Git Bash (MSYS2 Bash on Windows). `--cc --exe --timing` two-phase build mirrors existing `tb_top` Makefile pattern; avoids the `--binary` MSYS2 nested-make CreateProcess limitation discovered during T1 (see commit `6d29be3`).

## 3. Design — point-to-point role-1 testbench

No NoC crossbar, no router. The bridge sits between two golden gen_amba endpoints:

```
gen_amba BFM ─AXI(ID8,AD64,DA256)─→ NMU ── noc_intf (direct mosi↔miso) ── NSU ─AXI─→ gen_amba mem_axi
```

NMU/NSU NoC modports mate directly via one `noc_intf` instance. Credit stubbed → BFM-side stall is the only backpressure path. Traffic depth bounded by SV watchdog where it could otherwise hang (task G, §3.5/§3.7).

**No wb2axip protocol checker** — rev 6 originally bound the wb2axip slave checker on the BFM↔NMU side, but its non-pipelined-write model (`if (f_axi_wr_pending > 1) SLAVE_ASSERT(!awready)`) false-fires on AXI4-legal multi-beat bursts. Removed during T5 per `[[dont-silence-the-checker]]` policy: the checker is wrong about AXI4, the bridge is correct. Detection coverage falls back to (1) the DPI error pump in `tb_genamba.sv` (catches NMU/NSU c_model assertion fires) + (2) per-task SV data compares + (3) `error_flag`-trapped wrapper `$fatal`.

### 3.1 Clocking + build

`tb_genamba` is a self-clocked SV top built `--cc --exe --timing` (two-phase: Verilator generates C++ in `obj_genamba/`, then `$(MAKE) -C` compiles):
- Generates its own `ACLK`/`ARESETn` (single domain for DUT wraps + gen_amba VIP); 10 ns period (`always #5 ACLK = ~ACLK`).
- Calls `cmodel_init(scenario_path)` and the 5 `cmodel_*_create("name")` chandles **before** reset deassert, copying `tb_top.sv:70-76`.
- Uses a minimal `cosim/verilator/main_genamba.cpp` driver (~28 lines): just `topp->eval()` + `nextTimeSlot()` loop until `gotFinish()`. Does **not** reuse the existing `cosim/verilator/main.cpp` (which drives ACLK from C++ and would conflict with the SV-side `always #5` here).
- Per-cycle DPI error pump in a posedge `always` block, copying `tb_top.sv:374-388`: `c = cmodel_check_error(m); if (c) $fatal(1, m);`
- Single `final begin cmodel_finalize(); end` at end of sim (replaces main.cpp's normal finalize call).
- Does **not** use `cmodel_done()` for PASS — `master_adapter` is constructed but never ticked here; PASS comes from `error_flag==0` after all 7 BFM tasks complete.

New Verilator target `genamba` in `cosim/verilator/Makefile`: `--cc --exe --timing --top-module tb_genamba` + gen_amba `+define+`s + `-Icosim/sv/genamba`. SV source list (order matters): `ni_params_pkg.sv`, `ni_signals_pkg.sv`, `nmu_wrap.sv`, `nsu_wrap.sv`, `genamba/mem_axi.v`, `genamba_master_bfm.sv`, `tb_genamba.sv`. C source: `cmodel_dpi.cpp` + `main_genamba.cpp`. Link: `yaml-cpp`. (Stack-size flag intentionally omitted — Verilator 5.036 + MSYS2 mishandles commas in generated `.mk`.)

### 3.2 Widths + optional signals

- `mem_axi` parameterised at `AXI_WIDTH_AD=64`, `AXI_WIDTH_DA=256`, `AXI_WIDTH_ID=8`, `AXI_WIDTH_CID=0` (no crossbar → no companion ID), `SIZE_IN_BYTES=16384` (see §3.4 address allocation; rev 5 used 4096 which is insufficient).
- BFM drives the same widths.
- Build `+define+`s carry qos/cache/prot to match NMU/NSU `axi4_intf`.
- **AWREGION / ARREGION NOT marshalled by DPI wrappers** (verified: not in `nmu_wrap.sv`, `nsu_wrap.sv`, or `cmodel_dpi.cpp`). Drive BFM- and NSU-side region inputs to `4'b0`. Region transport is **untested in role 1** — DPI extension is a prerequisite.

### 3.3 Address window allocation (anti-aliasing)

`mem_axi` indexes `ADDR_LENGTH = ceil_log2(SIZE_IN_BYTES) = 14` low bits with `SIZE_IN_BYTES=16384` (`mem_axi.v:200`). All addresses below are byte offsets in `mem_axi`; bnum=16 means 16-byte narrow beats (DA=256-bit bus, half-lane).

| Task | Window (byte offsets) | Footprint | Beats × bnum |
|---|---|---|---|
| A baseline | `0x0000 – 0x00FF` (256 B) | 16 addrs × 16 B | 16 × 16 |
| B burst {4,8,16} | `0x0400 – 0x07FF` (1 KiB) | blen=16 worst-case = 1 KiB (4 outer addrs × blen=16 × bnum=16 = 1024 B); blen=4 / 8 passes reuse the same window sequentially (single owner, no aliasing) | 4 × blen × 16 |
| C outstanding {N=4, N=8} | `0x0800 – 0x09FF` (512 B) | 8 distinct addrs × 16 B = 128 B per pass | 8 × 16 |
| D outstanding burst | `0x0A00 – 0x0DFF` (1 KiB) | N=4 × blen=8 × 16 B = 512 B; reuse half per pass | up to 4 × 8 × 16 |
| E same-ID outstanding | `0x0E00 – 0x0EFF` (256 B) | 4 addrs × 16 B | 4 × 16 |
| F mixed R+W | `0x1000 – 0x11FF` (512 B) | N=4 W + N=4 R distinct addrs | 8 × 16 |
| G deep pressure | `0x1400 – 0x1FFF` (3 KiB) | N=16 × 16 B = 256 B per pass; two passes (N=8, N=16) | 16 × 16 |

Total used: ~6 KiB out of 16 KiB; remainder reserved for findings-driven extension. Each task records per-address expected data so write+single-readback at any address cannot mask aliasing across the window.

### 3.4 Components

| Item | Status | Source |
|---|---|---|
| `nmu_wrap.sv`, `nsu_wrap.sv` | Reuse unchanged | Existing |
| `cmodel_dpi.cpp` (chandle ABI) | Reuse unchanged | Existing |
| `cosim/sv/tb_genamba.sv` | NEW — self-clocked top + DPI lifecycle + BFM instance + DPI error pump + 1us watchdog | This effort |
| `cosim/sv/genamba_master_bfm.sv` | NEW — wraps `axi_master_tasks.v` + `mem_test_tasks.v` + B/R latches + R-shadow array + 6 adapter tasks (B–G; A reuses vendored `mem_test`) | This effort |
| `cosim/sv/genamba/{mem_axi,axi_master_tasks,mem_test_tasks}.v` | Vendored with project patches; see `cosim/sv/genamba/ATTRIBUTION.md` | `07fff1f` + T2/T3/T5 patches |
| `cosim/verilator/Makefile` (`genamba` target) | NEW | This effort |
| `cosim/verilator/main_genamba.cpp` (minimal Verilator main: `eval()` loop until `$finish`) | NEW | This effort (T1) |
| `cosim/verilator/run_genamba.sh` (wrapper that prepends MSYS2 paths) | NEW | This effort (T2) |

ChannelModel / Master / Slave chandles are created (per `tb_top.sv:71-73` pattern) so DPI state-machine assertions pass, but they are not ticked — the genamba BFM and `mem_axi` are the only active drivers.

### 3.5 Stimulus + checking — 7 BFM tasks

All helper tasks live in `genamba_master_bfm.sv`. Each task sets `error_flag=1` on data-compare mismatch. Upstream gen_amba pairs `error_flag` with an `always` watcher that raises `$finish(2)`; that watcher is **removed by a project patch** (Verilator X-randomization false-fires it at startup — see `ATTRIBUTION.md`). Failure termination is therefore wholly project-side: each wrapper calls `$fatal(1, "TASK X ...")` on `error_flag` rising (data mismatch, or B/R protocol-check escalation per `ATTRIBUTION.md`), guaranteeing a non-zero process exit. **Tasks run sequentially in a single `initial`; the first failure terminates the simulation and remaining tasks do not run** — this is intentional. `$display` "TASK X start/PASS" markers around each task provide aggregate reporting via the run log.

Each task uses a disjoint window per §3.3 to keep failure isolation per-task.

| # | Task | Implementation strategy | Coverage / parameter values |
|---|---|---|---|
| A | `test_baseline_mem_test` | Reuse vendored `mem_test(startA=0x0000, endA=0x00FF, bnum=16, delay=0)` — gen_amba's own self-check, INCR single-beat narrow | baseline; gen_amba self-checking |
| B | `test_burst_blen{4,8,16}` | Adapter-layer sequential per outer address: `bfm_post_aw` → `bfm_post_w` (blen beats) → `bfm_drain_b`, then `bfm_post_ar` → `bfm_drain_r` + per-beat compare. Fixed ID 0. (The vendored single-shot `axi_master_write/_read` fork AW/W/B internally and randomise IDs; combined with our forked tasks they also trip Verilator 5.036's coroutine splitter — amended in-flight, see plan Amendments.) | AXI INCR burst at `blen ∈ {4, 8, 16}`; (AXI4 INCR allows up to 256; gen_amba primitive caps at 16 — we sweep up to that cap) |
| C | `test_outstanding_N{4,8}` (parameterized fixture) | **Do not use** vendored `axi_master_*_multiple_outstanding` (broken per §2 caveat). Implement N outstanding via own wrapper that calls channel-level `axi_master_write_aw` × N (distinct AXI IDs `i+1`), then `axi_master_write_w` × N in matched order, then `axi_master_write_b` × N to drain B; reads issue via `_read_ar` × N and drain via the project-owned `bfm_drain_r` (R-shadow array; vendored `_read_r` is bypassed — see §3.6). Each address gets per-task-tracked expected data, compared after both phases complete. | N ∈ {4, 8}; distinct IDs per outstanding; stresses NMU AW/AR FIFO + NSU MetaBuffer depth |
| D | `test_outstanding_burst_N4_blen{4,8}` | Same channel-level wrapper as C but `axi_master_write_aw(..., len=blen-1)` + `axi_master_write_w` issues `blen` beats per ID | N=4 × blen ∈ {4, 8}; ROB pressure |
| E | `test_same_id_outstanding` | **Single sequencer per AW/W phase + concurrent B drain**: call `axi_master_write_aw(awid=FIXED, addr_i, ...)` × 4 serially with distinct addrs; AW issue order determines W issue order (AMBA AXI/ACE IHI 0022 §A5.3 — WID removed in AXI4, W beats follow AW issue order); then `_write_w` × 4 serially in the same order; **drain `_write_b` × 4 concurrently on a separate `fork ... join` branch** to avoid stalling the downstream B queue once W beats start landing. Reads are fully serial: `_read_ar` × 4 with shared ARID, then `bfm_drain_r` × 4 (same-ID R returns are ROB-ordered, so a concurrent drain adds nothing). Compare per-address. | **AXI4 same-ID ordering invariants** (IHI 0022 §A5.3): same-ID B responses + R returns observable in AW/AR issue order. ~30-line wrapper |
| F | `test_mixed_rw_concurrent` | Run C's write wrapper and C's read wrapper in `fork ... join` against pre-seeded distinct addrs (writes use addrs `0x1000-0x107F`, reads use seeded addrs `0x1100-0x117F`); each wrapper is a single-sequencer so no shared-driver conflict | NMU REQ + RSP plane concurrency under load |
| G | `test_deep_outstanding_pressure_N{8,16}` | Same channel-level wrapper as C with N ∈ {8, 16} (16 = vendored-primitive cap from §2 caveat). Use a flag-polling watchdog (Verilator `--timing` restricts `disable` to enclosing blocks; named-fork cancellation is fragile): <br>`fork begin : test_branch <issue/drain loops> done = 1; end join_none` then an inline `while (!done && cycle_count < WATCHDOG_CYCLES) @(posedge ACLK)` poll; `$fatal` on `!done`, `wait fork` on success (no detached thread survives the call) | **Stall ≠ deadlock evidence** under credit-stub backpressure. Watchdog heuristic in §3.7 |

### 3.6 BFM helper task implementation notes

- **Use channel-level primitives for B–G** (not the high-level single-shot `axi_master_write/_read` or `_multiple_outstanding` wrappers). Per §2 caveats, the vendored multi-outstanding helpers either have N≤16 array caps + per-address data-correspondence bugs, or randomise AXI ID. Wrapping at channel level (`axi_master_write_aw` / `_w` / `_b` / `_read_ar` / `_r`) gives us deterministic ID/address/data binding while still using vendored AXI sequencing inside each `*_aw`/`*_w` task.
- Each wrapper records `expected[addr]` before issuing, then post-issue reads back via `bfm_post_ar` + the project-owned `bfm_drain_r` and compares; mismatch sets `error_flag=1`. `bfm_drain_r` does NOT call vendored `axi_master_read_r` — it consumes an R-shadow array filled by a parallel `always` block (every `RVALID&&RREADY` handshake captures RDATA + RID/RRESP/RLAST), sidestepping the Verilator `--timing` per-beat procedural-read race, and checks the metadata per beat.
- Vendored `mem_test` (task A) is the only place we let gen_amba both drive and check; from B onwards our wrapper owns the compare.

### 3.7 Watchdog heuristic (task G)

Initial heuristic (calibration adjustment expected during plan execution):

```
beats_per_pass     = N × blen  (G uses single-beat per outstanding: blen=1, so = N)
                   = N=16 → 16 beats (per write or read phase; AW+W+B and AR+R phases multiply)
clock_period_ns    = 10  (§3.1)
ideal_min_ns       = beats_per_pass × clock_period_ns × phases ≈ 16 × 10 × 3 (AW + W + B serial worst case) = 480 ns
stall_factor       = 8   (HEURISTIC: per-beat latency under credit-stub + NoC + ROB serialisation. Existing
                          c_model/tests/common/test_loopback_latency.cpp only validates configured 2–8 tick
                          delays — does NOT establish an end-to-end stall multiplier. Calibrate from
                          measured task-G N=8 baseline before fixing the N=16 budget.)
safety_factor      = 4
WATCHDOG_NS_HEURISTIC = ideal_min_ns × stall_factor × safety_factor
                      = 480 × 8 × 4 ≈ 15 µs
```

Use `WATCHDOG_CYCLES = 2000` (≈ 20 µs at 10 ns period) in the SV `localparam` as initial value, with calibration step in plan: run G at N=8 first, measure actual completion time, set `WATCHDOG_CYCLES = measured_N8_cycles × (16/8) × 4` (safety) before running N=16. If task G fires the watchdog at the calibrated value, **that is itself the finding** — Phase 2's real credit flow control is the remediation, not a further watchdog bump.

### 3.8 Watchdog (replaces wb2axip checker)

Rev 6 originally specified wb2axip slave / master checkers tuned for outstanding pressure. Removed during T5 — the wb2axip slave checker's `if (f_axi_wr_pending > 1) SLAVE_ASSERT(!awready)` rule false-fires on AXI4-legal multi-beat writes (NMU correctly holds AWREADY high between W beats). User policy `[[dont-silence-the-checker]]` forbids `$assertoff` workarounds to mask a wrong checker, so the bind itself comes out.

Replacement coverage:
- **Per-task SV data compare** in `genamba_master_bfm.sv` — wrapper raises `error_flag=1` and `$fatal` on any per-address mismatch (already covers C–G's protocol bugs that would manifest as data corruption).
- **DPI error pump** in `tb_genamba.sv` (copies `tb_top.sv:374-388`) — surfaces any NMU/NSU c_model `cmodel_check_error` non-zero state with `$fatal`.
- **1 µs handshake-progress watchdog** in `tb_genamba.sv` — fires `$fatal` if no channel on either side completes a `valid && ready` handshake for 1 µs. Progress is handshake-based (not valid-as-activity) so a stuck transfer — VALID held high, READY never arriving — also trips it.
- **`+define+GENAMBA_DBG_AXI`** — opt-in per-cycle handshake `$display` block in `tb_genamba.sv` (default off; default runs stay quiet).

## 4. Out of scope (Phase 1)

- **Phase 2 — multi-master / role 2** — NoC bridge as AXI-fabric node, multi-instance NMU/NSU through generated gen_amba crossbar. Next spec.
- **AXI REGION transport** — DPI wrappers don't marshal it; would need a DPI extension. Tied to 0 at boundary.
- **AXI exclusive access** — `axi_master_rmw` does not exercise exclusivity on AXI4 (`lock[0]==0` after width truncation, §2). Deferred; would need a custom BFM wrapper plus exclusive monitor on `mem_axi` (gen_amba's `mem_axi` does not model exclusive monitors).
- **AXI FIXED and WRAP burst types** — gen_amba primitive's `burst` arg supports them but stress design (FIXED held-address semantics, WRAP boundary correctness) is orthogonal and deferred.
- **Injected W/R-channel backpressure via `delay=1`** — orthogonal to outstanding stress; gen_amba primitives expose the `delay` flag but Phase 1 keeps `delay=0` for deterministic latency analysis.
- **Full 256-bit single-beat** — gen_amba `get_mask` caps at 16 bytes (128-bit). Extending the mask is a gen_amba upstream contribution.
- **gen_amba's full native self-test driver** — impractically slow under `--timing`.
- **Real credit-based flow control** — cosim hard-codes `credit_return=false`; spike observes only the FIFO-full+ready=0 backpressure path. If task G's watchdog fires, that itself is the Phase-1 finding (Phase 2 prerequisite: real credit flow control).
- **Any change to DUT AXI ID width or `uint8_t` DPI ID path** — Phase 2 may revisit if xbar needs `WIDTH_SID`.

## 5. Success criteria

- `tb_genamba` builds under Verilator `--cc --exe --timing` (two-phase) as a self-clocked top with DUT DPI + gen_amba VIP coexisting in one executable (coexistence proof).
- Tasks A–G run sequentially; sim exits cleanly with `error_flag==0` (no `$fatal` from the wrapper's mismatch trap), `cmodel_check_error` returns 0 every cycle, and the 1 µs handshake-progress watchdog (§3.8) does not fire. Process exit code 0; any `$fatal` raises non-zero exit (Verilator ignores `$finish` arg, so wrapper uses `$fatal` for failure status).
- Task G's watchdog does **not** fire (`$fatal(1, "G watchdog fired ...")` does not appear in run log).
- One-page findings doc lists per-task PASS/`fail-here` outcomes, root-cause for any fail, observed protocol behaviour (e.g. same-ID R-return order at the BFM boundary, Task G stall-cycle counts), and residual work scoped for Phase 2.

## 6. Risks

- **Composition**: self-clocked SV top + `--timing` scheduler coexisting with DPI-driven beta-tick wraps in one binary — proven for individual wraps (`tb_top`), not for this composition.
- **Aliasing false-pass**: mitigated by pinned 16 KiB window + disjoint per-task sub-windows + per-address compare (§3.3).
- **Deep outstanding deadlock** (task G): stubbed credit-return + FIFO-full backpressure path — bridge must stall not deadlock; watchdog `$fatal` is terminal evidence. If it fires, Phase 2 needs real credit flow control before role 2 is meaningful.
- **Vendored primitive bugs / limits** (§2): we explicitly do not use `axi_master_*_multiple_outstanding` or `axi_master_rmw`; channel-level wrappers in §3.5 sidestep these. Wrapper correctness is the new risk surface — manageable (each wrapper ≤30 lines SV, all use vendored channel primitives).
- **Task E sequencer correctness**: same-ID write/read FIFO ordering must hold on the BFM side before we measure NMU ordering. AXI4 mandates same-ID W beats follow AW issue order and same-ID R returns follow AR issue order — wrapper enforces this by single-sequencer AW/W issue (the only fork is the concurrent B drain branch, which touches no AW/W signals); reads are fully serial.
- **Optional-signal alignment**: gen_amba BFM + `mem_axi` vs DUT `axi4_intf` qos/cache/prot defaults under the build `+define+`s.
- **Reset alignment**: DUT sync active-low `rst_ni` vs gen_amba `ARESETn` / async reset in `mem_axi`. Single-domain self-clocked top mitigates.
- **Loss of independent protocol checker** (§3.8): with wb2axip removed, real per-handshake protocol bugs (e.g. AWREADY glitching mid-burst) escape detection unless they corrupt data. Mitigation accepted because (a) the c_model bridges are already audited via the existing `tb_top` checker bind, and (b) the genamba role-1 testbench's purpose is to characterise outstanding/burst behaviour against a golden VIP, not to replace tb_top's per-handshake protocol guard.
