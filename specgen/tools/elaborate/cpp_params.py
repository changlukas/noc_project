"""C++ emitter for parameter defaults header (ni_params.h).

Consumes specgen/source/constants.yaml directly. Returns body only --
codegen.py prepends the provenance banner.

Output style follows project C++ convention (ni_flit_constants.h): namespace
ni + bare UPPER_SNAKE constexpr int symbols.
"""
from __future__ import annotations
from pathlib import Path
import sys

SPECGEN_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(SPECGEN_ROOT))

from ni_spec.handshake_schema import load_constants


def emit(src_path: Path, spec_version: str) -> str:
    constants = load_constants(src_path)
    lines: list[str] = []
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <cstddef>")
    lines.append("")
    lines.append("namespace ni {")
    lines.append("")

    # Resolve plain values for derived expression evaluation.
    plain_values: dict[str, int] = {}
    for domain in ("axi", "noc"):
        for n, s in constants.get(domain, {}).items():
            plain_values[n] = s["default"]

    def _emit_group(items: list[tuple[str, dict]], label: str, value_for) -> None:
        if not items:
            return
        lines.append(f"// {label}")
        name_col = max(len(s["cpp_symbol"]) for _, s in items)
        for n, s in items:
            sym = s["cpp_symbol"]
            val = value_for(n, s)
            lines.append(f"constexpr int {sym:<{name_col}} = {val};")
        lines.append("")

    _emit_group(
        list(constants.get("axi", {}).items()),
        "AXI parameter defaults",
        lambda _n, s: s["default"],
    )
    _emit_group(
        list(constants.get("noc", {}).items()),
        "NoC parameter defaults",
        lambda _n, s: s["default"],
    )
    _emit_group(
        list(constants.get("derived", {}).items()),
        "Derived parameter defaults",
        lambda _n, s: int(eval(s["expression"], {"__builtins__": {}}, plain_values)),
    )

    lines.append("}  // namespace ni")
    return "\n".join(lines) + "\n"
