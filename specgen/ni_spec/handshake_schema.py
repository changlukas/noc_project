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

try:
    import yaml
except ImportError:
    yaml = None


class HandshakeSchemaError(ValueError):
    """Raised when validation fails."""


_TOP_LEVEL_KEYS = {"schema_version", "axi", "noc", "derived"}
# Plain (axi/noc) and derived require different field sets:
#   plain   -> {type, default, sv_symbol, cpp_symbol}
#   derived -> {type, expression, sv_symbol, cpp_symbol}
_BASE_REQUIRED = {"type", "sv_symbol", "cpp_symbol"}
_PLAIN_REQUIRED = _BASE_REQUIRED | {"default"}
_DERIVED_REQUIRED = _BASE_REQUIRED | {"expression"}

# Per-kind allowed fields = required + kind-specific optional set.
_PLAIN_OPTIONAL = {"units", "description", "min", "max", "allowed"}
_DERIVED_OPTIONAL = {"units", "description", "constraint"}

_PLAIN_ALLOWED   = _PLAIN_REQUIRED | _PLAIN_OPTIONAL
_DERIVED_ALLOWED = _DERIVED_REQUIRED | _DERIVED_OPTIONAL
_PARAM_NAME_UPPER = re.compile(r"^[A-Z][A-Z0-9_]*$")
_FORBIDDEN_W_END = re.compile(r"_W$")
_SUPPORTED_TYPES = {"int"}
_INTERFACE_KINDS = {"axi4", "noc_link"}


# -------- constants.yaml --------

def load_constants(path: Path) -> Dict[str, Any]:
    if yaml is None:
        raise RuntimeError(
            f"PyYAML is required to load {path} (constants.yaml). "
            "Install with: pip install pyyaml"
        )
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
    required = _DERIVED_REQUIRED if is_derived else _PLAIN_REQUIRED
    allowed_fields = _DERIVED_ALLOWED if is_derived else _PLAIN_ALLOWED
    missing = required - set(spec)
    if missing:
        raise HandshakeSchemaError(f"{where}: missing required field(s): {sorted(missing)}")
    unknown = set(spec) - allowed_fields
    if unknown:
        raise HandshakeSchemaError(f"{where}: unknown field(s): {sorted(unknown)}")
    if spec["type"] not in _SUPPORTED_TYPES:
        raise HandshakeSchemaError(
            f"{where}: unknown type {spec['type']!r}; supported: {sorted(_SUPPORTED_TYPES)}"
        )
    if spec["type"] == "int":
        # Reject YAML bools (subclass of int) and any non-int default/min/max/allowed
        def _is_strict_int(v):
            return isinstance(v, int) and not isinstance(v, bool)

        if "default" in spec and not _is_strict_int(spec["default"]):
            raise HandshakeSchemaError(
                f"{where}: default {spec['default']!r} is not int (type was declared 'int')"
            )
        for key in ("min", "max"):
            if key in spec and not _is_strict_int(spec[key]):
                raise HandshakeSchemaError(
                    f"{where}: {key} {spec[key]!r} is not int (type was declared 'int')"
                )
        if "allowed" in spec:
            for elt in spec["allowed"]:
                if not _is_strict_int(elt):
                    raise HandshakeSchemaError(
                        f"{where}: allowed entry {elt!r} is not int (type was declared 'int')"
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
    # Allow ==, !=, <, >, <=, >= for constraint expressions
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
