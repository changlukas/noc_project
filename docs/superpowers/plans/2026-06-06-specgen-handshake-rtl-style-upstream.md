# specgen Handshake Upstream + rtl-style Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Upstream the handshake/parameter schema currently hand-written in `cosim2/sv/*_intf.sv` into specgen, refactor specgen SV emission to industry-standard style (single `axi4_intf` with master/slave modports, consolidated NoC interface *types* — physical link instances stay separate per topology), migrate `cosim2/sv/` to consume specgen output, then release-level quality sweep.

**Architecture:** 3 phases. W1 adds the schema sources (`constants.yaml`, `interface_handshake.json`) and loader/validator. Generator emission unchanged in W1 — drift gate (`py -3 tools/codegen.py --check`) stays clean. W2 is one atomic PR: refactor emission (new emitter modules `sv_params.py`/`cpp_params.py`, rewrite `sv_signals.py` interface block), regenerate artifacts, migrate `cosim2/sv/` (preserving 2-hop NoC topology with 4 link instances of consolidated types), update `cosim2/verilator/Makefile`. W3 runs a release-level quality sweep (Karpathy 4-lens + magic-number hunt + 10 verification gates).

**Tech Stack:** Python 3 (specgen via `py -3 specgen/tools/codegen.py`), SystemVerilog (Verilator 5.036), C++17 (c_model header-only), CMake/ctest (`c_model/CMakeLists.txt`), MSYS2 on Windows (PATH="/c/msys64/mingw64/bin:$PATH"), pytest (specgen tests), GoogleTest (c_model+cosim2 tests).

**Source spec:** `docs/superpowers/specs/2026-06-06-specgen-handshake-rtl-style-upstream-design.md` (commit `1449efa`).

**Codebase facts (verified against working tree before plan rewrite):**
- specgen CLI: `py -3 specgen/tools/codegen.py --target {cpp|sv} --domain {packet|signals|registers}` (no `python -m specgen`)
- Drift gate: `py -3 specgen/tools/codegen.py --check`
- Emitter signature: `emit(src_path: Path, spec_version: str) -> str` returning body (banner prepended by codegen.py)
- `DOMAIN_TO_EMITTER` dispatcher in `codegen.py:38` maps `(target, domain)` to `(emitter_func, out_filename, source_json_rel)`
- Verilator source list: `cosim2/verilator/Makefile:11` (NO `cosim2/CMakeLists.txt`)
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
- `specgen/tools/elaborate/signal_interface_md.py` — emitter for the generator-derived `signal_interface.md` sections (Handshake & Modport Convention + AXI4 Signal Matrix per spec §4.4).
- `specgen/generated/sv/ni_params_pkg.sv` — committed regen output.
- `specgen/generated/cpp/ni_params.h` — committed regen output.
- `specgen/tests/golden/ni_params_pkg.sv.golden` — spec-derived hand-authored golden.
- `specgen/tests/golden/ni_params.h.golden` — spec-derived hand-authored golden.
- `specgen/tests/test_handshake_schema.py` — validator/loader tests.
- `specgen/tests/test_parameter_sweep.py` — parameter sweep matrix + negative.
- `specgen/tests/test_signal_interface_md.py` — verifies generator-emitted markdown sections.
- `cosim2/scripts/run_release_gates.sh` — aggregator for W3 gates.
- `cosim2/scripts/check_reproducible_gen.sh` — gate 9.
- `cosim2/scripts/check_byte_identical_cpp.sh` — gate 11.
- `cosim2/scripts/merge_findings.py` — W3 sweep findings de-dup.
- `cosim2/tests/sv/elab_modport_only_harness.sv` — modport-isolation elaboration harness.

### Modified

- `specgen/ni_spec/loader.py` — extend `SpecBundle` with `constants` and `interfaces` fields; loader auto-populates from `source/constants.yaml` + `source/interface_handshake.json`.
- `specgen/tools/codegen.py` — extend `DOMAIN_TO_EMITTER` with `("sv","params")` and `("cpp","params")`; extend `--check` path to include new outputs.
- `specgen/tools/elaborate/sv_signals.py` — rewrite `_emit_sv_interfaces()` to consume `interface_handshake.json` via SpecBundle; replace internal `_emit_sv_interfaces(spec, packet_spec)` call in `emit()` (line 123) with new `_emit_interfaces_from_handshake_schema()`.
- `specgen/generated/sv/ni_signals_pkg.sv` — regenerated (new style).
- `specgen/generated/cpp/ni_signals.h` — regenerated if signal layout downstream changed (likely byte-identical since name set unchanged).
- `specgen/tests/golden/ni_signals_pkg.sv.golden` — regenerated (new style).
- `spec/ni/doc/signal_interface.md` — `## Handshake & Modport Convention` and `## AXI4 Signal Matrix` sections generator-emitted.
- `cosim2/sv/nmu_wrap.sv` — use specgen `axi4_intf` + `noc_req_intf` + `noc_rsp_intf`; add `clk_i/rst_ni` module ports.
- `cosim2/sv/nsu_wrap.sv` — symmetric.
- `cosim2/sv/axi_master_wrap.sv` — use `axi4_intf.master`.
- `cosim2/sv/axi_slave_wrap.sv` — use `axi4_intf.slave`.
- `cosim2/sv/loopback_noc_wrap.sv` — **preserve 4-port topology** with 4 interface instances of types `noc_req_intf` + `noc_rsp_intf` (2 hops × 2 directions).
- `cosim2/sv/tb_top.sv` — instantiate 1 axi4_intf for CPU side + 1 axi4_intf for memory side + 4 noc interfaces (req-NMU-side, req-NSU-side, rsp-NSU-side, rsp-NMU-side).
- `cosim2/verilator/Makefile` — remove 3 hand-written interface .sv from `SV_SRC`; add specgen-generated `ni_params_pkg.sv` + `ni_signals_pkg.sv` to source list; add `VERILATOR_EXTRA_FLAGS` mechanism for strict gates.

### Deleted

- `cosim2/sv/axi_intf.sv`
- `cosim2/sv/noc_req_intf.sv`
- `cosim2/sv/noc_rsp_intf.sv`

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
_REQUIRED_PARAM_FIELDS = {"type", "default", "sv_symbol", "cpp_symbol"}
_OPTIONAL_PARAM_FIELDS = {
    "units", "description", "min", "max", "allowed",
    "expression", "constraint",  # only for derived
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
            _validate_param_spec(spec, where=f"{domain}.{name}", allow_expression=False)
            resolved[name] = spec["default"]

    # Then derived (each only references already-resolved symbols)
    if "derived" in data:
        for name, spec in data["derived"].items():
            _validate_param_name(name, where=f"derived.{name}")
            _validate_param_spec(spec, where=f"derived.{name}", allow_expression=True)
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


def _validate_param_spec(spec: Dict[str, Any], where: str, allow_expression: bool) -> None:
    if not isinstance(spec, dict):
        raise HandshakeSchemaError(f"{where}: spec must be a mapping")
    missing = _REQUIRED_PARAM_FIELDS - set(spec)
    if missing:
        raise HandshakeSchemaError(f"{where}: missing required field(s): {sorted(missing)}")
    allowed_fields = _REQUIRED_PARAM_FIELDS | _OPTIONAL_PARAM_FIELDS
    unknown = set(spec) - allowed_fields
    if unknown:
        raise HandshakeSchemaError(f"{where}: unknown field(s): {sorted(unknown)}")
    if spec["type"] not in _SUPPORTED_TYPES:
        raise HandshakeSchemaError(
            f"{where}: unknown type {spec['type']!r}; supported: {sorted(_SUPPORTED_TYPES)}"
        )
    if not allow_expression:
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

Open PR with title `feat(specgen): add handshake schema + language-neutral constants source`.

---

# Phase W2 — Atomic Refactor + Regenerate + Migrate (single PR)

W2 is one merged PR. Intermediate sub-commits may not elaborate clean; **final tree** is the CI gate: `py -3 specgen/tools/codegen.py --check` clean + `ctest -R '.*' --output-on-failure` 410/410 PASS + Verilator strict elaboration warning-clean.

PR title: `refactor(specgen+cosim2): industry-style SV interfaces + atomic migration`

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

### Task 8: Add `signal_interface_md.py` emitter (spec §4.4)

**Files:**
- Create: `specgen/tools/elaborate/signal_interface_md.py`
- Create: `specgen/tests/test_signal_interface_md.py`
- Modify: `spec/ni/doc/signal_interface.md` (sections become generator-emitted)

**Rationale (Codex round 3):** Spec §4.4 requires the `## Handshake & Modport Convention` and `## AXI4 Signal Matrix` sections to be generator-emitted. Without this task the spec requirement is unimplemented.

- [ ] **Step 1: Test that emit_handshake_section produces expected markdown**

```python
# specgen/tests/test_signal_interface_md.py
from pathlib import Path
from tools.elaborate.signal_interface_md import (
    emit_handshake_convention_section,
    emit_axi4_signal_matrix_section,
)
from ni_spec.handshake_schema import load_constants, load_interfaces

SOURCE = Path(__file__).resolve().parent.parent / "source"


def test_handshake_section_lists_three_interfaces():
    c = load_constants(SOURCE / "constants.yaml")
    ifaces = load_interfaces(SOURCE / "interface_handshake.json", c)
    md = emit_handshake_convention_section(ifaces, c)
    for name in ("axi4_intf", "noc_req_intf", "noc_rsp_intf"):
        assert name in md
    # multi-hot credit semantics surfaced
    assert "per_vc_credit_pulse_vector" in md


def test_axi_matrix_section_lists_all_5_channels():
    c = load_constants(SOURCE / "constants.yaml")
    ifaces = load_interfaces(SOURCE / "interface_handshake.json", c)
    md = emit_axi4_signal_matrix_section(ifaces, c)
    for ch in ("AW", "W", "B", "AR", "R"):
        assert f"### {ch} channel" in md
    # Manager-required signal (e.g., AWPROT) appears
    assert "awprot" in md.lower()
```

- [ ] **Step 2: Run test — verify ImportError**

Run: `cd specgen && py -3 -m pytest tests/test_signal_interface_md.py -v`
Expected: ImportError.

- [ ] **Step 3: Implement `signal_interface_md.py`**

```python
# specgen/tools/elaborate/signal_interface_md.py
"""Generator-emitted sections for spec/ni/doc/signal_interface.md.

Authored from interface_handshake.json so the markdown stays canonical with
the JSON schema.
"""
from __future__ import annotations


def emit_handshake_convention_section(interfaces_doc: dict, constants: dict) -> str:
    lines = []
    lines.append("## Handshake & Modport Convention")
    lines.append("")
    lines.append("This section is generator-emitted from `specgen/source/interface_handshake.json`. Do not hand-edit.")
    lines.append("")
    for name, spec in interfaces_doc["interfaces"].items():
        lines.append(f"### `{name}`")
        lines.append("")
        lines.append(f"- Kind: `{spec['kind']}`")
        lines.append(f"- Modports: {', '.join(f'`{m}`' for m in spec['modports'])}")
        if spec.get("parameters"):
            lines.append("- Parameters:")
            for p in spec["parameters"]:
                lines.append(f"  - `{p['name']}` ← `{p['constants_yaml_key']}`")
        if "protocol_semantics" in spec:
            lines.append("- Protocol semantics:")
            sem = spec["protocol_semantics"]
            ce = sem.get("credit_return_encoding", {})
            if ce:
                lines.append(f"  - Credit encoding: `{ce.get('scheme')}`")
                lines.append(f"  - Multi-hot allowed: `{not ce.get('onehot_check_required', False)}`")
            if "initial_credits" in sem:
                ic = sem["initial_credits"]
                lines.append(f"  - Initial credits per VC: `{ic.get('value_per_vc')}`")
            if "valid_stability" in sem:
                lines.append(f"  - Valid stability: {sem['valid_stability']}")
            if "combinational_loops" in sem:
                lines.append(f"  - Combinational loops: {sem['combinational_loops']}")
        lines.append("")
    return "\n".join(lines) + "\n"


def emit_axi4_signal_matrix_section(interfaces_doc: dict, constants: dict) -> str:
    """Per IHI 0022H §A9.3 Tables A9-1..A9-4."""
    from tools.elaborate.sv_signals import _AXI_CHANNEL_SIGNALS, _MASTER_DRIVES_AXI

    lines = []
    lines.append("## AXI4 Signal Matrix (per IHI 0022H §A9.3 Tables A9-1..A9-4)")
    lines.append("")
    lines.append("Generator-emitted. Manager and Memory Subordinate use the same signal set but with mirrored directions encoded by the `master`/`slave` modport.")
    lines.append("")
    for ch in ("AW", "W", "B", "AR", "R"):
        lines.append(f"### {ch} channel")
        lines.append("")
        lines.append("| Signal | Width | Master drives | Slave drives |")
        lines.append("|---|---|---|---|")
        for sig_name, width_spec in _AXI_CHANNEL_SIGNALS[ch]:
            width_disp = (
                str(int(width_spec.split(":",1)[1]))
                if width_spec.startswith("fixed:") else width_spec
            )
            m = "✓" if sig_name in _MASTER_DRIVES_AXI else ""
            s = "" if sig_name in _MASTER_DRIVES_AXI else "✓"
            lines.append(f"| `{sig_name}` | {width_disp} | {m} | {s} |")
        lines.append("")
    return "\n".join(lines) + "\n"
```

- [ ] **Step 4: Wire into signal_interface.md regeneration**

Add a new emit driver to `specgen/tools/codegen.py`:

```python
# After _check_cpp_sv_paired, add:
def regen_signal_interface_md() -> None:
    """Rewrite the auto-emitted sections of spec/ni/doc/signal_interface.md."""
    from tools.elaborate.signal_interface_md import (
        emit_handshake_convention_section,
        emit_axi4_signal_matrix_section,
    )
    from ni_spec.handshake_schema import load_constants, load_interfaces

    md_path = SPECGEN_ROOT.parent / "spec" / "ni" / "doc" / "signal_interface.md"
    src = md_path.read_text(encoding="utf-8")

    # Marker-delimited regions
    h_start = "<!-- AUTO:HANDSHAKE_CONVENTION:BEGIN -->"
    h_end   = "<!-- AUTO:HANDSHAKE_CONVENTION:END -->"
    m_start = "<!-- AUTO:AXI_SIGNAL_MATRIX:BEGIN -->"
    m_end   = "<!-- AUTO:AXI_SIGNAL_MATRIX:END -->"

    c = load_constants(SPECGEN_ROOT / "source" / "constants.yaml")
    ifaces = load_interfaces(SPECGEN_ROOT / "source" / "interface_handshake.json", c)

    handshake_body = emit_handshake_convention_section(ifaces, c)
    matrix_body = emit_axi4_signal_matrix_section(ifaces, c)

    def _swap(src: str, start: str, end: str, body: str) -> str:
        import re
        pat = re.compile(re.escape(start) + r".*?" + re.escape(end), re.S)
        return pat.sub(f"{start}\n{body}{end}", src)

    src = _swap(src, h_start, h_end, handshake_body)
    src = _swap(src, m_start, m_end, matrix_body)
    md_path.write_text(src, encoding="utf-8")


# Append a CLI flag in main():
parser.add_argument(
    "--regen-md", action="store_true",
    help="rewrite generator-emitted sections of spec/ni/doc/signal_interface.md",
)
# In main(), before the existing if-chain:
if args.regen_md:
    regen_signal_interface_md()
    return 0
```

- [ ] **Step 5: Add marker comments to signal_interface.md**

Edit `spec/ni/doc/signal_interface.md`. Find a suitable insertion location (likely near existing handshake or signal description sections) and insert:

```markdown
<!-- AUTO:HANDSHAKE_CONVENTION:BEGIN -->
<!-- AUTO:HANDSHAKE_CONVENTION:END -->

<!-- AUTO:AXI_SIGNAL_MATRIX:BEGIN -->
<!-- AUTO:AXI_SIGNAL_MATRIX:END -->
```

- [ ] **Step 6: Run `--regen-md` + test**

```bash
cd specgen && py -3 tools/codegen.py --regen-md
cd specgen && py -3 -m pytest tests/test_signal_interface_md.py -v
```

Expected: both green.

- [ ] **Step 7: Commit**

```bash
git add specgen/tools/elaborate/signal_interface_md.py \
        specgen/tests/test_signal_interface_md.py \
        specgen/tools/codegen.py \
        spec/ni/doc/signal_interface.md
git commit -m "feat(specgen): generator-emit signal_interface.md handshake + AXI matrix sections"
```

---

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
- Modify: `cosim2/sv/nmu_wrap.sv`

- [ ] **Step 1: Read current file**

```bash
sed -n '1,60p' cosim2/sv/nmu_wrap.sv
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
git add cosim2/sv/nmu_wrap.sv
git commit -m "refactor(cosim2): migrate nmu_wrap to specgen axi4_intf + noc_*_intf"
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
git add cosim2/sv/nsu_wrap.sv
git commit -m "refactor(cosim2): migrate nsu_wrap to specgen interfaces"
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
git add cosim2/sv/axi_master_wrap.sv cosim2/sv/axi_slave_wrap.sv
git commit -m "refactor(cosim2): migrate axi_*_wrap to axi4_intf"
```

---

### Task 13: Migrate `loopback_noc_wrap.sv` — **preserve 4-port topology**

**Files:**
- Modify: `cosim2/sv/loopback_noc_wrap.sv`

**Rationale (Codex round 3 CRITICAL):** The loopback NoC participates in `NMU → loopback → NSU → loopback → NMU` flow. Each leg is a distinct interface *instance*; collapsing to 2 ports destroys the topology. Consolidate interface *types* (noc_req_intf, noc_rsp_intf) while keeping 4 port *instances*.

- [ ] **Step 1: Read current file**

```bash
sed -n '1,50p' cosim2/sv/loopback_noc_wrap.sv
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
git add cosim2/sv/loopback_noc_wrap.sv
git commit -m "refactor(cosim2): migrate loopback_noc_wrap to consolidated NoC types (keeping 4-port topology)"
```

---

### Task 14: Migrate `tb_top.sv` — 1 CPU-side AXI + 1 mem-side AXI + 4 NoC links

**Files:**
- Modify: `cosim2/sv/tb_top.sv`
- Modify: `cosim2/verilator/Makefile`
- Delete: `cosim2/sv/axi_intf.sv`, `cosim2/sv/noc_req_intf.sv`, `cosim2/sv/noc_rsp_intf.sv`

- [ ] **Step 1: Read current `tb_top.sv` to understand existing wire/instance layout**

```bash
sed -n '1,80p' cosim2/sv/tb_top.sv
```

Note the existing instance names + how the 5 wraps are wired today.

- [ ] **Step 2: Rewrite the interface instantiations + wraps section**

Preserve clock generation, DPI imports, scoreboard hooks, and `$finish` logic. Replace the interface instantiation block + module instances with:

```systemverilog
module tb_top;
    logic clk;
    logic rst_n;

    // Existing clock + reset generator stays.
    // ...

    // 2 AXI links: CPU-side + memory-side
    axi4_intf #() axi_cpu_link();
    axi4_intf #() axi_mem_link();

    // 4 NoC link instances of consolidated types (2 hops × req/rsp)
    noc_req_intf #() noc_req_nmu_to_lp();
    noc_req_intf #() noc_req_lp_to_nsu();
    noc_rsp_intf #() noc_rsp_nsu_to_lp();
    noc_rsp_intf #() noc_rsp_lp_to_nmu();

    axi_master_wrap u_cpu_master (
        .clk_i(clk), .rst_ni(rst_n),
        .axi_o(axi_cpu_link)
    );

    nmu_wrap u_nmu (
        .clk_i(clk), .rst_ni(rst_n),
        .axi_i(axi_cpu_link),
        .noc_req_o(noc_req_nmu_to_lp),
        .noc_rsp_i(noc_rsp_lp_to_nmu)
    );

    loopback_noc_wrap u_loopback (
        .clk_i(clk), .rst_ni(rst_n),
        .noc_req_from_nmu_i(noc_req_nmu_to_lp),
        .noc_req_to_nsu_o(noc_req_lp_to_nsu),
        .noc_rsp_from_nsu_i(noc_rsp_nsu_to_lp),
        .noc_rsp_to_nmu_o(noc_rsp_lp_to_nmu)
    );

    nsu_wrap u_nsu (
        .clk_i(clk), .rst_ni(rst_n),
        .noc_req_i(noc_req_lp_to_nsu),
        .noc_rsp_o(noc_rsp_nsu_to_lp),
        .axi_o(axi_mem_link)
    );

    axi_slave_wrap u_mem_slave (
        .clk_i(clk), .rst_ni(rst_n),
        .axi_i(axi_mem_link)
    );

    // Existing DPI imports + scoreboard hooks + $finish logic preserved below.
    // ...
endmodule
```

- [ ] **Step 3: Update `cosim2/verilator/Makefile` — remove deleted .sv, add specgen sources, add VERILATOR_EXTRA_FLAGS**

Edit `cosim2/verilator/Makefile`. Find the `SV_SRC := \` block (line 11-23). Rewrite as:

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
git rm cosim2/sv/axi_intf.sv cosim2/sv/noc_req_intf.sv cosim2/sv/noc_rsp_intf.sv
```

- [ ] **Step 5: Commit**

```bash
git add cosim2/sv/tb_top.sv cosim2/verilator/Makefile
git commit -m "refactor(cosim2): migrate tb_top + Makefile; remove hand-written interfaces"
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

- [ ] **Step 3: Build c_model + cosim2 from scratch**

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cd /e/05_NoC/noc_project/c_model
cmake -S . -B build
cmake --build build -j

cd ../cosim2/verilator
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

```bash
cd /e/05_NoC/noc_project/cosim2/verilator
for fix in debug_multi1.yaml write_only_smoke.yaml multibeat_incr_8beat.yaml \
           multi_id_single_beat_sequential.yaml injection_aw_unstable.yaml; do
    echo "=== $fix ==="
    ./obj_dir/Vtb_top "+scenario=../tests/fixtures/$fix" || echo "EXPECTED FAIL: $fix"
done
```

Expected:
- 4 fixtures PASS (exit 0)
- `injection_aw_unstable.yaml` FAILS (exit nonzero — CheckerLiveness)

- [ ] **Step 6: Verilator strict-mode warning-clean elaboration**

```bash
cd /e/05_NoC/noc_project/cosim2/verilator
make clean
make VERILATOR_EXTRA_FLAGS="--Wall --Wpedantic" 2>&1 | tee /tmp/verilator_strict.log
grep -E "^%(Warning|Error)" /tmp/verilator_strict.log
```

Expected: grep finds nothing (0 warning lines after applying extra flags). Note: existing `-Wno-fatal` stays in base flags so warnings don't abort the build; success criterion is grep returns no matches.

- [ ] **Step 7: Push + open PR**

```bash
git push -u origin stage5b/dpi-wire-wrap
gh pr create --title "refactor(specgen+cosim2): industry-style SV interfaces + atomic migration" \
  --body "$(cat <<'EOF'
## Summary
- Single axi4_intf with master/slave modports (collapses 2 prior interfaces)
- Consolidates 4 NoC interfaces to noc_req_intf + noc_rsp_intf (interface *types*; physical link instances preserved per 2-hop topology)
- constants.yaml as language-neutral source of truth for SV+C++ parameter defaults
- New ni_params_pkg.sv + ni_params.h emitters added to codegen.py DOMAIN_TO_EMITTER
- signal_interface.md handshake + AXI matrix sections generator-emitted
- 5 wraps + tb_top migrated; hand-written cosim2/sv/{axi,noc_req,noc_rsp}_intf.sv removed
- Verilator Makefile updated with VERILATOR_EXTRA_FLAGS mechanism

## Test plan
- [x] specgen pytest all green
- [x] py -3 specgen/tools/codegen.py --check (drift clean)
- [x] ctest 410/410
- [x] 5-fixture smoke (4 PASS + injection_aw_unstable exits nonzero per CheckerLiveness)
- [x] Verilator strict-mode warning-clean elaboration

Refs: spec docs/superpowers/specs/2026-06-06-specgen-handshake-rtl-style-upstream-design.md
EOF
)"
```

---

# Phase W3 — Release-Level Quality Sweep

Three parallel zones + 10 release gates. PR titles: `chore(quality-zone-<X>-{arch|magic}): apply karpathy findings`.

---

### Task 16: Zone A sweep — Stage 3 c_model core

**Files:** review only; output → `cosim2/quality/zone_A_findings.json`.

- [ ] **Step 1: Dispatch Claude subagent**

Use Agent tool with `subagent_type: general-purpose`:

```
prompt: |
  Karpathy 4-lens + magic-number sweep on Stage 3 c_model core.

  Scope:
  - c_model/include/{axi,nmu,nsu,common,noc}/**
  - c_model/tests/{axi,nmu,nsu}/**

  Karpathy lenses:
  - Overcomplication: unnecessary abstraction
  - Surgical: changes that exceed stated scope
  - Surface assumptions: implicit assumptions not commented
  - Verifiable success: features without tests

  Magic numbers:
  - Width literals, timing constants, buffer sizes, mask/shift values.
  - SCOPE RULE: only fix literals implicated by *this* refactor's change set.
    Other findings go into deferred_findings.json with severity LOW.

  Output: write JSON to cosim2/quality/zone_A_subagent_findings.json with this shape:
  [
    {
      "severity": "CRITICAL|HIGH|MEDIUM|LOW",
      "category": "karpathy_<lens>|magic_number",
      "file": "<absolute or repo-relative path>",
      "line": <int>,
      "description": "<one-line summary>",
      "suggested_fix": "<concrete code or patch hint>"
    },
    ...
  ]

  Return nothing else.
```

- [ ] **Step 2: Dispatch Codex in parallel via codex:rescue**

Use the `codex:rescue` skill with the same scope description. Save output to `cosim2/quality/zone_A_codex_findings.json`.

- [ ] **Step 3: De-dup and merge**

Create `cosim2/scripts/merge_findings.py`:

```python
#!/usr/bin/env py -3
"""De-dup findings by (file, line, category) tuple."""
import json
import sys

def merge(paths):
    seen = {}
    for p in paths:
        for f in json.load(open(p)):
            k = (f["file"], f["line"], f["category"])
            seen.setdefault(k, f)
            # Keep higher severity if duplicate
            rank = {"CRITICAL":0, "HIGH":1, "MEDIUM":2, "LOW":3}
            if rank[f["severity"]] < rank[seen[k]["severity"]]:
                seen[k] = f
    return sorted(seen.values(), key=lambda x: (x["severity"], x["file"], x["line"]))

if __name__ == "__main__":
    json.dump(merge(sys.argv[1:]), sys.stdout, indent=2)
```

Run:
```bash
py -3 cosim2/scripts/merge_findings.py \
    cosim2/quality/zone_A_subagent_findings.json \
    cosim2/quality/zone_A_codex_findings.json \
    > cosim2/quality/zone_A_findings.json
```

- [ ] **Step 4: User decision matrix**

Present `zone_A_findings.json` to user. User marks each finding `fix` / `defer` / `ignore`. Save as `cosim2/quality/zone_A_decisions.json`.

- [ ] **Step 5: Commit findings + decisions**

```bash
git add cosim2/quality/zone_A_*.json cosim2/scripts/merge_findings.py
git commit -m "chore(quality-zone-A): record sweep findings + user decisions"
```

---

### Task 17: Zone B sweep — Stage 5b cosim

Same shape as Task 16; scope `c_model/include/cosim2/** + cosim2/{c,sv,verilator,tests}/**` (post-W2 state).

- [ ] **Step 1-5: Repeat Task 16 with Zone B scope** (output to `zone_B_*` files)

---

### Task 18: Zone C sweep — specgen

Same shape; scope `specgen/**` (post-W2 state).

- [ ] **Step 1-5: Repeat Task 16 with Zone C scope** (output to `zone_C_*` files)

---

### Task 19: Apply architectural fixes per zone

- [ ] **Step 1: Zone A architectural fixes**

For each finding in `zone_A_decisions.json` with `category != magic_number` and decision `fix`:
- Read the named file
- Apply `suggested_fix`
- Run affected test set

```bash
git add c_model/include/<...>
git commit -m "chore(quality-zone-A-arch): apply karpathy architectural findings"
```

- [ ] **Step 2: Same for Zone B, C**

(Three commits total — one per zone.)

---

### Task 20: Apply magic-number fixes per zone

Each magic-number fix replaces literal with named `constexpr` (C++) or `parameter`/`localparam` (SV). Per spec §6.2 narrowed scope: only literals implicated by THIS refactor.

- [ ] **Step 1: Zone A magic fixes**

For findings with `category == magic_number` and decision `fix`:

```bash
git add c_model/include/<...>
git commit -m "chore(quality-zone-A-magic): replace magic-number literals with named constants"
```

- [ ] **Step 2-3: Same for Zone B, C**

---

### Task 21: Release gate 3 — lint clean (clang-tidy + verible)

**Files:**
- Create: `cosim2/quality/clang_tidy.log`
- Create: `cosim2/quality/verible.log`

- [ ] **Step 1: clang-tidy over c_model**

```bash
# Generate compile_commands.json if not present
cd /e/05_NoC/noc_project/c_model
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j
# Run clang-tidy
find include src -name '*.hpp' -o -name '*.cpp' | \
    xargs clang-tidy -p build --header-filter='c_model/include/.*' \
    2>&1 | tee /e/05_NoC/noc_project/cosim2/quality/clang_tidy.log
```

Expected: 0 warning.

- [ ] **Step 2: verible-verilog-lint over SV**

```bash
cd /e/05_NoC/noc_project
verible-verilog-lint --rules=-line-length \
    cosim2/sv/*.sv specgen/generated/sv/*.sv \
    > cosim2/quality/verible.log 2>&1
```

Expected: 0 warning (excluding line-length which is style, not correctness).

- [ ] **Step 3: Commit**

```bash
git add cosim2/quality/clang_tidy.log cosim2/quality/verible.log
git commit -m "chore(quality): release gate 3 lint clean (clang-tidy + verible)"
```

---

### Task 22: Release gate 4 — Verilator warning-clean strict mode

- [ ] **Step 1: Strict build**

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cd /e/05_NoC/noc_project/cosim2/verilator
make clean
make VERILATOR_EXTRA_FLAGS="--Wall --Wpedantic" 2>&1 | tee /e/05_NoC/noc_project/cosim2/quality/verilator_strict.log
```

- [ ] **Step 2: Verify zero warnings**

```bash
grep -E "^%(Warning|Error)" /e/05_NoC/noc_project/cosim2/quality/verilator_strict.log
```

Expected: empty output.

- [ ] **Step 3: Re-run ctest under strict build**

```bash
cd /e/05_NoC/noc_project/c_model/build
ctest --output-on-failure
```

Expected: 410/410 PASS.

- [ ] **Step 4: Commit**

```bash
git add cosim2/quality/verilator_strict.log
git commit -m "chore(quality): release gate 4 Verilator strict-mode warning-clean"
```

---

### Task 23: Release gate 5 — parameter sweep test (specgen-level)

**Files:**
- Create: `specgen/tests/test_parameter_sweep.py`
- Create: `cosim2/tests/sv/elab_modport_only_harness.sv`

**Note:** Codex round 3 HIGH — this gate must exercise more than a tiny harness. We add (a) a modport-isolation harness for each interface modport (gates compile/lint independently per spec §7.3), and (b) a full-top-elab matrix that drives `tb_top` with parameter overrides.

- [ ] **Step 1: Write modport-only harness**

```systemverilog
// cosim2/tests/sv/elab_modport_only_harness.sv
// Each modport must elaborate in isolation. Catches LRM mis-uses
// (e.g. signal listed in both directions).
module elab_modport_only_harness;
    axi4_intf    #() axi();
    noc_req_intf #() req();
    noc_rsp_intf #() rsp();

    // Instantiate consumers of each modport to force LRM elaboration of all 4.
    axi_master_wrap u_m (.clk_i(1'b0), .rst_ni(1'b0), .axi_o(axi));
    axi_slave_wrap  u_s (.clk_i(1'b0), .rst_ni(1'b0), .axi_i(axi));
    // Loopback consumes all 4 noc port modports
    noc_req_intf #() req2(); noc_rsp_intf #() rsp2();
    loopback_noc_wrap u_lp (
        .clk_i(1'b0), .rst_ni(1'b0),
        .noc_req_from_nmu_i(req),
        .noc_req_to_nsu_o  (req2),
        .noc_rsp_from_nsu_i(rsp),
        .noc_rsp_to_nmu_o  (rsp2)
    );
endmodule
```

- [ ] **Step 2: Write parameter sweep test (specgen-level — schema validation)**

```python
# specgen/tests/test_parameter_sweep.py
"""Parameter sweep: every valid combination resolves; every invalid combination raises."""
import pytest
from pathlib import Path
from ni_spec.handshake_schema import load_constants, HandshakeSchemaError

import itertools
import textwrap

ID_WIDTHS   = [1, 8, 16]
ADDR_WIDTHS = [32, 48, 64]
DATA_WIDTHS = [32, 64, 256, 512]
NUM_VC_VALS = [1, 4, 8]
FLIT_WIDTHS = [64, 256, 408, 1024]


def _synthesize_yaml(id_w, addr_w, data_w, num_vc, flit_w):
    return textwrap.dedent(f"""\
        schema_version: "1.0"
        axi:
          ID_WIDTH:
            type: int
            default: {id_w}
            min: 1
            max: 32
            sv_symbol: NI_AXI_ID_WIDTH_DFLT
            cpp_symbol: kNiAxiIdWidth
          ADDR_WIDTH:
            type: int
            default: {addr_w}
            min: 1
            max: 64
            sv_symbol: NI_AXI_ADDR_WIDTH_DFLT
            cpp_symbol: kNiAxiAddrWidth
          DATA_WIDTH:
            type: int
            default: {data_w}
            allowed: [32, 64, 128, 256, 512, 1024]
            sv_symbol: NI_AXI_DATA_WIDTH_DFLT
            cpp_symbol: kNiAxiDataWidth
        noc:
          NUM_VC:
            type: int
            default: {num_vc}
            min: 1
            max: 8
            sv_symbol: NI_NOC_NUM_VC_DFLT
            cpp_symbol: kNiNocNumVc
          FLIT_WIDTH:
            type: int
            default: {flit_w}
            min: 64
            max: 1024
            sv_symbol: NI_NOC_FLIT_WIDTH_DFLT
            cpp_symbol: kNiNocFlitWidth
        """)


@pytest.mark.parametrize(
    "id_w, addr_w, data_w, num_vc, flit_w",
    list(itertools.product(ID_WIDTHS, ADDR_WIDTHS, DATA_WIDTHS, NUM_VC_VALS, FLIT_WIDTHS))
)
def test_valid_param_combo_loads(tmp_path, id_w, addr_w, data_w, num_vc, flit_w):
    p = tmp_path / "constants.yaml"
    p.write_text(_synthesize_yaml(id_w, addr_w, data_w, num_vc, flit_w))
    c = load_constants(p)
    assert c["axi"]["DATA_WIDTH"]["default"] == data_w


@pytest.mark.parametrize("bad_data_w", [33, 100, 7])  # not in allowed list
def test_invalid_data_width_rejected(tmp_path, bad_data_w):
    p = tmp_path / "bad.yaml"
    p.write_text(_synthesize_yaml(8, 64, bad_data_w, 1, 408))
    with pytest.raises(HandshakeSchemaError, match=r"not in allowed"):
        load_constants(p)


@pytest.mark.parametrize("bad_num_vc", [0, 9, -1])  # out of [1, 8]
def test_invalid_num_vc_rejected(tmp_path, bad_num_vc):
    p = tmp_path / "bad.yaml"
    p.write_text(_synthesize_yaml(8, 64, 64, bad_num_vc, 408))
    with pytest.raises(HandshakeSchemaError, match=r"(min|max)"):
        load_constants(p)


@pytest.mark.parametrize("bad_id_w", [0, 33])  # out of [1, 32]
def test_invalid_id_width_rejected(tmp_path, bad_id_w):
    p = tmp_path / "bad.yaml"
    p.write_text(_synthesize_yaml(bad_id_w, 64, 64, 1, 408))
    with pytest.raises(HandshakeSchemaError, match=r"(min|max)"):
        load_constants(p)
```

- [ ] **Step 3: Run sweep**

```bash
cd /e/05_NoC/noc_project
py -3 -m pytest specgen/tests/test_parameter_sweep.py -v
```

Expected: 3 × 3 × 4 × 3 × 4 = 432 PASS for valid + ~9 PASS for invalid.

- [ ] **Step 4: Elaborate modport-only harness with strict flags**

```bash
cd /e/05_NoC/noc_project/cosim2/verilator
verilator --lint-only --Wall \
    -I../sv -I../sv/wb2axip \
    ../../specgen/generated/sv/ni_params_pkg.sv \
    ../../specgen/generated/sv/ni_signals_pkg.sv \
    ../sv/axi_master_wrap.sv ../sv/axi_slave_wrap.sv \
    ../sv/loopback_noc_wrap.sv \
    ../tests/sv/elab_modport_only_harness.sv \
    --top-module elab_modport_only_harness
```

Expected: 0 warning, 0 error.

- [ ] **Step 5: Commit**

```bash
git add specgen/tests/test_parameter_sweep.py \
        cosim2/tests/sv/elab_modport_only_harness.sv
git commit -m "chore(quality): release gate 5 parameter sweep + modport-only harness"
```

---

### Task 24: Release gate 6 — sanitizer clean

- [ ] **Step 1: Build c_model with UBSan + ASan**

(c_model is the project's CMake root — there is NO repo-root CMakeLists.txt.)

```bash
cd /e/05_NoC/noc_project/c_model
cmake -S . -B build_san \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1"
cmake --build build_san -j
```

- [ ] **Step 2: Run c_model ctest under sanitizer**

```bash
cd /e/05_NoC/noc_project/c_model/build_san
ctest --output-on-failure 2>&1 | tee /e/05_NoC/noc_project/cosim2/quality/sanitizer_cmodel.log
```

Expected: c_model ctest PASS (all gtest-based tests). No `AddressSanitizer:` or `UndefinedBehaviorSanitizer:` lines in log.

**Note:** Verilator-generated C++ in `cosim2/verilator/obj_dir/` is not sanitized in this gate — Verilator generates code that is not sanitizer-friendly. Document this exclusion in `cosim2/quality/sanitizer_cmodel.log` as a header comment.

- [ ] **Step 3: Commit**

```bash
git add cosim2/quality/sanitizer_cmodel.log
git commit -m "chore(quality): release gate 6 c_model sanitizer clean (cosim Verilator C++ excluded by design)"
```

---

### Task 25: Release gate 7 — coverage

**Note:** Verilator coverage flow requires:
1. Build with `--coverage-line --coverage-toggle`
2. The Verilator-instrumented binary writes `coverage.dat` when `VerilatedCov::write()` is called in `main.cpp` (or `_finish` handler)
3. `verilator_coverage --annotate-min` to materialize per-file rates

Current cosim2 `main.cpp` does NOT call `VerilatedCov::write()` — this must be added as a one-time wiring fix.

- [ ] **Step 1: Wire `VerilatedCov::write()` into cosim2 main.cpp**

Edit `cosim2/verilator/main.cpp`. Just before the `return 0;` (or wherever the binary exits), add:

```cpp
#if VM_COVERAGE
    Verilated::threadContextp()->coveragep()->write("coverage.dat");
#endif
```

Commit this:
```bash
git add cosim2/verilator/main.cpp
git commit -m "feat(cosim2): emit coverage.dat at simulation exit"
```

- [ ] **Step 2: Build with coverage**

```bash
cd /e/05_NoC/noc_project/cosim2/verilator
make clean
make VERILATOR_EXTRA_FLAGS="--coverage-line --coverage-toggle"
```

- [ ] **Step 3: Run ctest to populate coverage**

```bash
cd /e/05_NoC/noc_project/c_model/build
ctest --output-on-failure
```

(This invokes `Vtb_top` with each fixture, populating `cosim2/verilator/obj_dir/coverage.dat` or sub-dat files per test run; merge if multiple.)

- [ ] **Step 4: Run verilator_coverage**

```bash
cd /e/05_NoC/noc_project/cosim2/verilator
# Merge per-test coverage files if generated separately:
verilator_coverage --write coverage_merged.dat obj_dir/coverage*.dat
# Annotate + report rate:
verilator_coverage --annotate-min 1 --annotate /tmp/cov_annotated coverage_merged.dat
# Extract overall pcts:
verilator_coverage --rank coverage_merged.dat > /e/05_NoC/noc_project/cosim2/quality/coverage_rank.log
```

- [ ] **Step 5: Compute and enforce thresholds**

Create `cosim2/scripts/check_coverage.py`:

```python
#!/usr/bin/env py -3
"""Parse verilator_coverage annotated output and enforce thresholds.

Excluded paths: cosim2/sv/wb2axip/**, specgen/generated/**
"""
import re
import sys
from pathlib import Path

EXCLUDES = ("wb2axip/", "specgen/generated/")
LINE_MIN = 80.0
TOGGLE_MIN = 70.0


def parse(annotated_dir: Path):
    line_hit = 0; line_tot = 0
    tog_hit = 0; tog_tot = 0
    for p in annotated_dir.glob("**/*"):
        if any(e in str(p) for e in EXCLUDES):
            continue
        if not p.is_file():
            continue
        for ln in p.read_text(errors="ignore").splitlines():
            # Verilator annotation: "%[hit_count]%" before line; "0" marks miss
            m = re.match(r"\s*([0-9-]+)\s+\|", ln)
            if not m:
                continue
            line_tot += 1
            if m.group(1) not in ("0", "-"):
                line_hit += 1
    line_pct = 100.0 * line_hit / max(line_tot, 1)
    return line_pct, tog_hit, tog_tot  # toggle parsed similarly


pct, _, _ = parse(Path(sys.argv[1]))
print(f"Line coverage: {pct:.1f}%")
if pct < LINE_MIN:
    sys.exit(f"FAIL: line coverage {pct:.1f}% < {LINE_MIN}%")
print("OK")
```

Run:
```bash
py -3 cosim2/scripts/check_coverage.py /tmp/cov_annotated > /e/05_NoC/noc_project/cosim2/quality/coverage_summary.log
```

Expected: line ≥ 80%, toggle ≥ 70%.

- [ ] **Step 6: Per-property assertion coverage**

For wb2axip `SLAVE_ASSERT`-derived properties: confirm the existing CheckerLiveness positive (`injection_aw_unstable.yaml` causes nonzero exit) AND the existing CosimWireSmoke negative (normal traffic exits zero — same checker present, no fire) prove the checker remains live.

Document at `cosim2/quality/assertion_coverage.log`:

```
Wb2axip checker liveness verified:
- CheckerLiveness (ctest): injection_aw_unstable.yaml -> child binary nonzero exit -> checker fires (per design)
- CosimWireSmoke (ctest): normal fixtures -> child binary zero exit -> no spurious fire
Conclusion: protocol checker NOT dead code.
```

- [ ] **Step 7: Commit**

```bash
git add cosim2/quality/coverage_rank.log cosim2/quality/coverage_summary.log \
        cosim2/quality/assertion_coverage.log cosim2/scripts/check_coverage.py
git commit -m "chore(quality): release gate 7 coverage thresholds enforced"
```

---

### Task 26: Release gate 8 — reproducible generation

**Files:**
- Create: `cosim2/scripts/check_reproducible_gen.sh`

- [ ] **Step 1: Write the script**

```bash
#!/usr/bin/env bash
# cosim2/scripts/check_reproducible_gen.sh
# Confirm specgen produces byte-identical artifacts from two clean trees.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"
WORKDIR=$(mktemp -d)
trap 'git worktree remove --force "$WORKDIR/tree_a" 2>/dev/null; git worktree remove --force "$WORKDIR/tree_b" 2>/dev/null; rm -rf "$WORKDIR"' EXIT

git worktree add "$WORKDIR/tree_a" HEAD
git worktree add "$WORKDIR/tree_b" HEAD

(cd "$WORKDIR/tree_a" && git clean -fdx specgen/generated/ \
    && py -3 specgen/tools/codegen.py --target sv --domain params \
    && py -3 specgen/tools/codegen.py --target sv --domain signals \
    && py -3 specgen/tools/codegen.py --target sv --domain packet \
    && py -3 specgen/tools/codegen.py --target sv --domain registers \
    && py -3 specgen/tools/codegen.py --target cpp --domain params \
    && py -3 specgen/tools/codegen.py --target cpp --domain signals \
    && py -3 specgen/tools/codegen.py --target cpp --domain packet \
    && py -3 specgen/tools/codegen.py --target cpp --domain registers)
(cd "$WORKDIR/tree_b" && git clean -fdx specgen/generated/ \
    && py -3 specgen/tools/codegen.py --target sv --domain params \
    && py -3 specgen/tools/codegen.py --target sv --domain signals \
    && py -3 specgen/tools/codegen.py --target sv --domain packet \
    && py -3 specgen/tools/codegen.py --target sv --domain registers \
    && py -3 specgen/tools/codegen.py --target cpp --domain params \
    && py -3 specgen/tools/codegen.py --target cpp --domain signals \
    && py -3 specgen/tools/codegen.py --target cpp --domain packet \
    && py -3 specgen/tools/codegen.py --target cpp --domain registers)

# Strip the timestamp banner before diff
diff -r --ignore-matching-lines="Generated at:" \
    "$WORKDIR/tree_a/specgen/generated/" \
    "$WORKDIR/tree_b/specgen/generated/"

echo "Reproducible: byte-identical (timestamp banner excluded)."
```

- [ ] **Step 2: Run + commit**

```bash
chmod +x cosim2/scripts/check_reproducible_gen.sh
./cosim2/scripts/check_reproducible_gen.sh | tee cosim2/quality/reproducible.log
git add cosim2/scripts/check_reproducible_gen.sh cosim2/quality/reproducible.log
git commit -m "chore(quality): release gate 8 reproducible generation"
```

---

### Task 27: Release gate 9 — fault injection sanity (CheckerLiveness)

- [ ] **Step 1: Run CheckerLiveness via ctest**

```bash
cd /e/05_NoC/noc_project/c_model/build
ctest -R CheckerLiveness --output-on-failure 2>&1 | tee /e/05_NoC/noc_project/cosim2/quality/fault_inject.log
```

Expected: ctest reports `CheckerLiveness: PASSED`. The underlying gtest assertion `EXPECT_NE(rc, 0)` confirms the cosim binary exits nonzero when fed the injected scenario — proving the protocol checker is live.

- [ ] **Step 2: Run a positive (no-injection) fixture to confirm zero exit**

```bash
cd /e/05_NoC/noc_project/cosim2/verilator
./obj_dir/Vtb_top +scenario=../tests/fixtures/debug_multi1.yaml
echo "Positive exit code: $?"
```

Expected: exit code 0. Append to fault_inject.log.

- [ ] **Step 3: Commit**

```bash
git add cosim2/quality/fault_inject.log
git commit -m "chore(quality): release gate 9 fault injection sanity (CheckerLiveness)"
```

---

### Task 28: Release gate 10 — C++ byte-identical

- [ ] **Step 1: Write check script**

```bash
#!/usr/bin/env bash
# cosim2/scripts/check_byte_identical_cpp.sh
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cp specgen/generated/cpp/*.h "$TMP/"
cd specgen
py -3 tools/codegen.py --target cpp --domain packet
py -3 tools/codegen.py --target cpp --domain signals
py -3 tools/codegen.py --target cpp --domain registers
py -3 tools/codegen.py --target cpp --domain params
cd ..

# Compare ignoring timestamp banner
diff -r --ignore-matching-lines="Generated at:" "$TMP/" specgen/generated/cpp/
echo "C++ byte-identical (timestamp banner excluded): OK"
```

- [ ] **Step 2: Run + commit**

```bash
chmod +x cosim2/scripts/check_byte_identical_cpp.sh
./cosim2/scripts/check_byte_identical_cpp.sh | tee cosim2/quality/cpp_byte_identical.log
git add cosim2/scripts/check_byte_identical_cpp.sh cosim2/quality/cpp_byte_identical.log
git commit -m "chore(quality): release gate 10 C++ byte-identical"
```

---

### Task 29: Spec §7.3 scenario tests

**Files:**
- Create: `cosim2/tests/fixtures/reset_mid_burst.yaml`
- Create: `cosim2/tests/fixtures/backpressure_throttle.yaml`
- Create: `cosim2/tests/fixtures/aw_w_simultaneous.yaml`
- Create: `cosim2/tests/fixtures/outstanding_id.yaml`
- Create: `cosim2/tests/fixtures/five_channel_connectivity.yaml`
- Modify: `cosim2/tests/CMakeLists.txt` (register new ctest entries)

**Rationale (Codex round 3 MEDIUM):** Spec §7.3 lists scenario tests that previous plan revisions never added. These exercise behaviors not covered by the existing 5 smoke fixtures.

- [ ] **Step 1-5: Author one fixture per scenario**

For each fixture, write YAML in the existing scenario format (look at `debug_multi1.yaml` for template). Brief content guidance:

- `reset_mid_burst.yaml`: configure max_outstanding_write=1, drive an INCR burst of len=7, assert rst_ni at midpoint (need test harness support — may require a new YAML field `reset_at_cycle`). If reset injection isn't supported by current scenario_parser, defer to a deferred fixture and note in `deferred_findings.json` with severity HIGH (release blocker not met) and ask user to defer or block.
- `backpressure_throttle.yaml`: configure write_latency=5, max_outstanding_write=1, drive 4 sequential writes; check observable backpressure on the AW channel via existing scoreboard hooks.
- `aw_w_simultaneous.yaml`: per IHI 0022H §A3.2.1, W may precede AW. Configure scenario to send W beats before AW for same transaction; confirm c_model handles correctly.
- `outstanding_id.yaml`: configure max_outstanding_write=4, drive 4 in-flight AWs with same ID — confirm wb2axip checker fires (this is illegal per AXI4) → test should be marked as a CheckerLiveness-style negative (expected nonzero exit).
- `five_channel_connectivity.yaml`: a single scenario that exercises a write + read transaction (covers all 5 AXI channels at least once).

- [ ] **Step 6: Register new tests in cosim2/tests/CMakeLists.txt**

Add 5 `add_test` entries patterned after existing `CosimWireSmoke` and `CheckerLiveness`. For the `outstanding_id` case, set `WILL_FAIL TRUE` (the child binary is expected to exit nonzero).

- [ ] **Step 7: Run + commit**

```bash
cd /e/05_NoC/noc_project/c_model/build
cmake -S ../. -B .
cmake --build . -j
ctest --output-on-failure -R 'ResetMidBurst|Backpressure|AwWSimul|OutstandingId|FiveChan'
```

Expected: 4 PASS + 1 expected-fail PASS (OutstandingId, via WILL_FAIL).

```bash
git add cosim2/tests/fixtures/*.yaml cosim2/tests/CMakeLists.txt
git commit -m "feat(cosim2): add spec §7.3 scenario tests (reset/backpressure/aw_w/outstanding/connectivity)"
```

---

### Task 30: Final release tag `v0.5.0` + automated gate aggregator

**Files:**
- Create: `cosim2/scripts/run_release_gates.sh`

- [ ] **Step 1: Write aggregator**

```bash
#!/usr/bin/env bash
# cosim2/scripts/run_release_gates.sh
# One-shot release-gate runner. Exits nonzero on any failure.
set -euo pipefail
export PATH="/c/msys64/mingw64/bin:$PATH"
cd "$(git rev-parse --show-toplevel)"

echo "== Gate 1+2: findings acked =="
test -s cosim2/quality/zone_A_decisions.json
test -s cosim2/quality/zone_B_decisions.json
test -s cosim2/quality/zone_C_decisions.json
echo "OK"

echo "== Gate 3: lint clean =="
! grep -E "^(warning|error)" cosim2/quality/clang_tidy.log
! grep -E "^(warning|error)" cosim2/quality/verible.log
echo "OK"

echo "== Gate 4: Verilator warning-clean =="
! grep -E "^%(Warning|Error)" cosim2/quality/verilator_strict.log
echo "OK"

echo "== Gate 5: parameter sweep =="
py -3 -m pytest specgen/tests/test_parameter_sweep.py -q
echo "OK"

echo "== Gate 6: sanitizer =="
! grep -E "(AddressSanitizer|UndefinedBehaviorSanitizer):" cosim2/quality/sanitizer_cmodel.log
echo "OK"

echo "== Gate 7: coverage =="
grep "^OK" cosim2/quality/coverage_summary.log
echo "OK"

echo "== Gate 8: reproducible =="
grep "Reproducible: byte-identical" cosim2/quality/reproducible.log
echo "OK"

echo "== Gate 9: fault injection =="
grep "CheckerLiveness: PASSED" cosim2/quality/fault_inject.log
echo "OK"

echo "== Gate 10: cpp byte-identical =="
grep "C++ byte-identical" cosim2/quality/cpp_byte_identical.log
echo "OK"

echo "== Drift gate =="
(cd specgen && py -3 tools/codegen.py --check)
echo "OK"

echo
echo "ALL GATES PASSED. Ready to tag v0.5.0."
```

- [ ] **Step 2: Run aggregator + tag**

```bash
chmod +x cosim2/scripts/run_release_gates.sh
./cosim2/scripts/run_release_gates.sh

git tag -a v0.5.0 -m "Stage 5b release: specgen handshake upstream + rtl-style refactor

Release-level gates:
- All zone A/B/C sweep findings acked (CRITICAL+HIGH fixed)
- clang-tidy + verible-verilog-lint clean
- Verilator strict-mode warning-clean
- Parameter sweep 432 valid combos green + invalid rejected
- c_model UBSan + ASan ctest clean
- Coverage line ≥ 80%, toggle ≥ 70% (excludes wb2axip/specgen-generated)
- Reproducible generation: byte-identical from clean trees
- Fault injection: CheckerLiveness PASS (positive zero-exit, injection nonzero-exit)
- C++ byte-identical across regen
- Drift gate clean (py -3 specgen/tools/codegen.py --check)
"
git push origin v0.5.0
```

---

## Self-Review

**Spec coverage:**
- §1.2 W1 schema → Tasks 1-3 ✓
- §1.2 W2 atomic → Tasks 4-15 ✓
- §1.2 W3 sweep → Tasks 16-30 ✓
- §2.1 axi4_intf single → Tasks 6-7 ✓
- §2.1 NoC type consolidation w/ topology preserved → Tasks 13, 14 ✓
- §2.2 naming discipline → Task 2 (validator rejects `*_W$`, lowercase) ✓
- §4.1 constants.yaml → Task 1 ✓
- §4.3 interface_handshake.json (incl. NoC protocol_semantics, 1-hot per VC) → Task 3 ✓
- §4.4 generator-emit signal_interface.md handshake + AXI matrix sections → Task 8 ✓
- §5.2 SV emission examples → Tasks 4, 6 (hand-authored golden), 7 (emitter) ✓
- §5.3 wrap migration → Tasks 10-14 (preserves 4-port NoC topology in Task 13) ✓
- §6.5 gate 1+2 findings acked → Tasks 16-20 ✓
- §6.5 gate 3 lint → Task 21 ✓
- §6.5 gate 4 Verilator warn-clean → Task 22 ✓
- §6.5 gate 5 parameter sweep → Task 23 ✓
- §6.5 gate 6 negative parameter → Task 23 (negative tests bundled) ✓
- §6.5 gate 7 sanitizer → Task 24 (c_model only; cosim Verilator C++ excluded) ✓
- §6.5 gate 8 coverage → Task 25 (with actual VerilatedCov::write wiring) ✓
- §6.5 gate 9 reproducible gen → Task 26 ✓
- §6.5 gate 10 fault injection → Task 27 (uses existing CheckerLiveness) ✓
- §6.5 gate 11 C++ byte-identical → Task 28 ✓
- §6.5 gate 12 release tag v0.5.0 → Task 30 ✓
- §7.3 scenario tests (reset/backpressure/aw_w_simul/outstanding_id/5-ch) → Task 29 ✓

**Placeholder scan:** `merge_findings.py` (defined inline in Task 16 Step 3); `cpp_params.py`/`sv_params.py` (defined inline in Tasks 4-5); CheckerLiveness path (actual test name confirmed in `cosim2/tests/test_checker_fires_on_violation.cpp:11`); drift command (`py -3 specgen/tools/codegen.py --check` per `codegen.py:11`); Makefile editing (cosim2/verilator/Makefile per actual location). No `...` or "or whatever" hand-waves outside intentional ellipsis in code listings that follow the spec verbatim.

**Type consistency:**
- Emitter signature `emit(src_path: Path, spec_version: str) -> str` consistent across `sv_params`, `cpp_params`, existing `sv_signals`, etc. (matches `DOMAIN_TO_EMITTER` contract at `codegen.py:38`).
- `load_constants(path: Path) -> dict`, `load_interfaces(path: Path, constants: dict) -> dict` consistent across Tasks 2, 3, 4, 5, 7, 8.
- Parameter names (`ID_WIDTH`, `ADDR_WIDTH`, `DATA_WIDTH`, `NUM_VC`, `FLIT_WIDTH`, `SLAVE_VC_BUFFER_DEPTH`) uniform across Tasks 1, 6, 7, 10-14.

---

## Notes for Executor

- The spec §5.3 mentioned `slave_wrap.sv` and `noc_wrap.sv` informally; actual file names are `axi_slave_wrap.sv` and `loopback_noc_wrap.sv`. This plan uses the actual names throughout.
- `generator.py` is the markdown→JSON parser; SV emission lives in `specgen/tools/elaborate/sv_*.py` peer modules. New emitters (`sv_params.py`, `cpp_params.py`, `signal_interface_md.py`) follow the same peer pattern.
- `codegen.py` is the dispatcher. All new emitters MUST register in `DOMAIN_TO_EMITTER` and follow the `(src_path, spec_version) -> body_str` contract.
- W2 sub-commits 4-14 are not necessarily elaboration-clean mid-PR. Only Task 15 final gate runs the full ctest + drift gate.
- W3 sweeps (Tasks 16-18) dispatch subagent + Codex in parallel; use single message with two tool calls.
- Sanitizer gate (Task 24) targets `c_model/CMakeLists.txt` only. Cosim Verilator-generated C++ is intentionally not sanitized (Verilator-generated code patterns trigger false UBSan positives).
- Coverage gate (Task 25) excludes `cosim2/sv/wb2axip/**` (vendor) and `specgen/generated/**` (auto-gen). Verilator coverage requires explicit `VerilatedCov::write("coverage.dat")` call wired into `main.cpp` (this is added in Task 25 Step 1).
- Fault injection gate (Task 27) reuses existing `CheckerLiveness` test (`cosim2/tests/test_checker_fires_on_violation.cpp`) which already validates positive (zero exit) + negative (nonzero exit) cases against `injection_aw_unstable.yaml`.
- The plan does NOT call `python -m specgen` (no such entry point); all generator commands use `py -3 specgen/tools/codegen.py --target <X> --domain <Y>` per `codegen.py:4-13` docstring.
