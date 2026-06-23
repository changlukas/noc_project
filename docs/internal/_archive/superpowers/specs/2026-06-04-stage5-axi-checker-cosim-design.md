# Stage 5 NMU/NSU AXI4 boundary conformity co-sim (narrow PoC) — design

**Status**: design (brainstorm output, awaiting spec review + writing-plans)
**Date**: 2026-06-04
**Branch**: TBD (new branch off `main` @ 32c2d5a, after Stage 3 NI closed)
**Predecessors on `main`**: Stage 3 NI closed (`nmu::Nmu` + `nsu::Nsu` top-level shipped; ctest 393/393 green)

## 1. Motivation

Main plan §5 (`docs/noc_cmodel_rtl_plan.md`) describes a full mixed C-model / RTL co-sim with three ComponentHandle backends and a 5-scenario mix-and-match matrix. That's a large investment.

Before paying that cost, we want a **narrow first foothold**: does `Nmu` / `Nsu` produce AXI4-conformant traffic at the AXI boundary at all? If not, fix the C-model first; if yes, the rest of Stage 5 has a baseline.

The first foothold is: bind an OSS AXI4 SVA protocol checker on the C-model's two AXI boundaries (NMU manager-facing, NSU memory-facing) and let it observe traffic produced by the existing C++ testbench.

## 2. Scope

**In scope**:
- New `c_model/include/cosim/` headers — DPI adapter wrapping the existing C++ tick loop.
- New `cosim/` tree — SV testbench shell, DPI bridge, vendored ZipCPU wb2axip, Verilator build harness, ctest entry.
- Verilator 5.x on Windows MSYS2 native build path.
- One GoogleTest entry that drives 2–3 existing YAML scenarios through the co-sim path.

**Out of scope** (deferred to follow-up rounds):
- ComponentHandle three-backend abstraction (main plan §5.2).
- Swapping RTL `nmu.sv` / `nsu.sv` into the loop (main plan §5.3 mix matrix).
- Stage 4 NoC fabric multi-NI co-sim.
- VCS sign-off port (separate follow-up; spec design preserves this path 0-rewrite).
- Async clock domain crossing (this round assumes `aclk == noc_clk`).
- Expanded scenario coverage beyond the smoke set (narrow / WRAP / unaligned / exclusive added in later rounds).

Existing c_model `Nmu` / `Nsu` / `AxiMaster` / `AxiSlave` / `memory` / `scoreboard` are **not modified**.

## 3. Anchored decisions

| Decision | Choice | Rationale |
|---|---|---|
| Verification approach | C-model logic + DPI export pin snapshots + SV-side passive SVA observer | Wire-level visibility required for SVA bind; existing C++ test infra reused as stimulus |
| Boundary cut | Both AXI(1) NMU side + AXI(2) NSU side | Both are DUT; fail-localization needs per-boundary checker |
| Stimulus / endpoint source | Existing C++ `axi_master.hpp` + `axi_slave.hpp` + `memory.hpp` | CLAUDE.md reuse principle; already verified through ctest 393/393 |
| SVA checker OSS | ZipCPU wb2axip `bench/formal/faxi_master.v` + `faxi_slave.v` | Apache 2.0; AXI4-Full; immediate-assertion + `$past` / `$stable` only (no `sequence`/`##`/`disable iff`) → Verilator-runnable; dual-use to VCS with 0 file change |
| DPI direction | (b) SV master — HDL kernel owns clock, DPI imports call C++ tick service | IEEE 1800 DPI design intent; aligns with `dpi_ref/sdram` pattern; same SV shell runs on Verilator and VCS |
| Simulator (this round) | Verilator 5.044 stable on Windows MSYS2 native | wb2axip needs no SVA features beyond 5.044 baseline |
| Simulator (sign-off) | Same SV shell ported to VCS — 0 .sv / .c changes | Apache 2.0 dual-use; SV master direction is portable |
| wb2axip integration | Vendor under `cosim/sv/wb2axip/` with commit hash + ATTRIBUTION.md | Match existing `c_model/include/axi/ATTRIBUTION.md` pattern |
| Ceremony scale | Short spec (this doc); narrow PoC; no risk matrix | Match scope per memory `feedback-match-ceremony-to-feature-complexity` |

## 4. Architecture

### 4.1 End-to-end view

```
                    C++ side (DPI service provider)
   ┌──────────────────────────────────────────────────────────────┐
   │ ┌──────────┐  ┌──────────────────────────────┐  ┌──────────┐ │
   │ │AxiMaster │─►│ Nmu ── NoC loopback stub ── │─►│AxiSlave +│ │
   │ │ (reuse)  │  │ Nsu  (existing C++ DUTs)    │  │memory    │ │
   │ └──────────┘  └──────────────────────────────┘  └──────────┘ │
   │       │              │              │                │       │
   │       └──┬───────────┘              └────────┬───────┘       │
   │          ▼                                   ▼               │
   │     AXI(1) pin snapshot              AXI(2) pin snapshot     │
   │     (AwBeat/WBeat/ArBeat/             (same struct family)   │
   │      BBeat/RBeat per channel)                                │
   │          ▲                                   ▲               │
   │          │   DPI exports called per HDL clock tick           │
   └──────────│───────────────────────────────────│───────────────┘
              │                                   │
   ┌──────────│───────────────────────────────────│───────────────┐
   │   SV testbench shell  (Verilator 5.044 → VCS, same .sv)      │
   │          │                                   │               │
   │   ┌──────▼─────────────┐           ┌─────────▼───────────┐   │
   │   │ nmu_cmodel_proxy   │           │ nsu_cmodel_proxy    │   │
   │   │  (DPI driver →     │           │  (DPI driver →      │   │
   │   │   AXI bundle wire) │           │   AXI bundle wire)  │   │
   │   └────────┬───────────┘           └─────────┬───────────┘   │
   │            │ axi_if (1)                      │ axi_if (2)    │
   │   ┌────────▼───────────┐           ┌─────────▼───────────┐   │
   │   │ faxi_slave.v       │           │ faxi_master.v       │   │
   │   │ (wb2axip vendor —  │           │ (wb2axip vendor —   │   │
   │   │  asserts NMU       │           │  asserts NSU        │   │
   │   │  manager-facing    │           │  memory-facing      │   │
   │   │  AXI4 legality)    │           │  AXI4 legality)     │   │
   │   └────────────────────┘           └─────────────────────┘   │
   │             clock / reset gen (tb_axi_conformity)            │
   └──────────────────────────────────────────────────────────────┘
```

### 4.2 Per-cycle timeline

```
posedge clk in SV
   │
   ├── (proxy) cmodel_tick()      ─►  C++: tick AxiMaster, Nmu, stub, Nsu, AxiSlave once
   │
   ├── (proxy) cmodel_nmu_get_aw/w/ar/b/r(...)  ─►  C++: snapshot AXI(1) pin state
   ├── (proxy) cmodel_nsu_get_aw/w/ar/b/r(...)  ─►  C++: snapshot AXI(2) pin state
   │
   ├── proxy assign snapshots to axi_if bundle wires
   │
   └── faxi_slave / faxi_master observe wires
         │
         ├─ if (rule violated)  ──►  $error("..."); — Verilator harness exits non-zero
         └─ else                 ──►  next cycle
```

C-model `Nmu` / `Nsu` modules are unchanged — they continue to expose the existing `tick()` API used by integration testbenches today. The new `axi_dpi_adapter` wraps the whole orchestrated tick (master + nmu + stub + nsu + slave) as one DPI service entry.

### 4.3 File layout

```
c_model/include/cosim/
├── pin_snapshot.hpp            # per-channel POD struct; bit-for-bit aligned with axi/types.hpp
└── axi_dpi_adapter.hpp         # AxiDpiAdapter: owns master/nmu/stub/nsu/slave/scoreboard;
                                #   provides tick() + per-channel snapshot getters

cosim/
├── sv/
│   ├── tb_axi_conformity.sv    # TB top: clock + reset + proxy + checker bind
│   ├── nmu_cmodel_proxy.sv     # DPI-driven proxy: C++ side AXI(1) → SV wire
│   ├── nsu_cmodel_proxy.sv     # symmetric: AXI(2)
│   ├── axi_if.sv               # AXI bundle interface (parameterized widths)
│   └── wb2axip/
│       ├── faxi_master.v       # vendored, Apache 2.0, commit pinned
│       ├── faxi_slave.v        # vendored, Apache 2.0, commit pinned
│       ├── sim_wrapper.svh     # SLAVE_ASSUME→assert + f_past_valid init macros
│       └── ATTRIBUTION.md      # per c_model/include/axi/ATTRIBUTION.md style
├── c/
│   └── axi_dpi.c               # DPI export bodies; thin glue to AxiDpiAdapter
├── verilator/
│   ├── Makefile                # Windows MSYS2 native; pinned Verilator 5.044
│   └── main.cpp                # Verilator harness top — verilated TB instance
└── tests/
    └── test_nmu_nsu_axi_conformity.cpp   # GoogleTest entry; ctest hook
```

`Nmu` / `Nsu` class internals are not touched.

## 5. DPI interface

### 5.1 Granularity decision

Per-channel struct (AW / W / AR / B / R each get one getter). Not per-signal (too chatty), not per-bundle (poor debug). Each struct mirrors the existing `c_model/include/axi/types.hpp` definitions field-for-field — no parallel struct universe.

### 5.2 Function signatures (sketch — exact widths set in implementation phase)

```c
/* lifecycle */
void cmodel_init(const char *scenario_yaml_path);
void cmodel_finalize(void);

/* per-cycle tick */
void cmodel_tick(void);

/* NMU AXI(1) — Nmu is the subordinate */
void cmodel_nmu_get_aw(svBit *valid, svBit *ready,
                       svBitVecVal *id, svBitVecVal *addr,
                       svBitVecVal *len, svBitVecVal *size, svBitVecVal *burst,
                       svBitVecVal *lock, svBitVecVal *cache,
                       svBitVecVal *prot, svBitVecVal *qos);
void cmodel_nmu_get_w (svBit *valid, svBit *ready,
                       svBitVecVal *data, svBitVecVal *strb, svBit *last);
void cmodel_nmu_get_ar(/* same as aw */);
void cmodel_nmu_get_b (svBit *valid, svBit *ready,
                       svBitVecVal *id, svBitVecVal *resp);
void cmodel_nmu_get_r (svBit *valid, svBit *ready,
                       svBitVecVal *id, svBitVecVal *data,
                       svBitVecVal *resp, svBit *last);

/* NSU AXI(2) — symmetric, Nsu is the manager */
void cmodel_nsu_get_aw(/* same shape as nmu */);
/* ... */
```

### 5.3 Scenario control

Scenario YAML stays on the C++ side, reusing `c_model/include/axi/scenario_parser.hpp`. The Verilator harness passes the path via plusarg:

```
./Vtb_axi_conformity +scenario=sim/test_patterns/burst_incr_8beat.yaml
```

`cmodel_init` parses the YAML and primes the existing AxiMaster, exactly as today's integration testbench does.

## 6. wb2axip vendoring

- Drop the two `.v` files into `cosim/sv/wb2axip/` verbatim.
- Pin upstream commit hash in `cosim/sv/wb2axip/ATTRIBUTION.md` (Apache 2.0 attribution + modification log; style matches `c_model/include/axi/ATTRIBUTION.md`).
- **No edits to upstream `.v`**. Adapt via `sim_wrapper.svh` (header-include) that:
  - Maps `SLAVE_ASSUME` → `assert` (formal-only construct in simulation).
  - Maps standalone `assume(...)` → `assert(...)` for simulation mode.
  - Provides `f_past_valid` initialization guard (Verilator init-X protection per Dan Gisselquist's documented caveat).

Modification log entry in ATTRIBUTION.md will state: "no source files modified; behavioral shim provided via sim_wrapper.svh."

## 7. Test plan + Karpathy 4-lens

### 7.1 Drift gates (per CLAUDE.md quality-gates)

```
cd specgen
py -3 -m pytest -q
py -3 tools/codegen.py --check
py -3 tools/gen_inventory.py --check
cd ../c_model && cmake --build build && ctest --test-dir build -j 1
```

Existing 393/393 baseline holds. The new cosim test adds 1 ctest entry; expected `394/394` after this round.

### 7.2 Smoke scenarios

The new GoogleTest entry runs three existing YAML scenarios end-to-end through the Verilator harness:
- `burst_incr_8beat.yaml` — basic INCR
- `4kb_cross.yaml` — boundary edge case
- `multi_outstanding_stress.yaml` — multi-outstanding ordering

Pass criterion: `$error()` never fires + Verilator exit 0 + GoogleTest pass.

### 7.3 Karpathy 4-lens

- **Overcomplication?** Narrow PoC; no ComponentHandle abstraction; no RTL swap; no Stage 4 fabric. Single new tree (`cosim/`) plus two cosim headers.
- **Surgical?** `Nmu` / `Nsu` / `axi/` headers untouched. New code is purely additive.
- **Surface assumptions**:
  - wb2axip immediate-assertion subset runs cleanly on Verilator 5.044 — SV construct audit confirmed (no `sequence` / `##` / `disable iff`), but Dan's header note "not intended to be functional" implies 1–2 days of integration friction (macro wrapping, init-reg).
  - C-model tick advances one unified clock per DPI call — current scope only (`aclk == noc_clk`); when §5.2 multi-backend lands this must be re-evaluated.
  - DPI per-channel struct shape exactly mirrors `axi/types.hpp` — no parallel struct definitions.
- **Verifiable success?** Three smoke scenarios run wb2axip-clean → ctest 394/394. Any wb2axip property fire surfaces as `$error()` quoting the property identifier (e.g., `ap_AW_STABLE_AWID`) — straight pointer to AXI4 IHI 0022 rule.

## 8. Open items deferred to writing-plans / implementation

These do not affect architecture; they are concrete implementation decisions to settle during the next skill step:

- Exact bit widths in DPI struct (driven by existing `axi/types.hpp` typedefs).
- `axi_if.sv` parameterization style (`interface` with modport vs. plain bundle).
- Verilator build flags and MSYS2 toolchain pinning.
- Exact SV macro syntax in `sim_wrapper.svh` (behavioral semantics already pinned in §6).

## 9. References

- Main plan: `docs/noc_cmodel_rtl_plan.md` §5 (mixed co-sim), §6 (file tree pre-reservation under `cosim/`)
- OSS: https://github.com/ZipCPU/wb2axip — `bench/formal/faxi_master.v`, `bench/formal/faxi_slave.v` (Apache 2.0)
- DPI pattern reference: `dpi_ref/sdram/{axi_dram_wrapper.sv, dram_dpi.sv, dram_dpi.c}` (junior-style; SV-master direction same; pin granularity differs — junior is per-cell, this design is per-channel)
- Existing attribution style: `c_model/include/axi/ATTRIBUTION.md`
- IEEE 1800 SystemVerilog DPI-C (HDL kernel as timing master)
- AMBA AXI4 spec IHI 0022 (rules wb2axip property IDs reference)
