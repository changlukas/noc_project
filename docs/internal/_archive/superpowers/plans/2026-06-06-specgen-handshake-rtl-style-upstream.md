# specgen Handshake Upstream + rtl-style Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Upstream the handshake/parameter schema currently hand-written in `cosim/sv/*_intf.sv` into specgen, refactor specgen SV emission to industry-standard style (single `axi4_intf` with master/slave modports, consolidated NoC interface *types* — physical link instances stay separate per topology), and migrate `cosim/sv/` to consume specgen output.

**Architecture:** 2 phases. W1 adds the schema sources (`constants.yaml`, `interface_handshake.json`) and loader/validator. Generator emission unchanged in W1 — drift gate (`py -3 specgen/tools/codegen.py --check`) stays clean. W2 is one atomic PR: refactor emission (new peer emitter modules `sv_params.py`/`cpp_params.py`, rewrite `sv_signals.py` interface block), regenerate artifacts, migrate `cosim/sv/` (preserving 2-hop NoC topology with 4 link instances of consolidated types), update `cosim/verilator/Makefile`. Release-level quality sweep is **deferred** to a follow-up spec — see Phase W3 stub at end of plan.

**Tech Stack:** Python 3 (specgen via `py -3 specgen/tools/codegen.py`), SystemVerilog (Verilator 5.036), C++17 (c_model header-only), CMake/ctest (`c_model/CMakeLists.txt`), MSYS2 on Windows (PATH="/c/msys64/mingw64/bin:$PATH"), pytest (specgen tests), GoogleTest (c_model+cosim tests).

**Source spec:** `docs/superpowers/specs/2026-06-06-specgen-handshake-rtl-style-upstream-design.md` (refreshed through commit `abacdd1`).

**Codebase facts (verified against working tree before plan rewrite):**
- specgen CLI: `py -3 specgen/tools/codegen.py --target {cpp|sv} --domain {packet|signals|registers}` (no `python -m specgen`)
- Drift gate: `py -3 specgen/tools/codegen.py --check`
- Emitter signature: `emit(src_path: Path, spec_version: str) -> str` returning body (banner prepended by codegen.py)
- `DOMAIN_TO_EMITTER` dispatcher in `codegen.py:38` maps `(target, domain)` to `(emitter_func, out_filename, source_json_rel)`
- Verilator source list: `cosim/verilator/Makefile:11` (NO `cosim/CMakeLists.txt`)
- Makefile uses `VERILATOR_FLAGS := ...` recursive assign + already has `-Wno-fatal`; strict mode needs new mechanism
- c_model build root: `c_model/CMakeLists.txt` (NO root CMakeLists.txt)
- Fault injection test: `ctest -R CheckerLiveness` (test PASS when child binary exits nonzero; scenario `injection_aw_unstable.yaml`)
- Cosim2 wrap files: `nmu_wrap.sv`, `nsu_wrap.sv`, `axi_master_wrap.sv`, `axi_slave_wrap.sv`, `loopback_noc_wrap.sv`
- sv_signals.py:emit() uses `out: list[str]` variable; calls `_emit_sv_interfaces(spec, packet_spec)` at line 123

---

## File Structure

### Created

- `specgen/source/constants.yaml` — language-neutral parameter source (SV+C++).
- `specgen/source/interface_handshake.json` — interface schema (axi4_intf, noc_req_intf, noc_rsp_intf).
- `specgen/ni_spec/handshake_schema.py` — loader + comprehensive validator (per spec §4.2).
- `specgen/tools/elaborate/sv_params.py` — new emitter for `ni_params_pkg.sv` (peer to `sv_signals.py`/`sv_packet.py`).
- `specgen/tools/elaborate/cpp_params.py` — new emitter for `ni_params.h` (peer to `cpp_signals.py`/`cpp_packet.py`).
- `specgen/generated/sv/ni_params_pkg.sv` — committed regen output.
- `specgen/generated/cpp/ni_params.h` — committed regen output.
- `specgen/tests/golden/ni_params_pkg.sv.golden` — spec-derived hand-authored golden.
- `specgen/tests/golden/ni_params.h.golden` — spec-derived hand-authored golden.
- `specgen/tests/test_handshake_schema.py` — validator/loader tests.

**Deferred to release-sweep follow-up spec** (NOT created in this plan):
- `specgen/tools/elaborate/signal_interface_md.py` + `specgen/tests/test_signal_interface_md.py` (per-IHI A9-1..A9-4 matrix needs careful classification — see deferred-stub section)
- `specgen/tests/test_parameter_sweep.py` (requires `tb_top` parameter forwarding)
- `cosim/scripts/{run_release_gates,check_reproducible_gen,check_byte_identical_cpp,merge_findings,verilator_param_sweep,check_coverage,check_decisions}.{sh,py}` (release-level tooling)
- `cosim/tests/sv/elab_modport_only_harness.sv` (release-level lint)

### Modified

- `specgen/ni_spec/loader.py` — extend `SpecBundle` with `constants` and `interfaces` fields; loader auto-populates from `source/constants.yaml` + `source/interface_handshake.json`.
- `specgen/tools/codegen.py` — extend `DOMAIN_TO_EMITTER` with `("sv","params")` and `("cpp","params")`; extend `--check` path to include new outputs.
- `specgen/tools/elaborate/sv_signals.py` — rewrite `_emit_sv_interfaces()` to consume `interface_handshake.json` via SpecBundle; replace internal `_emit_sv_interfaces(spec, packet_spec)` call in `emit()` (line 123) with new `_emit_interfaces_from_handshake_schema()`.
- `specgen/generated/sv/ni_signals_pkg.sv` — regenerated (new style).
- `specgen/generated/cpp/ni_signals.h` — regenerated if signal layout downstream changed (likely byte-identical since name set unchanged).
- `specgen/tests/golden/ni_signals_pkg.sv.golden` — regenerated (new style).
- ~~`spec/ni/doc/signal_interface.md`~~ — generator-emit of `## Handshake & Modport Convention` and `## AXI4 Signal Matrix` sections is **DEFERRED** to release-sweep follow-up spec (see spec §4.4); this plan does NOT touch this file.
- `cosim/sv/nmu_wrap.sv` — use specgen `axi4_intf` + `noc_req_intf` + `noc_rsp_intf`; add `clk_i/rst_ni` module ports.
- `cosim/sv/nsu_wrap.sv` — symmetric.
- `cosim/sv/axi_master_wrap.sv` — use `axi4_intf.master`.
- `cosim/sv/axi_slave_wrap.sv` — use `axi4_intf.slave`.
- `cosim/sv/loopback_noc_wrap.sv` — **preserve 4-port topology** with 4 interface instances of types `noc_req_intf` + `noc_rsp_intf` (2 hops × 2 directions).
- `cosim/sv/tb_top.sv` — instantiate 1 axi4_intf for CPU side + 1 axi4_intf for memory side + 4 noc interfaces (req-NMU-side, req-NSU-side, rsp-NSU-side, rsp-NMU-side).
- `cosim/verilator/Makefile` — remove 3 hand-written interface .sv from `SV_SRC`; add specgen-generated `ni_params_pkg.sv` + `ni_signals_pkg.sv` to source list; add `VERILATOR_EXTRA_FLAGS` mechanism for strict gates.

### Deleted

- `cosim/sv/axi_intf.sv`
- `cosim/sv/noc_req_intf.sv`
- `cosim/sv/noc_rsp_intf.sv`

---

# Phase W1 — Schema Sources (single PR, drift gate clean throughout)

PR title: `feat(specgen): add handshake schema + language-neutral constants source`

---

### Task 1: Hand-author `constants.yaml` + spec-derived golden files

**Files:**
- Create: `specgen/source/constants.yaml`
- Create: `specgen/tests/golden/ni_params_pkg.sv.golden`
- Create: `specgen/tests/golden/ni_params.h.golden`

**Rationale:** Golden files MUST be authored independently from the implementation (per Codex round 3 MEDIUM — "self-blessing bug"). Authoring from the spec §5.2 example ensures the impl is forced to match the spec, not the other way around.

- [ ] **Step 1: Write `constants.yaml`** (verbatim from spec §4.1)

```yaml
# specgen/source/constants.yaml
# Language-neutral parameter defaults. Source of truth for both ni_params_pkg.sv
# and ni_params.h. See design spec §4.1.
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

- [ ] **Step 2: Hand-author `ni_params_pkg.sv.golden`** (from spec §5.2; resolve `DATA_WIDTH/8 = 32`)

```systemverilog
package ni_params_pkg;

    // AXI parameter defaults
    parameter int unsigned NI_AXI_ID_WIDTH_DFLT   = 8;
    parameter int unsigned NI_AXI_ADDR_WIDTH_DFLT = 64;
    parameter int unsigned NI_AXI_DATA_WIDTH_DFLT = 256;

    // NoC parameter defaults
    parameter int unsigned NI_NOC_NUM_VC_DFLT                = 1;
    parameter int unsigned NI_NOC_FLIT_WIDTH_DFLT            = 408;
    parameter int unsigned NI_NOC_SLAVE_VC_BUFFER_DEPTH_DFLT = 4;

    // Derived parameter defaults
    parameter int unsigned NI_AXI_WSTRB_WIDTH_DFLT = 32;

endpackage
```

- [ ] **Step 3: Hand-author `ni_params.h.golden`** (must match existing `ni_flit_constants.h` style — `noc::` namespace; check that header against the actual file)

```cpp
#pragma once

#include <cstddef>

namespace noc {

// AXI parameter defaults
constexpr int kNiAxiIdWidth   = 8;
constexpr int kNiAxiAddrWidth = 64;
constexpr int kNiAxiDataWidth = 256;

// NoC parameter defaults
constexpr int kNiNocNumVc               = 1;
constexpr int kNiNocFlitWidth           = 408;
constexpr int kNiNocSlaveVcBufferDepth  = 4;

// Derived parameter defaults
constexpr int kNiAxiWstrbWidth = 32;

}  // namespace noc
```

- [ ] **Step 4: Sanity-check YAML parses**

Run: `py -3 -c "import yaml; d = yaml.safe_load(open('specgen/source/constants.yaml')); print(d['noc']['FLIT_WIDTH'])"`
Expected: `{'type': 'int', 'units': 'bits', ...}` printed.

- [ ] **Step 5: Commit**

```bash
git add specgen/source/constants.yaml \
        specgen/tests/golden/ni_params_pkg.sv.golden \
        specgen/tests/golden/ni_params.h.golden
git commit -m "feat(specgen): add constants.yaml + spec-derived param goldens"
```

---

### Task 2: Implement `handshake_schema.py` validator (all §4.2 rules) + tests

**Files:**
- Create: `specgen/ni_spec/handshake_schema.py`
- Create: `specgen/tests/test_handshake_schema.py`

**Validator covers ALL §4.2 rules:** unknown top-level keys, unknown per-parameter fields, missing required field, type check, default vs range, default vs allowed, naming discipline (`*_W$`, UPPER_SNAKE_CASE), derived constraint evaluation, derived expression symbol-ordering (must reference only already-defined params), circular reference detection in derived expressions, interface schema (kind enum, parameter references valid, modport list non-empty).

- [ ] **Step 1: Write failing tests (broad coverage)**

```python
# specgen/tests/test_handshake_schema.py
"""Validator tests for constants.yaml + interface_handshake.json.

Covers all §4.2 rules from the design spec.
"""
from pathlib import Path
import pytest

from ni_spec.handshake_schema import (
    load_constants,
    load_interfaces,
    HandshakeSchemaError,
)

SOURCE_DIR = Path(__file__).resolve().parent.parent / "source"


# ---- constants.yaml positive cases ----

def test_load_constants_returns_expected_shape():
    c = load_constants(SOURCE_DIR / "constants.yaml")
    assert c["schema_version"] == "1.0"
    assert c["axi"]["ID_WIDTH"]["default"] == 8
    assert c["axi"]["DATA_WIDTH"]["allowed"] == [32, 64, 128, 256, 512, 1024]
    assert c["noc"]["FLIT_WIDTH"]["sv_symbol"] == "NI_NOC_FLIT_WIDTH_DFLT"
    assert c["noc"]["SLAVE_VC_BUFFER_DEPTH"]["default"] == 4
    assert c["derived"]["WSTRB_WIDTH"]["expression"] == "DATA_WIDTH / 8"


# ---- constants.yaml naming discipline ----

def test_rejects_abbreviated_w_suffix(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "noc:\n"
        "  FLIT_W:\n"
        "    type: int\n    default: 408\n"
        "    sv_symbol: X\n    cpp_symbol: x\n"
    )
    with pytest.raises(HandshakeSchemaError, match=r"FLIT_W.*abbreviated.*_WIDTH"):
        load_constants(bad)


def test_rejects_lowercase_param_name(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "axi:\n"
        "  id_width:\n"
        "    type: int\n    default: 8\n"
        "    sv_symbol: X\n    cpp_symbol: x\n"
    )
    with pytest.raises(HandshakeSchemaError, match=r"UPPER_SNAKE_CASE"):
        load_constants(bad)


# ---- constants.yaml structural validation ----

def test_rejects_unknown_top_level_key(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text('schema_version: "1.0"\nbogus: {}\n')
    with pytest.raises(HandshakeSchemaError, match=r"unknown top-level"):
        load_constants(bad)


def test_rejects_unknown_per_param_field(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "axi:\n"
        "  ID_WIDTH:\n"
        "    type: int\n    default: 8\n"
        "    sv_symbol: X\n    cpp_symbol: x\n"
        "    bogus_field: true\n"
    )
    with pytest.raises(HandshakeSchemaError, match=r"unknown.*bogus_field"):
        load_constants(bad)


def test_rejects_missing_required_field(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "axi:\n"
        "  ID_WIDTH:\n    type: int\n    default: 8\n"
    )
    with pytest.raises(HandshakeSchemaError, match=r"missing.*sv_symbol"):
        load_constants(bad)


def test_rejects_wrong_type(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "axi:\n"
        "  ID_WIDTH:\n"
        "    type: bool\n    default: true\n"
        "    sv_symbol: X\n    cpp_symbol: x\n"
    )
    with pytest.raises(HandshakeSchemaError, match=r"unknown type.*bool"):
        load_constants(bad)


# ---- range/allowed checks ----

def test_rejects_default_above_max(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "axi:\n"
        "  ID_WIDTH:\n"
        "    type: int\n    default: 100\n    min: 1\n    max: 32\n"
        "    sv_symbol: X\n    cpp_symbol: x\n"
    )
    with pytest.raises(HandshakeSchemaError, match=r"default 100.*max 32"):
        load_constants(bad)


def test_rejects_default_not_in_allowed(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "axi:\n"
        "  DATA_WIDTH:\n"
        "    type: int\n    default: 33\n    allowed: [32, 64]\n"
        "    sv_symbol: X\n    cpp_symbol: x\n"
    )
    with pytest.raises(HandshakeSchemaError, match=r"default 33 not in allowed"):
        load_constants(bad)


# ---- derived expression checks ----

def test_rejects_derived_referencing_undefined_param(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "derived:\n"
        "  FOO_WIDTH:\n"
        "    type: int\n"
        '    expression: "UNDEFINED_PARAM / 2"\n'
        "    sv_symbol: X\n    cpp_symbol: x\n"
    )
    with pytest.raises(HandshakeSchemaError, match=r"undefined.*UNDEFINED_PARAM"):
        load_constants(bad)


def test_rejects_circular_derived_expression(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "derived:\n"
        "  A_WIDTH:\n"
        "    type: int\n"
        '    expression: "B_WIDTH"\n'
        "    sv_symbol: A_SYM\n    cpp_symbol: a_sym\n"
        "  B_WIDTH:\n"
        "    type: int\n"
        '    expression: "A_WIDTH"\n'
        "    sv_symbol: B_SYM\n    cpp_symbol: b_sym\n"
    )
    with pytest.raises(HandshakeSchemaError, match=r"undefined.*B_WIDTH"):
        load_constants(bad)


def test_rejects_derived_constraint_violation(tmp_path):
    """If DATA_WIDTH default doesn't satisfy WSTRB_WIDTH constraint, error."""
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "axi:\n"
        "  DATA_WIDTH:\n"
        "    type: int\n    default: 33\n"  # not multiple of 8
        "    sv_symbol: X\n    cpp_symbol: x\n"
        "derived:\n"
        "  WSTRB_WIDTH:\n"
        "    type: int\n"
        '    expression: "DATA_WIDTH / 8"\n'
        '    constraint: "DATA_WIDTH % 8 == 0"\n'
        "    sv_symbol: Y\n    cpp_symbol: y\n"
    )
    with pytest.raises(HandshakeSchemaError, match=r"constraint.*violated"):
        load_constants(bad)


# ---- interface_handshake.json positive case ----

def test_load_interfaces_returns_three_named_interfaces():
    c = load_constants(SOURCE_DIR / "constants.yaml")
    data = load_interfaces(SOURCE_DIR / "interface_handshake.json", c)
    assert set(data["interfaces"].keys()) == {
        "axi4_intf", "noc_req_intf", "noc_rsp_intf"
    }


def test_noc_req_intf_protocol_semantics_complete():
    c = load_constants(SOURCE_DIR / "constants.yaml")
    data = load_interfaces(SOURCE_DIR / "interface_handshake.json", c)
    sem = data["interfaces"]["noc_req_intf"]["protocol_semantics"]
    assert sem["credit_return_encoding"]["scheme"] == "per_vc_credit_pulse_vector"
    assert sem["credit_return_encoding"]["onehot_check_required"] is False
    assert sem["initial_credits"]["value_per_vc"] == "SLAVE_VC_BUFFER_DEPTH"
    assert sem["combinational_loops"].startswith("forbidden")


# ---- interface_handshake.json negative cases ----

def test_rejects_interface_with_unknown_kind(tmp_path):
    c = load_constants(SOURCE_DIR / "constants.yaml")
    bad = tmp_path / "bad.json"
    bad.write_text('{"schema_version":"1.0","interfaces":{"x":{"kind":"weird","parameters":[],"modports":["m"]}}}')
    with pytest.raises(HandshakeSchemaError, match=r"unknown.*kind.*weird"):
        load_interfaces(bad, c)


def test_rejects_interface_parameter_referencing_unknown_const(tmp_path):
    c = load_constants(SOURCE_DIR / "constants.yaml")
    bad = tmp_path / "bad.json"
    bad.write_text(
        '{"schema_version":"1.0","interfaces":{'
        '"axi4_intf":{"kind":"axi4","channels":["AW"],'
        '"parameters":[{"name":"X","constants_yaml_key":"noc.BOGUS"}],'
        '"modports":["master","slave"]}'
        '}}'
    )
    with pytest.raises(HandshakeSchemaError, match=r"unknown constants.yaml key.*noc\.BOGUS"):
        load_interfaces(bad, c)


def test_rejects_interface_with_empty_modports(tmp_path):
    c = load_constants(SOURCE_DIR / "constants.yaml")
    bad = tmp_path / "bad.json"
    bad.write_text(
        '{"schema_version":"1.0","interfaces":{'
        '"x":{"kind":"axi4","channels":["AW"],"parameters":[],"modports":[]}'
        '}}'
    )
    with pytest.raises(HandshakeSchemaError, match=r"empty modports"):
        load_interfaces(bad, c)
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd specgen && py -3 -m pytest tests/test_handshake_schema.py -v 2>&1 | head -30`
Expected: `ImportError: cannot import name 'load_constants'` (module not yet implemented).

- [ ] **Step 3: Implement the validator**

```python
# specgen/ni_spec/handshake_schema.py
"""Loader + validator for constants.yaml and interface_handshake.json.

Covers all spec §4.2 rules:
- Unknown top-level key / unknown per-parameter field rejected
- Required fields enforced (type, default, sv_symbol, cpp_symbol)
- Type validation (currently only "int" supported)
- Range validation (default vs min/max/allowed)
- Naming discipline (no *_W abbreviation; UPPER_SNAKE_CASE)
- Derived expression: symbol ordering (only references already-defined params)
- Derived expression: circular reference detection (follows from ordering)
- Derived expression: constraint evaluation (e.g. DATA_WIDTH % 8 == 0)
- Interface schema: kind enum, parameter refs resolve, modports non-empty
"""
from __future__ import annotations
import json
import re
from pathlib import Path
from typing import Any, Dict

import yaml


class HandshakeSchemaError(ValueError):
    """Raised when validation fails."""


_TOP_LEVEL_KEYS = {"schema_version", "axi", "noc", "derived"}
# Plain (axi/noc) and derived require different field sets:
#   plain   → {type, default, sv_symbol, cpp_symbol}
#   derived → {type, expression, sv_symbol, cpp_symbol}
_BASE_REQUIRED = {"type", "sv_symbol", "cpp_symbol"}
_PLAIN_EXTRA   = {"default"}
_DERIVED_EXTRA = {"expression"}
_OPTIONAL_PARAM_FIELDS = {
    "units", "description", "min", "max", "allowed",
    "expression", "default",  # both legal but only one is required-per-kind
    "constraint",              # only for derived
}
_PARAM_NAME_UPPER = re.compile(r"^[A-Z][A-Z0-9_]*$")
_FORBIDDEN_W_END = re.compile(r"_W$")
_SUPPORTED_TYPES = {"int"}
_INTERFACE_KINDS = {"axi4", "noc_link"}


# -------- constants.yaml --------

def load_constants(path: Path) -> Dict[str, Any]:
    data = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise HandshakeSchemaError(f"{path}: top level must be a mapping")
    unknown = set(data) - _TOP_LEVEL_KEYS
    if unknown:
        raise HandshakeSchemaError(
            f"{path}: unknown top-level keys: {sorted(unknown)}"
        )
    if data.get("schema_version") != "1.0":
        raise HandshakeSchemaError(f"{path}: schema_version must be \"1.0\"")

    # Validate plain params first (axi, noc)
    resolved: Dict[str, int] = {}
    for domain in ("axi", "noc"):
        if domain not in data:
            continue
        for name, spec in data[domain].items():
            _validate_param_name(name, where=f"{domain}.{name}")
            _validate_param_spec(spec, where=f"{domain}.{name}", is_derived=False)
            resolved[name] = spec["default"]

    # Then derived (each only references already-resolved symbols)
    if "derived" in data:
        for name, spec in data["derived"].items():
            _validate_param_name(name, where=f"derived.{name}")
            _validate_param_spec(spec, where=f"derived.{name}", is_derived=True)
            value = _eval_derived(name, spec, resolved)
            resolved[name] = value
            if "constraint" in spec:
                if not _eval_constraint(spec["constraint"], resolved):
                    raise HandshakeSchemaError(
                        f"derived.{name}: constraint {spec['constraint']!r} "
                        f"violated by resolved params {resolved}"
                    )

    return data


def _validate_param_name(name: str, where: str) -> None:
    if not _PARAM_NAME_UPPER.match(name):
        raise HandshakeSchemaError(
            f"{where}: parameter name {name!r} must be UPPER_SNAKE_CASE"
        )
    if _FORBIDDEN_W_END.search(name):
        raise HandshakeSchemaError(
            f"{where}: parameter name {name!r} uses abbreviated _W suffix; "
            f"use _WIDTH per naming discipline"
        )


def _validate_param_spec(spec: Dict[str, Any], where: str, is_derived: bool) -> None:
    if not isinstance(spec, dict):
        raise HandshakeSchemaError(f"{where}: spec must be a mapping")
    required = _BASE_REQUIRED | (_DERIVED_EXTRA if is_derived else _PLAIN_EXTRA)
    missing = required - set(spec)
    if missing:
        raise HandshakeSchemaError(f"{where}: missing required field(s): {sorted(missing)}")
    allowed_fields = required | _OPTIONAL_PARAM_FIELDS
    unknown = set(spec) - allowed_fields
    if unknown:
        raise HandshakeSchemaError(f"{where}: unknown field(s): {sorted(unknown)}")
    if spec["type"] not in _SUPPORTED_TYPES:
        raise HandshakeSchemaError(
            f"{where}: unknown type {spec['type']!r}; supported: {sorted(_SUPPORTED_TYPES)}"
        )
    if not is_derived:
        d = spec["default"]
        if "min" in spec and d < spec["min"]:
            raise HandshakeSchemaError(f"{where}: default {d} < min {spec['min']}")
        if "max" in spec and d > spec["max"]:
            raise HandshakeSchemaError(f"{where}: default {d} > max {spec['max']}")
        if "allowed" in spec and d not in spec["allowed"]:
            raise HandshakeSchemaError(
                f"{where}: default {d} not in allowed set {spec['allowed']}"
            )


_SAFE_EXPR_PATTERN = re.compile(r"^[A-Z0-9_+\-*/ %()]+$")


def _eval_derived(name: str, spec: Dict[str, Any], resolved: Dict[str, int]) -> int:
    expr = spec["expression"]
    if not _SAFE_EXPR_PATTERN.match(expr):
        raise HandshakeSchemaError(
            f"derived.{name}: expression contains disallowed characters: {expr!r}"
        )
    # Symbol-ordering: every UPPER_SNAKE_CASE token must already be resolved
    for tok in re.findall(r"[A-Z][A-Z0-9_]*", expr):
        if tok not in resolved:
            raise HandshakeSchemaError(
                f"derived.{name}: expression references undefined symbol {tok!r}"
            )
    try:
        return int(eval(expr, {"__builtins__": {}}, resolved))
    except Exception as exc:
        raise HandshakeSchemaError(f"derived.{name}: eval failed: {exc}")


def _eval_constraint(constraint: str, resolved: Dict[str, int]) -> bool:
    if not _SAFE_EXPR_PATTERN.match(constraint.replace("==", "").replace("!=", "")):
        # Allow == != for constraint expressions
        if not re.match(r"^[A-Z0-9_+\-*/ %()=!<>]+$", constraint):
            raise HandshakeSchemaError(f"constraint disallowed chars: {constraint!r}")
    try:
        return bool(eval(constraint, {"__builtins__": {}}, resolved))
    except Exception as exc:
        raise HandshakeSchemaError(f"constraint eval failed: {exc}")


# -------- interface_handshake.json --------

def load_interfaces(path: Path, constants: Dict[str, Any]) -> Dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema_version") != "1.0":
        raise HandshakeSchemaError(f"{path}: schema_version must be \"1.0\"")
    if "interfaces" not in data:
        raise HandshakeSchemaError(f"{path}: missing 'interfaces' key")

    for iface_name, iface_spec in data["interfaces"].items():
        kind = iface_spec.get("kind")
        if kind not in _INTERFACE_KINDS:
            raise HandshakeSchemaError(
                f"{iface_name}: unknown kind {kind!r}; supported: {sorted(_INTERFACE_KINDS)}"
            )
        modports = iface_spec.get("modports", [])
        if not modports:
            raise HandshakeSchemaError(f"{iface_name}: empty modports list")
        for p in iface_spec.get("parameters", []):
            ref = p.get("constants_yaml_key", "")
            domain, _, key = ref.partition(".")
            if domain not in constants or key not in (constants.get(domain) or {}):
                raise HandshakeSchemaError(
                    f"{iface_name}: parameter {p['name']} references unknown "
                    f"constants.yaml key {ref!r}"
                )
    return data
```

- [ ] **Step 4: Run tests until all pass**

Run: `cd specgen && py -3 -m pytest tests/test_handshake_schema.py -v`
Expected: all positive constants/interfaces tests fail with `FileNotFoundError: interface_handshake.json` — that file gets created in Task 3. All negative tests should PASS.

- [ ] **Step 5: Commit**

```bash
git add specgen/ni_spec/handshake_schema.py specgen/tests/test_handshake_schema.py
git commit -m "feat(specgen): add handshake_schema loader+validator covering all spec §4.2 rules"
```

---

### Task 3: Create `interface_handshake.json` + wire into `SpecBundle`

**Files:**
- Create: `specgen/source/interface_handshake.json`
- Modify: `specgen/ni_spec/loader.py`

- [ ] **Step 1: Write `interface_handshake.json`** (full content from spec §4.3, including NoC `protocol_semantics`)

Copy the full block from spec §4.3 verbatim. Critical fields:
- `axi4_intf`: kind=axi4, parameters[ID_WIDTH/ADDR_WIDTH/DATA_WIDTH], channels=[AW,W,B,AR,R], modports=[master,slave]
- `noc_req_intf`: kind=noc_link, parameters[NUM_VC/FLIT_WIDTH/SLAVE_VC_BUFFER_DEPTH], signals[valid/flit/credit_return], modports=[master,slave], protocol_semantics={transfer_condition, credit_return_encoding={scheme=per_vc_credit_pulse_vector, onehot_check_required=false}, initial_credits, valid_stability, combinational_loops=forbidden}
- `noc_rsp_intf`: same shape as noc_req_intf

- [ ] **Step 2: Extend `SpecBundle` with new fields**

Edit `specgen/ni_spec/loader.py`. After the existing `SpecBundle` dataclass (line 27-35), modify it to include the new fields. The minimal patch:

```python
# Replace the existing @dataclass block with:
@dataclass
class SpecBundle:
    """一次載入相關的所有 spec 檔。packet 為必須，其餘按存在性可選。"""
    spec_dir: Path
    packet: dict
    packet_schema: Optional[dict] = None
    nmu: Optional[dict] = None
    nsu: Optional[dict] = None
    md_dir: Optional[Path] = None
    constants: Optional[dict] = field(default=None)
    interfaces: Optional[dict] = field(default=None)
```

The dataclass already imports `field` (line 4). If not, add `from dataclasses import dataclass, field`.

- [ ] **Step 3: Extend `load_spec_bundle()` to populate new fields**

After the existing `if md_dir is not None:` block (line 55) in `load_spec_bundle`, append before `return bundle`:

```python
    # Load constants.yaml + interface_handshake.json if present.
    # These live in specgen/source/, sibling to ni_spec/, so derive from
    # this file's __file__ path. (spec_dir parameter is the JSON spec dir,
    # which is typically generated/json/ — different directory.)
    from ni_spec.handshake_schema import load_constants, load_interfaces
    specgen_root = Path(__file__).resolve().parent.parent  # specgen/
    constants_yaml = specgen_root / "source" / "constants.yaml"
    interfaces_json = specgen_root / "source" / "interface_handshake.json"
    if constants_yaml.exists():
        bundle.constants = load_constants(constants_yaml)
    if interfaces_json.exists() and bundle.constants is not None:
        bundle.interfaces = load_interfaces(interfaces_json, bundle.constants)
```

- [ ] **Step 4: Run tests — all positive cases now pass**

Run: `cd specgen && py -3 -m pytest tests/test_handshake_schema.py -v`
Expected: all tests PASS.

- [ ] **Step 5: Drift gate sanity check**

Run: `cd specgen && py -3 tools/codegen.py --check`
Expected: exit 0 (no generated artifact changed; loader extension doesn't touch emit paths yet).

- [ ] **Step 6: Commit**

```bash
git add specgen/source/interface_handshake.json specgen/ni_spec/loader.py
git commit -m "feat(specgen): wire constants.yaml + interface_handshake.json into SpecBundle"
```

---

### W1 PR gate

- [ ] `cd specgen && py -3 -m pytest tests/ -v` — all green
- [ ] `cd specgen && py -3 tools/codegen.py --check` — exit 0 (drift clean)
- [ ] `git diff main -- specgen/generated/` — empty

- [ ] **Push branch + open PR**

```bash
git push -u origin stage5b/dpi-wire-wrap
gh pr create --title "feat(specgen): add handshake schema + language-neutral constants source" \
    --body "$(cat <<'EOF'
## Summary
- New specgen/source/constants.yaml (language-neutral SV+C++ parameter source)
- New specgen/source/interface_handshake.json (interface + handshake + modport schema)
- New specgen/ni_spec/handshake_schema.py (loader + validator covering all spec §4.2 rules)
- SpecBundle extended with constants + interfaces fields (no emission change yet)

## Test plan
- [x] specgen pytest all green (10+ new validator tests)
- [x] py -3 specgen/tools/codegen.py --check (drift clean — no regen artifact change)

Refs: spec docs/superpowers/specs/2026-06-06-specgen-handshake-rtl-style-upstream-design.md
EOF
)"
```

W2 PR opens on the same branch after W1 merges to main.

---

# Phase W2 — Atomic Refactor + Regenerate + Migrate (single PR)

W2 is one merged PR. Intermediate sub-commits may not elaborate clean; **final tree** is the CI gate: `py -3 specgen/tools/codegen.py --check` clean + `ctest --output-on-failure` 410/410 PASS + Verilator BASELINE elaboration (0 `%Error`; warnings under the existing `-Wno-fatal` baseline are tolerated). Strict-mode warning-clean elaboration is deferred to the release-sweep follow-up spec.

PR title: `refactor(specgen+cosim): industry-style SV interfaces + atomic migration`

---

### Task 4: New emitter `sv_params.py` + golden test (TDD)

**Files:**
- Create: `specgen/tools/elaborate/sv_params.py`
- Modify: `specgen/tools/codegen.py` (register new emitter)
- Modify: `specgen/tests/test_codegen_sv.py` (golden compare)

- [ ] **Step 1: Add failing golden test**

Append to `specgen/tests/test_codegen_sv.py`:

```python
def test_emit_ni_params_pkg_sv_matches_spec_derived_golden():
    """sv_params.emit() output must match the hand-authored golden from spec §5.2."""
    from pathlib import Path
    from tools.elaborate import sv_params
    from ni_spec.loader import load_spec_version

    src = Path(__file__).resolve().parent.parent / "source" / "constants.yaml"
    out = sv_params.emit(src, load_spec_version())

    golden = (Path(__file__).resolve().parent / "golden" / "ni_params_pkg.sv.golden").read_text()
    assert out == golden, "emitted output differs from spec-derived golden"
```

- [ ] **Step 2: Run test — verify it fails (import error)**

Run: `cd specgen && py -3 -m pytest tests/test_codegen_sv.py::test_emit_ni_params_pkg_sv_matches_spec_derived_golden -v`
Expected: `ImportError: cannot import name 'sv_params'`.

- [ ] **Step 3: Write `sv_params.py` emitter**

```python
# specgen/tools/elaborate/sv_params.py
"""SV emitter for parameter defaults package (ni_params_pkg.sv).

Consumes specgen/source/constants.yaml directly (not via SpecBundle, to mirror
the cpp_packet/cpp_signals direct-source-path convention).
Returns body only — codegen.py prepends the provenance banner.
"""
from __future__ import annotations
from pathlib import Path

from ni_spec.handshake_schema import load_constants


def emit(src_path: Path, spec_version: str) -> str:
    constants = load_constants(src_path)
    lines: list[str] = []
    lines.append("package ni_params_pkg;")
    lines.append("")

    def _emit_domain(domain: str, label: str) -> None:
        if domain not in constants:
            return
        lines.append(f"    // {label}")
        # Column-align the parameter names for readability.
        items = list(constants[domain].items())
        name_col = max(len(spec["sv_symbol"]) for _, spec in items)
        for _, spec in items:
            sym = spec["sv_symbol"]
            val = spec["default"]
            lines.append(f"    parameter int unsigned {sym:<{name_col}} = {val};")
        lines.append("")

    _emit_domain("axi", "AXI parameter defaults")
    _emit_domain("noc", "NoC parameter defaults")

    if "derived" in constants:
        lines.append("    // Derived parameter defaults")
        items = list(constants["derived"].items())
        name_col = max(len(spec["sv_symbol"]) for _, spec in items)
        # For SV emission, resolve expression to a literal integer.
        # Walk axi and noc first to get a value table.
        values: dict[str, int] = {}
        for domain in ("axi", "noc"):
            for n, s in constants.get(domain, {}).items():
                values[n] = s["default"]
        for name, spec in items:
            expr = spec["expression"]
            resolved = int(eval(expr, {"__builtins__": {}}, values))
            sym = spec["sv_symbol"]
            lines.append(f"    parameter int unsigned {sym:<{name_col}} = {resolved};")
        lines.append("")

    lines.append("endpackage")
    return "\n".join(lines) + "\n"
```

- [ ] **Step 4: Register in `DOMAIN_TO_EMITTER`**

Edit `specgen/tools/codegen.py`. Add to imports (line 33):

```python
from tools.elaborate import sv_packet, sv_signals, sv_registers, sv_params
```

Append to `DOMAIN_TO_EMITTER` dict (line 38-45):

```python
DOMAIN_TO_EMITTER: dict[tuple[str, str], tuple] = {
    ("cpp", "packet"):    (cpp_packet.emit,    "ni_flit_constants.h", "generated/json/ni_packet.json"),
    ("cpp", "signals"):   (cpp_signals.emit,   "ni_signals.h",        "generated/json/ni_signals.json"),
    ("cpp", "registers"): (cpp_registers.emit, "ni_regs.h",           "generated/json/ni_registers.json"),
    ("sv",  "packet"):    (sv_packet.emit,    "ni_flit_pkg.sv",       "generated/json/ni_packet.json"),
    ("sv",  "signals"):   (sv_signals.emit,   "ni_signals_pkg.sv",    "generated/json/ni_signals.json"),
    ("sv",  "registers"): (sv_registers.emit, "ni_regs_pkg.sv",       "generated/json/ni_registers.json"),
    ("sv",  "params"):    (sv_params.emit,    "ni_params_pkg.sv",     "source/constants.yaml"),
}
```

Add `"params"` to the `--domain` choices (line 249):

```python
parser.add_argument(
    "--domain", choices=["packet", "signals", "registers", "params"],
    help="spec domain to emit",
)
```

- [ ] **Step 5: Run test — verify it passes**

Run: `cd specgen && py -3 -m pytest tests/test_codegen_sv.py::test_emit_ni_params_pkg_sv_matches_spec_derived_golden -v`
Expected: PASS.

- [ ] **Step 6: Regenerate and confirm output matches golden after banner stripping**

Run: `cd specgen && py -3 tools/codegen.py --target sv --domain params`
Expected: stderr `wrote .../ni_params_pkg.sv`.

Check: `diff <(tail -n +5 specgen/generated/sv/ni_params_pkg.sv) specgen/tests/golden/ni_params_pkg.sv.golden`
Expected: 0 diff (provenance banner is 4 lines of `//`).

- [ ] **Step 7: Commit**

```bash
git add specgen/tools/elaborate/sv_params.py specgen/tools/codegen.py \
        specgen/tests/test_codegen_sv.py specgen/generated/sv/ni_params_pkg.sv
git commit -m "feat(specgen): add sv_params emitter for ni_params_pkg.sv"
```

---

### Task 5: New emitter `cpp_params.py` + golden test (TDD)

**Files:**
- Create: `specgen/tools/elaborate/cpp_params.py`
- Modify: `specgen/tools/codegen.py` (register)
- Modify: `specgen/tests/test_codegen.py` (golden compare — use existing C++ test file)

- [ ] **Step 1: Find existing C++ codegen test file**

Run: `grep -l "ni_flit_constants" specgen/tests/test_codegen*.py`
Expected: identifies the test file used for C++ goldens (likely `test_codegen.py`).

- [ ] **Step 2: Add failing test**

Append to that file:

```python
def test_emit_ni_params_h_matches_spec_derived_golden():
    from pathlib import Path
    from tools.elaborate import cpp_params
    from ni_spec.loader import load_spec_version

    src = Path(__file__).resolve().parent.parent / "source" / "constants.yaml"
    out = cpp_params.emit(src, load_spec_version())

    golden = (Path(__file__).resolve().parent / "golden" / "ni_params.h.golden").read_text()
    assert out == golden
```

- [ ] **Step 3: Run test — verify import failure**

Run: `cd specgen && py -3 -m pytest -v -k ni_params_h`
Expected: `ImportError`.

- [ ] **Step 4: Implement `cpp_params.py`**

```python
# specgen/tools/elaborate/cpp_params.py
"""C++ emitter for parameter defaults header (ni_params.h)."""
from __future__ import annotations
from pathlib import Path

from ni_spec.handshake_schema import load_constants


def emit(src_path: Path, spec_version: str) -> str:
    constants = load_constants(src_path)
    lines: list[str] = []
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <cstddef>")
    lines.append("")
    lines.append("namespace noc {")
    lines.append("")

    def _emit_domain(domain: str, label: str) -> None:
        if domain not in constants:
            return
        lines.append(f"// {label}")
        items = list(constants[domain].items())
        name_col = max(len(spec["cpp_symbol"]) for _, spec in items)
        for _, spec in items:
            sym = spec["cpp_symbol"]
            val = spec["default"]
            lines.append(f"constexpr int {sym:<{name_col}} = {val};")
        lines.append("")

    _emit_domain("axi", "AXI parameter defaults")
    _emit_domain("noc", "NoC parameter defaults")

    if "derived" in constants:
        lines.append("// Derived parameter defaults")
        items = list(constants["derived"].items())
        name_col = max(len(spec["cpp_symbol"]) for _, spec in items)
        values: dict[str, int] = {}
        for domain in ("axi", "noc"):
            for n, s in constants.get(domain, {}).items():
                values[n] = s["default"]
        for name, spec in items:
            expr = spec["expression"]
            resolved = int(eval(expr, {"__builtins__": {}}, values))
            sym = spec["cpp_symbol"]
            lines.append(f"constexpr int {sym:<{name_col}} = {resolved};")
        lines.append("")

    lines.append("}  // namespace noc")
    return "\n".join(lines) + "\n"
```

- [ ] **Step 5: Register in `DOMAIN_TO_EMITTER`**

Add to `specgen/tools/codegen.py` imports + dict:

```python
from tools.elaborate import cpp_packet, cpp_signals, cpp_registers, cpp_params

DOMAIN_TO_EMITTER = {
    ...,
    ("cpp", "params"):    (cpp_params.emit,   "ni_params.h",          "source/constants.yaml"),
    ...
}
```

- [ ] **Step 6: Run test + regen + commit**

```bash
cd specgen && py -3 -m pytest -v -k ni_params_h
# Expected: PASS
cd specgen && py -3 tools/codegen.py --target cpp --domain params
git add specgen/tools/elaborate/cpp_params.py specgen/tools/codegen.py \
        specgen/tests/test_codegen.py specgen/generated/cpp/ni_params.h
git commit -m "feat(specgen): add cpp_params emitter for ni_params.h"
```

---

### Task 6: Hand-author `ni_signals_pkg.sv.golden` for new shape (spec-derived)

**Files:**
- Replace: `specgen/tests/golden/ni_signals_pkg.sv.golden`

**Rationale (Codex MEDIUM):** Author the golden by hand from spec §5.2 BEFORE implementing the emitter rewrite, so the emitter must match the spec rather than the emitter "blessing" its own output.

- [ ] **Step 1: Read current golden to understand the package preamble**

Run: `head -30 specgen/tests/golden/ni_signals_pkg.sv.golden`
Note: existing file starts with `\`ifndef NI_SIGNALS_PKG_SVH` ... `package ni_signals_pkg;` ... reset constants ... `endpackage` ... old interface blocks. The new golden keeps the package preamble (reset constants are unchanged) and replaces the interface blocks.

- [ ] **Step 2: Author the new golden**

Write `specgen/tests/golden/ni_signals_pkg.sv.golden`:
- Keep the existing `\`ifndef NI_SIGNALS_PKG_SVH ... package ni_signals_pkg; ... reset constants ... endpackage` preamble verbatim (copy from current file).
- Replace all old `interface ni_*_intf;` blocks with the spec §5.2 forms:
    - `axi4_intf` with `parameters (ID_WIDTH, ADDR_WIDTH, DATA_WIDTH)`, full 5 channels, `master` + `slave` modports
    - `noc_req_intf` with `parameters (NUM_VC, FLIT_WIDTH, SLAVE_VC_BUFFER_DEPTH)`, 3 signals, 2 modports
    - `noc_rsp_intf` same shape as noc_req
- Keep the trailing `\`endif // NI_SIGNALS_PKG_SVH`.

Hand-author from spec §5.2 verbatim (copy the SV example blocks).

- [ ] **Step 3: Commit golden first**

```bash
git add specgen/tests/golden/ni_signals_pkg.sv.golden
git commit -m "chore(specgen): hand-author ni_signals_pkg.sv golden for new shape"
```

(At this point the corresponding test fails — pytest gold compare diff vs unchanged generator output. This is the intentional W2 mid-PR breakage.)

---

### Task 7: Rewrite `_emit_sv_interfaces()` in `sv_signals.py`

**Files:**
- Modify: `specgen/tools/elaborate/sv_signals.py`

- [ ] **Step 1: Read current `_emit_sv_interfaces()` + `emit()` to understand integration**

Run: `sed -n '40,130p' specgen/tools/elaborate/sv_signals.py`
Note: `emit()` calls `_emit_sv_interfaces(spec, packet_spec)` at line 123. We need to replace this call with one that reads `interface_handshake.json` via the new SpecBundle path.

- [ ] **Step 2: Rewrite the interface emission**

Replace lines 43-78 (the entire `_emit_sv_interfaces` function) with:

```python
# Per-channel signal lists per IHI 0022H §A9.3 Tables A9-1..A9-4.
# Fixed widths per IHI 0022H §A2.2-A2.6.
_AXI_CHANNEL_SIGNALS = {
    "AW": [
        ("awid",     "ID_WIDTH"),   ("awaddr",   "ADDR_WIDTH"),
        ("awlen",    "fixed:8"),    ("awsize",   "fixed:3"),
        ("awburst",  "fixed:2"),    ("awlock",   "fixed:1"),
        ("awcache",  "fixed:4"),    ("awprot",   "fixed:3"),
        ("awqos",    "fixed:4"),    ("awregion", "fixed:4"),
        ("awvalid",  "fixed:1"),    ("awready",  "fixed:1"),
    ],
    "W": [
        ("wdata",  "DATA_WIDTH"),  ("wstrb",  "WSTRB_WIDTH"),
        ("wlast",  "fixed:1"),     ("wvalid", "fixed:1"),
        ("wready", "fixed:1"),
    ],
    "B": [
        ("bid",    "ID_WIDTH"),  ("bresp",  "fixed:2"),
        ("bvalid", "fixed:1"),   ("bready", "fixed:1"),
    ],
    "AR": [
        ("arid",     "ID_WIDTH"),   ("araddr",   "ADDR_WIDTH"),
        ("arlen",    "fixed:8"),    ("arsize",   "fixed:3"),
        ("arburst",  "fixed:2"),    ("arlock",   "fixed:1"),
        ("arcache",  "fixed:4"),    ("arprot",   "fixed:3"),
        ("arqos",    "fixed:4"),    ("arregion", "fixed:4"),
        ("arvalid",  "fixed:1"),    ("arready",  "fixed:1"),
    ],
    "R": [
        ("rid",    "ID_WIDTH"),   ("rdata",  "DATA_WIDTH"),
        ("rresp",  "fixed:2"),    ("rlast",  "fixed:1"),
        ("rvalid", "fixed:1"),    ("rready", "fixed:1"),
    ],
}

_MASTER_DRIVES_AXI = {
    "awid","awaddr","awlen","awsize","awburst","awlock","awcache",
    "awprot","awqos","awregion","awvalid",
    "wdata","wstrb","wlast","wvalid",
    "bready",
    "arid","araddr","arlen","arsize","arburst","arlock","arcache",
    "arprot","arqos","arregion","arvalid",
    "rready",
}


def _format_width(width_spec: str) -> str:
    if width_spec.startswith("fixed:"):
        n = int(width_spec.split(":", 1)[1])
        return "" if n == 1 else f"[{n-1}:0]"
    return f"[{width_spec}-1:0]"


def _emit_axi4_intf(name: str, spec: dict, constants: dict) -> list[str]:
    out: list[str] = []
    params = spec["parameters"]
    out.append(f"interface {name} #(")
    for i, p in enumerate(params):
        domain, key = p["constants_yaml_key"].split(".")
        sym = constants[domain][key]["sv_symbol"]
        comma = "," if i < len(params) - 1 else ""
        out.append(f"    parameter int unsigned {p['name']:11s} = ni_params_pkg::{sym}{comma}")
    out.append(");")
    out.append("    localparam int unsigned WSTRB_WIDTH = DATA_WIDTH / 8;")
    out.append("")
    for ch in spec["channels"]:
        for sig_name, width_spec in _AXI_CHANNEL_SIGNALS[ch]:
            w = _format_width(width_spec)
            if w:
                out.append(f"    logic {w:18s} {sig_name};")
            else:
                out.append(f"    logic                   {sig_name};")
        out.append("")
    master_drv = [s for ch in spec["channels"] for s, _ in _AXI_CHANNEL_SIGNALS[ch] if s in _MASTER_DRIVES_AXI]
    slave_drv  = [s for ch in spec["channels"] for s, _ in _AXI_CHANNEL_SIGNALS[ch] if s not in _MASTER_DRIVES_AXI]
    out.append("    modport master (")
    out.append("        output " + ", ".join(master_drv) + ",")
    out.append("        input  " + ", ".join(slave_drv))
    out.append("    );")
    out.append("")
    out.append("    modport slave (")
    out.append("        input  " + ", ".join(master_drv) + ",")
    out.append("        output " + ", ".join(slave_drv))
    out.append("    );")
    out.append(f"endinterface : {name}")
    return out


def _emit_noc_intf(name: str, spec: dict, constants: dict) -> list[str]:
    out: list[str] = []
    params = spec["parameters"]
    out.append(f"interface {name} #(")
    for i, p in enumerate(params):
        domain, key = p["constants_yaml_key"].split(".")
        sym = constants[domain][key]["sv_symbol"]
        comma = "," if i < len(params) - 1 else ""
        out.append(f"    parameter int unsigned {p['name']:21s} = ni_params_pkg::{sym}{comma}")
    out.append(");")
    out.append("    logic                    valid;")
    out.append("    logic [FLIT_WIDTH-1:0]   flit;")
    out.append("    logic [NUM_VC-1:0]       credit_return;")
    out.append("")
    out.append("    modport master ( output valid, flit, input  credit_return );")
    out.append("    modport slave  ( input  valid, flit, output credit_return );")
    out.append(f"endinterface : {name}")
    return out


def _emit_interfaces_from_handshake_schema(interfaces_doc: dict, constants: dict) -> list[str]:
    """Emit all interface blocks per interface_handshake.json.

    Returns lines; SV interfaces must live OUTSIDE any package (LRM constraint).
    """
    out: list[str] = []
    for name, spec in interfaces_doc["interfaces"].items():
        if spec["kind"] == "axi4":
            out.extend(_emit_axi4_intf(name, spec, constants))
        elif spec["kind"] == "noc_link":
            out.extend(_emit_noc_intf(name, spec, constants))
        else:
            raise ValueError(f"unknown interface kind: {spec['kind']}")
        out.append("")
    return out
```

- [ ] **Step 3: Update `emit()` to call new function**

In `emit()` (line 81+), replace the call at line 123:

```python
# OLD:
# out.extend(_emit_sv_interfaces(spec, packet_spec))

# NEW:
from ni_spec.handshake_schema import load_constants, load_interfaces
specgen_root = Path(__file__).resolve().parent.parent.parent  # specgen/
constants = load_constants(specgen_root / "source" / "constants.yaml")
interfaces_doc = load_interfaces(specgen_root / "source" / "interface_handshake.json", constants)
out.extend(_emit_interfaces_from_handshake_schema(interfaces_doc, constants))
```

- [ ] **Step 4: Run pytest — golden compare should pass**

Run: `cd specgen && py -3 -m pytest tests/test_codegen_sv.py -v 2>&1 | tail -30`
Expected: golden test for `ni_signals_pkg.sv` PASS (matches the hand-authored golden from Task 6).

If diff exists: inspect with `diff <(py -3 tools/codegen.py --target sv --domain signals --out /tmp/sv) tests/golden/ni_signals_pkg.sv.golden` and adjust emitter formatting until byte-identical.

- [ ] **Step 5: Regen + drift gate**

```bash
cd specgen && py -3 tools/codegen.py --target sv --domain signals
cd specgen && py -3 tools/codegen.py --check
```

Expected: drift gate exits 0.

- [ ] **Step 6: Commit**

```bash
git add specgen/tools/elaborate/sv_signals.py \
        specgen/generated/sv/ni_signals_pkg.sv
git commit -m "refactor(specgen): rewrite SV interface emission for axi4_intf + consolidated NoC types"
```

---

### Task 8: `signal_interface.md` generator-emit — **DEFERRED**

Per spec §4.4 (deferred to release-sweep follow-up spec): the `## Handshake & Modport Convention` and `## AXI4 Signal Matrix (per IHI 0022H §A9.3)` sections of `signal_interface.md` will be generator-emitted. Codex round 5 verified that the per-row Manager-required / Memory-Subordinate-required classification is non-trivial and Codex round 6 caught an attempted shortcut classifying most signals incorrectly.

Skipping this task in this plan keeps W2 self-consistent: no generated markdown is committed with incorrect signal-role data. The follow-up spec authors the markdown emitter with line-by-line IHI Table A9-1..A9-4 reading + separate review.

This task is intentionally a no-op: skip and proceed to Task 9.

### Task 9: Regenerate all C++ artifacts + verify byte-identical (defensive)

**Files:**
- Possibly modify: `specgen/generated/cpp/ni_signals.h`

- [ ] **Step 1: Regen everything**

```bash
cd specgen
py -3 tools/codegen.py --target cpp --domain packet
py -3 tools/codegen.py --target cpp --domain signals
py -3 tools/codegen.py --target cpp --domain registers
py -3 tools/codegen.py --target cpp --domain params
py -3 tools/codegen.py --target sv  --domain packet
py -3 tools/codegen.py --target sv  --domain signals
py -3 tools/codegen.py --target sv  --domain registers
py -3 tools/codegen.py --target sv  --domain params
```

- [ ] **Step 2: Verify drift**

```bash
cd specgen && py -3 tools/codegen.py --check
```

Expected: exit 0.

- [ ] **Step 3: Commit any incidental cpp regen**

```bash
git add specgen/generated/cpp/
git diff --cached --stat
git commit -m "chore(specgen): regenerate cpp artifacts (incidental byte refresh)"
```

(If `git diff --cached --stat` is empty, skip the commit — there's nothing to add.)

---

### Task 10: Migrate `nmu_wrap.sv`

**Files:**
- Modify: `cosim/sv/nmu_wrap.sv`

- [ ] **Step 1: Read current file**

```bash
sed -n '1,60p' cosim/sv/nmu_wrap.sv
```

Note current parameters, port list, and how each `axi_intf` / `noc_*_intf` reference is used in `always_ff` blocks.

- [ ] **Step 2: Rewrite module header**

Replace the module header (top of file through closing `);`) with:

```systemverilog
module nmu_wrap #(
    parameter int unsigned ID_WIDTH              = ni_params_pkg::NI_AXI_ID_WIDTH_DFLT,
    parameter int unsigned ADDR_WIDTH            = ni_params_pkg::NI_AXI_ADDR_WIDTH_DFLT,
    parameter int unsigned DATA_WIDTH            = ni_params_pkg::NI_AXI_DATA_WIDTH_DFLT,
    parameter int unsigned NUM_VC                = ni_params_pkg::NI_NOC_NUM_VC_DFLT,
    parameter int unsigned FLIT_WIDTH            = ni_params_pkg::NI_NOC_FLIT_WIDTH_DFLT,
    parameter int unsigned SLAVE_VC_BUFFER_DEPTH = ni_params_pkg::NI_NOC_SLAVE_VC_BUFFER_DEPTH_DFLT
) (
    input  logic              clk_i,
    input  logic              rst_ni,
    axi4_intf.slave           axi_i,
    noc_req_intf.master       noc_req_o,
    noc_rsp_intf.slave        noc_rsp_i
);
```

- [ ] **Step 3: Rename in-body interface accesses**

Inside the module body, mechanically rename:
- All `<old_intf_name>.<sig>` references to use the new interface names:
    - `axi_i.<sig>` (where `<sig>` is the bare AMBA name, e.g., `awvalid`, `wdata`, `bid`)
    - `noc_req_o.valid` / `noc_req_o.flit` / `noc_req_o.credit_return`
    - `noc_rsp_i.valid` / `noc_rsp_i.flit` / `noc_rsp_i.credit_return`
- Old prefixed forms in the body (e.g., `noc_req_valid` if used as a bundle signal name) → `noc_req_o.valid`. Do this via grep + targeted Edit.

- [ ] **Step 4: Commit**

```bash
git add cosim/sv/nmu_wrap.sv
git commit -m "refactor(cosim): migrate nmu_wrap to specgen axi4_intf + noc_*_intf"
```

---

### Task 11: Migrate `nsu_wrap.sv`

Same shape as Task 10 with directions mirrored:

- [ ] **Step 1: Rewrite header**

```systemverilog
module nsu_wrap #(
    parameter int unsigned ID_WIDTH              = ni_params_pkg::NI_AXI_ID_WIDTH_DFLT,
    parameter int unsigned ADDR_WIDTH            = ni_params_pkg::NI_AXI_ADDR_WIDTH_DFLT,
    parameter int unsigned DATA_WIDTH            = ni_params_pkg::NI_AXI_DATA_WIDTH_DFLT,
    parameter int unsigned NUM_VC                = ni_params_pkg::NI_NOC_NUM_VC_DFLT,
    parameter int unsigned FLIT_WIDTH            = ni_params_pkg::NI_NOC_FLIT_WIDTH_DFLT,
    parameter int unsigned SLAVE_VC_BUFFER_DEPTH = ni_params_pkg::NI_NOC_SLAVE_VC_BUFFER_DEPTH_DFLT
) (
    input  logic              clk_i,
    input  logic              rst_ni,
    noc_req_intf.slave        noc_req_i,
    noc_rsp_intf.master       noc_rsp_o,
    axi4_intf.master          axi_o
);
```

- [ ] **Step 2: Body renames + commit**

```bash
git add cosim/sv/nsu_wrap.sv
git commit -m "refactor(cosim): migrate nsu_wrap to specgen interfaces"
```

---

### Task 12: Migrate `axi_master_wrap.sv` + `axi_slave_wrap.sv`

- [ ] **Step 1: Rewrite `axi_master_wrap.sv` header**

```systemverilog
module axi_master_wrap #(
    parameter int unsigned ID_WIDTH    = ni_params_pkg::NI_AXI_ID_WIDTH_DFLT,
    parameter int unsigned ADDR_WIDTH  = ni_params_pkg::NI_AXI_ADDR_WIDTH_DFLT,
    parameter int unsigned DATA_WIDTH  = ni_params_pkg::NI_AXI_DATA_WIDTH_DFLT
) (
    input  logic       clk_i,
    input  logic       rst_ni,
    axi4_intf.master   axi_o
);
```

Body renames: existing `<old>.<sig>` → `axi_o.<sig>`.

- [ ] **Step 2: Rewrite `axi_slave_wrap.sv` header**

```systemverilog
module axi_slave_wrap #(
    parameter int unsigned ID_WIDTH    = ni_params_pkg::NI_AXI_ID_WIDTH_DFLT,
    parameter int unsigned ADDR_WIDTH  = ni_params_pkg::NI_AXI_ADDR_WIDTH_DFLT,
    parameter int unsigned DATA_WIDTH  = ni_params_pkg::NI_AXI_DATA_WIDTH_DFLT
) (
    input  logic       clk_i,
    input  logic       rst_ni,
    axi4_intf.slave    axi_i
);
```

Body renames: `<old>.<sig>` → `axi_i.<sig>`.

- [ ] **Step 3: Commit**

```bash
git add cosim/sv/axi_master_wrap.sv cosim/sv/axi_slave_wrap.sv
git commit -m "refactor(cosim): migrate axi_*_wrap to axi4_intf"
```

---

### Task 13: Migrate `loopback_noc_wrap.sv` — **preserve 4-port topology**

**Files:**
- Modify: `cosim/sv/loopback_noc_wrap.sv`

**Rationale (Codex round 3 CRITICAL):** The loopback NoC participates in `NMU → loopback → NSU → loopback → NMU` flow. Each leg is a distinct interface *instance*; collapsing to 2 ports destroys the topology. Consolidate interface *types* (noc_req_intf, noc_rsp_intf) while keeping 4 port *instances*.

- [ ] **Step 1: Read current file**

```bash
sed -n '1,50p' cosim/sv/loopback_noc_wrap.sv
```

Note the existing 4 ports — typically `noc_req_in`, `noc_req_out`, `noc_rsp_in`, `noc_rsp_out` (or similarly named).

- [ ] **Step 2: Rewrite header preserving all 4 port instances**

```systemverilog
module loopback_noc_wrap #(
    parameter int unsigned NUM_VC                = ni_params_pkg::NI_NOC_NUM_VC_DFLT,
    parameter int unsigned FLIT_WIDTH            = ni_params_pkg::NI_NOC_FLIT_WIDTH_DFLT,
    parameter int unsigned SLAVE_VC_BUFFER_DEPTH = ni_params_pkg::NI_NOC_SLAVE_VC_BUFFER_DEPTH_DFLT
) (
    input  logic              clk_i,
    input  logic              rst_ni,
    // Request: NMU -> loopback -> NSU
    noc_req_intf.slave        noc_req_from_nmu_i,   // NMU drives, loopback receives
    noc_req_intf.master       noc_req_to_nsu_o,     // loopback drives, NSU receives
    // Response: NSU -> loopback -> NMU
    noc_rsp_intf.slave        noc_rsp_from_nsu_i,
    noc_rsp_intf.master       noc_rsp_to_nmu_o
);
```

- [ ] **Step 3: Rename body references**

Each prior single-direction port reference becomes the appropriately-named new port:
- old `noc_req_in.valid` → `noc_req_from_nmu_i.valid`
- old `noc_req_out.valid` → `noc_req_to_nsu_o.valid`
- ... etc.

- [ ] **Step 4: Commit**

```bash
git add cosim/sv/loopback_noc_wrap.sv
git commit -m "refactor(cosim): migrate loopback_noc_wrap to consolidated NoC types (keeping 4-port topology)"
```

---

### Task 14: Migrate `tb_top.sv` — 1 CPU-side AXI + 1 mem-side AXI + 4 NoC links

**Files:**
- Modify: `cosim/sv/tb_top.sv`
- Modify: `cosim/verilator/Makefile`
- Delete: `cosim/sv/axi_intf.sv`, `cosim/sv/noc_req_intf.sv`, `cosim/sv/noc_rsp_intf.sv`

- [ ] **Step 1: Read current `tb_top.sv` to understand existing wire/instance layout**

```bash
sed -n '1,80p' cosim/sv/tb_top.sv
```

Note the existing instance names + how the 5 wraps are wired today.

- [ ] **Step 2: READ existing `tb_top.sv` and identify what to preserve**

Run: `sed -n '1,40p' cosim/sv/tb_top.sv`

Observe:
- `tb_top` has `clk_i` and `rst_ni` as **input ports** driven by `main.cpp` (via Verilator's top eval loop) — DO NOT redeclare them as internal `logic`
- There is NO embedded clock generator in `tb_top.sv` itself; `main.cpp` toggles the clock
- DPI imports, scoreboard wiring, and `$finish` triggers live mid-file — preserve them

The rewrite touches ONLY the interface instantiation + module instance block. Preserve all other lines verbatim.

- [ ] **Step 3: Rewrite the interface instantiation + module-instance block**

Identify the existing `axi_intf #() ...;`, `noc_req_intf #() ...;`, and module instance lines. Replace that block (only) with:

```systemverilog
    // 2 AXI links: CPU-side + memory-side
    axi4_intf #() axi_cpu_link();
    axi4_intf #() axi_mem_link();

    // 4 NoC link instances of consolidated types (2 hops × req/rsp)
    noc_req_intf #() noc_req_nmu_to_lp();
    noc_req_intf #() noc_req_lp_to_nsu();
    noc_rsp_intf #() noc_rsp_nsu_to_lp();
    noc_rsp_intf #() noc_rsp_lp_to_nmu();

    axi_master_wrap u_cpu_master (
        .clk_i(clk_i), .rst_ni(rst_ni),
        .axi_o(axi_cpu_link)
    );

    nmu_wrap u_nmu (
        .clk_i(clk_i), .rst_ni(rst_ni),
        .axi_i(axi_cpu_link),
        .noc_req_o(noc_req_nmu_to_lp),
        .noc_rsp_i(noc_rsp_lp_to_nmu)
    );

    loopback_noc_wrap u_loopback (
        .clk_i(clk_i), .rst_ni(rst_ni),
        .noc_req_from_nmu_i(noc_req_nmu_to_lp),
        .noc_req_to_nsu_o(noc_req_lp_to_nsu),
        .noc_rsp_from_nsu_i(noc_rsp_nsu_to_lp),
        .noc_rsp_to_nmu_o(noc_rsp_lp_to_nmu)
    );

    nsu_wrap u_nsu (
        .clk_i(clk_i), .rst_ni(rst_ni),
        .noc_req_i(noc_req_lp_to_nsu),
        .noc_rsp_o(noc_rsp_nsu_to_lp),
        .axi_o(axi_mem_link)
    );

    axi_slave_wrap u_mem_slave (
        .clk_i(clk_i), .rst_ni(rst_ni),
        .axi_i(axi_mem_link)
    );
```

Module header (`module tb_top (input logic clk_i, input logic rst_ni);` or whatever it actually is), DPI imports, `final` blocks, scoreboard hooks, and `$finish` logic are NOT touched.

- [ ] **Step 3: Update `cosim/verilator/Makefile` — remove deleted .sv, add specgen sources, add VERILATOR_EXTRA_FLAGS**

Edit `cosim/verilator/Makefile`. Find the `SV_SRC := \` block (line 11-23). Rewrite as:

```makefile
SV_SRC := \
    $(SPECGEN_INC)/../sv/ni_params_pkg.sv \
    $(SPECGEN_INC)/../sv/ni_signals_pkg.sv \
    $(SPECGEN_INC)/../sv/ni_flit_pkg.sv \
    $(SPECGEN_INC)/../sv/ni_regs_pkg.sv \
    $(COSIM_ROOT)/sv/axi_master_wrap.sv \
    $(COSIM_ROOT)/sv/nmu_wrap.sv \
    $(COSIM_ROOT)/sv/loopback_noc_wrap.sv \
    $(COSIM_ROOT)/sv/nsu_wrap.sv \
    $(COSIM_ROOT)/sv/axi_slave_wrap.sv \
    $(COSIM_ROOT)/sv/wb2axip/faxi_wstrb.v \
    $(COSIM_ROOT)/sv/wb2axip/faxi_master.v \
    $(COSIM_ROOT)/sv/wb2axip/faxi_slave.v \
    $(COSIM_ROOT)/sv/tb_top.sv
```

Add `SPECGEN_SV_INC` near the top (line 7 area):

```makefile
SPECGEN_SV_INC := $(PROJ_ROOT)/specgen/generated/sv
```

And replace `$(SPECGEN_INC)/../sv/` with `$(SPECGEN_SV_INC)/` in the SV_SRC list.

**Add `VERILATOR_EXTRA_FLAGS` mechanism** (Codex round 3 HIGH). After the existing `VERILATOR_FLAGS := ...` block (lines 31-49), insert:

```makefile
# Optional strict-mode flag injection for release gates.
# Override with: make VERILATOR_EXTRA_FLAGS="--Wall --Wpedantic"
VERILATOR_EXTRA_FLAGS ?=
VERILATOR_FLAGS += $(VERILATOR_EXTRA_FLAGS)
```

And in the actual build invocation (line 62), keep `$(VERILATOR_FLAGS)` (which now includes the extras).

- [ ] **Step 4: Delete the three hand-written interface files**

```bash
git rm cosim/sv/axi_intf.sv cosim/sv/noc_req_intf.sv cosim/sv/noc_rsp_intf.sv
```

- [ ] **Step 5: Commit**

```bash
git add cosim/sv/tb_top.sv cosim/verilator/Makefile
git commit -m "refactor(cosim): migrate tb_top + Makefile; remove hand-written interfaces"
```

---

### Task 15: W2 final CI gate

**Files:** none (verification only).

- [ ] **Step 1: specgen pytest**

```bash
cd specgen && py -3 -m pytest -v
```

Expected: all green.

- [ ] **Step 2: Drift gate**

```bash
cd specgen && py -3 tools/codegen.py --check
```

Expected: exit 0.

- [ ] **Step 3: Build c_model + cosim from scratch**

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cd /e/05_NoC/noc_project/c_model
cmake -S . -B build
cmake --build build -j

cd ../cosim/verilator
make clean
make
```

Expected: clean build, no errors.

- [ ] **Step 4: ctest full sweep**

```bash
cd /e/05_NoC/noc_project/c_model/build
ctest --output-on-failure
```

Expected: 410/410 PASS (matches Stage 5b baseline).

- [ ] **Step 5: Run the explicit 5-fixture smoke set** (per Codex round 3 MEDIUM)

Use two separate loops — positive fixtures fail loudly; negative fixture must exit nonzero or the gate FAILS:

```bash
cd /e/05_NoC/noc_project/cosim/verilator
set -e
echo "=== positive fixtures (must exit 0) ==="
for fix in debug_multi1.yaml write_only_smoke.yaml \
           multibeat_incr_8beat.yaml multi_id_single_beat_sequential.yaml; do
    echo "--- $fix ---"
    ./obj_dir/Vtb_top "+scenario=../tests/fixtures/$fix"
done
echo "=== negative fixture (must exit nonzero) ==="
if ./obj_dir/Vtb_top "+scenario=../tests/fixtures/injection_aw_unstable.yaml"; then
    echo "GATE FAIL: injection_aw_unstable.yaml exited zero (checker dead?)"
    exit 1
fi
echo "=== smoke set OK ==="
set +e
```

Expected: positive loop completes with no error; negative fixture exits nonzero as required; "smoke set OK" printed.

- [ ] **Step 6: Verilator baseline elaboration**

```bash
set -o pipefail
cd /e/05_NoC/noc_project/cosim/verilator
make clean
make 2>&1 | tee /tmp/verilator_build.log
! grep -q "^%Error" /tmp/verilator_build.log
```

Expected: the final `! grep -q` exits 0 (no `%Error` lines found). `set -o pipefail` ensures a failing `make` propagates; the negated `grep` inverts grep's "no match = exit 1" so absence-of-errors becomes the success condition. Warnings under the existing Makefile baseline (`-Wno-fatal` in `VERILATOR_FLAGS`) are tolerated at this gate — strict-mode warning-clean elaboration is deferred to the release-sweep follow-up spec.

- [ ] **Step 7: Push + open PR**

```bash
git push -u origin stage5b/dpi-wire-wrap
gh pr create --title "refactor(specgen+cosim): industry-style SV interfaces + atomic migration" \
  --body "$(cat <<'EOF'
## Summary
- Single axi4_intf with master/slave modports (collapses 2 prior interfaces)
- Consolidates 4 NoC interfaces to noc_req_intf + noc_rsp_intf (interface *types*; physical link instances preserved per 2-hop topology)
- constants.yaml as language-neutral source of truth for SV+C++ parameter defaults
- New ni_params_pkg.sv + ni_params.h emitters added to codegen.py DOMAIN_TO_EMITTER
- 5 wraps + tb_top migrated; hand-written cosim/sv/{axi,noc_req,noc_rsp}_intf.sv removed
- cosim/verilator/Makefile SV_SRC list updated for specgen-generated sources

## Test plan
- [x] specgen pytest all green
- [x] py -3 specgen/tools/codegen.py --check (drift clean)
- [x] ctest 410/410
- [x] 5-fixture smoke (4 PASS + injection_aw_unstable exits nonzero — CheckerLiveness DPI-error injection PRESERVED)
- [x] Verilator baseline elaboration: 0 errors

Note: release-level verification (strict warning-clean elaboration, lint, sanitizer, coverage thresholds, wb2axip protocol-violation fault injection, parameter override sweep, release tag) is deferred to a follow-up spec — see plan §"Phase W3 — DEFERRED". This PR does NOT remove or weaken the existing DPI-error injection (`CheckerLiveness` ctest); only adds nothing further.

Refs: spec docs/superpowers/specs/2026-06-06-specgen-handshake-rtl-style-upstream-design.md
EOF
)"
```

---

# Deferred work (post-W2)

After Codex round 5 surfaced unresolved HIGH-severity gaps when this plan attempted to bundle 12 release-level verification gates into one document, **release-level verification is deferred to a follow-up spec.** The Karpathy 4-lens + magic-number sweep is independently deferred to its own already-written spec (`2026-06-06-karpathy-quality-sweep-design.md`).

### Why deferred

Codex round 5 verified concrete codebase gaps in each gate:

- **Parameter sweep**: `tb_top` has no `ID_WIDTH/ADDR_WIDTH/...` parameters — `-G<PARAM>=<value>` overrides cannot work without first parameterizing `tb_top` and forwarding into all interface instantiations.
- **Coverage**: Verilator's `%` and `~` annotation markers mean *below-threshold* and *partial coverage*, not separate line vs toggle. Official parsing requires `--annotate-points` and parsing `type=line` / `type=toggle` records, or parsing the coverage data file directly.
- **AXI signal matrix (per IHI 0022H Tables A9-1..A9-4)**: many signals previously marked "Required for Manager" are actually OPTIONAL (`AWID`, `AWLEN`, `AWSIZE`, `AWBURST`, `AWLOCK`, `AWCACHE`, `WSTRB`, `BID`, `BRESP`). Correct classification requires line-by-line table reading, not a single-pass annotation.
- **Fault injection (protocol violation)**: existing `scenario_parser` only supports `config.inject.mode/cycle`. A transaction-level `inject.wlast_violation` field requires schema extension + `AxiMaster` extension + `MasterShellAdapter` extension. This is multi-file work, not a fixture-author task.
- **Outstanding-ID test**: same-ID outstanding writes are LEGAL per AXI4 §A5.2.2; testing them needs a scoreboard assertion observing B-response issue order. The assertion code was not previously specified.
- **Aggregator**: `ctest -R <pattern>` returns 0 when matching no tests; deferred/missing tests silently pass. Requires `--no-tests=error`.

### Deferred scope inventory

Each item is an independent sub-task — likely each needing its own brainstorm in the follow-up spec:

| Gate | Description |
|---|---|
| Karpathy 4-lens + magic-number sweep | 3-zone parallel subagent + Codex, findings de-dup, user decision matrix |
| Lint clean | clang-tidy over c_model + verible-verilog-lint over SV |
| Verilator strict warning-clean | `--Wall --Wpedantic` over existing baseline; requires `VERILATOR_EXTRA_FLAGS` mechanism |
| Parameter sweep (specgen schema) | yaml validator matrix 432 combos + invalid negative |
| Parameter sweep (SV elaboration) | tb_top parameterized + Verilator `-G` corner matrix |
| Sanitizer | UBSan + ASan over c_model (Verilator C++ excluded) |
| Coverage | per-scenario `coverage_<name>.dat` + official format parsing + line/toggle thresholds + exclusions |
| Reproducible generation | 2-clean-tree byte-identical check |
| Fault injection (DPI path) | existing `CheckerLiveness` |
| Fault injection (wb2axip protocol path) | `scenario_parser` extension + AxiMaster `wlast_violation` inject + new ctest |
| C++ byte-identical | regen-then-diff |
| Spec §7.3 scenarios | backpressure / outstanding-ID (legal in-order assert) / 5-channel connectivity; reset-mid-burst + AW-W-simultaneous further blocked on parser extension |
| Release tag `v0.5.0` | tagging + automated gate aggregator script |

### Follow-up path

The follow-up spec lives at `docs/superpowers/specs/YYYY-MM-DD-release-quality-sweep-design.md` (date TBD). Each gate above gets its own brainstorm + design section, with the verification command verified against the actual codebase before plan-writing.

---

## Self-Review

**Spec coverage (W1+W2 only):**
- §1.2 W1 schema → Tasks 1-3 ✓
- §1.2 W2 atomic → Tasks 4-15 ✓ (Task 8 deferred-no-op per spec §4.4)
- §2.1 axi4_intf single → Tasks 6-7 ✓
- §2.1 NoC type consolidation w/ topology preserved → Tasks 13, 14 ✓
- §2.2 naming discipline → Task 2 (validator rejects `*_W$`, lowercase) ✓
- §4.1 constants.yaml → Task 1 ✓
- §4.3 interface_handshake.json (3 interfaces: 1 AXI + 2 NoC types; incl. NoC `protocol_semantics`, multi-hot credit pulse vector) → Task 3 ✓
- §4.4 signal_interface.md generator-emit → DEFERRED → Task 8 is a deferred no-op ✓
- §5.2 SV emission examples → Tasks 4, 6 (hand-authored golden), 7 (emitter rewrite) ✓
- §5.3 wrap migration → Tasks 10-14 (preserves 4-link NoC topology in Task 13) ✓
- §6 W1/W2 test plan → Task 15 final CI gate ✓
- §7 Open Item O8 (release-sweep deferred) → recorded as deferred-stub section, no tasks in this plan ✓

**Placeholder scan:** `sv_params.py` / `cpp_params.py` / `handshake_schema.py` all defined inline with full code in Tasks 2, 4, 5. Drift command (`py -3 specgen/tools/codegen.py --check` per `codegen.py:11`). Makefile path (`cosim/verilator/Makefile` per actual location, NOT `cosim/CMakeLists.txt`). No `...` or "or whatever" hand-waves outside intentional ellipsis in code listings that follow the spec verbatim.

**Type consistency:**
- Emitter signature `emit(src_path: Path, spec_version: str) -> str` consistent across `sv_params`, `cpp_params`, existing `sv_signals` (matches `DOMAIN_TO_EMITTER` contract at `codegen.py:38`).
- `load_constants(path: Path) -> dict`, `load_interfaces(path: Path, constants: dict) -> dict` consistent across Tasks 2, 3, 4, 5, 7.
- Parameter names (`ID_WIDTH`, `ADDR_WIDTH`, `DATA_WIDTH`, `NUM_VC`, `FLIT_WIDTH`, `SLAVE_VC_BUFFER_DEPTH`) uniform across Tasks 1, 6, 7, 10-14.

---

## Notes for Executor

- The spec §5.3 mentioned `slave_wrap.sv` and `noc_wrap.sv` informally; actual file names are `axi_slave_wrap.sv` and `loopback_noc_wrap.sv`. This plan uses the actual names throughout.
- `generator.py` is the markdown→JSON parser; SV emission lives in `specgen/tools/elaborate/sv_*.py` peer modules. New emitters in scope: `sv_params.py` + `cpp_params.py`. (`signal_interface_md.py` is **deferred** — not created by this plan.)
- `codegen.py` is the dispatcher. All new emitters MUST register in `DOMAIN_TO_EMITTER` and follow the `(src_path, spec_version) -> body_str` contract.
- W2 sub-commits 4-14 are not necessarily elaboration-clean mid-PR. Only Task 15 final gate runs the full ctest + drift gate.
- The plan does NOT call `python -m specgen` (no such entry point); all generator commands use `py -3 specgen/tools/codegen.py --target <X> --domain <Y>` per `codegen.py:4-13` docstring.
- **Release-level verification is intentionally NOT in this plan**; it is deferred to a follow-up spec. This plan's terminal state is a merged W2 PR with ctest 410/410 green + drift gate clean — sufficient for further development but NOT a release.
- After codegraph MCP is live (next session), executors can run `codegraph_explore` over specific symbol clusters (e.g., `Axi*Master scenario_parser load_doc DOMAIN_TO_EMITTER`) instead of grep+read sweeps for codebase orientation.

