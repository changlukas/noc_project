#!/usr/bin/env python3
"""Content-address builds so workstation clock skew cannot reuse stale objects."""
import hashlib
from pathlib import Path
import sys
kind = sys.argv[1]
digest = hashlib.sha256(" ".join(sys.argv).encode())
if kind == "sv":
    paths = [Path("files.f"), Path("topology_pkg.sv")]
    paths += [p for root in ("repo", "deps") for p in Path(root).rglob("*") if p.suffix in (".sv", ".svh")]
elif kind == "cpp":
    paths = [p for root in ("repo/ref_model", "repo/specgen/generated/cpp")
             for p in Path(root).rglob("*") if p.suffix in (".h", ".hpp", ".cpp")]
else:
    paths = [p for p in Path("deps/yaml-cpp").rglob("*") if p.is_file()]
for path in sorted(paths):
    digest.update(str(path).encode())
    digest.update(path.read_bytes())
print(digest.hexdigest()[:12])
