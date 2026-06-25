#!/usr/bin/env python
"""Unified codegen entry point.

Usage:
    py -3 tools/codegen.py --target cpp --domain packet --out generated/cpp/
    py -3 tools/codegen.py --target cpp --domain signals --out generated/cpp/
    py -3 tools/codegen.py --target sv --domain packet --out generated/sv/
    py -3 tools/codegen.py --target sv --domain signals --out generated/sv/
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

# Ensure specgen/ is on the import path.
SPECGEN_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SPECGEN_ROOT))
# Ensure tools/ sub-packages are importable as "tools.elaborate.*".
TOOLS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_DIR.parent))

from ni_spec.loader import load_spec_version, load_spec_bundle
from ni_spec.invariants import check_all
from tools.elaborate import common
from tools.elaborate import cpp_packet, cpp_signals, cpp_params
from tools.elaborate import sv_packet, sv_signals, sv_params


# Maps (target, domain) -> (emitter_func, output_filename, source_json_rel)
# source_json_rel is relative to SPECGEN_ROOT/.
DOMAIN_TO_EMITTER: dict[tuple[str, str], tuple] = {
    ("cpp", "packet"):    (cpp_packet.emit,    "ni_flit_constants.h", "generated/json/ni_packet.json"),
    ("cpp", "signals"):   (cpp_signals.emit,   "ni_signals.h",        "generated/json/ni_signals.json"),
    ("cpp", "params"):    (cpp_params.emit,    "ni_params.h",         "source/constants.yaml"),
    ("sv",  "packet"):    (sv_packet.emit,    "ni_flit_pkg.sv",       "generated/json/ni_packet.json"),
    ("sv",  "signals"):   (sv_signals.emit,   "ni_signals_pkg.sv",    "generated/json/ni_signals.json"),
    ("sv",  "params"):    (sv_params.emit,    "ni_params_pkg.sv",     "source/constants.yaml"),
}

# Per-config domains handled separately (require extra args; not in --check sweep).
_NOC_TYPES_VALID_VC = (1, 2, 4, 8)

# Default output directories per target.
_DEFAULT_OUT: dict[str, str] = {
    "cpp": "generated/cpp",
    "sv":  "generated/sv",
}

# Committed snapshot directories per target (for --check).
_COMMITTED_DIR: dict[str, Path] = {
    "cpp": SPECGEN_ROOT / "generated" / "cpp",
    "sv":  SPECGEN_ROOT / "generated" / "sv",
}


def _resolve_source(src_rel: str) -> Path:
    """Resolve a source JSON path relative to SPECGEN_ROOT/."""
    return SPECGEN_ROOT / src_rel


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


def _strip_provenance(lines: list[str]) -> list[str]:
    """Strip provenance metadata that varies independently of elaborated content.

    Removes:
      - ``// Generated at: <UTC>``  (re-runs change the timestamp)
      - ``// Source SHA: <hash>``   (JSON content evolves as the spec schema
                                     is normalised; the elaborated body stays
                                     the same)

    Aligns this gate with ``specgen/tests/test_byte_identical_golden.py``
    which strips both lines. Without alignment, a JSON-schema renormalisation
    that leaves the elaborated body untouched would turn this gate red while
    the byte-identical golden test stayed green.
    """
    return [
        l for l in lines
        if not l.startswith("// Generated at:") and not l.startswith("// Source SHA:")
    ]


# Backwards-compatible alias for any external caller still using the old name.
_strip_timestamp = _strip_provenance


def _check_invariants() -> bool:
    """Run Layer 1/2 invariants (check_all) against the committed JSON spec
    + constants.yaml. Returns True if no ERROR Issue, False otherwise.

    The drift diff only catches "regenerated != committed"; it cannot catch a
    config that is internally consistent (no drift) yet semantically illegal,
    e.g. an over-sized mesh that exceeds the flit dst_id/vc_id field capacity.
    check_all binds those cross-source constraints, so wire it into the gate.
    """
    json_dir = SPECGEN_ROOT / "generated" / "json"
    if not (json_dir / "ni_packet.json").exists():
        print(f"[skip] invariants: {json_dir}/ni_packet.json not found", file=sys.stderr)
        return True
    bundle = load_spec_bundle(json_dir)
    issues = check_all(bundle)
    errors = [i for i in issues if i.severity == "ERROR"]
    if errors:
        print("[invariant] ERROR issue(s) found:", file=sys.stderr)
        for i in errors:
            print(f"  [{i.check}] {i.message}", file=sys.stderr)
        return False
    return True


def _run_emit_noc_types(num_vc: int, out_dir: Path) -> Path:
    """Emit noc_types_pkg_vc{N}.sv for the given num_vc. Returns written path."""
    out_name = f"noc_types_pkg_vc{num_vc}.sv"
    spec_version = load_spec_version()
    body = sv_signals.emit_noc_types_pkg(num_vc, spec_version)
    # Use a synthetic source path for the banner (no real JSON; num_vc is the input).
    banner = common.provenance_banner(SPECGEN_ROOT / "source" / "constants.yaml")
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / out_name
    out_path.write_text(banner + body, encoding="ascii", errors="strict")
    return out_path


def cmd_emit(args: argparse.Namespace) -> int:
    if not args.domain:
        print("ERROR: --domain is required with --target", file=sys.stderr)
        return 2

    # noc_types is a per-config domain; handled separately from DOMAIN_TO_EMITTER.
    if args.target == "sv" and args.domain == "noc_types":
        if args.num_vc is None:
            print("ERROR: --num-vc N is required for --domain noc_types", file=sys.stderr)
            return 2
        out_dir = Path(args.out) if args.out else SPECGEN_ROOT / _DEFAULT_OUT[args.target]
        try:
            written = _run_emit_noc_types(args.num_vc, out_dir)
            print(f"wrote {written}", file=sys.stderr)
            return 0
        except Exception as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            return 1

    out_dir = Path(args.out) if args.out else SPECGEN_ROOT / _DEFAULT_OUT[args.target]
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

    Provenance metadata in the banner is excluded from comparison:
    both ``// Generated at:`` and ``// Source SHA:`` lines are stripped
    before diffing (see ``_strip_provenance``). This matches the policy
    of ``specgen/tests/test_byte_identical_golden.py``.
    Also runs check_all invariants (Layer 1/2): a config that is drift-free
    but semantically illegal (e.g. mesh dims exceeding flit field capacity)
    fails here even though no header diff would surface it.
    Exits 0 if all files match and no ERROR invariant, 1 otherwise.
    """
    all_ok = _check_invariants()

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

            fresh_lines   = _strip_provenance(fresh_path.read_text(encoding="ascii").splitlines())
            committed_lines = _strip_provenance(committed_path.read_text(encoding="ascii").splitlines())

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

    return 0 if all_ok else 1


def cmd_lint_sv(_args: argparse.Namespace) -> int:
    """Run verilator lint-only smoke test on generated/sv/*.sv.

    Skips gracefully if verilator is not in PATH.
    """
    verilator = shutil.which("verilator")
    if verilator is None:
        print("[skip] verilator not in PATH -- SV lint smoke test skipped", file=sys.stderr)
        return 0

    sv_dir = SPECGEN_ROOT / "generated" / "sv"
    sv_files = sorted(sv_dir.glob("*.sv"))
    if not sv_files:
        print("[skip] no .sv files found in generated/sv/ -- run --target sv first", file=sys.stderr)
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
        "--domain", choices=["packet", "signals", "params", "noc_types"],
        help="spec domain to emit",
    )
    parser.add_argument(
        "--num-vc", type=int, default=None,
        help="number of virtual channels (required for --domain noc_types)",
    )
    parser.add_argument(
        "--out", default=None,
        help="output directory (default: specgen/generated/cpp/ for cpp, specgen/generated/sv/ for sv)",
    )
    parser.add_argument(
        "--check", action="store_true",
        help="regen to scratch dir and diff vs committed; exit 1 on drift",
    )
    parser.add_argument(
        "--lint-sv", action="store_true",
        help="run verilator --lint-only on generated/sv/*.sv; skips gracefully if verilator not in PATH",
    )
    args = parser.parse_args()

    if args.check:
        return cmd_check(args)
    if args.lint_sv:
        return cmd_lint_sv(args)
    return cmd_emit(args)


if __name__ == "__main__":
    sys.exit(main())
