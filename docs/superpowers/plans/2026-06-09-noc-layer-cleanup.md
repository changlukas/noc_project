# NoC Layer Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Rename `LoopbackNoc` → `ChannelModel` (pure rename) and unify `noc_req_intf` + `noc_rsp_intf` → `noc_intf` with `mosi`/`miso` modports.

**Spec:** `docs/superpowers/specs/2026-06-09-noc-layer-cleanup-design.md` (commit `b8901c3`).

**Commit chain (3 commits):**

| # | Type | Scope |
|---|---|---|
| C1 | `refactor: rename LoopbackNoc -> ChannelModel` | 6 `git mv` + identifiers + config key + ~25 content refs incl. 4 shell adapter/io files + integration `CMakeLists.txt` |
| C2 | `refactor(specgen): merge ni_signals.json NoC interfaces (NMU/NSU)` | 4 NoC entries → 2 (`NOC_INTF_MOSI`, `NOC_INTF_MISO`); cpp regen; golden + 3 tests updated |
| C3 | `refactor(specgen+cosim): merge noc_intf with mosi/miso + SV wraps` | `interface_handshake.json` merge; `sv_signals.py` modport-from-JSON; SV regen; golden + 2 pytests; nmu/nsu/channel_model/tb_top wraps |

C3 combines old C3+C4 because regen of `ni_signals_pkg.sv` deletes `noc_req_intf`/`noc_rsp_intf`, which the wraps still reference — splitting would break SV elaboration mid-chain.

**Env note:** Windows MinGW GCC 15.2 ICE on `test_meta_buffer` + `test_channel_model_per_vc_credit` (post-rename) is pre-existing — not introduced.

---

## Task 1 — C1: Rename LoopbackNoc → ChannelModel

Pure rename. No structural change. `make check` must stay clean.

### Files

**Renamed (6 `git mv`):**

| Before | After |
|---|---|
| `cosim/sv/loopback_noc_wrap.sv` | `cosim/sv/channel_model_wrap.sv` |
| `c_model/include/cosim/loopback_noc_shell_adapter.hpp` | `c_model/include/cosim/channel_model_shell_adapter.hpp` |
| `c_model/include/cosim/loopback_noc_shell_io.hpp` | `c_model/include/cosim/channel_model_shell_io.hpp` |
| `c_model/tests/common/loopback_noc.hpp` | `c_model/tests/common/channel_model.hpp` |
| `c_model/tests/cosim/test_loopback_noc_shell_adapter.cpp` | `c_model/tests/cosim/test_channel_model_shell_adapter.cpp` |
| `c_model/tests/common/test_loopback_noc_per_vc_credit.cpp` | `c_model/tests/common/test_channel_model_per_vc_credit.cpp` |

**Content-only updates (no rename):**

- DPI + SV: `cosim/c/cmodel_dpi.cpp`, `cosim/c/cmodel_dpi.h`, `cosim/sv/nmu_wrap.sv`, `cosim/sv/nsu_wrap.sv`, `cosim/sv/tb_top.sv`, `cosim/verilator/Makefile`
- c_model production: `c_model/include/nmu/nmu.hpp`, `c_model/include/cosim/flit_byte_conv.hpp`, `c_model/include/cosim/poc_defaults.hpp`
- c_model shell adapter/io (Codex MUST-FIX): `c_model/include/cosim/nmu_shell_adapter.hpp:28,61-62`, `c_model/include/cosim/nmu_shell_io.hpp:18`, `c_model/include/cosim/nsu_shell_adapter.hpp:33,66-67`, `c_model/include/cosim/nsu_shell_io.hpp:22`
- c_model noc abstract: `c_model/include/noc/noc_req_in.hpp:8`, `noc_req_out.hpp:7`, `noc_rsp_in.hpp:8`, `noc_rsp_out.hpp:7` (comment only)
- Config: `c_model/include/ni/port_params.hpp:41-45,55,78-80`, `c_model/config/port_params.yaml:37`
- Tests: `c_model/tests/nmu/{test_packetize,test_depacketize,test_nmu,test_vc_arbiter,test_rob}.cpp`, `c_model/tests/nsu/{test_nsu_depacketize,test_nsu_vc_arbiter,test_nsu_packetize,test_nsu}.cpp`, `c_model/tests/common/test_loopback_latency.cpp` (includes only, file name kept), `c_model/tests/integration/test_request_response_loopback.cpp` (includes only)
- CMakeLists (Codex MUST-FIX adds integration): `c_model/tests/cosim/CMakeLists.txt`, `c_model/tests/common/CMakeLists.txt`, `c_model/tests/integration/CMakeLists.txt:16,21`
- Scenarios + docs: `tests/scenarios/AX4-STR-003_multi_dst_stress/scenario.yaml`, `docs/architecture.md`, `CLAUDE.md`
- Spec self-reference (number fix): `docs/superpowers/specs/2026-06-09-noc-layer-cleanup-design.md:73` — `9 c_model tests` → `11 c_model tests` (test list lists 11 names)

### Steps

- [ ] **1.1: 6 `git mv`**

  ```bash
  git mv cosim/sv/loopback_noc_wrap.sv cosim/sv/channel_model_wrap.sv
  git mv c_model/include/cosim/loopback_noc_shell_adapter.hpp c_model/include/cosim/channel_model_shell_adapter.hpp
  git mv c_model/include/cosim/loopback_noc_shell_io.hpp c_model/include/cosim/channel_model_shell_io.hpp
  git mv c_model/tests/common/loopback_noc.hpp c_model/tests/common/channel_model.hpp
  git mv c_model/tests/cosim/test_loopback_noc_shell_adapter.cpp c_model/tests/cosim/test_channel_model_shell_adapter.cpp
  git mv c_model/tests/common/test_loopback_noc_per_vc_credit.cpp c_model/tests/common/test_channel_model_per_vc_credit.cpp
  ```

- [ ] **1.2: Identifier substitution table (apply across renamed files + all content-only files listed above)**

  | Pattern | Replacement |
  |---|---|
  | `LoopbackNoc` (class) | `ChannelModel` |
  | `LoopbackNocShellAdapter` | `ChannelModelShellAdapter` |
  | `LoopbackNocInputs` / `LoopbackNocOutputs` | `ChannelModelInputs` / `ChannelModelOutputs` |
  | `loopback_noc_wrap` (SV module) | `channel_model_wrap` |
  | `cmodel_loopback_noc_{tick,set_inputs,get_outputs}` | `cmodel_channel_model_*` |
  | `#include "common/loopback_noc.hpp"` | `#include "common/channel_model.hpp"` |
  | `#include "cosim/loopback_noc_shell_adapter.hpp"` | `#include "cosim/channel_model_shell_adapter.hpp"` |
  | `#include "cosim/loopback_noc_shell_io.hpp"` | `#include "cosim/channel_model_shell_io.hpp"` |
  | `loopback_noc_req_depth` / `loopback_noc_rsp_depth` (struct field) | `channel_model_req_depth` / `channel_model_rsp_depth` |
  | `root["loopback_noc"]["req_depth"]` etc. (YAML key) | `root["channel_model"]["req_depth"]` |
  | YAML top-level key `loopback_noc:` | `channel_model:` |
  | `kPoCLoopbackNocDepth` (if present) | `kPoCChannelModelDepth` |

  Preserve substrings about topology/latency (e.g. `test_loopback_latency.cpp`, `test_request_response_loopback.cpp` file names, comments mentioning "loopback topology" or "loopback latency feature").

- [ ] **1.3: Spec self-reference fix**

  `docs/superpowers/specs/2026-06-09-noc-layer-cleanup-design.md:73` — change `9 c_model tests` to `11 c_model tests` (the parenthetical list already contains 11 names).

- [ ] **1.4: Final survey — no surviving references in active scope**

  ```powershell
  py -3 -c "import subprocess; r = subprocess.run(['git','grep','-nI','-E','LoopbackNoc|loopback_noc|cmodel_loopback_noc','--','*.py','*.cpp','*.hpp','*.h','*.sv','*.svh','*.md','*.yaml','Makefile','CMakeLists.txt',':!docs/_archive/',':!docs/superpowers/specs/2026-06-0[2-8]-*',':!docs/superpowers/plans/2026-06-0[2-8]-*',':!docs/superpowers/specs/2026-06-09-noc-layer-cleanup-design.md',':!docs/superpowers/plans/2026-06-09-noc-layer-cleanup.md'], capture_output=True, text=True); print(r.stdout); print('---STDERR---', r.stderr)"
  ```

  Expected: empty stdout. The spec + this plan are excluded because they intentionally cite the old identifiers in "before" tables.

  Exceptions allowed in non-excluded scope: file names `test_loopback_latency.cpp`, `test_request_response_loopback.cpp` (preserved by design — they describe topology, not the class).

- [ ] **1.5: Gates**

  ```bash
  py -3 specgen/tools/codegen.py --check                                                                                              # exit 0
  py -3 -m pytest specgen/tests/ 2>&1 | tail -3                                                                                       # all PASS
  py -3 tools/lint_docs.py README.md docs/architecture.md docs/development.md docs/_archive/README.md tests/scenarios/README.md         # OK
  py -3 tools/lint_scenarios.py                                                                                                       # 37 dirs OK
  make check PYTHON3="py -3" 2>&1 | tail -10                                                                                          # green modulo pre-existing GCC ICE
  ```

- [ ] **1.6: Commit**

  ```bash
  git add -A
  git commit -m "$(cat <<'EOF'
  refactor: rename LoopbackNoc -> ChannelModel

  Class was not a loopback (master sends r/w op, slave generates
  response; the c_model NoC layer is a behavioural channel model).

  6 git mv + identifier substitution across renamed files + ~25
  content-only files (DPI, SV wraps, c_model shell adapter/io,
  noc abstract, port_params, tests, scenarios, docs). Config key
  port_params.yaml:loopback_noc -> :channel_model with paired
  struct-field rename + loader rewire. CMakeLists test enumeration
  + integration CMakeLists path updated. Pure rename, no structural
  change.

  Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
  EOF
  )"
  ```

---

## Task 2 — C2: Merge ni_signals.json NoC interfaces (NMU + NSU only)

Auto-emitted pin structs collapse from `NocReqOutPins`+`NocRspInPins` (NMU) and `NocReqInPins`+`NocRspOutPins` (NSU) → `NocIntfMosiPins` (NMU) + `NocIntfMisoPins` (NSU). **ChannelModel side has no JSON entry** — `ni_signals.json` only covers NMU/NSU/CSR.

### Files

- Edit: `specgen/generated/json/ni_signals.json` (4 NoC interface entries → 2)
- Regen output: `specgen/generated/cpp/ni_signals.h`
- Refresh: `specgen/tests/golden/ni_signals.h.golden`
- Tests: `specgen/tests/test_signals_schema.py`, `specgen/tests/test_signals_resolver.py`, `specgen/tests/cpp_smoke/test_pins_compile.cpp:16,19,25-26`, `c_model/tests/test_pins_smoke.cpp` (if struct names referenced)
- Polish: `c_model/include/noc/noc_req_out.hpp:7`, `noc_req_in.hpp:8`, `noc_rsp_in.hpp:8`, `noc_rsp_out.hpp:7` — stale comments now also reference old separate-interface terminology

### Steps

- [ ] **2.1: Edit `specgen/generated/json/ni_signals.json`**

  Per-block transformations:

  **NMU side** (current entries at `NOC_REQ_OUT` line 449 + `NOC_RSP_IN` line 507):

  Collapse the two `interfaces[]` entries into one:

  ```json
  {
    "name": "NOC_INTF_MOSI",
    "block": "NMU",
    "direction": "out request, in response",
    "protocol": "NoC",
    "clock": "noc_clk_i",
    "reset": "noc_rst_ni",
    "signals": [
      { "width_param": null,        "direction": "output", "width_expr": "1",          "pin_name": "noc_req_valid_o",   "reset_behavior": {"kind":"async-active-low","value":"0","domain":"noc_rst_ni"}, "presence": null },
      { "width_param": "FLIT_WIDTH","direction": "output",                              "pin_name": "noc_req_flit_o",    "reset_behavior": {"kind":"async-active-low","value":"0","domain":"noc_rst_ni"}, "presence": null, "width_expr": null },
      { "width_param": "NUM_VC",    "direction": "input",                               "pin_name": "noc_req_credit_i",  "reset_behavior": {"kind":"external_driven"}, "presence": null, "width_expr": null },
      { "width_param": null,        "direction": "input",  "width_expr": "1",          "pin_name": "noc_rsp_valid_i",   "reset_behavior": {"kind":"external_driven"}, "presence": null },
      { "width_param": "FLIT_WIDTH","direction": "input",                               "pin_name": "noc_rsp_flit_i",    "reset_behavior": {"kind":"external_driven"}, "presence": null, "width_expr": null },
      { "width_param": "NUM_VC",    "direction": "output",                              "pin_name": "noc_rsp_credit_o",  "reset_behavior": {"kind":"async-active-low","value":"0","domain":"noc_rst_ni"}, "presence": null, "width_expr": null }
    ],
    "port_parameters": [ /* keep NUM_VC + FLIT_WIDTH from either source entry */ ]
  }
  ```

  **NSU side** (current `NOC_REQ_IN` line 986 + `NOC_RSP_OUT` line 1501):

  Collapse to one `NOC_INTF_MISO` with the same 6 signals but mirrored directions:
  - `noc_req_valid_i` (input), `noc_req_flit_i` (input), `noc_req_credit_o` (output, reset noc_rst_ni)
  - `noc_rsp_valid_o` (output, reset noc_rst_ni), `noc_rsp_flit_o` (output, reset noc_rst_ni), `noc_rsp_credit_i` (input)

  Preserve `port_parameters` (`NUM_VC`, `FLIT_WIDTH`) from the source entries; both entries declare the same params.

- [ ] **2.2: Regenerate cpp signals + verify**

  ```bash
  py -3 specgen/tools/codegen.py --target cpp --domain signals
  py -3 -c "open('specgen/generated/cpp/ni_signals.h').read().count('NocIntfMosiPins')" # > 0
  py -3 -c "open('specgen/generated/cpp/ni_signals.h').read().count('NocIntfMisoPins')" # > 0
  py -3 -c "open('specgen/generated/cpp/ni_signals.h').read().count('NocReqOutPins')"   # == 0
  ```

- [ ] **2.3: Refresh golden**

  ```bash
  cp specgen/generated/cpp/ni_signals.h specgen/tests/golden/ni_signals.h.golden
  py -3 -m pytest specgen/tests/test_byte_identical_golden.py::test_golden_cpp_signals -v   # PASS
  ```

- [ ] **2.4: Update `specgen/tests/test_signals_schema.py` + `test_signals_resolver.py`**

  - Find all assertions naming `"NOC_REQ_OUT"`, `"NOC_RSP_IN"`, `"NOC_REQ_IN"`, `"NOC_RSP_OUT"` and update interface-count + name expectations.
  - For per-interface signal-set lookups, expect the 6-signal merged set under `NOC_INTF_MOSI` / `NOC_INTF_MISO`.

- [ ] **2.5: Update `specgen/tests/cpp_smoke/test_pins_compile.cpp`**

  Exact replacements:

  ```cpp
  // Lines 16-17:
  -    ni::pins::NocReqOutPins req_out{};
  -    req_out.reset_outputs();
  +    ni::pins::NocIntfMosiPins noc_mosi{};
  +    noc_mosi.reset_outputs();

  // Lines 19-20:
  -    ni::pins::NocRspInPins rsp_in{};
  -    rsp_in.reset_outputs();
  // (delete — covered by noc_mosi above)

  // Lines 25-26:
  -    ni::pins::NocReqInPins  req_in{};  req_in.reset_outputs();
  -    ni::pins::NocRspOutPins rsp_out{}; rsp_out.reset_outputs();
  +    ni::pins::NocIntfMisoPins noc_miso{}; noc_miso.reset_outputs();
  ```

- [ ] **2.6: Update `c_model/tests/test_pins_smoke.cpp` if it names old struct types**

  ```bash
  grep -n "NocReqOutPins\|NocRspInPins\|NocReqInPins\|NocRspOutPins" c_model/tests/test_pins_smoke.cpp
  ```

  Apply the same struct-name substitution as 2.5.

- [ ] **2.7: Polish comments in `c_model/include/noc/noc_*_*.hpp`**

  Files: `noc_req_out.hpp:7`, `noc_req_in.hpp:8`, `noc_rsp_in.hpp:8`, `noc_rsp_out.hpp:7`.

  These comments reference the old NocReqOutPins / NocRspInPins struct names in their "mirrors the ni_signals.json pin struct ..." line. Update each to the new `NocIntfMosiPins` (NMU req_out/rsp_in) / `NocIntfMisoPins` (NSU req_in/rsp_out) name.

- [ ] **2.8: Gates**

  ```bash
  py -3 specgen/tools/codegen.py --check                   # exit 0
  py -3 -m pytest specgen/tests/ 2>&1 | tail -3            # all PASS
  make check PYTHON3="py -3" 2>&1 | tail -10               # green modulo GCC ICE
  ```

- [ ] **2.9: Commit**

  ```bash
  git add specgen/generated/json/ni_signals.json specgen/generated/cpp/ni_signals.h specgen/tests/golden/ni_signals.h.golden specgen/tests/test_signals_schema.py specgen/tests/test_signals_resolver.py specgen/tests/cpp_smoke/test_pins_compile.cpp c_model/tests/test_pins_smoke.cpp c_model/include/noc/noc_req_out.hpp c_model/include/noc/noc_req_in.hpp c_model/include/noc/noc_rsp_in.hpp c_model/include/noc/noc_rsp_out.hpp
  git commit -m "$(cat <<'EOF'
  refactor(specgen): merge ni_signals.json NoC interfaces (NMU + NSU)

  4 NoC interface entries -> 2: NMU NOC_INTF_MOSI carries req out +
  rsp in (6 signals); NSU NOC_INTF_MISO mirror. cpp_signals.py emits
  NocIntfMosiPins / NocIntfMisoPins from the per-interface signal
  bundle (no emitter change). Golden refreshed; pin-bundle tests
  + 4 noc/*.hpp stale comments updated.

  Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
  EOF
  )"
  ```

---

## Task 3 — C3: Merge interface_handshake.json + mosi/miso modports + SV wraps

C3 + C4 combined: regenerating `ni_signals_pkg.sv` deletes `noc_req_intf`/`noc_rsp_intf`, but the wraps reference them. Splitting would break SV elaboration. So JSON merge + sv_signals.py change + regen + golden + SV wrap edits land in one commit.

### Files

- Edit: `specgen/source/interface_handshake.json` (2 NoC entries → 1)
- Edit: `specgen/tools/elaborate/sv_signals.py:115-180` — `_emit_axi4_intf` + `_emit_noc_intf` read modport names from `iface_spec["modports"]`
- Regen output: `specgen/generated/sv/ni_signals_pkg.sv`
- Refresh: `specgen/tests/golden/ni_signals_pkg.sv.golden`
- Tests: `specgen/tests/test_handshake_schema.py:248-269`, `specgen/tests/test_codegen_sv.py`
- SV wraps: `cosim/sv/nmu_wrap.sv`, `cosim/sv/nsu_wrap.sv`, `cosim/sv/channel_model_wrap.sv` (renamed in C1), `cosim/sv/tb_top.sv`

### Steps

- [ ] **3.1: Edit `specgen/source/interface_handshake.json`**

  Replace the two entries `noc_req_intf` (lines 16-46) and `noc_rsp_intf` (lines 48-62) with one merged entry. Use field name **`modports`** (the existing schema field, validated in `specgen/ni_spec/handshake_schema.py:216-218`).

  ```json
  "noc_intf": {
    "kind": "noc_link",
    "parameters": [
      { "name": "NUM_VC",                "constants_yaml_key": "noc.NUM_VC" },
      { "name": "FLIT_WIDTH",            "constants_yaml_key": "noc.FLIT_WIDTH" },
      { "name": "SLAVE_VC_BUFFER_DEPTH", "constants_yaml_key": "noc.SLAVE_VC_BUFFER_DEPTH" }
    ],
    "signals": [
      { "name": "req_valid",         "width_expr": "1",          "driven_by": "mosi" },
      { "name": "req_flit",          "width_expr": "FLIT_WIDTH", "driven_by": "mosi" },
      { "name": "req_credit_return", "width_expr": "NUM_VC",     "driven_by": "miso" },
      { "name": "rsp_valid",         "width_expr": "1",          "driven_by": "miso" },
      { "name": "rsp_flit",          "width_expr": "FLIT_WIDTH", "driven_by": "miso" },
      { "name": "rsp_credit_return", "width_expr": "NUM_VC",     "driven_by": "mosi" }
    ],
    "modports": ["mosi", "miso"],
    "protocol_semantics": {
      "transfer_condition": "producer-internal credit counter for selected VC > 0 AND <chan>_valid asserted; selected VC encoded inside <chan>_flit header. Each channel (req, rsp) has independent credit accounting.",
      "credit_return_encoding": {
        "scheme": "per_vc_credit_pulse_vector",
        "semantics": "<chan>_credit_return[v] asserted for exactly 1 cycle releases 1 credit for VC v. Multi-hot permitted across VCs in the same cycle.",
        "output_discipline": "registered (no combinational path from <chan>_valid input to <chan>_credit_return output)",
        "reset_value": "all zeros",
        "onehot_check_required": false
      },
      "initial_credits": {
        "scheme": "configured_at_reset",
        "value_per_vc": "SLAVE_VC_BUFFER_DEPTH",
        "note": "Producer assumes SLAVE_VC_BUFFER_DEPTH credits per VC per channel after rst_ni deassert."
      },
      "valid_stability": "once <chan>_valid asserted, <chan>_valid + <chan>_flit MUST hold until handshake completes (always-accept on receiver side; backpressure via producer credit exhaustion). See IHI 0022H §A3.2.1 analog.",
      "combinational_loops": "forbidden — <chan>_credit_return must be a registered output of the receiver"
    }
  }
  ```

  Keep `axi4_intf` entry's `modports: ["master", "slave"]` unchanged.

- [ ] **3.2: Edit `specgen/tools/elaborate/sv_signals.py`**

  Two helper functions hardcode modport names; both must read from the JSON instead.

  In `_emit_axi4_intf` (line 115) — replace the hardcoded `"master"` / `"slave"` in lines 139-147:

  ```python
  # Add near top of function (after iface_spec.get("parameters", [])):
  modports = iface_spec.get("modports", ["master", "slave"])
  mp_out, mp_in = modports[0], modports[1]

  # Then lines 139-147 become:
  out.append(f"  modport {mp_out} (")
  out.append(f"    output {', '.join(master_sigs)},")
  out.append(f"    input  {', '.join(slave_sigs)}")
  out.append("  );")
  out.append("")
  out.append(f"  modport {mp_in} (")
  out.append(f"    input  {', '.join(master_sigs)},")
  out.append(f"    output {', '.join(slave_sigs)}")
  out.append("  );")
  ```

  In `_emit_noc_intf` (line 152-180) — line 169-178:

  ```python
  modports = iface_spec.get("modports", ["master", "slave"])
  mp_out, mp_in = modports[0], modports[1]
  out_names = [s["name"] for s in signals if s["driven_by"] == mp_out]
  in_names  = [s["name"] for s in signals if s["driven_by"] == mp_in]
  out.append(
      f"  modport {mp_out} ( output {', '.join(out_names)}, "
      f"input  {', '.join(in_names)} );"
  )
  out.append(
      f"  modport {mp_in}  ( input  {', '.join(out_names)}, "
      f"output {', '.join(in_names)} );"
  )
  ```

  Note: `driven_by` semantics change from `master`/`slave` to whichever modport names the JSON declares (so `noc_intf` uses `mosi`/`miso`, axi4 keeps `master`/`slave`). This matches the JSON edit in step 3.1.

- [ ] **3.3: Regenerate SV signals + verify**

  ```bash
  py -3 specgen/tools/codegen.py --target sv --domain signals
  py -3 -c "s=open('specgen/generated/sv/ni_signals_pkg.sv').read(); assert 'interface noc_intf' in s; assert 'modport mosi' in s; assert 'modport miso' in s; assert 'interface noc_req_intf' not in s; assert 'interface noc_rsp_intf' not in s; print('OK')"
  ```

- [ ] **3.4: Refresh golden**

  ```bash
  cp specgen/generated/sv/ni_signals_pkg.sv specgen/tests/golden/ni_signals_pkg.sv.golden
  py -3 -m pytest specgen/tests/test_byte_identical_golden.py::test_golden_sv_signals -v   # PASS
  ```

- [ ] **3.5: Update `specgen/tests/test_handshake_schema.py:248-269`**

  Two tests reference the old 2-entry layout; collapse to single-entry expectations:

  ```python
  # test_load_interfaces_returns_three_named_interfaces (line 250)
  # rename + adjust expected set:
  def test_load_interfaces_returns_two_named_interfaces():
      c = load_constants(SOURCE_DIR / "constants.yaml")
      data = load_interfaces(SOURCE_DIR / "interface_handshake.json", c)
      assert set(data["interfaces"].keys()) == {"axi4_intf", "noc_intf"}
      assert data["interfaces"]["noc_intf"]["modports"] == ["mosi", "miso"]

  # test_noc_req_intf_protocol_semantics_complete (line 260) — rename + retarget:
  def test_noc_intf_protocol_semantics_complete():
      c = load_constants(SOURCE_DIR / "constants.yaml")
      data = load_interfaces(SOURCE_DIR / "interface_handshake.json", c)
      sem = data["interfaces"]["noc_intf"]["protocol_semantics"]
      assert sem["credit_return_encoding"]["scheme"] == "per_vc_credit_pulse_vector"
      assert sem["credit_return_encoding"]["onehot_check_required"] is False
      assert sem["initial_credits"]["value_per_vc"] == "SLAVE_VC_BUFFER_DEPTH"
      assert sem["combinational_loops"].startswith("forbidden")
  ```

  Also drop the `pytest.skip(...)` guards on these tests (the file now exists).

- [ ] **3.6: Update `specgen/tests/test_codegen_sv.py`**

  ```bash
  grep -n "noc_req_intf\|noc_rsp_intf\|modport master\|modport slave" specgen/tests/test_codegen_sv.py
  ```

  Replace each `"interface noc_req_intf"` / `"interface noc_rsp_intf"` assertion with `"interface noc_intf"`; replace `"modport master"` / `"modport slave"` assertions on the NoC side with `"modport mosi"` / `"modport miso"`. Keep axi4-side assertions (`"interface axi4_intf"`, `"modport master"`, `"modport slave"`) unchanged.

- [ ] **3.7: Edit `cosim/sv/nmu_wrap.sv` port list + signal references + comments**

  **Naming convention:** wrap port name = modport name, so each of the 4 wrap-side NoC ports is `noc_mosi` or `noc_miso`. NMU drives toward the channel → modport `mosi`, port name `noc_mosi`.

  Port list: replace

  ```sv
  axi4_intf.slave           axi_i,
  noc_req_intf.master       noc_req_o,
  noc_rsp_intf.slave        noc_rsp_i
  ```

  with

  ```sv
  axi4_intf.slave           axi_i,
  noc_intf.mosi             noc_mosi
  ```

  Signal-reference replacements inside the wrap body:

  | Before | After |
  |---|---|
  | `noc_req_o.valid` | `noc_mosi.req_valid` |
  | `noc_req_o.flit` | `noc_mosi.req_flit` |
  | `noc_req_o.credit_return` | `noc_mosi.req_credit_return` |
  | `noc_rsp_i.valid` | `noc_mosi.rsp_valid` |
  | `noc_rsp_i.flit` | `noc_mosi.rsp_flit` |
  | `noc_rsp_i.credit_return` | `noc_mosi.rsp_credit_return` |

  **Comment updates** (Codex MUST-FIX — no surviving `noc_req_intf` / `noc_rsp_intf` / `noc_req_o` / `noc_rsp_i` mention in active comments):

  ```bash
  grep -n "noc_req_intf\|noc_rsp_intf\|noc_req_o\|noc_rsp_i" cosim/sv/nmu_wrap.sv
  ```

  Update every match in the file header, port-block banner, and inline comments to describe the merged `noc_intf.mosi` bundle.

- [ ] **3.8: Edit `cosim/sv/nsu_wrap.sv` symmetrically**

  NSU receives from the channel → modport `miso`, port name `noc_miso`.

  Port list:

  ```sv
  noc_req_intf.slave        noc_req_i,
  noc_rsp_intf.master       noc_rsp_o,
  axi4_intf.master          axi_o
  ```

  becomes

  ```sv
  noc_intf.miso             noc_miso,
  axi4_intf.master          axi_o
  ```

  Signal-reference replacements:

  | Before | After |
  |---|---|
  | `noc_req_i.valid` | `noc_miso.req_valid` |
  | `noc_req_i.flit` | `noc_miso.req_flit` |
  | `noc_req_i.credit_return` | `noc_miso.req_credit_return` |
  | `noc_rsp_o.valid` | `noc_miso.rsp_valid` |
  | `noc_rsp_o.flit` | `noc_miso.rsp_flit` |
  | `noc_rsp_o.credit_return` | `noc_miso.rsp_credit_return` |

  Comment updates: same `grep -n "noc_req_intf\|noc_rsp_intf\|noc_req_i\|noc_rsp_o"` sweep on `cosim/sv/nsu_wrap.sv`, update every comment match.

- [ ] **3.9: Edit `cosim/sv/channel_model_wrap.sv` (file already renamed in C1)**

  channel_model has two NoC sides. Per the wrap-port = modport-name convention, each port is named after its modport: the channel-model NMU side uses modport `miso` → port `noc_miso`; the channel-model NSU side uses modport `mosi` → port `noc_mosi`.

  Current 4 NoC ports collapse to 2:

  ```sv
  noc_req_intf.slave        noc_req_from_nmu_i,
  noc_req_intf.master       noc_req_to_nsu_o,
  noc_rsp_intf.slave        noc_rsp_from_nsu_i,
  noc_rsp_intf.master       noc_rsp_to_nmu_o
  ```

  becomes

  ```sv
  noc_intf.miso             noc_miso,   // NMU-facing side
  noc_intf.mosi             noc_mosi    // NSU-facing side
  ```

  Signal-reference rewires:

  | Before | After |
  |---|---|
  | `noc_req_from_nmu_i.valid` | `noc_miso.req_valid` |
  | `noc_req_from_nmu_i.flit` | `noc_miso.req_flit` |
  | `noc_req_from_nmu_i.credit_return` | `noc_miso.req_credit_return` |
  | `noc_rsp_to_nmu_o.valid` | `noc_miso.rsp_valid` |
  | `noc_rsp_to_nmu_o.flit` | `noc_miso.rsp_flit` |
  | `noc_rsp_to_nmu_o.credit_return` | `noc_miso.rsp_credit_return` |
  | `noc_req_to_nsu_o.valid` | `noc_mosi.req_valid` |
  | `noc_req_to_nsu_o.flit` | `noc_mosi.req_flit` |
  | `noc_req_to_nsu_o.credit_return` | `noc_mosi.req_credit_return` |
  | `noc_rsp_from_nsu_i.valid` | `noc_mosi.rsp_valid` |
  | `noc_rsp_from_nsu_i.flit` | `noc_mosi.rsp_flit` |
  | `noc_rsp_from_nsu_i.credit_return` | `noc_mosi.rsp_credit_return` |

  Comment updates: same grep + update sweep on `cosim/sv/channel_model_wrap.sv`.

- [ ] **3.10: Edit `cosim/sv/tb_top.sv`**

  `tb_top.sv` declares 6 interfaces (lines 69-108): 2 `axi4_intf` (master↔NMU at 69-73, NSU↔slave at 104-108) + 4 `noc_*_intf` (NMU↔channel_model + channel_model↔NSU at 76-101). **Only the 4 NoC interfaces collapse**; the 2 AXI interfaces stay unchanged.

  Replace the 4 NoC intf declarations (lines 76-101 + their banner comments) with 2:

  ```sv
  // [2] nmu_channel_model — NoC bundle between nmu_wrap and channel_model_wrap (NMU side)
  noc_intf #(
      .NUM_VC(NUM_VC),
      .FLIT_WIDTH(FLIT_WIDTH),
      .SLAVE_VC_BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH)
  ) nmu_channel_model ();

  // [3] channel_model_nsu — NoC bundle between channel_model_wrap and nsu_wrap (NSU side)
  noc_intf #(
      .NUM_VC(NUM_VC),
      .FLIT_WIDTH(FLIT_WIDTH),
      .SLAVE_VC_BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH)
  ) channel_model_nsu ();
  ```

  Update banner comment block at line 6-10 (the topology diagram) to reflect the merged interface names.

  Update the 3 instance port connections (`u_nmu` at 133-139, `u_loopback` at 142-153 — also rename `u_loopback` → `u_channel_model` per C1, `u_nsu` at 156-169):

  | Instance | Before | After |
  |---|---|---|
  | `u_nmu` | `.noc_req_o(nmu_loopback_req.master), .noc_rsp_i(loopback_nmu_rsp.slave)` | `.noc_mosi(nmu_channel_model.mosi)` |
  | `u_channel_model` | 4 NoC ports | `.noc_miso(nmu_channel_model.miso), .noc_mosi(channel_model_nsu.mosi)` |
  | `u_nsu` | `.noc_req_i(loopback_nsu_req.slave), .noc_rsp_o(nsu_loopback_rsp.master)` | `.noc_miso(channel_model_nsu.miso)` |

- [ ] **3.11: C3 final survey (Codex MUST-FIX)**

  After 3.7-3.10, no surviving `noc_req_intf` / `noc_rsp_intf` reference in active scope:

  ```powershell
  py -3 -c "import subprocess; r = subprocess.run(['git','grep','-nI','-E','noc_req_intf|noc_rsp_intf','--','*.py','*.cpp','*.hpp','*.h','*.sv','*.svh','*.md','*.yaml','Makefile','CMakeLists.txt',':!docs/_archive/',':!docs/superpowers/specs/2026-06-0[2-8]-*',':!docs/superpowers/plans/2026-06-0[2-8]-*',':!docs/superpowers/specs/2026-06-09-noc-layer-cleanup-design.md',':!docs/superpowers/plans/2026-06-09-noc-layer-cleanup.md'], capture_output=True, text=True); print(r.stdout); print('---STDERR---', r.stderr)"
  ```

  Expected: empty stdout. Any surviving comment / docstring (e.g. `specgen/tests/test_codegen_sv.py`) — fix in place before commit.

- [ ] **3.12: Gates**

  ```bash
  py -3 specgen/tools/codegen.py --check                   # exit 0
  py -3 -m pytest specgen/tests/ 2>&1 | tail -3            # all PASS
  make build PYTHON3="py -3" 2>&1 | tail -10               # clean SV elaboration (modulo GCC ICE)
  make check PYTHON3="py -3" 2>&1 | tail -10               # green modulo GCC ICE
  ```

  Verify: Verilator passes new `noc_intf` declaration; DPI symbols (`cmodel_channel_model_*`) resolve between `cmodel_dpi.h` and `channel_model_wrap.sv`.

- [ ] **3.13: Commit**

  ```bash
  git add specgen/source/interface_handshake.json specgen/tools/elaborate/sv_signals.py specgen/generated/sv/ni_signals_pkg.sv specgen/tests/golden/ni_signals_pkg.sv.golden specgen/tests/test_handshake_schema.py specgen/tests/test_codegen_sv.py cosim/sv/nmu_wrap.sv cosim/sv/nsu_wrap.sv cosim/sv/channel_model_wrap.sv cosim/sv/tb_top.sv
  git commit -m "$(cat <<'EOF'
  refactor(specgen+cosim): merge noc_intf with mosi/miso modports

  interface_handshake.json: noc_req_intf + noc_rsp_intf -> noc_intf
  (6 signals, mosi/miso modports). sv_signals.py reads modport names
  from JSON; axi4_intf keeps master/slave. SV wraps collapse 2 NoC
  ports -> 1 noc_intf bundle (nmu/nsu/channel_model + tb_top). DPI
  signatures unchanged. Regen + golden refresh + 2 specgen tests
  updated.

  Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
  EOF
  )"
  ```

---

## Deferred (Codex SHOULD-FIX, out of scope here)

Codex flagged that `sv_signals.py` now reads `modports[0:2]` and `driven_by` directly from JSON without schema-level guards (e.g. a misspelled `"driven_by": "mosI"` would silently drop the signal from both modports). Possible hardening:

- `specgen/ni_spec/handshake_schema.py`: require exactly 2 unique modports per interface; validate every `driven_by` value is one of those modports.
- Add 2 negative tests (`bad-modport-count`, `unknown-driven-by`).

Skipped this cleanup because there is exactly one `noc_link` interface in the JSON and the merge is reviewed end-to-end here. Track as separate task if a second `noc_link` is ever added.

---

## Post-implementation gate

- [ ] **P.1: Codex final acceptance review**

  ```bash
  git diff origin/main..HEAD > .noc_cleanup_diff.patch
  ```

  Dispatch `codex:codex-rescue` with the diff. Verify:
  - 3 commits implement the spec exactly
  - No surviving `LoopbackNoc` / `loopback_noc` / `noc_req_intf` / `noc_rsp_intf` outside historical scope
  - `codegen.py --check` exit 0
  - `pytest specgen/tests/` all PASS
  - `make check` green modulo pre-existing GCC ICE

- [ ] **P.2: If Codex flags issues — fix inline + amend or fixup commit. Re-run until clean.**

- [ ] **P.3: Cleanup + push**

  ```bash
  rm .noc_cleanup_diff.patch
  git push origin main
  ```
