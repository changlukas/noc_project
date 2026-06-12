"""Drift gate for the feature inventory MD."""
from __future__ import annotations
import json
import subprocess
import sys
from pathlib import Path

SPECGEN_ROOT  = Path(__file__).resolve().parent.parent
PROJECT_ROOT  = SPECGEN_ROOT.parent
INVENTORY_MD  = PROJECT_ROOT / "c_model" / "FEATURE_INVENTORY.md"
GENERATOR     = SPECGEN_ROOT / "tools" / "gen_inventory.py"


def test_inventory_md_exists():
    assert INVENTORY_MD.exists(), \
        f"committed inventory missing at {INVENTORY_MD}; run gen_inventory.py"


def test_inventory_md_up_to_date():
    r = subprocess.run(
        [sys.executable, str(GENERATOR), "--check"],
        capture_output=True, text=True,
    )
    assert r.returncode == 0, \
        f"FEATURE_INVENTORY.md drift detected.\nstdout: {r.stdout}\nstderr: {r.stderr}"


def test_inventory_covers_all_features_in_json():
    spec = json.loads((SPECGEN_ROOT / "source" / "noc_function_blocks.json").read_text(encoding="utf-8"))
    md   = INVENTORY_MD.read_text(encoding="utf-8")
    for block in spec["blocks"]:
        for feat in block["features"]:
            assert feat["id"] in md, f"feature {feat['id']} missing from inventory MD"
