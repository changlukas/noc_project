"""Spec 載入與 bundle 組裝。Format-agnostic：吃 .json / .yaml / .yml。"""

from __future__ import annotations
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional, Union
import json
import sys

try:
    import yaml
except ImportError:
    yaml = None


def load_doc(path: Union[str, Path]) -> dict:
    """讀單一 spec 檔。YAML 走 safe_load。"""
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    if p.suffix.lower() in (".yaml", ".yml"):
        if yaml is None:
            raise RuntimeError(f"YAML 載入需 PyYAML: pip install pyyaml ({p})")
        return yaml.safe_load(text)
    return json.loads(text)


@dataclass
class SpecBundle:
    """一次載入相關的所有 spec 檔。packet 為必須，其餘按存在性可選。"""
    spec_dir: Path
    packet: dict
    packet_schema: Optional[dict] = None
    nmu: Optional[dict] = None
    nsu: Optional[dict] = None
    md_dir: Optional[Path] = None


def load_spec_bundle(spec_dir: Union[str, Path], md_dir: Optional[Union[str, Path]] = None) -> SpecBundle:
    """從 spec_dir 載入 ni_packet.json (必須) + ni_packet.schema.json / ni_nmu.json / ni_nsu.json (可選)。"""
    d = Path(spec_dir)
    packet_path = d / "ni_packet.json"
    if not packet_path.exists():
        raise FileNotFoundError(f"找不到 {packet_path}")
    bundle = SpecBundle(spec_dir=d, packet=load_doc(packet_path))

    schema_path = d / "ni_packet.schema.json"
    if schema_path.exists():
        bundle.packet_schema = load_doc(schema_path)

    for attr, fname in (("nmu", "ni_nmu.json"), ("nsu", "ni_nsu.json")):
        p = d / fname
        if p.exists():
            setattr(bundle, attr, load_doc(p))

    if md_dir is not None:
        bundle.md_dir = Path(md_dir)
    return bundle


def load_spec_version() -> str:
    """Read spec/ni/VERSION (single source of truth for spec_version).

    Looks for the file relative to the specgen parent directory:
        noc_project/spec/ni/VERSION  (one-line semver, no trailing newline content).
    """
    specgen_root = Path(__file__).resolve().parent.parent
    version_file = specgen_root.parent / "spec" / "ni" / "VERSION"
    if not version_file.exists():
        raise FileNotFoundError(f"spec/ni/VERSION not found at {version_file}")
    return version_file.read_text(encoding="utf-8").strip()
