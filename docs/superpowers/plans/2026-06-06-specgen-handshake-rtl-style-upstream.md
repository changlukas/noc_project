# specgen Handshake Upstream + rtl-style Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Upstream the handshake/parameter schema currently hand-written in `cosim2/sv/*_intf.sv` into specgen, refactor specgen SV emission to industry-standard style (single `axi4_intf` with master/slave modports, consolidated NoC interfaces with multi-hot credit pulse vector), migrate `cosim2/sv/` to consume specgen output, then release-level quality sweep.

**Architecture:** 3 phases. W1 adds the schema sources (`constants.yaml`, `interface_handshake.json`) and loader validation without changing emission — drift gate stays clean. W2 is one atomic PR that refactors emission, regenerates artifacts, and migrates `cosim2/sv/` together — intermediate sub-commits may be elaboration-broken, but the merged tree is CI-gated. W3 runs a release-level quality sweep (Karpathy 4-lens + magic-number hunt + 10 verification gates).

**Tech Stack:** Python 3 (specgen generator), SystemVerilog (Verilator 5.036), C++17 (c_model), CMake/ctest, MSYS2 on Windows (PATH="/c/msys64/mingw64/bin:$PATH"), pytest (specgen tests), GoogleTest (c_model tests).

**Source spec:** `docs/superpowers/specs/2026-06-06-specgen-handshake-rtl-style-upstream-design.md` (commit `1449efa`).

---

## File Structure

### Created

- `specgen/source/constants.yaml` — language-neutral single source of truth for all SV/C++ parameters (replaces ad-hoc `ni_flit_constants.h` symbol references).
- `specgen/source/interface_handshake.json` — declares the 3 final interfaces (`axi4_intf`, `noc_req_intf`, `noc_rsp_intf`) with channels/signals/modports/protocol semantics.
- `specgen/ni_spec/handshake_schema.py` — loader + validator for the two new sources (one module to keep parsing logic isolated; touch points are minimal).
- `specgen/generated/sv/ni_params_pkg.sv` — SV `parameter int unsigned` defaults from `constants.yaml`.
- `specgen/generated/cpp/ni_params.h` — C++ `constexpr` constants from `constants.yaml`.
- `specgen/tests/golden/ni_params_pkg.sv.golden` — golden for the new SV package.
- `specgen/tests/golden/ni_params.h.golden` — golden for the new C++ header.
- `specgen/tests/test_handshake_schema.py` — validator/loader tests for `constants.yaml` and `interface_handshake.json`.
- `specgen/tests/test_parameter_sweep.py` — W3 gate 5/6: parameter matrix elaboration + negative validation.
- `cosim2/scripts/run_release_gates.sh` — orchestrator for W3 verification gates.
- `cosim2/scripts/check_reproducible_gen.sh` — W3 gate 9: byte-identical generation from clean trees.
- `cosim2/scripts/check_byte_identical_cpp.sh` — W3 gate 11.

### Modified

- `specgen/ni_spec/loader.py` — call into `handshake_schema.load()` during normal load.
- `specgen/tools/elaborate/sv_signals.py` — rewrite `_emit_sv_interfaces()` and add `_emit_sv_params_pkg()`; consume `interface_handshake.json` instead of inferring from `signal_interface.md` for handshake/modport.
- `specgen/tools/codegen.py` — register new emitters (`ni_params_pkg.sv`, `ni_params.h`).
- `specgen/tests/golden/ni_signals_pkg.sv.golden` — fully regenerated (new style).
- `specgen/tests/golden/ni_signals.h.golden` — regenerated.
- `spec/ni/doc/signal_interface.md` — generator now emits the `## Handshake & Modport Convention` and `## AXI4 Signal Matrix` sections from JSON. Hand-edited prose for those two sections gets replaced.
- `cosim2/sv/nmu_wrap.sv` — use specgen interfaces; add `clk_i / rst_ni` module ports.
- `cosim2/sv/nsu_wrap.sv` — same.
- `cosim2/sv/axi_master_wrap.sv` — use `axi4_intf.master`.
- `cosim2/sv/axi_slave_wrap.sv` — use `axi4_intf.slave`.
- `cosim2/sv/loopback_noc_wrap.sv` — use `noc_req_intf` + `noc_rsp_intf`.
- `cosim2/sv/tb_top.sv` — instantiate specgen interfaces; route `clk_i / rst_ni` as module ports.
- `cosim2/CMakeLists.txt` — drop the removed `*_intf.sv` from the verilate source list; ensure `ni_params_pkg.sv` is added before `ni_signals_pkg.sv`.

### Deleted

- `cosim2/sv/axi_intf.sv`
- `cosim2/sv/noc_req_intf.sv`
- `cosim2/sv/noc_rsp_intf.sv`

---

# Phase W1 — Schema Sources (single PR)

W1 introduces the new source files plus a loader/validator. **Generator emission is not yet changed** — drift gate stays clean throughout. PR title: `feat(specgen): add handshake schema + language-neutral constants source`.

---

### Task 1: Create `constants.yaml` source-of-truth

**Files:**
- Create: `specgen/source/constants.yaml`

- [ ] **Step 1: Write the file**

```yaml
# specgen/source/constants.yaml
# Language-neutral parameter defaults. Generator emits both SV (ni_params_pkg.sv)
# and C++ (ni_params.h) from this file. See spec §4.1.
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
    description: "Per-VC buffer depth at NoC slave; initial credit count exposed to producer"
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

- [ ] **Step 2: Sanity-check YAML parses**

Run: `py -3 -c "import yaml; print(yaml.safe_load(open('specgen/source/constants.yaml')))"`
Expected: dict-of-dicts printed, no exception.

- [ ] **Step 3: Commit**

```bash
git add specgen/source/constants.yaml
git commit -m "feat(specgen): add constants.yaml language-neutral parameter source"
```

---

### Task 2: Create `handshake_schema.py` loader + tests

**Files:**
- Create: `specgen/ni_spec/handshake_schema.py`
- Create: `specgen/tests/test_handshake_schema.py`

- [ ] **Step 1: Write the failing test**

```python
# specgen/tests/test_handshake_schema.py
"""Tests for constants.yaml + interface_handshake.json loader/validator."""
from pathlib import Path
import pytest

from ni_spec.handshake_schema import (
    load_constants,
    HandshakeSchemaError,
)

SOURCE_DIR = Path(__file__).resolve().parent.parent / "source"


def test_load_constants_returns_dict_with_expected_keys():
    c = load_constants(SOURCE_DIR / "constants.yaml")
    assert c["schema_version"] == "1.0"
    assert "axi" in c
    assert "noc" in c
    assert "derived" in c
    assert c["axi"]["ID_WIDTH"]["default"] == 8
    assert c["axi"]["DATA_WIDTH"]["allowed"] == [32, 64, 128, 256, 512, 1024]
    assert c["noc"]["FLIT_WIDTH"]["sv_symbol"] == "NI_NOC_FLIT_WIDTH_DFLT"
    assert c["noc"]["FLIT_WIDTH"]["cpp_symbol"] == "kNiNocFlitWidth"


def test_load_constants_rejects_abbreviated_w_suffix(tmp_path):
    """Per naming discipline (spec §2.2): names matching *_W$ are forbidden."""
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "noc:\n"
        "  FLIT_W:\n"
        "    type: int\n"
        "    default: 408\n"
        "    sv_symbol: X\n"
        "    cpp_symbol: x\n"
    )
    with pytest.raises(HandshakeSchemaError, match="FLIT_W.*abbreviated"):
        load_constants(bad)


def test_load_constants_rejects_lowercase_param_name(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "axi:\n"
        "  id_width:\n"
        "    type: int\n"
        "    default: 8\n"
        "    sv_symbol: X\n"
        "    cpp_symbol: x\n"
    )
    with pytest.raises(HandshakeSchemaError, match="lowercase"):
        load_constants(bad)


def test_load_constants_rejects_unknown_top_level_key(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text('schema_version: "1.0"\nbogus: {}\n')
    with pytest.raises(HandshakeSchemaError, match="unknown"):
        load_constants(bad)


def test_load_constants_rejects_missing_required_field(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "axi:\n"
        "  ID_WIDTH:\n"
        "    type: int\n"
        "    default: 8\n"
        # missing sv_symbol/cpp_symbol
    )
    with pytest.raises(HandshakeSchemaError, match="missing.*sv_symbol"):
        load_constants(bad)


def test_load_constants_rejects_default_outside_range(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "axi:\n"
        "  ID_WIDTH:\n"
        "    type: int\n"
        "    default: 100\n"
        "    min: 1\n"
        "    max: 32\n"
        "    sv_symbol: X\n"
        "    cpp_symbol: x\n"
    )
    with pytest.raises(HandshakeSchemaError, match="default.*max"):
        load_constants(bad)


def test_load_constants_rejects_default_not_in_allowed(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        'schema_version: "1.0"\n'
        "axi:\n"
        "  DATA_WIDTH:\n"
        "    type: int\n"
        "    default: 33\n"
        "    allowed: [32, 64]\n"
        "    sv_symbol: X\n"
        "    cpp_symbol: x\n"
    )
    with pytest.raises(HandshakeSchemaError, match="default.*allowed"):
        load_constants(bad)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd specgen && py -3 -m pytest tests/test_handshake_schema.py -v`
Expected: ImportError on `ni_spec.handshake_schema`.

- [ ] **Step 3: Write loader/validator implementation**

```python
# specgen/ni_spec/handshake_schema.py
"""Loader + validator for constants.yaml and interface_handshake.json.

Both files are project-scoped and emit into multiple downstream artifacts
(SV parameters, C++ constants, interface modules), so a strict validator
catches schema drift at load time rather than as silent regenerated diffs.

See design spec §4.2 for the rule set.
"""
from __future__ import annotations
import json
import re
from pathlib import Path
from typing import Any, Dict

import yaml


class HandshakeSchemaError(ValueError):
    """Raised when constants.yaml or interface_handshake.json fails validation."""


_TOP_LEVEL_KEYS = {"schema_version", "axi", "noc", "derived"}
_REQUIRED_PARAM_FIELDS = {"type", "default", "sv_symbol", "cpp_symbol"}
_PARAM_NAME_PATTERN = re.compile(r"^[A-Z][A-Z0-9_]*$")
_FORBIDDEN_W_SUFFIX = re.compile(r"_W$")


def load_constants(path: Path) -> Dict[str, Any]:
    """Load constants.yaml and run all validation rules."""
    text = path.read_text(encoding="utf-8")
    data = yaml.safe_load(text)
    if not isinstance(data, dict):
        raise HandshakeSchemaError(f"{path}: top level must be a mapping")

    unknown = set(data.keys()) - _TOP_LEVEL_KEYS
    if unknown:
        raise HandshakeSchemaError(
            f"{path}: unknown top-level keys: {sorted(unknown)}"
        )
    if data.get("schema_version") != "1.0":
        raise HandshakeSchemaError(
            f"{path}: schema_version must be \"1.0\""
        )

    for domain in ("axi", "noc"):
        if domain not in data:
            continue
        for name, spec in data[domain].items():
            _validate_param_name(name, where=f"{domain}.{name}")
            _validate_param_spec(spec, where=f"{domain}.{name}")

    if "derived" in data:
        for name, spec in data["derived"].items():
            _validate_param_name(name, where=f"derived.{name}")
            if "expression" not in spec:
                raise HandshakeSchemaError(
                    f"derived.{name}: missing expression"
                )

    return data


def _validate_param_name(name: str, where: str) -> None:
    if not _PARAM_NAME_PATTERN.match(name):
        if name != name.upper():
            raise HandshakeSchemaError(
                f"{where}: parameter name must be UPPER_SNAKE_CASE; "
                f"lowercase characters not allowed"
            )
        raise HandshakeSchemaError(
            f"{where}: parameter name must be UPPER_SNAKE_CASE"
        )
    if _FORBIDDEN_W_SUFFIX.search(name):
        raise HandshakeSchemaError(
            f"{where}: parameter name uses abbreviated `_W` suffix; "
            f"use `_WIDTH` per naming discipline"
        )


def _validate_param_spec(spec: Dict[str, Any], where: str) -> None:
    missing = _REQUIRED_PARAM_FIELDS - set(spec.keys())
    if missing:
        raise HandshakeSchemaError(
            f"{where}: missing required field(s): {sorted(missing)}"
        )
    default = spec["default"]
    if "min" in spec and default < spec["min"]:
        raise HandshakeSchemaError(
            f"{where}: default {default} < min {spec['min']}"
        )
    if "max" in spec and default > spec["max"]:
        raise HandshakeSchemaError(
            f"{where}: default {default} > max {spec['max']}"
        )
    if "allowed" in spec and default not in spec["allowed"]:
        raise HandshakeSchemaError(
            f"{where}: default {default} not in allowed set {spec['allowed']}"
        )


def load_interfaces(path: Path, constants: Dict[str, Any]) -> Dict[str, Any]:
    """Load interface_handshake.json and cross-check parameter references."""
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema_version") != "1.0":
        raise HandshakeSchemaError(
            f"{path}: schema_version must be \"1.0\""
        )
    if "interfaces" not in data:
        raise HandshakeSchemaError(f"{path}: missing 'interfaces' key")

    for iface_name, iface_spec in data["interfaces"].items():
        for p in iface_spec.get("parameters", []):
            ref = p.get("constants_yaml_key", "")
            domain, _, key = ref.partition(".")
            if domain not in constants or key not in constants[domain]:
                raise HandshakeSchemaError(
                    f"{iface_name}: parameter {p['name']} references "
                    f"unknown constants.yaml key {ref!r}"
                )
    return data
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd specgen && py -3 -m pytest tests/test_handshake_schema.py -v`
Expected: 7 PASS.

- [ ] **Step 5: Commit**

```bash
git add specgen/ni_spec/handshake_schema.py specgen/tests/test_handshake_schema.py
git commit -m "feat(specgen): add handshake_schema loader + validator for constants.yaml"
```

---

### Task 3: Create `interface_handshake.json` + extend tests

**Files:**
- Create: `specgen/source/interface_handshake.json`
- Modify: `specgen/tests/test_handshake_schema.py` (append tests)

- [ ] **Step 1: Write the failing test (append to existing test file)**

Append at end of `specgen/tests/test_handshake_schema.py`:

```python
def test_load_interfaces_returns_three_interfaces():
    c = load_constants(SOURCE_DIR / "constants.yaml")
    from ni_spec.handshake_schema import load_interfaces
    data = load_interfaces(SOURCE_DIR / "interface_handshake.json", c)
    assert set(data["interfaces"].keys()) == {
        "axi4_intf", "noc_req_intf", "noc_rsp_intf"
    }


def test_load_interfaces_rejects_unknown_parameter_ref(tmp_path):
    from ni_spec.handshake_schema import load_interfaces
    c = load_constants(SOURCE_DIR / "constants.yaml")
    bad = tmp_path / "bad.json"
    bad.write_text('{"schema_version":"1.0","interfaces":{"x":{"parameters":'
                   '[{"name":"FOO","constants_yaml_key":"noc.BOGUS"}]}}}')
    with pytest.raises(HandshakeSchemaError, match="unknown constants.yaml key"):
        load_interfaces(bad, c)


def test_noc_req_intf_has_protocol_semantics():
    c = load_constants(SOURCE_DIR / "constants.yaml")
    from ni_spec.handshake_schema import load_interfaces
    data = load_interfaces(SOURCE_DIR / "interface_handshake.json", c)
    sem = data["interfaces"]["noc_req_intf"]["protocol_semantics"]
    assert sem["credit_return_encoding"]["scheme"] == "per_vc_credit_pulse_vector"
    assert sem["credit_return_encoding"]["onehot_check_required"] is False
    assert sem["initial_credits"]["value_per_vc"] == "SLAVE_VC_BUFFER_DEPTH"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd specgen && py -3 -m pytest tests/test_handshake_schema.py::test_load_interfaces_returns_three_interfaces -v`
Expected: FileNotFoundError on `interface_handshake.json`.

- [ ] **Step 3: Write the JSON file (full content per spec §4.3)**

Create `specgen/source/interface_handshake.json` with the content from spec §4.3 verbatim. Brief shape (full JSON in spec section §4.3):

```json
{
  "schema_version": "1.0",
  "interfaces": {
    "axi4_intf": {
      "kind": "axi4",
      "parameters": [
        {"name": "ID_WIDTH",   "constants_yaml_key": "axi.ID_WIDTH"},
        {"name": "ADDR_WIDTH", "constants_yaml_key": "axi.ADDR_WIDTH"},
        {"name": "DATA_WIDTH", "constants_yaml_key": "axi.DATA_WIDTH"}
      ],
      "channels": ["AW", "W", "B", "AR", "R"],
      "signal_matrix_source": "IHI_0022H_TABLE_A9_1_to_A9_4",
      "modports": ["master", "slave"]
    },
    "noc_req_intf": { /* see spec §4.3 */ },
    "noc_rsp_intf": { /* see spec §4.3 */ }
  }
}
```

Copy the full block from spec §4.3 into the JSON file (the noc_req_intf entry must include the complete `protocol_semantics` block with `transfer_condition`, `credit_return_encoding`, `initial_credits`, `valid_stability`, `combinational_loops`).

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd specgen && py -3 -m pytest tests/test_handshake_schema.py -v`
Expected: 10 PASS.

- [ ] **Step 5: Wire loader into normal load path**

Modify `specgen/ni_spec/loader.py`: at the end of the existing top-level `load_doc()` (or equivalent entry), add:

```python
from ni_spec.handshake_schema import load_constants, load_interfaces
constants = load_constants(SOURCE_DIR / "constants.yaml")
interfaces = load_interfaces(SOURCE_DIR / "interface_handshake.json", constants)
doc.constants = constants
doc.interfaces = interfaces
```

(Adapt to actual `loader.py` structure; the goal is `load_doc()` populates the new fields. Inspect the file first to find the right insertion point.)

- [ ] **Step 6: Verify drift gate still clean**

Run: `cd specgen && py -3 -m pytest -x -q`
Expected: all existing tests pass; no generator artifact has changed yet.

- [ ] **Step 7: Commit**

```bash
git add specgen/source/interface_handshake.json specgen/tests/test_handshake_schema.py specgen/ni_spec/loader.py
git commit -m "feat(specgen): add interface_handshake.json + wire loader"
```

---

### W1 PR gate

Before merging the W1 PR:

- [ ] `py -3 -m pytest specgen/tests/ -v` — all green
- [ ] `git diff main -- specgen/generated/` — empty (no regenerated artifact change)
- [ ] PR description lists the 3 new source files and the loader.py insertion

PR title: `feat(specgen): add handshake schema + language-neutral constants source`

---

# Phase W2 — Atomic Refactor + Regenerate + Migrate (single PR)

W2 is one merged PR. Sub-commits inside the PR are for code-review readability; intermediate sub-commits may not elaborate cleanly. **Final tree** must pass: ctest 410/410 + drift gate clean + Verilator warning-clean elaboration.

PR title: `refactor(specgen+cosim2): industry-style SV interfaces + atomic migration`

---

### Task 4: Emit `ni_params_pkg.sv` from `constants.yaml`

**Files:**
- Modify: `specgen/tools/elaborate/sv_signals.py`
- Modify: `specgen/tools/codegen.py`
- Create: `specgen/tests/golden/ni_params_pkg.sv.golden`
- Modify: `specgen/tests/test_codegen_sv.py`

- [ ] **Step 1: Write the failing test**

Append to `specgen/tests/test_codegen_sv.py`:

```python
def test_ni_params_pkg_sv_emitted(tmp_path):
    """ni_params_pkg.sv is emitted from constants.yaml and matches golden."""
    from ni_spec.handshake_schema import load_constants
    from tools.elaborate.sv_signals import emit_params_pkg

    c = load_constants(Path("specgen/source/constants.yaml"))
    out = emit_params_pkg(c, spec_version="1.0")

    golden = Path("specgen/tests/golden/ni_params_pkg.sv.golden").read_text()
    assert out == golden
```

- [ ] **Step 2: Run test to verify it fails**

Run: `py -3 -m pytest specgen/tests/test_codegen_sv.py::test_ni_params_pkg_sv_emitted -v`
Expected: ImportError on `emit_params_pkg`.

- [ ] **Step 3: Implement `emit_params_pkg()`**

Add to `specgen/tools/elaborate/sv_signals.py`:

```python
def emit_params_pkg(constants: dict, spec_version: str) -> str:
    """Emit ni_params_pkg.sv from constants.yaml. See design spec §5.2."""
    lines: list[str] = []
    lines.append(f"// Generated by specgen v{spec_version} -- DO NOT EDIT")
    lines.append("// Source: specgen/source/constants.yaml")
    lines.append("")
    lines.append("package ni_params_pkg;")

    def _emit_domain(domain: str, label: str) -> None:
        if domain not in constants:
            return
        lines.append(f"    // {label}")
        for name, spec in constants[domain].items():
            sym = spec["sv_symbol"]
            val = spec["default"]
            lines.append(f"    parameter int unsigned {sym} = {val};")
        lines.append("")

    _emit_domain("axi", "AXI parameter defaults")
    _emit_domain("noc", "NoC parameter defaults")

    if "derived" in constants:
        lines.append("    // Derived parameter defaults")
        for name, spec in constants["derived"].items():
            sym = spec["sv_symbol"]
            expr = spec["expression"]
            # Replace bare DATA_WIDTH etc. with their sv_symbol
            resolved = expr
            for domain in ("axi", "noc"):
                for k, v in constants.get(domain, {}).items():
                    resolved = resolved.replace(k, str(v["default"]))
            lines.append(f"    parameter int unsigned {sym} = {resolved};")
        lines.append("")

    lines.append("endpackage")
    return "\n".join(lines) + "\n"
```

- [ ] **Step 4: Generate golden by hand-running emitter once**

Run:
```bash
py -3 -c "
from pathlib import Path
import sys; sys.path.insert(0, 'specgen')
from ni_spec.handshake_schema import load_constants
from tools.elaborate.sv_signals import emit_params_pkg
c = load_constants(Path('specgen/source/constants.yaml'))
out = emit_params_pkg(c, spec_version='1.0')
Path('specgen/tests/golden/ni_params_pkg.sv.golden').write_text(out)
print(out)
"
```

Expected output (also is the golden):
```systemverilog
// Generated by specgen v1.0 -- DO NOT EDIT
// Source: specgen/source/constants.yaml

package ni_params_pkg;
    // AXI parameter defaults
    parameter int unsigned NI_AXI_ID_WIDTH_DFLT = 8;
    parameter int unsigned NI_AXI_ADDR_WIDTH_DFLT = 64;
    parameter int unsigned NI_AXI_DATA_WIDTH_DFLT = 256;

    // NoC parameter defaults
    parameter int unsigned NI_NOC_NUM_VC_DFLT = 1;
    parameter int unsigned NI_NOC_FLIT_WIDTH_DFLT = 408;
    parameter int unsigned NI_NOC_SLAVE_VC_BUFFER_DEPTH_DFLT = 4;

    // Derived parameter defaults
    parameter int unsigned NI_AXI_WSTRB_WIDTH_DFLT = 32;

endpackage
```

- [ ] **Step 5: Wire emitter into `codegen.py`**

In `specgen/tools/codegen.py`, find the section that writes existing `*_pkg.sv` outputs and add:

```python
from tools.elaborate.sv_signals import emit_params_pkg
out_dir = Path("specgen/generated/sv")
(out_dir / "ni_params_pkg.sv").write_text(
    emit_params_pkg(doc.constants, spec_version)
)
```

- [ ] **Step 6: Run tests to verify**

Run: `py -3 -m pytest specgen/tests/test_codegen_sv.py::test_ni_params_pkg_sv_emitted -v`
Expected: PASS.

Run: `py -3 -m specgen` (or whatever the entry command is — check `specgen/ni_spec/__main__.py`)
Expected: `specgen/generated/sv/ni_params_pkg.sv` regenerated, byte-identical to golden.

- [ ] **Step 7: Commit (review-only sub-commit inside W2 PR)**

```bash
git add specgen/tools/elaborate/sv_signals.py specgen/tools/codegen.py \
        specgen/tests/golden/ni_params_pkg.sv.golden \
        specgen/tests/test_codegen_sv.py \
        specgen/generated/sv/ni_params_pkg.sv
git commit -m "feat(specgen): emit ni_params_pkg.sv from constants.yaml"
```

---

### Task 5: Emit `ni_params.h` from `constants.yaml`

**Files:**
- Modify: `specgen/tools/elaborate/sv_signals.py` (or add `cpp_params.py` — match existing CPP emitter layout)
- Create: `specgen/tests/golden/ni_params.h.golden`
- Modify: `specgen/tests/test_codegen_sv.py` (or `test_codegen_cpp.py` if separate)

- [ ] **Step 1: Investigate existing CPP emit pattern**

Run: `grep -rn "def emit" specgen/tools/elaborate/ | grep -i cpp`
Action: locate the CPP emitter file (likely `specgen/tools/elaborate/cpp_constants.py` or in `codegen.py`). Add `emit_params_h()` next to it.

- [ ] **Step 2: Write the failing test**

Append to the same test file used for the existing CPP emit tests:

```python
def test_ni_params_h_emitted():
    from ni_spec.handshake_schema import load_constants
    # Adjust import path to where you placed emit_params_h
    from tools.elaborate.sv_signals import emit_params_h
    c = load_constants(Path("specgen/source/constants.yaml"))
    out = emit_params_h(c, spec_version="1.0")
    golden = Path("specgen/tests/golden/ni_params.h.golden").read_text()
    assert out == golden
```

- [ ] **Step 3: Implement `emit_params_h()`**

```python
def emit_params_h(constants: dict, spec_version: str) -> str:
    """Emit ni_params.h from constants.yaml."""
    lines: list[str] = []
    lines.append(f"// Generated by specgen v{spec_version} -- DO NOT EDIT")
    lines.append("// Source: specgen/source/constants.yaml")
    lines.append("")
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <cstddef>")
    lines.append("")
    lines.append("namespace noc {")

    def _emit_domain(domain: str, label: str) -> None:
        if domain not in constants:
            return
        lines.append(f"// {label}")
        for name, spec in constants[domain].items():
            sym = spec["cpp_symbol"]
            val = spec["default"]
            lines.append(f"constexpr int {sym} = {val};")
        lines.append("")

    _emit_domain("axi", "AXI parameter defaults")
    _emit_domain("noc", "NoC parameter defaults")

    if "derived" in constants:
        lines.append("// Derived parameter defaults")
        for name, spec in constants["derived"].items():
            sym = spec["cpp_symbol"]
            expr = spec["expression"]
            resolved = expr
            for domain in ("axi", "noc"):
                for k, v in constants.get(domain, {}).items():
                    resolved = resolved.replace(k, str(v["default"]))
            lines.append(f"constexpr int {sym} = {resolved};")
        lines.append("")

    lines.append("}  // namespace noc")
    return "\n".join(lines) + "\n"
```

- [ ] **Step 4: Generate golden + wire to codegen.py**

Run:
```bash
py -3 -c "
from pathlib import Path
import sys; sys.path.insert(0, 'specgen')
from ni_spec.handshake_schema import load_constants
from tools.elaborate.sv_signals import emit_params_h
c = load_constants(Path('specgen/source/constants.yaml'))
Path('specgen/tests/golden/ni_params.h.golden').write_text(
    emit_params_h(c, spec_version='1.0'))
"
```

Add to `specgen/tools/codegen.py`:
```python
from tools.elaborate.sv_signals import emit_params_h
(Path("specgen/generated/cpp") / "ni_params.h").write_text(
    emit_params_h(doc.constants, spec_version)
)
```

- [ ] **Step 5: Run tests + regenerate**

Run: `py -3 -m pytest specgen/tests/ -v -k params`
Expected: 2 PASS.

Run: `py -3 -m specgen`
Expected: `specgen/generated/cpp/ni_params.h` regenerated.

- [ ] **Step 6: Commit**

```bash
git add specgen/tools/elaborate/sv_signals.py specgen/tools/codegen.py \
        specgen/tests/golden/ni_params.h.golden specgen/tests/test_codegen_sv.py \
        specgen/generated/cpp/ni_params.h
git commit -m "feat(specgen): emit ni_params.h from constants.yaml"
```

---

### Task 6: Refactor `_emit_sv_interfaces()` — `axi4_intf` consolidated

**Files:**
- Modify: `specgen/tools/elaborate/sv_signals.py:43-76` (current `_emit_sv_interfaces`)
- Modify: `specgen/tests/test_codegen_sv.py`

- [ ] **Step 1: Read the current emitter**

Run: `sed -n '43,80p' specgen/tools/elaborate/sv_signals.py`
Note: the current code groups by interface from `signals_pin_by_interface()`. New code branches on `interface_handshake.json` definitions instead.

- [ ] **Step 2: Write the failing test**

Add to `specgen/tests/test_codegen_sv.py`:

```python
def test_emit_axi4_intf_single_interface_with_modports():
    from ni_spec.handshake_schema import load_constants, load_interfaces
    from tools.elaborate.sv_signals import emit_sv_interface_blocks

    c = load_constants(Path("specgen/source/constants.yaml"))
    ifs = load_interfaces(Path("specgen/source/interface_handshake.json"), c)

    out = "\n".join(emit_sv_interface_blocks(ifs, c))
    # Single axi4_intf, parameterized, modports master+slave
    assert "interface axi4_intf #(" in out
    assert "parameter int unsigned ID_WIDTH   = ni_params_pkg::NI_AXI_ID_WIDTH_DFLT" in out
    assert "modport master" in out
    assert "modport slave"  in out
    # The collapsed single interface, NOT two
    assert "axi_master_port_intf" not in out
    assert "axi_slave_port_intf"  not in out
    # AMBA neutral lowercase signal names
    assert "logic [ID_WIDTH-1:0]    awid" in out
    assert "logic                   awvalid" in out
    assert "logic                   awready" in out
    # All 5 channels
    for sig in ("awvalid", "wvalid", "bvalid", "arvalid", "rvalid"):
        assert sig in out
```

- [ ] **Step 3: Run test to verify it fails**

Run: `py -3 -m pytest specgen/tests/test_codegen_sv.py::test_emit_axi4_intf_single_interface_with_modports -v`
Expected: FAIL (function `emit_sv_interface_blocks` does not yet emit consolidated `axi4_intf`).

- [ ] **Step 4: Implement `emit_sv_interface_blocks()`**

Replace `_emit_sv_interfaces()` (line 43 in current `sv_signals.py`) with a new public `emit_sv_interface_blocks()` that consumes `interface_handshake.json`. New code body:

```python
# Constants per IHI 0022H §A2.2-A2.6 fixed widths
_AXI_FIXED_WIDTH = {
    "awlen": 8, "wlen": 8, "arlen": 8,
    "awsize": 3, "arsize": 3,
    "awburst": 2, "arburst": 2,
    "awlock": 1, "arlock": 1,
    "awcache": 4, "arcache": 4,
    "awprot": 3, "arprot": 3,
    "awqos": 4, "arqos": 4,
    "awregion": 4, "arregion": 4,
    "bresp": 2, "rresp": 2,
}

# Per-channel signal lists per IHI 0022H §A9.3 Tables A9-1..A9-4
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
    "AR": [  # same shape as AW
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

# Master drives "request" signals; slave drives "ready/response" signals
_MASTER_DRIVES = {
    "awid","awaddr","awlen","awsize","awburst","awlock","awcache",
    "awprot","awqos","awregion","awvalid",
    "wdata","wstrb","wlast","wvalid",
    "bready",
    "arid","araddr","arlen","arsize","arburst","arlock","arcache",
    "arprot","arqos","arregion","arvalid",
    "rready",
}


def _format_width(width_spec: str) -> str:
    """'ID_WIDTH' -> '[ID_WIDTH-1:0]   '; 'fixed:8' -> '[7:0]            '
    'fixed:1' -> '                  ' (1-bit signals get no range).
    """
    if width_spec.startswith("fixed:"):
        n = int(width_spec.split(":", 1)[1])
        if n == 1:
            return ""
        return f"[{n-1}:0]"
    return f"[{width_spec}-1:0]"


def emit_sv_interface_blocks(interfaces_doc: dict, constants: dict) -> list[str]:
    """Emit all interface blocks per design spec §5.2.

    Returns lines; SV interfaces must live OUTSIDE any package.
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
    out.append(f"    localparam int unsigned WSTRB_WIDTH = DATA_WIDTH / 8;")
    out.append("")

    # Signal declarations, channel-by-channel
    for ch in spec["channels"]:
        for sig_name, width_spec in _AXI_CHANNEL_SIGNALS[ch]:
            w = _format_width(width_spec)
            if w:
                out.append(f"    logic {w:18s} {sig_name};")
            else:
                out.append(f"    logic                   {sig_name};")
        out.append("")

    # modports
    out.append("    modport master (")
    out.append("        output " + ", ".join(
        s for ch in spec["channels"] for s, _ in _AXI_CHANNEL_SIGNALS[ch]
        if s in _MASTER_DRIVES))
    out.append("        ,")
    out.append("        input  " + ", ".join(
        s for ch in spec["channels"] for s, _ in _AXI_CHANNEL_SIGNALS[ch]
        if s not in _MASTER_DRIVES))
    out.append("    );")
    out.append("")
    out.append("    modport slave (")
    out.append("        input  " + ", ".join(
        s for ch in spec["channels"] for s, _ in _AXI_CHANNEL_SIGNALS[ch]
        if s in _MASTER_DRIVES))
    out.append("        ,")
    out.append("        output " + ", ".join(
        s for ch in spec["channels"] for s, _ in _AXI_CHANNEL_SIGNALS[ch]
        if s not in _MASTER_DRIVES))
    out.append("    );")
    out.append(f"endinterface : {name}")
    return out
```

- [ ] **Step 5: Run test to verify it passes**

Run: `py -3 -m pytest specgen/tests/test_codegen_sv.py::test_emit_axi4_intf_single_interface_with_modports -v`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add specgen/tools/elaborate/sv_signals.py specgen/tests/test_codegen_sv.py
git commit -m "feat(specgen): emit single axi4_intf with master/slave modports"
```

---

### Task 7: Implement NoC interface emission (consolidated 4→2)

**Files:**
- Modify: `specgen/tools/elaborate/sv_signals.py` (add `_emit_noc_intf`)
- Modify: `specgen/tests/test_codegen_sv.py`

- [ ] **Step 1: Write the failing test**

```python
def test_emit_noc_req_intf_consolidated_with_multi_hot_credit():
    from ni_spec.handshake_schema import load_constants, load_interfaces
    from tools.elaborate.sv_signals import emit_sv_interface_blocks

    c = load_constants(Path("specgen/source/constants.yaml"))
    ifs = load_interfaces(Path("specgen/source/interface_handshake.json"), c)
    out = "\n".join(emit_sv_interface_blocks(ifs, c))

    # Consolidated: NO old endpoint-split interface names anywhere
    for old in ("ni_noc_req_out_intf", "ni_noc_req_in_intf",
                "ni_noc_rsp_out_intf", "ni_noc_rsp_in_intf"):
        assert old not in out
    # New names
    assert "interface noc_req_intf" in out
    assert "interface noc_rsp_intf" in out
    # Multi-hot credit_return is NUM_VC wide
    assert "logic [NUM_VC-1:0]       credit_return" in out
    # Modports
    assert "modport master ( output valid, flit, input  credit_return )" in out
    assert "modport slave  ( input  valid, flit, output credit_return )" in out
```

- [ ] **Step 2: Run test to verify it fails**

Run: `py -3 -m pytest specgen/tests/test_codegen_sv.py::test_emit_noc_req_intf_consolidated_with_multi_hot_credit -v`
Expected: FAIL.

- [ ] **Step 3: Implement `_emit_noc_intf()`**

Add to `sv_signals.py`:

```python
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
```

- [ ] **Step 4: Run test + sanity check**

Run: `py -3 -m pytest specgen/tests/test_codegen_sv.py -v -k noc`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add specgen/tools/elaborate/sv_signals.py specgen/tests/test_codegen_sv.py
git commit -m "feat(specgen): consolidate 4 NoC interfaces -> noc_req_intf + noc_rsp_intf"
```

---

### Task 8: Wire new emitter into `emit()` (replaces `_emit_sv_interfaces`)

**Files:**
- Modify: `specgen/tools/elaborate/sv_signals.py:81-` (function `emit()`)

- [ ] **Step 1: Read `emit()`**

Run: `sed -n '81,140p' specgen/tools/elaborate/sv_signals.py`

- [ ] **Step 2: Replace the call to `_emit_sv_interfaces(...)` with the new emitter**

Find the line in `emit()` that calls `_emit_sv_interfaces(signals_spec, packet_spec)` (around line 121 per earlier grep). Replace with:

```python
# OLD:
# lines.extend(_emit_sv_interfaces(signals_spec, packet_spec))

# NEW:
from ni_spec.handshake_schema import load_constants, load_interfaces
constants = load_constants(Path("specgen/source/constants.yaml"))
interfaces_doc = load_interfaces(Path("specgen/source/interface_handshake.json"), constants)
lines.extend(emit_sv_interface_blocks(interfaces_doc, constants))
```

(If `emit()` already receives `constants` and `interfaces_doc` through arguments via `codegen.py`, prefer parameter passing over re-loading; adapt to the actual call signature.)

- [ ] **Step 3: Run pytest to see breakage**

Run: `py -3 -m pytest specgen/tests/ -v`
Expected: golden tests (`ni_signals_pkg.sv.golden`) now fail because new emission differs from old golden. **This is intentional** — Task 9 regenerates the goldens.

- [ ] **Step 4: Commit (intermediate; not green)**

```bash
git add specgen/tools/elaborate/sv_signals.py
git commit -m "refactor(specgen): wire emit_sv_interface_blocks into emit()"
```

---

### Task 9: Regenerate `specgen/generated/` + update goldens

**Files:**
- Modify: `specgen/generated/sv/ni_signals_pkg.sv`
- Modify: `specgen/generated/sv/ni_params_pkg.sv` (auto)
- Modify: `specgen/generated/cpp/ni_params.h` (auto)
- Modify: `specgen/tests/golden/ni_signals_pkg.sv.golden`
- Modify: `specgen/tests/golden/ni_params_pkg.sv.golden`
- Modify: `specgen/tests/golden/ni_params.h.golden`

- [ ] **Step 1: Regenerate**

Run: `py -3 -m specgen`
Expected: all `specgen/generated/{sv,cpp}/*` files rewritten.

- [ ] **Step 2: Sanity-check `ni_signals_pkg.sv` byte-diff vs old**

Run: `git diff specgen/generated/sv/ni_signals_pkg.sv`
Inspect: confirms (a) old 4 NoC interfaces gone; (b) `axi4_intf` appears with parameters + modports; (c) `noc_req_intf` + `noc_rsp_intf` appear with multi-hot credit.

- [ ] **Step 3: Update goldens (deliberate)**

```bash
cp specgen/generated/sv/ni_signals_pkg.sv specgen/tests/golden/ni_signals_pkg.sv.golden
cp specgen/generated/sv/ni_params_pkg.sv specgen/tests/golden/ni_params_pkg.sv.golden
cp specgen/generated/cpp/ni_params.h specgen/tests/golden/ni_params.h.golden
# ni_signals.h regen if signal layout downstream changed:
cp specgen/generated/cpp/ni_signals.h specgen/tests/golden/ni_signals.h.golden
```

- [ ] **Step 4: Run all specgen tests**

Run: `py -3 -m pytest specgen/tests/ -v`
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add specgen/generated/ specgen/tests/golden/
git commit -m "chore(specgen): regenerate sv/cpp artifacts + update goldens"
```

---

### Task 10: Migrate `cosim2/sv/nmu_wrap.sv`

**Files:**
- Modify: `cosim2/sv/nmu_wrap.sv`

- [ ] **Step 1: Read current module signature**

Run: `head -40 cosim2/sv/nmu_wrap.sv`
Note current parameters (e.g., `ID_WIDTH`, etc.) and port list.

- [ ] **Step 2: Rewrite module header**

Replace the module header (top portion of file before `always_ff` blocks) with:

```systemverilog
module nmu_wrap #(
    parameter int unsigned ID_WIDTH               = ni_params_pkg::NI_AXI_ID_WIDTH_DFLT,
    parameter int unsigned ADDR_WIDTH             = ni_params_pkg::NI_AXI_ADDR_WIDTH_DFLT,
    parameter int unsigned DATA_WIDTH             = ni_params_pkg::NI_AXI_DATA_WIDTH_DFLT,
    parameter int unsigned NUM_VC                 = ni_params_pkg::NI_NOC_NUM_VC_DFLT,
    parameter int unsigned FLIT_WIDTH             = ni_params_pkg::NI_NOC_FLIT_WIDTH_DFLT,
    parameter int unsigned SLAVE_VC_BUFFER_DEPTH  = ni_params_pkg::NI_NOC_SLAVE_VC_BUFFER_DEPTH_DFLT
) (
    input  logic                   clk_i,
    input  logic                   rst_ni,
    axi4_intf.slave                axi_i,
    noc_req_intf.master            noc_req_o,
    noc_rsp_intf.slave             noc_rsp_i
);
```

Inside the module body, any reference to `axi_intf.<signal>` becomes `axi_i.<signal>` (the same name; only the binding identifier changes). Any reference to `noc_req_out.<signal>` becomes `noc_req_o.<signal>`; rename `noc_req_credit` → `credit_return`, `noc_req_valid` → `valid`, `noc_req_flit` → `flit` (per consolidation mapping in spec §5.3).

- [ ] **Step 3: Verilator-elaborate just this module (best-effort)**

(Optional sanity check; final elaboration happens in Task 14.)

- [ ] **Step 4: Commit (intermediate; may not elaborate alone)**

```bash
git add cosim2/sv/nmu_wrap.sv
git commit -m "refactor(cosim2): migrate nmu_wrap to specgen interfaces"
```

---

### Task 11: Migrate `cosim2/sv/nsu_wrap.sv`

**Files:**
- Modify: `cosim2/sv/nsu_wrap.sv`

- [ ] **Step 1: Replace module header**

```systemverilog
module nsu_wrap #(
    parameter int unsigned ID_WIDTH               = ni_params_pkg::NI_AXI_ID_WIDTH_DFLT,
    parameter int unsigned ADDR_WIDTH             = ni_params_pkg::NI_AXI_ADDR_WIDTH_DFLT,
    parameter int unsigned DATA_WIDTH             = ni_params_pkg::NI_AXI_DATA_WIDTH_DFLT,
    parameter int unsigned NUM_VC                 = ni_params_pkg::NI_NOC_NUM_VC_DFLT,
    parameter int unsigned FLIT_WIDTH             = ni_params_pkg::NI_NOC_FLIT_WIDTH_DFLT,
    parameter int unsigned SLAVE_VC_BUFFER_DEPTH  = ni_params_pkg::NI_NOC_SLAVE_VC_BUFFER_DEPTH_DFLT
) (
    input  logic                   clk_i,
    input  logic                   rst_ni,
    noc_req_intf.slave             noc_req_i,
    noc_rsp_intf.master            noc_rsp_o,
    axi4_intf.master               axi_o
);
```

Body changes: rename signal references symmetric to Task 10 (signal flow reversed; `axi_o.<sig>` for AXI master side, `noc_req_i.<sig>` / `noc_rsp_o.<sig>` for NoC sides).

- [ ] **Step 2: Commit**

```bash
git add cosim2/sv/nsu_wrap.sv
git commit -m "refactor(cosim2): migrate nsu_wrap to specgen interfaces"
```

---

### Task 12: Migrate `axi_master_wrap.sv` + `axi_slave_wrap.sv`

**Files:**
- Modify: `cosim2/sv/axi_master_wrap.sv`
- Modify: `cosim2/sv/axi_slave_wrap.sv`

- [ ] **Step 1: Replace `axi_master_wrap.sv` header**

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

Body: rename interface-bound references to `axi_o.<sig>`.

- [ ] **Step 2: Replace `axi_slave_wrap.sv` header**

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

Body: rename interface-bound references to `axi_i.<sig>`.

- [ ] **Step 3: Commit**

```bash
git add cosim2/sv/axi_master_wrap.sv cosim2/sv/axi_slave_wrap.sv
git commit -m "refactor(cosim2): migrate axi_master_wrap + axi_slave_wrap to axi4_intf"
```

---

### Task 13: Migrate `loopback_noc_wrap.sv`

**Files:**
- Modify: `cosim2/sv/loopback_noc_wrap.sv`

- [ ] **Step 1: Replace module header**

```systemverilog
module loopback_noc_wrap #(
    parameter int unsigned NUM_VC                = ni_params_pkg::NI_NOC_NUM_VC_DFLT,
    parameter int unsigned FLIT_WIDTH            = ni_params_pkg::NI_NOC_FLIT_WIDTH_DFLT,
    parameter int unsigned SLAVE_VC_BUFFER_DEPTH = ni_params_pkg::NI_NOC_SLAVE_VC_BUFFER_DEPTH_DFLT
) (
    input  logic              clk_i,
    input  logic              rst_ni,
    noc_req_intf.slave        noc_req_i,
    noc_rsp_intf.master       noc_rsp_o
);
```

Body: rename `noc_*_valid` → `valid`, `noc_*_flit` → `flit`, `noc_*_credit` → `credit_return`. Loopback wires `noc_req_i.flit → noc_rsp_o.flit`-equivalent path.

- [ ] **Step 2: Commit**

```bash
git add cosim2/sv/loopback_noc_wrap.sv
git commit -m "refactor(cosim2): migrate loopback_noc_wrap to specgen interfaces"
```

---

### Task 14: Migrate `tb_top.sv` + delete hand-written interfaces

**Files:**
- Modify: `cosim2/sv/tb_top.sv`
- Modify: `cosim2/CMakeLists.txt`
- Delete: `cosim2/sv/axi_intf.sv`, `cosim2/sv/noc_req_intf.sv`, `cosim2/sv/noc_rsp_intf.sv`

- [ ] **Step 1: Rewrite `tb_top.sv` to use specgen interfaces**

In `tb_top.sv`, replace the existing instantiation of the hand-written intf with:

```systemverilog
module tb_top;
    logic clk;
    logic rst_n;

    // Clock + reset gen as before...

    axi4_intf      #() axi_at_cpu();         // master side from cpu agent
    axi4_intf      #() axi_at_mem();         // slave side at memory
    noc_req_intf   #() noc_req();
    noc_rsp_intf   #() noc_rsp();

    axi_master_wrap   u_cpu_axi   (.clk_i(clk), .rst_ni(rst_n), .axi_o(axi_at_cpu));
    nmu_wrap          u_nmu       (.clk_i(clk), .rst_ni(rst_n),
                                    .axi_i(axi_at_cpu),
                                    .noc_req_o(noc_req),
                                    .noc_rsp_i(noc_rsp));
    loopback_noc_wrap u_noc       (.clk_i(clk), .rst_ni(rst_n),
                                    .noc_req_i(noc_req),
                                    .noc_rsp_o(noc_rsp));
    nsu_wrap          u_nsu       (.clk_i(clk), .rst_ni(rst_n),
                                    .noc_req_i(noc_req),
                                    .noc_rsp_o(noc_rsp),
                                    .axi_o(axi_at_mem));
    axi_slave_wrap    u_mem       (.clk_i(clk), .rst_ni(rst_n), .axi_i(axi_at_mem));

    // Existing test orchestration logic stays.
endmodule
```

(Wire up the actual existing tb logic — clock/reset gen, DPI imports, scoreboard hooks — under the new module names.)

- [ ] **Step 2: Update CMakeLists.txt**

In `cosim2/CMakeLists.txt`, find the verilator source list. Remove the three deleted files and ensure `ni_params_pkg.sv` precedes `ni_signals_pkg.sv` in the source order:

```cmake
set(VERILATE_SOURCES
    ${CMAKE_SOURCE_DIR}/specgen/generated/sv/ni_params_pkg.sv     # NEW
    ${CMAKE_SOURCE_DIR}/specgen/generated/sv/ni_signals_pkg.sv
    ${CMAKE_SOURCE_DIR}/specgen/generated/sv/ni_flit_pkg.sv
    ${CMAKE_SOURCE_DIR}/specgen/generated/sv/ni_regs_pkg.sv
    # DELETED: ${CMAKE_SOURCE_DIR}/cosim2/sv/axi_intf.sv
    # DELETED: ${CMAKE_SOURCE_DIR}/cosim2/sv/noc_req_intf.sv
    # DELETED: ${CMAKE_SOURCE_DIR}/cosim2/sv/noc_rsp_intf.sv
    ${CMAKE_SOURCE_DIR}/cosim2/sv/wb2axip/faxi_slave.v
    ${CMAKE_SOURCE_DIR}/cosim2/sv/wb2axip/faxi_master.v
    ${CMAKE_SOURCE_DIR}/cosim2/sv/nmu_wrap.sv
    ${CMAKE_SOURCE_DIR}/cosim2/sv/nsu_wrap.sv
    ${CMAKE_SOURCE_DIR}/cosim2/sv/axi_master_wrap.sv
    ${CMAKE_SOURCE_DIR}/cosim2/sv/axi_slave_wrap.sv
    ${CMAKE_SOURCE_DIR}/cosim2/sv/loopback_noc_wrap.sv
    ${CMAKE_SOURCE_DIR}/cosim2/sv/tb_top.sv
)
```

- [ ] **Step 3: Delete the three hand-written interface files**

```bash
git rm cosim2/sv/axi_intf.sv cosim2/sv/noc_req_intf.sv cosim2/sv/noc_rsp_intf.sv
```

- [ ] **Step 4: Commit**

```bash
git add cosim2/sv/tb_top.sv cosim2/CMakeLists.txt
git commit -m "refactor(cosim2): migrate tb_top + remove hand-written interfaces"
```

---

### Task 15: W2 final CI gate

**Files:** none (verification only)

- [ ] **Step 1: Full ctest + drift gate**

Run from MSYS2 mingw64 shell:
```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cd /e/05_NoC/noc_project
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Expected: 410/410 PASS (matches pre-W2 baseline).

- [ ] **Step 2: Drift gate**

```bash
py -3 specgen/tools/check_drift.py  # or whatever the existing drift-check command is
```

Expected: empty diff (regenerated files match committed).

- [ ] **Step 3: Verilator warning-clean elaboration**

```bash
make -C cosim2/verilator clean
VERILATOR_FLAGS="--Wall --Wpedantic --assert" make -C cosim2/verilator
```

Expected: 0 warning, 0 error.

- [ ] **Step 4: Push branch + open PR**

```bash
git push -u origin stage5b/dpi-wire-wrap
gh pr create --title "refactor(specgen+cosim2): industry-style SV interfaces + atomic migration" \
  --body "$(cat <<'EOF'
## Summary
- Single axi4_intf with master/slave modports (collapses prior 2 separate interfaces)
- Consolidates 4 NoC interfaces -> noc_req_intf + noc_rsp_intf
- New constants.yaml as language-neutral source of truth for SV+C++ parameter defaults
- All 5 wraps + tb_top migrated to specgen interfaces
- Hand-written cosim2/sv/{axi,noc_req,noc_rsp}_intf.sv removed

## Test plan
- [x] specgen pytest 全綠
- [x] cosim2 ctest 410/410
- [x] drift gate clean
- [x] Verilator warning-clean elaboration

Refs: docs/superpowers/specs/2026-06-06-specgen-handshake-rtl-style-upstream-design.md
EOF
)"
```

---

# Phase W3 — Release-Level Quality Sweep

Three parallel zones. Each zone gets one Claude subagent + one Codex review. Findings de-duplicated, user reviewed, then applied.

PR title pattern: `chore(quality-zone-<X>-{arch|magic}): apply karpathy findings`

---

### Task 16: Zone A sweep — Stage 3 c_model core

**Files:** review only (no writes); produces `cosim2/quality/zone_A_findings.json`.

- [ ] **Step 1: Dispatch Claude subagent**

Use Agent tool with `subagent_type: general-purpose`:
```
prompt: |
  Karpathy-guidelines + magic-number sweep on Stage 3 c_model core.
  Files: c_model/include/{axi,nmu,nsu,common,noc}/** + c_model/tests/{axi,nmu,nsu}/**
  Lenses (axis 1): overcomplication, surgical, surface assumptions, verifiable success
  Magic numbers (axis 2): width literal, timing constant, buffer size, mask/shift
  Magic-number scope rule: only fix literals implicated by *this* refactor; other findings go to deferred list.
  Output: JSON to cosim2/quality/zone_A_subagent_findings.json
  Format: [{severity:CRITICAL|HIGH|MEDIUM|LOW, category, file, line, description, suggested_fix}]
```

- [ ] **Step 2: Dispatch Codex in parallel**

Use `codex:rescue` skill with the same scope. Save its output to `cosim2/quality/zone_A_codex_findings.json`.

- [ ] **Step 3: De-dup + merge**

```bash
py -3 cosim2/scripts/merge_findings.py \
  cosim2/quality/zone_A_subagent_findings.json \
  cosim2/quality/zone_A_codex_findings.json \
  > cosim2/quality/zone_A_findings.json
```

(Write `merge_findings.py` if not present — dedup by file:line+category.)

- [ ] **Step 4: User review + decision matrix**

Present findings to user. User marks each: `fix` / `defer` / `ignore`. Output: `cosim2/quality/zone_A_decisions.json`.

- [ ] **Step 5: Commit findings + decisions**

```bash
git add cosim2/quality/zone_A_*
git commit -m "chore(quality-zone-A): record findings + user decisions"
```

---

### Task 17: Zone B sweep — Stage 5b cosim

Same process as Task 16, applied to `c_model/include/cosim2/** + cosim2/{c,sv,verilator,tests}/**` (post-W2 state). Output: `cosim2/quality/zone_B_findings.json` + decisions.

- [ ] **Step 1-5: Repeat Task 16 with zone B file set**

---

### Task 18: Zone C sweep — specgen

Same process applied to `specgen/**` (post-W2 state). Output: `cosim2/quality/zone_C_findings.json` + decisions.

- [ ] **Step 1-5: Repeat Task 16 with zone C file set**

---

### Task 19: Apply architectural fixes (per zone)

**Files:** per `zone_<X>_decisions.json[*].file`

For each zone with `fix`-marked architectural findings, write the fix per `suggested_fix`. Commit per-zone:

- [ ] **Step 1: Apply Zone A architectural fixes**

For each finding marked `fix` and `category != magic_number`:
- Read the file
- Apply `suggested_fix` (or refine if needed)
- Run the affected test set

```bash
git add c_model/include/<...>
git commit -m "chore(quality-zone-A-arch): apply architectural findings"
```

- [ ] **Step 2: Same for Zone B, C**

Three commits total (one per zone).

---

### Task 20: Apply magic-number fixes (per zone)

Same pattern as Task 19, but for `category == magic_number`. Each magic-number fix replaces literal with a named `constexpr` (C++) or `parameter` / `localparam` (SV).

- [ ] **Step 1-3: Per-zone commits**

---

### Task 21: Run release gate 3 — lint clean

**Files:** none (verification).

- [ ] **Step 1: clang-tidy over c_model**

```bash
cd build && py -3 run-clang-tidy.py -p . -header-filter='c_model/include/.*' \
  c_model/include c_model/src 2>&1 | tee ../cosim2/quality/clang_tidy.log
```

Expected: 0 warning.

- [ ] **Step 2: verible-verilog-lint over SV**

```bash
verible-verilog-lint cosim2/sv/*.sv specgen/generated/sv/*.sv \
  > cosim2/quality/verible.log
```

Expected: 0 warning.

- [ ] **Step 3: Commit logs**

```bash
git add cosim2/quality/clang_tidy.log cosim2/quality/verible.log
git commit -m "chore(quality): release gate 3 lint clean"
```

---

### Task 22: Run release gate 4 — Verilator warning-clean

- [ ] **Step 1: Build + elaborate with strict warnings**

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
make -C cosim2/verilator clean
VERILATOR_FLAGS="--Wall --Wpedantic --assert -Wno-fatal" \
  make -C cosim2/verilator 2>&1 | tee cosim2/quality/verilator.log
```

Expected: 0 warning + 0 error.

- [ ] **Step 2: ctest 410/410 with same build**

```bash
ctest --test-dir build --output-on-failure
```

Expected: 410/410 PASS.

- [ ] **Step 3: Commit log**

```bash
git add cosim2/quality/verilator.log
git commit -m "chore(quality): release gate 4 Verilator warning-clean"
```

---

### Task 23: Run release gate 5 — parameter sweep test

**Files:**
- Create: `specgen/tests/test_parameter_sweep.py`
- Create: `cosim2/cmake/parameter_sweep.cmake`

- [ ] **Step 1: Write the sweep test (Python orchestration)**

```python
# specgen/tests/test_parameter_sweep.py
"""Parameter sweep: confirm specgen + Verilator elaborate green for the matrix."""
import itertools
import subprocess
from pathlib import Path
import pytest

ID_WIDTHS    = [1, 8, 16]
ADDR_WIDTHS  = [32, 48, 64]
DATA_WIDTHS  = [32, 64, 256, 512]
NUM_VC_VALS  = [1, 4, 8]
FLIT_WIDTHS  = [64, 256, 408, 1024]


@pytest.mark.parametrize("id_w, addr_w, data_w, num_vc, flit_w",
    list(itertools.product(ID_WIDTHS, ADDR_WIDTHS, DATA_WIDTHS, NUM_VC_VALS, FLIT_WIDTHS)))
def test_param_combo_elaborates(tmp_path, id_w, addr_w, data_w, num_vc, flit_w):
    # Run a tiny harness with --parameter overrides
    cmd = [
        "verilator", "--lint-only", "--Wall", "--assert",
        f"-GID_WIDTH={id_w}", f"-GADDR_WIDTH={addr_w}", f"-GDATA_WIDTH={data_w}",
        f"-GNUM_VC={num_vc}", f"-GFLIT_WIDTH={flit_w}",
        "specgen/generated/sv/ni_params_pkg.sv",
        "specgen/generated/sv/ni_signals_pkg.sv",
        "cosim2/tests/sv/elab_harness.sv",
    ]
    r = subprocess.run(cmd, capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
```

Create `cosim2/tests/sv/elab_harness.sv` — a tiny module that instantiates `axi4_intf` and `noc_req_intf` with the override parameters, just for elaboration.

- [ ] **Step 2: Negative parameter test (gate 6)**

```python
@pytest.mark.parametrize("bad_data_w, bad_num_vc, bad_id_w", [
    (33, 1, 8),   # DATA_WIDTH not multiple of 8
    (32, 0, 8),   # NUM_VC < min
    (32, 1, 0),   # ID_WIDTH < min
])
def test_invalid_params_rejected_by_validator(bad_data_w, bad_num_vc, bad_id_w):
    """constants.yaml validator must reject out-of-range defaults."""
    # Synthesize an invalid yaml and confirm load_constants raises
    ...
```

- [ ] **Step 3: Run sweep**

```bash
py -3 -m pytest specgen/tests/test_parameter_sweep.py -v
```

Expected: 3 × 3 × 4 × 3 × 4 = 432 PASS for positive sweep + 3 PASS for negative.

- [ ] **Step 4: Commit**

```bash
git add specgen/tests/test_parameter_sweep.py cosim2/tests/sv/elab_harness.sv
git commit -m "chore(quality): release gate 5+6 parameter sweep + negative"
```

---

### Task 24: Run release gate 7 — sanitizer clean

- [ ] **Step 1: Rebuild with UBSan + ASan**

```bash
cmake -S . -B build_san -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
cmake --build build_san -j
```

- [ ] **Step 2: Run ctest under sanitizer**

```bash
ctest --test-dir build_san --output-on-failure 2>&1 | tee cosim2/quality/sanitizer.log
```

Expected: 410/410 PASS, no AddressSanitizer / UndefinedBehaviorSanitizer messages.

- [ ] **Step 3: Commit log**

```bash
git add cosim2/quality/sanitizer.log
git commit -m "chore(quality): release gate 7 sanitizer clean"
```

---

### Task 25: Run release gate 8 — coverage threshold

- [ ] **Step 1: Build with coverage**

```bash
make -C cosim2/verilator clean
VERILATOR_FLAGS="--coverage-line --coverage-toggle --assert" make -C cosim2/verilator
ctest --test-dir build --output-on-failure
```

- [ ] **Step 2: Aggregate + report**

```bash
verilator_coverage --annotate cosim2/quality/coverage cosim2/verilator/obj_dir/coverage.dat
lcov --capture --directory build --output-file cosim2/quality/coverage.info \
  --exclude '*/wb2axip/*' --exclude '*/specgen/generated/*'
genhtml cosim2/quality/coverage.info --output-directory cosim2/quality/coverage_html
```

- [ ] **Step 3: Check thresholds**

Line ≥ 80%, Toggle ≥ 70%. If below: write findings to `deferred_findings.json` with rationale; block release if user does not accept defer.

- [ ] **Step 4: Per-property assertion coverage**

For each `SLAVE_ASSERT` in `cosim2/sv/wb2axip/faxi_slave.v`, confirm it fired in at least 1 positive + 1 negative scenario. Cross-reference with the existing fault-injection set; add scenarios if any property uncovered.

- [ ] **Step 5: Commit**

```bash
git add cosim2/quality/coverage*
git commit -m "chore(quality): release gate 8 coverage threshold"
```

---

### Task 26: Run release gate 9 — reproducible generation

**Files:**
- Create: `cosim2/scripts/check_reproducible_gen.sh`

- [ ] **Step 1: Write the script**

```bash
#!/usr/bin/env bash
# cosim2/scripts/check_reproducible_gen.sh
# Confirm specgen produces byte-identical artifacts from two clean trees.
set -euo pipefail

WORKDIR=$(mktemp -d)
git worktree add "$WORKDIR/tree_a" HEAD
git worktree add "$WORKDIR/tree_b" HEAD

(cd "$WORKDIR/tree_a" && git clean -fdx specgen/generated/ && py -3 -m specgen)
(cd "$WORKDIR/tree_b" && git clean -fdx specgen/generated/ && py -3 -m specgen)

diff -r "$WORKDIR/tree_a/specgen/generated/" "$WORKDIR/tree_b/specgen/generated/"

echo "Reproducible: byte-identical."

git worktree remove --force "$WORKDIR/tree_a"
git worktree remove --force "$WORKDIR/tree_b"
rm -rf "$WORKDIR"
```

- [ ] **Step 2: Run it**

```bash
chmod +x cosim2/scripts/check_reproducible_gen.sh
./cosim2/scripts/check_reproducible_gen.sh
```

Expected: "Reproducible: byte-identical."

- [ ] **Step 3: Commit**

```bash
git add cosim2/scripts/check_reproducible_gen.sh
git commit -m "chore(quality): release gate 9 reproducible generation"
```

---

### Task 27: Run release gate 10 — fault injection sanity

- [ ] **Step 1: Run negative scenario**

Use the existing fault-injection harness from Stage 5a (commit `391f13d`). Inject `WLAST` not on last beat.

```bash
ctest --test-dir build --output-on-failure -R fault_inject_wlast
```

Expected: test FAILS with wb2axip `$error("AXI4 spec violation ...")`.

- [ ] **Step 2: Run same scenario without injection**

Disable injection flag; rerun:

Expected: test PASSES.

- [ ] **Step 3: Commit log**

```bash
echo "Fault injection: negative fired \$error, positive passed." > cosim2/quality/fault_inject.log
git add cosim2/quality/fault_inject.log
git commit -m "chore(quality): release gate 10 fault injection sanity"
```

---

### Task 28: Run release gate 11 — C++ byte-identical

- [ ] **Step 1: Write check script**

```bash
#!/usr/bin/env bash
# cosim2/scripts/check_byte_identical_cpp.sh
set -euo pipefail
TMP=$(mktemp -d)
cp specgen/generated/cpp/*.h "$TMP/"
py -3 -m specgen
diff -r "$TMP/" specgen/generated/cpp/
echo "C++ byte-identical: OK"
rm -rf "$TMP"
```

- [ ] **Step 2: Run + commit**

```bash
chmod +x cosim2/scripts/check_byte_identical_cpp.sh
./cosim2/scripts/check_byte_identical_cpp.sh
git add cosim2/scripts/check_byte_identical_cpp.sh
git commit -m "chore(quality): release gate 11 C++ byte-identical"
```

---

### Task 29: Run release gate 12 — release tag `v0.5.0`

- [ ] **Step 1: Confirm all prior gates green**

Run: `cat cosim2/quality/*.log` — visually confirm zero warnings/errors.

- [ ] **Step 2: Tag**

```bash
git tag -a v0.5.0 -m "Stage 5b release: specgen handshake upstream + rtl-style refactor

Release-level gates passed:
- ctest 410/410
- drift gate clean
- Verilator warning-clean elaboration
- clang-tidy + verible-verilog-lint clean
- Parameter sweep 432 combos green
- UBSan + ASan ctest 410/410 clean
- Coverage: line ≥ 80%, toggle ≥ 70%, assertion 100%
- Reproducible generation byte-identical
- Fault injection: negative \$error fires, positive passes
- C++ byte-identical across regen
"
git push origin v0.5.0
```

---

## Self-Review

**Spec coverage:**
- §1.2 W1 schema → Tasks 1-3 ✓
- §1.2 W2 atomic → Tasks 4-15 ✓
- §1.2 W3 sweep → Tasks 16-29 ✓
- §2.1 axi4_intf single → Task 6 ✓
- §2.1 NoC 4→2 consolidation → Task 7 ✓
- §2.2 naming discipline enforcement → Task 2 (validator rejects `*_W$`) ✓
- §4.1 constants.yaml → Task 1 ✓
- §4.3 interface_handshake.json with NoC protocol_semantics → Task 3 ✓
- §5.2 SV emission examples → Tasks 4-7 ✓
- §5.3 wrap migration → Tasks 10-13 ✓
- §6.5 12 release gates → Tasks 21-29 (gates 3-12); gates 1-2 are findings-acked from Tasks 16-20 ✓

**Placeholder scan:** plan contains no TBD/TODO. Steps with `...` denote "adapt to actual file content" (e.g., when rewriting existing modules — the rewriter must read the file first). These are necessary because the existing file contents are too large to inline.

**Type consistency:**
- `emit_params_pkg(constants, spec_version)` / `emit_params_h(constants, spec_version)` / `emit_sv_interface_blocks(interfaces_doc, constants)` — signatures consistent across Tasks 4, 5, 6, 7, 8.
- Parameter names `ID_WIDTH`, `ADDR_WIDTH`, `DATA_WIDTH`, `NUM_VC`, `FLIT_WIDTH`, `SLAVE_VC_BUFFER_DEPTH` — used uniformly in Tasks 1, 6, 7, 10-13.
- Port-suffix convention `_i / _o / _ni` ↔ modport `slave / master` — applied consistently in Tasks 10-13.

---

## Notes for Executor

- The spec previously referenced `slave_wrap.sv` and `noc_wrap.sv`; **actual filenames are `axi_slave_wrap.sv` and `loopback_noc_wrap.sv`** — this plan uses the actual names.
- SV emitter lives at `specgen/tools/elaborate/sv_signals.py`, not directly in `specgen/ni_spec/generator.py`. The `generator.py` is a markdown→JSON parser.
- W2 sub-commits 4-14 are intentionally not all elaboration-clean mid-PR. Only Task 15's final gate runs the full ctest.
- W3 sweeps (Tasks 16-18) dispatch subagent + Codex in parallel. Use a single message with two tool-uses when dispatching.
- Coverage gate (Task 25) excludes `cosim2/sv/wb2axip/**` and `specgen/generated/**` from line/toggle thresholds.
