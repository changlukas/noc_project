# specgen Handshake Upstream + rtl-forge Style Refactor — Design Spec

**Date:** 2026-06-06
**Status:** Design — pending user review
**Branch:** stage5b/dpi-wire-wrap (continuation)
**Supersedes:** none
**References:** Arm AMBA AXI/ACE IHI 0022H §A2.2–A2.6, §A3.1.2, §A3.2.1, §A9.3 (Tables A9-1 to A9-4)

---

## 1. Motivation & Scope

### 1.1 Motivation

Stage 5b shipped a working co-sim by hand-writing three SystemVerilog interfaces in `cosim2/sv/` (`axi_intf.sv`, `noc_req_intf.sv`, `noc_rsp_intf.sv`) plus five wrap modules. These hand-written interfaces are **not aligned** with specgen-generated `ni_signals_pkg.sv`:

- specgen has no handshake fields (no `valid` / `ready` / `credit_return`)
- specgen splits NoC into 4 endpoint-directional interfaces (`*_out_intf` / `*_in_intf`)
- specgen has no modports
- specgen uses hardcoded signal widths, no SystemVerilog parameters

The dual source of truth introduces drift risk — every NI signal change has to be applied in two places. Additionally specgen's emission style does not follow the project's rtl-forge convention (industry-standard `_i / _o / _ni` module-port suffix, parameterized interfaces, modport-based direction).

### 1.2 In scope

| Work item | Description |
|---|---|
| W1 | Upstream the handshake/parameter schema into specgen source (new `constants.yaml`, new `interface_handshake.json`, extend `signal_interface.md`). |
| W2 | Atomic PR: refactor `specgen/ni_spec/generator.py` to emit industry-standard SV interfaces; regenerate `specgen/generated/`; migrate `cosim2/sv/` (delete hand-written interfaces; rewrite 5 wraps + `tb_top.sv`). |

### 1.3 Out of scope

- Real RTL `nmu.sv` / `nsu.sv` implementation (Stage 5+).
- wb2axip replacement protocol checker (Stage 5c+).
- VCS DPI-RTL backend port (Stage 5c+).
- Functional changes to c_model behavior.
- AxUSER / WUSER / BUSER / RUSER signals (cosim2 currently does not carry them; future spec may add).
- **Release-level quality sweep** (Karpathy 4-lens + magic-number hunt + lint + Verilator warning-clean + parameter sweep + sanitizer + coverage + reproducible generation + fault injection + release tag). Each of these gates is a non-trivial sub-spec (real coverage parsing, scenario-parser extension for fault injection, tb_top parameter forwarding for sweep, etc.). Moved to a follow-up spec — see §7 Open Items.

### 1.4 Success criteria

1. `cosim2/sv/` contains no hand-written `*_intf.sv`. All interfaces come from `specgen/generated/sv/`.
2. Existing `cosim2` ctest suite (410 tests) stays green; existing 5 fixture smoke stays green; existing drift gate stays clean.
3. specgen pytest suite green after schema + emitter additions, with new tests covering all §4.2 validator rules.

---

## 2. Anchored Conventions

### 2.1 Convention bundle (the "industry-standard" choice)

| Layer | Convention | Rationale |
|---|---|---|
| AXI signal naming (inside interface) | Neutral lowercase AMBA name (`awvalid`, `awready`, `awaddr`, `wdata`, `bresp`, ...) — no `_i / _o` suffix | IHI 0022H §A9.3; direction is relative to master/slave and cannot be encoded in a single suffix |
| AXI interface count | **Single `axi4_intf`** with `master` / `slave` modports | Signal set is identical between master-side and slave-side instantiations; only direction (modport) differs |
| AXI interface required signals | Per IHI 0022H §A9.3 Tables A9-1 to A9-4 (per-role matrix; not a flat "required + optional") | Manager and Memory Subordinate roles differ; e.g., `AWPROT` required for Manager, optional for Memory Subordinate |
| AXI signal fixed widths | `AxLEN=8`, `AxSIZE=3`, `AxBURST=2`, `AxLOCK=1`, `AxCACHE=4`, `AxPROT=3`, `AxQOS=4`, `AxREGION=4`, `xRESP=2`, `WSTRB_WIDTH=DATA_WIDTH/8` | IHI 0022H §A2.2–A2.6 |
| AXI modport names | `master / slave` (legacy) — generator emits this set | IHI 0022H §A1.3 uses Master/Slave; Arm current terminology is Manager/Subordinate as a synonym alias. Single emission for reading consistency; future generator flip may add Manager/Subordinate aliases |
| NoC signal naming | Neutral (`valid`, `flit`, `credit_return`) — no `noc_req_` prefix (interface name already namespaces) | Codex review: prefix duplicates interface namespace |
| NoC interface count | **Two interfaces** (`noc_req_intf`, `noc_rsp_intf`) — consolidates current 4 (`*_out_intf` / `*_in_intf`) | Endpoint-direction split is per-instantiation concern (encoded by modport), not per-interface concern |
| NoC modport names | `master / slave` (driver / receiver) — aligned with AXI | Single terminology across both interface families simplifies generator and consumer code |
| Interface parameters | AXI: `ID_WIDTH`, `ADDR_WIDTH`, `DATA_WIDTH`; NoC: `NUM_VC`, `FLIT_WIDTH`, `SLAVE_VC_BUFFER_DEPTH` | All defaults sourced from `constants.yaml` (see §5.1) |
| Clock/reset | `clk_i` / `rst_ni` NOT inside interface; carried by module port list | Vendor-IP-compatible (AMD PG115, ARM IP catalogue convention); combined module boundary — not interface alone — is AXI compliant |
| Reset semantics | `rst_ni` active-low, async assert OK, **sync deassert** | IHI 0022H §A3.1.2 |
| Module port suffix | `_i` / `_o` / `_ni` — **project convention**, not general SV semantic | rtl-forge style; documents data-flow intent at module boundary. Modport selection is independent of port suffix |
| External vendor IP wrapper | Flatten modport to `m_axi_*` / `s_axi_*` ports | Future, out of this spec scope; required for Vivado / Quartus IP integrator |

### 2.2 Naming discipline

Project-defined identifiers use full words. Industry-standard acronyms remain abbreviated.

| Category | Rule | Example |
|---|---|---|
| Project parameter / constant | Full word | `FLIT_WIDTH` (not `FLIT_W`), `ID_WIDTH`, `ADDR_WIDTH`, `DATA_WIDTH` |
| Industry-standard acronym | Preserve | `VC`, `ID`, `AXI`, `AMBA`, `NoC`, `ROB`, `DPI`, `NMU`, `NSU` |
| SystemVerilog community suffix | Preserve | `_intf`, `_pkg` (project standardizes on `_intf`; `_if` not used) |
| AMBA signal name | Per IHI 0022H lowercase form | `awvalid`, `wlast`, `bresp` (never expanded) |
| C++ identifier | camelCase per existing c_model style | `kNiNocFlitWidth` (not `kNiNocFlitW`) |
| SV parameter default symbol | UPPER_SNAKE_CASE with `_DFLT` suffix | `NI_NOC_FLIT_WIDTH_DFLT` |

Generator enforces: any parameter name matching `*_W$` (W ending without IDTH) → validation error. Any parameter name containing lowercase → validation error.

### 2.3 Deferred (intentionally not changed)

- `ni_regs_pkg.sv` — register interface schema; not in this scope.
- `ni_flit_pkg.sv` — flit struct definitions; aligned with c_model header; not in this scope.
- `cosim2/sv/wb2axip/` — vendored Apache 2.0 source; frozen.

---

## 3. Architecture & Phase Ordering

### 3.1 Source-of-truth chain (after refactor)

```
spec/ni/doc/signal_interface.md           (handshake/parameter sections generator-emitted)
specgen/source/ni_function_blocks.json    (feature inventory — unchanged)
specgen/source/constants.yaml             (NEW — language-neutral constants source)
specgen/source/interface_handshake.json   (NEW — handshake + modport schema)
                |
                v  specgen/ni_spec/generator.py  (REFACTORED)
                |
specgen/generated/
    +-- sv/ni_params_pkg.sv               (NEW — SV parameter defaults)
    +-- sv/ni_signals_pkg.sv              (REGEN — industry-style interfaces)
    +-- sv/ni_flit_pkg.sv                 (unchanged)
    +-- sv/ni_regs_pkg.sv                 (unchanged)
    +-- cpp/ni_params.h                   (NEW — C++ constants)
    +-- cpp/ni_flit_constants.h           (unchanged for non-parameter constants)
    +-- cpp/ni_signals.h                  (REGEN — keeps c_model alignment)
    +-- cpp/ni_regs.h                     (unchanged)
                |
                v  consumed by
                |
cosim2/sv/                                (W2 migration)
    +-- nmu_wrap.sv, nsu_wrap.sv,
    +-- axi_master_wrap.sv, slave_wrap.sv,
    +-- noc_wrap.sv, tb_top.sv
    (REMOVED: axi_intf.sv, noc_req_intf.sv, noc_rsp_intf.sv)
```

### 3.2 Phase plan

| Phase | Work | Drift gate | Commit granularity |
|---|---|---|---|
| **W1** | Add `constants.yaml`, `interface_handshake.json`, extend `signal_interface.md`. Generator does NOT yet consume new fields. | Clean | Single PR. `feat(specgen): add handshake schema + language-neutral constants` |
| **W2** | Atomic PR: refactor generator emission → regenerate `specgen/generated/` → migrate `cosim2/sv/` (delete hand-written intf, rewrite 5 wraps + tb_top). | Final tree green; intermediate sub-commits may be elaboration-broken (reviewable but not CI-gated) | Single PR, multiple review-only sub-commits. Final CI gate: ctest 410/410 + drift gate + Verilator warning-clean elaboration |
| **W3** | Release-level quality sweep. No code changes outside what findings dictate. | N/A | One PR per zone (A: Stage 3 c_model core; B: Stage 5b cosim; C: specgen). Magic-number fixes split from architectural fixes |

W2 is intentionally non-bisectable mid-PR because changing the interface contract is atomic with all consumers. Final-tree CI gates the merge. The sub-commit ordering inside W2 is for code-review readability, not for git-bisect.

### 3.3 Invariants preserved from Stage 5b

- **β-tick discipline**: all SV wraps register their outputs (one cycle latency per hop).
- **Hermetic singleton**: cross-component data only via SV wires; DPI exports do not call across components directly.
- **wb2axip frozen**: any signal-name impedance mismatch is resolved at the wrap port-connection level, not by modifying vendored source.

### 3.4 Boundary translation responsibility

| Boundary | Translator | Spec area |
|---|---|---|
| spec source ↔ generated SV | `generator.py` | §5 |
| generated SV interface ↔ wrap module port | wrap port list (`_i / _o / _ni` + modport) | §6 |
| wrap C++ side ↔ DPI | existing `cosim2/c/dpi_bridge.cpp` | not in scope |
| specgen interface ↔ vendor IP (Vivado / Quartus) | future flatten wrapper | not in scope |

---

## 4. W1 — Source Schema Extension

### 4.1 New file: `specgen/source/constants.yaml`

Language-neutral single source of truth. Generator emits both SV (`ni_params_pkg.sv`) and C++ (`ni_params.h`) from this file. Eliminates the prior dual ownership where `ni_flit_constants.h` was referenced from SV emission without a clear contract.

```yaml
schema_version: "1.0"

axi:
  ID_WIDTH:
    type: int
    units: bits
    description: "AXI transaction ID width"
    default: 8
    min: 1
    max: 32
    sv_symbol: NI_AXI_ID_WIDTH_DFLT
    cpp_symbol: kNiAxiIdWidth

  ADDR_WIDTH:
    type: int
    units: bits
    description: "AXI address bus width"
    default: 64
    min: 1
    max: 64
    sv_symbol: NI_AXI_ADDR_WIDTH_DFLT
    cpp_symbol: kNiAxiAddrWidth

  DATA_WIDTH:
    type: int
    units: bits
    description: "AXI data bus width"
    default: 256
    allowed: [32, 64, 128, 256, 512, 1024]
    sv_symbol: NI_AXI_DATA_WIDTH_DFLT
    cpp_symbol: kNiAxiDataWidth

noc:
  NUM_VC:
    type: int
    units: count
    description: "Number of virtual channels per NoC link"
    default: 1
    min: 1
    max: 8
    sv_symbol: NI_NOC_NUM_VC_DFLT
    cpp_symbol: kNiNocNumVc

  FLIT_WIDTH:
    type: int
    units: bits
    description: "NoC flit payload width"
    default: 408
    min: 64
    max: 1024
    sv_symbol: NI_NOC_FLIT_WIDTH_DFLT
    cpp_symbol: kNiNocFlitWidth

  SLAVE_VC_BUFFER_DEPTH:
    type: int
    units: flits
    description: "Per-VC buffer depth at NoC slave; defines initial credit count exposed to producer"
    default: 4
    min: 1
    max: 64
    sv_symbol: NI_NOC_SLAVE_VC_BUFFER_DEPTH_DFLT
    cpp_symbol: kNiNocSlaveVcBufferDepth

derived:
  WSTRB_WIDTH:
    type: int
    units: bits
    description: "AXI write strobe width"
    expression: "DATA_WIDTH / 8"
    constraint: "DATA_WIDTH % 8 == 0"
    sv_symbol: NI_AXI_WSTRB_WIDTH_DFLT
    cpp_symbol: kNiAxiWstrbWidth
```

### 4.2 Generator parsing rules

- Unknown top-level keys → validation error.
- Unknown per-parameter fields → validation error.
- Missing required field (`type`, `default`, `sv_symbol`, `cpp_symbol`) → validation error.
- `default` must satisfy `min` / `max` / `allowed` / `constraint` → validation error otherwise.
- `derived.*.expression` evaluated only over previously defined parameters; circular reference → error.

### 4.3 New file: `specgen/source/interface_handshake.json`

Schema for the four interfaces (one AXI, two NoC). The AXI consolidation per §2.1 means a single `axi4_intf` entry.

```json
{
  "schema_version": "1.0",
  "interfaces": {
    "axi4_intf": {
      "kind": "axi4",
      "parameters": [
        { "name": "ID_WIDTH",   "constants_yaml_key": "axi.ID_WIDTH"   },
        { "name": "ADDR_WIDTH", "constants_yaml_key": "axi.ADDR_WIDTH" },
        { "name": "DATA_WIDTH", "constants_yaml_key": "axi.DATA_WIDTH" }
      ],
      "channels": ["AW", "W", "B", "AR", "R"],
      "signal_matrix_source": "IHI_0022H_TABLE_A9_1_to_A9_4",
      "modports": ["master", "slave"]
    },

    "noc_req_intf": {
      "kind": "noc_link",
      "parameters": [
        { "name": "NUM_VC",                "constants_yaml_key": "noc.NUM_VC" },
        { "name": "FLIT_WIDTH",            "constants_yaml_key": "noc.FLIT_WIDTH" },
        { "name": "SLAVE_VC_BUFFER_DEPTH", "constants_yaml_key": "noc.SLAVE_VC_BUFFER_DEPTH" }
      ],
      "signals": [
        { "name": "valid",         "width_expr": "1",          "driven_by": "master" },
        { "name": "flit",          "width_expr": "FLIT_WIDTH", "driven_by": "master" },
        { "name": "credit_return", "width_expr": "NUM_VC",     "driven_by": "slave"  }
      ],
      "modports": ["master", "slave"],
      "protocol_semantics": {
        "transfer_condition": "producer-internal credit counter for selected VC > 0 AND valid asserted; selected VC encoded inside flit header (NOT a separate interface signal)",
        "credit_return_encoding": {
          "scheme": "per_vc_credit_pulse_vector",
          "semantics": "credit_return[v] asserted for exactly 1 cycle releases 1 credit for VC v. Multi-hot is permitted — multiple VCs may release credits in the same cycle.",
          "output_discipline": "registered (no combinational path from valid input to credit_return output)",
          "reset_value": "all zeros",
          "onehot_check_required": false
        },
        "initial_credits": {
          "scheme": "configured_at_reset",
          "value_per_vc": "SLAVE_VC_BUFFER_DEPTH",
          "note": "Producer assumes SLAVE_VC_BUFFER_DEPTH credits per VC available after rst_ni deassert; slave guarantees buffer has SLAVE_VC_BUFFER_DEPTH free slots per VC at reset exit"
        },
        "valid_stability": "once valid asserted, valid + flit MUST hold until handshake completes (always-accept on slave side; backpressure is purely via producer credit exhaustion upstream of this interface). See IHI 0022H §A3.2.1 analog.",
        "combinational_loops": "forbidden — credit_return must be a registered output of the slave"
      }
    },

    "noc_rsp_intf": {
      "kind": "noc_link",
      "parameters": [
        { "name": "NUM_VC",                "constants_yaml_key": "noc.NUM_VC" },
        { "name": "FLIT_WIDTH",            "constants_yaml_key": "noc.FLIT_WIDTH" },
        { "name": "SLAVE_VC_BUFFER_DEPTH", "constants_yaml_key": "noc.SLAVE_VC_BUFFER_DEPTH" }
      ],
      "signals": [
        { "name": "valid",         "width_expr": "1",          "driven_by": "master" },
        { "name": "flit",          "width_expr": "FLIT_WIDTH", "driven_by": "master" },
        { "name": "credit_return", "width_expr": "NUM_VC",     "driven_by": "slave"  }
      ],
      "modports": ["master", "slave"],
      "protocol_semantics": "same as noc_req_intf"
    }
  }
}
```

### 4.4 `signal_interface.md` extension

Add a `## Handshake & Modport Convention` section. Content is **generator-emitted** from `interface_handshake.json` — no hand-edited prose that can drift from JSON.

Add a `## AXI4 Signal Matrix (per IHI 0022H §A9.3)` section. Content is also generator-emitted: a per-role table for each of the 5 channels (AW, W, B, AR, R), with columns Manager-required / Memory-Subordinate-required, width source, and reset value.

### 4.5 W1 commit boundary

Single PR. Generator change in this PR is only loader + validator stubs; no SV / C++ emission change. Existing `ni_params_pkg.sv` / `ni_params.h` files are not yet created (they appear in W2). Drift gate stays clean because no generated artifact changes.

PR title: `feat(specgen): add handshake schema + language-neutral constants source`

---

## 5. W2 — Atomic Refactor + Regenerate + Migrate

### 5.1 Generator refactor scope (`specgen/ni_spec/generator.py`)

New functions:

| Function | Purpose |
|---|---|
| `_load_constants_yaml()` | Parse `constants.yaml`, validate per §4.2 rules |
| `_load_handshake_schema()` | Parse `interface_handshake.json`, cross-check parameter references against `constants.yaml` |
| `_emit_sv_params_pkg()` | Emit `specgen/generated/sv/ni_params_pkg.sv` with parameter defaults |
| `_emit_cpp_params_header()` | Emit `specgen/generated/cpp/ni_params.h` with `constexpr` constants |
| `_emit_sv_interface()` (rewrite) | Parameterized header + modport block emission; consume `interface_handshake.json` |
| `_emit_sv_signal_decl()` (rewrite) | Use SV parameter (`logic [ID_WIDTH-1:0]`) not hardcoded literal |
| `_emit_sv_modport()` | Read `driven_by`, emit `master` / `slave` modport with comma-separated entries |
| `_consolidate_noc_interfaces()` | Merge the prior 4 `*_out_intf` / `*_in_intf` into 2 bundled interfaces |
| `_emit_signal_interface_md()` | Generator-emit the handshake table and AXI signal matrix into `signal_interface.md` |
| `_validate_naming_consistency()` | Reject `*_W$` parameter names; reject lowercase parameter names |

Removed code paths:

- Endpoint-direction-split NoC emission (4-interface variant).
- Hardcoded width literal emission.
- The single `endpoint` modport.

Unchanged:

- `_emit_cpp_constants()` for non-parameter constants stays in `ni_flit_constants.h`.
- `_emit_sv_flit_pkg()` — `ni_flit_pkg.sv` not in scope.
- `_emit_sv_regs_pkg()` — register interface not in scope.

Magic-number cleanup inside `generator.py` itself is scoped narrowly: only literals that are about to be replaced by `constants.yaml`-driven values. Other generator-internal literals are not touched in W2 (they go to W3 zone C sweep).

### 5.2 Generated SV interface — final form

```systemverilog
// specgen/generated/sv/ni_params_pkg.sv
package ni_params_pkg;
    parameter int unsigned NI_AXI_ID_WIDTH_DFLT               = 8;
    parameter int unsigned NI_AXI_ADDR_WIDTH_DFLT             = 64;
    parameter int unsigned NI_AXI_DATA_WIDTH_DFLT             = 256;
    parameter int unsigned NI_AXI_WSTRB_WIDTH_DFLT            = 32;
    parameter int unsigned NI_NOC_NUM_VC_DFLT                 = 1;
    parameter int unsigned NI_NOC_FLIT_WIDTH_DFLT             = 408;
    parameter int unsigned NI_NOC_SLAVE_VC_BUFFER_DEPTH_DFLT  = 4;
endpackage

// specgen/generated/sv/ni_signals_pkg.sv (excerpt)
interface axi4_intf #(
    parameter int unsigned ID_WIDTH   = ni_params_pkg::NI_AXI_ID_WIDTH_DFLT,
    parameter int unsigned ADDR_WIDTH = ni_params_pkg::NI_AXI_ADDR_WIDTH_DFLT,
    parameter int unsigned DATA_WIDTH = ni_params_pkg::NI_AXI_DATA_WIDTH_DFLT
);
    localparam int unsigned WSTRB_WIDTH = DATA_WIDTH / 8;

    logic [ID_WIDTH-1:0]    awid;
    logic [ADDR_WIDTH-1:0]  awaddr;
    logic [7:0]             awlen;
    logic [2:0]             awsize;
    logic [1:0]             awburst;
    logic                   awlock;
    logic [3:0]             awcache;
    logic [2:0]             awprot;
    logic [3:0]             awqos;
    logic [3:0]             awregion;
    logic                   awvalid;
    logic                   awready;
    // ... W, B, AR, R channels (full A9.3 set per per-role matrix) ...

    modport master (
        output awid, awaddr, awlen, awsize, awburst, awlock, awcache,
               awprot, awqos, awregion, awvalid,
        input  awready
        // ... rest of master directions ...
    );

    modport slave (
        input  awid, awaddr, awlen, awsize, awburst, awlock, awcache,
               awprot, awqos, awregion, awvalid,
        output awready
        // ... rest of slave directions ...
    );
endinterface

interface noc_req_intf #(
    parameter int unsigned NUM_VC                = ni_params_pkg::NI_NOC_NUM_VC_DFLT,
    parameter int unsigned FLIT_WIDTH            = ni_params_pkg::NI_NOC_FLIT_WIDTH_DFLT,
    parameter int unsigned SLAVE_VC_BUFFER_DEPTH = ni_params_pkg::NI_NOC_SLAVE_VC_BUFFER_DEPTH_DFLT
);
    logic                    valid;
    logic [FLIT_WIDTH-1:0]   flit;
    logic [NUM_VC-1:0]       credit_return;

    modport master ( output valid, flit, input  credit_return );
    modport slave  ( input  valid, flit, output credit_return );
endinterface
```

### 5.3 cosim2 migration

#### Files removed

- `cosim2/sv/axi_intf.sv`
- `cosim2/sv/noc_req_intf.sv`
- `cosim2/sv/noc_rsp_intf.sv`

#### Files modified

| File | Change |
|---|---|
| `cosim2/sv/nmu_wrap.sv` | Port list adds `clk_i`, `rst_ni`. Uses `axi4_intf.slave axi_i`, `noc_req_intf.master noc_req_o`, `noc_rsp_intf.slave noc_rsp_i` |
| `cosim2/sv/nsu_wrap.sv` | Symmetric: `noc_req_intf.slave noc_req_i`, `noc_rsp_intf.master noc_rsp_o`, `axi4_intf.master axi_o` |
| `cosim2/sv/axi_master_wrap.sv` | Uses `axi4_intf.master axi_o` |
| `cosim2/sv/slave_wrap.sv` | Uses `axi4_intf.slave axi_i` |
| `cosim2/sv/noc_wrap.sv` (LoopbackNoc, NullNoc) | Uses `noc_req_intf` + `noc_rsp_intf` with appropriate modports |
| `cosim2/sv/tb_top.sv` | Instantiates specgen interfaces with default parameters; clk/rst generated at tb_top scope and passed as module ports |

Module-port suffix mapping (per §2.1, project convention only):

- `_i` ↔ data flows INTO this module (typically `.slave` modport selected)
- `_o` ↔ data flows OUT of this module (typically `.master` modport selected)
- `_ni` ↔ active-low input (used only for `rst_ni`)

#### NoC 4→2 consolidation mapping

| Old (specgen) | New | Modport |
|---|---|---|
| `ni_noc_req_out_intf` | `noc_req_intf` | `master` |
| `ni_noc_req_in_intf` | `noc_req_intf` | `slave` |
| `ni_noc_rsp_out_intf` | `noc_rsp_intf` | `master` |
| `ni_noc_rsp_in_intf` | `noc_rsp_intf` | `slave` |

Signal name mapping inside each:

| Old | New |
|---|---|
| `noc_req_valid`  | `valid` |
| `noc_req_flit`   | `flit` |
| `noc_req_credit` | `credit_return` |
| `noc_rsp_valid`  | `valid` |
| `noc_rsp_flit`   | `flit` |
| `noc_rsp_credit` | `credit_return` |

#### wb2axip wire-up

wb2axip vendored module expects port names like `i_axi_awvalid`. specgen emits `awvalid`. Mapping happens at the wrap port-connection level — no edit to `cosim2/sv/wb2axip/*.v`:

```systemverilog
faxi_slave u_faxi_slave (
    .i_clk         (clk_i),
    .i_axi_reset_n (rst_ni),
    .i_axi_awvalid (axi_i.awvalid),
    .i_axi_awready (axi_i.awready),
    // ...
);
```

### 5.4 W2 PR sub-commit ordering (review-only, not CI-gated mid-PR)

1. `constants.yaml` + `_load_constants_yaml` + emit `ni_params_pkg.sv` + `ni_params.h`.
2. `interface_handshake.json` + `_load_handshake_schema` + cross-reference checks.
3. `_emit_sv_interface` rewrite + `axi4_intf` consolidation.
4. `_consolidate_noc_interfaces` + 2-interface NoC emission.
5. Regenerate `specgen/generated/sv/`.
6. Update specgen golden test fixtures.
7. Delete `cosim2/sv/axi_intf.sv` + `noc_req_intf.sv` + `noc_rsp_intf.sv`.
8. Rewrite the 5 wraps.
9. Rewrite `tb_top.sv`.
10. Final CI gate (ctest 410/410 + drift gate + Verilator warning-clean elaboration).

### 5.5 W2 risks

- Specgen interface signal set differs subtly from prior hand-written set → Verilator elaboration error catches at sub-commit 9.
- wb2axip port-name mapping verbosity in each wrap → acceptable; one-time pasted block per wrap.
- Specgen golden tests may have larger diff scope than `ni_signals_pkg.sv` alone → list explicit diff in PR description for user review.

---

## 6. Test Plan

### 6.1 During W1

- Existing specgen pytest suite stays green (`specgen/tests/test_generator_sv.py` etc.).
- New test: malformed `constants.yaml` (unknown key, missing required field, default outside constraint) → raises validation error.
- New test: malformed `interface_handshake.json` (parameter references undefined `constants.yaml` key) → raises validation error.

### 6.2 During W2

- Sub-commits 1–9 may be temporarily broken — not CI-gated.
- Final CI gate after sub-commit 10:
    - Specgen pytest green (with regenerated golden fixtures).
    - cosim2 ctest 410/410 green.
    - 5 existing fixture smoke (`debug_multi1.yaml`, `write_only_smoke.yaml`, `multibeat_incr_8beat.yaml`, etc.) green.
    - Existing drift gate clean (no diff between `specgen/generated/cpp/*` and c_model headers).
    - Verilator elaboration warning-clean (existing baseline; strict-mode sweep is deferred to release-sweep spec).

---

## 7. Open Items & Risks

| # | Item | Mitigation |
|---|---|---|
| O1 | Specgen golden file diff scope may exceed `ni_signals_pkg.sv` | W2 PR description lists explicit diff for user review before merge |
| O2 | One of the 5 wraps may have a signal-set mismatch surfacing only at elaboration | Verilator elaboration error pinpoints; sub-commit 9 is last; revert is per-file |
| O3 | wb2axip signal name mismatch with specgen | Explicit port mapping inside each wrap — no edit to vendored source |
| O4 | NoC credit semantics (multi-hot pulse, registered output) must match existing c_model `MetaBuffer` credit accounting | W2 atomic PR includes regression run of existing cosim2 credit-flow scenarios (LoopbackNoc, NullNoc); failure blocks merge |
| O5 | `axi4_intf` consolidation: two wraps (`axi_master_wrap` and `slave_wrap`) instantiate the same interface type with different modports — verify Verilator elaboration accepts | Covered by W2 final CI gate |
| O6 | AxUSER/WUSER/BUSER/RUSER excluded — future addition requires schema extension + regenerate + consumer migration in another atomic PR | Tracked here; not in this spec |
| O7 | `manager / subordinate` modport alias requested by future RTL stage | Generator scaffolding leaves space; current emission single `master / slave` only |
| **O8** | **Release-level quality sweep (Karpathy 4-lens + magic-number hunt + 12 verification gates) deferred to follow-up spec.** Each gate is a non-trivial sub-spec (real Verilator coverage parsing per official format; scenario-parser extension for protocol-violation fault injection; tb_top parameter forwarding for sweep; etc.). Codex round 5 review of an earlier attempt to bundle them into this plan revealed substantial unresolved HIGH-severity gaps. | Follow-up spec to brainstorm separately: `docs/superpowers/specs/YYYY-MM-DD-release-quality-sweep-design.md` (TBD date). Each gate handled as its own sub-task with concrete tooling commands verified against the actual codebase. |

---

## 8. Karpathy 4-Lens Self-Check (on this spec itself)

| Lens | Check |
|---|---|
| Overcomplication | Handshake JSON describes exactly 3 interfaces (1 AXI + 2 NoC). No framework abstraction. `constants.yaml` is flat per-domain, not a generic config DSL |
| Surgical | `ni_regs_pkg.sv`, `ni_flit_pkg.sv`, c_model functional code, wb2axip — all explicit out-of-scope. W2 PR's atomic nature limits surface to listed file set. **Release-level sweep deferred to follow-up spec** after Codex round 5 surfaced unresolved HIGH-severity gaps when attempting to bundle 12 gates into one plan |
| Surface assumptions | §2.1 table makes every convention explicit; clk / rst routing noted as project convention not AMBA mandate |
| Verifiable success | §1.4 success criteria are testable; §6 final CI gate is concrete (specgen pytest + drift gate + ctest 410/410 + 5 fixture smoke + Verilator baseline) |

---

## 9. References

- **Arm AMBA AXI/ACE Protocol Specification IHI 0022H** — accessible at developer.arm.com (Arm AMBA documentation portal). Specifically:
    - §A1.3 — Master / Slave terminology (legacy), now Manager / Subordinate
    - §A2.2–A2.6 — Fixed signal widths (AxLEN, AxSIZE, AxBURST, AxLOCK, AxCACHE, AxPROT, AxQOS, AxREGION, xRESP, WSTRB derived)
    - §A3.1.2 — Clock and reset behavior (active-low reset, async assert, sync deassert)
    - §A3.2.1 — Handshake / VALID-READY stability requirements
    - §A8.3 — User signals (deferred from this spec)
    - §A9.3 Tables A9-1 to A9-4 — Per-role signal presence matrix
- **AMD Versal AI Edge PG115** — for `M_AXI_*` / `S_AXI_*` flattened port-name convention reference.
- Project memories: `feedback-naming-full-word-no-abbreviation`, `feedback-verification-ip-fault-injection`, `feedback-dont-silence-the-checker`, `feedback-codex-review-each-round`.

---

## 10. Approval

- [ ] User review (pending after W3 deferral trim)
- [x] Spec self-review pass (this document — see §8)
- [x] Codex review round 1 (incorporated)
- [x] Codex review round 2 (incorporated)
- [x] Codex review rounds 3–5 (round 5 surfaced unresolved W3 gaps; resolved by deferring §6 to follow-up spec)

After user approval, the implementation plan at `docs/superpowers/plans/2026-06-06-specgen-handshake-rtl-style-upstream.md` (W1+W2 only) is the executable artifact for this scope. The deferred release-level sweep gets its own brainstorm → spec → plan cycle.
