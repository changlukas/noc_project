#!/usr/bin/env python
"""Unified codegen entry point.

Usage:
    py -3 tools/codegen.py --target cpp --domain packet --out include/
    py -3 tools/codegen.py --target cpp --domain signals --out include/
    py -3 tools/codegen.py --target cpp --domain registers --out include/
    py -3 tools/codegen.py --target sv --domain packet --out rtl_pkg/
    py -3 tools/codegen.py --target sv --domain signals --out rtl_pkg/
    py -3 tools/codegen.py --target sv --domain registers --out rtl_pkg/
    py -3 tools/codegen.py --check        # regen + diff vs committed; exit 1 on drift
    py -3 tools/codegen.py --lint-sv      # verilator lint smoke test (skips if not in PATH)
"""
from __future__ import annotations
import argparse
import difflib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# Ensure spec_validate/ is on the import path.
SPEC_VALIDATE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SPEC_VALIDATE))
# Ensure tools/ sub-packages are importable as "tools.elaborate.*".
TOOLS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_DIR.parent))

from ni_spec.loader import load_spec_version
from tools.elaborate import common
from tools.elaborate import cpp_packet, cpp_signals, cpp_registers
from tools.elaborate import sv_packet, sv_signals, sv_registers


# Maps (target, domain) -> (emitter_func, output_filename, source_json_rel)
# source_json_rel is relative to SPEC_VALIDATE/.
DOMAIN_TO_EMITTER: dict[tuple[str, str], tuple] = {
    ("cpp", "packet"):    (cpp_packet.emit,    "ni_flit_constants.h", "generated/ni_packet.json"),
    ("cpp", "signals"):   (cpp_signals.emit,   "ni_signals.h",        "generated/ni_signals.json"),
    ("cpp", "registers"): (cpp_registers.emit, "ni_regs.h",           "generated/ni_registers.json"),
    ("sv",  "packet"):    (sv_packet.emit,    "ni_flit_pkg.sv",       "generated/ni_packet.json"),
    ("sv",  "signals"):   (sv_signals.emit,   "ni_signals_pkg.sv",    "generated/ni_signals.json"),
    ("sv",  "registers"): (sv_registers.emit, "ni_regs_pkg.sv",       "generated/ni_registers.json"),
}

# Default output directories per target.
_DEFAULT_OUT: dict[str, str] = {
    "cpp": "include",
    "sv":  "rtl_pkg",
}

# Committed snapshot directories per target (for --check).
_COMMITTED_DIR: dict[str, Path] = {
    "cpp": SPEC_VALIDATE / "include",
    "sv":  SPEC_VALIDATE / "rtl_pkg",
}


def _resolve_source(src_rel: str) -> Path:
    """Resolve a source JSON path relative to SPEC_VALIDATE/."""
    return SPEC_VALIDATE / src_rel


def run_emit(target: str, domain: str, out_dir: Path) -> Path:
    """Run one emitter and write the output file.  Returns the written path."""
    key = (target, domain)
    if key not in DOMAIN_TO_EMITTER:
        raise ValueError(f"Unknown (target, domain): {key}")

    emitter_fn, out_name, src_rel = DOMAIN_TO_EMITTER[key]
    src_path = _resolve_source(src_rel)

    spec_version = load_spec_version()
    body = emitter_fn(src_path, spec_version)
    banner = common.provenance_banner(src_path)

    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / out_name
    out_path.write_text(banner + body, encoding="ascii", errors="strict")
    return out_path


def _strip_timestamp(lines: list[str]) -> list[str]:
    """Remove the '// Generated at:' line so timestamps don't cause false drift."""
    return [l for l in lines if not l.startswith("// Generated at:")]


def _check_cpp_sv_paired(out_dir: Path) -> list[str]:
    """Verify C++ struct field names match SV interface signal names per bundle.

    Field NAMES are compared after stripping ``_i``/``_o`` from the C++ side
    (SV interface signals already have the suffix stripped by sv_signals.py).
    Field TYPES are not compared -- wide signals are ``std::array<uint8_t,N>``
    in C++ but ``logic [W-1:0]`` in SV, and that asymmetry is intentional.
    """
    import re
    errors: list[str] = []
    cpp_text = (out_dir / "include" / "ni_signals.h").read_text(encoding="ascii")
    sv_text  = (out_dir / "rtl_pkg" / "ni_signals_pkg.sv").read_text(encoding="ascii")

    # C++: struct <Name>Pins { ... };   fields are either uint*-scalar or std::array<...> array.
    cpp_bundles: dict[str, set[str]] = {}
    for m in re.finditer(r"struct (\w+Pins)\s*\{([^}]*)\}", cpp_text, re.S):
        bundle = m.group(1)
        fields = re.findall(r"(?:uint\w+|std::array<[^>]+>)\s+(\w+);", m.group(2))
        cpp_bundles[bundle] = set(fields)

    # SV: interface ni_<lc_name>_intf; ... endinterface
    # Re-map the SV interface ID back to PascalCase + "Pins" to align with C++ bundle name.
    sv_bundles: dict[str, set[str]] = {}
    for m in re.finditer(r"interface ni_(\w+)_intf;(.*?)endinterface", sv_text, re.S):
        iface_lc = m.group(1)
        pascal = "".join(p.capitalize() for p in iface_lc.split("_")) + "Pins"
        sigs = re.findall(r"logic(?:\s*\[[^\]]*\])?\s+(\w+);", m.group(2))
        sv_bundles[pascal] = set(sigs)

    # Compare names (strip _i/_o from C++ fields).
    all_bundles = set(cpp_bundles) | set(sv_bundles)
    for bundle in sorted(all_bundles):
        cpp_fields = cpp_bundles.get(bundle, set())
        sv_fields  = sv_bundles.get(bundle, set())
        cpp_stripped = {
            f[:-2] if f.endswith(("_i", "_o")) else f
            for f in cpp_fields
        }
        diff = cpp_stripped.symmetric_difference(sv_fields)
        if diff:
            errors.append(f"{bundle}: C++ <-> SV pin mismatch: {sorted(diff)}")
    return errors


def cmd_emit(args: argparse.Namespace) -> int:
    if not args.domain:
        print("ERROR: --domain is required with --target", file=sys.stderr)
        return 2

    out_dir = Path(args.out) if args.out else SPEC_VALIDATE / _DEFAULT_OUT[args.target]
    try:
        written = run_emit(args.target, args.domain, out_dir)
        print(f"wrote {written}", file=sys.stderr)
        return 0
    except FileNotFoundError as exc:
        print(f"ERROR: source JSON not found: {exc}", file=sys.stderr)
        return 1
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


def cmd_check(_args: argparse.Namespace) -> int:
    """Regen all targets to a temp dir and diff vs committed directories.

    The timestamp line in the banner is excluded from comparison.
    Exits 0 if all files match, 1 if any drift is detected.
    """
    all_ok = True

    with tempfile.TemporaryDirectory() as tmp:
        fresh_base = Path(tmp)
        for (target, domain), (_, out_name, src_rel) in DOMAIN_TO_EMITTER.items():
            src_path = _resolve_source(src_rel)
            if not src_path.exists():
                print(f"[skip] {target}/{domain}: source JSON not found ({src_rel})", file=sys.stderr)
                continue

            fresh_dir = fresh_base / target
            try:
                fresh_path = run_emit(target, domain, fresh_dir)
            except Exception as exc:
                print(f"[error] {target}/{domain}: {exc}", file=sys.stderr)
                all_ok = False
                continue

            committed_dir = _COMMITTED_DIR[target]
            committed_path = committed_dir / fresh_path.name
            if not committed_path.exists():
                print(f"[missing committed] {target}/{out_name}")
                all_ok = False
                continue

            fresh_lines   = _strip_timestamp(fresh_path.read_text(encoding="ascii").splitlines())
            committed_lines = _strip_timestamp(committed_path.read_text(encoding="ascii").splitlines())

            if fresh_lines != committed_lines:
                all_ok = False
                diff = list(difflib.unified_diff(
                    committed_lines,
                    fresh_lines,
                    fromfile=f"committed/{fresh_path.name}",
                    tofile=f"regen/{fresh_path.name}",
                    lineterm="",
                ))
                print(f"[drift] {fresh_path.name}:")
                print("\n".join(diff[:40]))

    # Paired check: C++ pin-bundle struct fields must match SV interface
    # signal names one-to-one. Operates on COMMITTED files -- drift in either
    # the .h or .sv that breaks the pairing is a hard error.
    paired_errors = _check_cpp_sv_paired(SPEC_VALIDATE)
    if paired_errors:
        all_ok = False
        print("[paired-check] C++ ni_signals.h <-> SV ni_signals_pkg.sv mismatch:")
        for err in paired_errors:
            print(f"  {err}")

    return 0 if all_ok else 1


def cmd_lint_sv(_args: argparse.Namespace) -> int:
    """Run verilator lint-only smoke test on rtl_pkg/*.sv.

    Skips gracefully if verilator is not in PATH.
    """
    verilator = shutil.which("verilator")
    if verilator is None:
        print("[skip] verilator not in PATH -- SV lint smoke test skipped", file=sys.stderr)
        return 0

    sv_dir = SPEC_VALIDATE / "rtl_pkg"
    sv_files = sorted(sv_dir.glob("*.sv"))
    if not sv_files:
        print("[skip] no .sv files found in rtl_pkg/ -- run --target sv first", file=sys.stderr)
        return 0

    cmd = [verilator, "--lint-only", "--Wall"] + [str(f) for f in sv_files]
    print(f"[lint-sv] running: {' '.join(cmd)}", file=sys.stderr)
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    if result.returncode == 0:
        print("[lint-sv] PASS", file=sys.stderr)
    else:
        print(f"[lint-sv] FAIL (exit {result.returncode})", file=sys.stderr)
    return result.returncode


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Unified codegen for NI spec (C++ headers and SV packages)."
    )
    parser.add_argument(
        "--target", choices=["cpp", "sv"], default="cpp",
        help="output language target (default: cpp)",
    )
    parser.add_argument(
        "--domain", choices=["packet", "signals", "registers"],
        help="spec domain to emit",
    )
    parser.add_argument(
        "--out", default=None,
        help="output directory (default: spec_validate/include/ for cpp, spec_validate/rtl_pkg/ for sv)",
    )
    parser.add_argument(
        "--check", action="store_true",
        help="regen to scratch dir and diff vs committed; exit 1 on drift",
    )
    parser.add_argument(
        "--lint-sv", action="store_true",
        help="run verilator --lint-only on rtl_pkg/*.sv; skips gracefully if verilator not in PATH",
    )
    args = parser.parse_args()

    if args.check:
        return cmd_check(args)
    if args.lint_sv:
        return cmd_lint_sv(args)
    return cmd_emit(args)


if __name__ == "__main__":
    sys.exit(main())
