"""Issue list 的呈現與 exit code 推導。

從 invariants.py 分離出來，是因為「校驗 vs 呈現」是兩個關注點：
- invariants.py 回 Issue list（純資料、可給 C-model assertion 或 IDE 用）
- report.py 把 list 轉成 terminal 輸出或 exit code（呈現層）
"""

from __future__ import annotations
from typing import Iterable
from .invariants import Issue


def n_err(issues: Iterable[Issue]) -> int:
    return sum(1 for i in issues if i.severity == "ERROR")


def n_warn(issues: Iterable[Issue]) -> int:
    return sum(1 for i in issues if i.severity == "WARN")


def print_report(issues, target_name: str = "ni_packet.json", show_layers: dict = None) -> int:
    """印出標準格式報告；回傳建議的 exit code（0 / 1）。

    show_layers: 可選 dict 例 {'Layer 1': 'PASS', 'Layer 2': 'PASS'} 印在分隔線後。
    """
    print(f"== NI Spec Validator ==  target: {target_name}")
    print("-" * 68)
    if show_layers:
        for layer, status in show_layers.items():
            print(f"  {layer:28s}: {status}")
        print("-" * 68)
    for i in issues:
        tag = "✗ ERROR" if i.severity == "ERROR" else "! WARN "
        print(f"  [{tag}] [{i.check}] {i.message}")
    if issues:
        print("-" * 68)
    e, w = n_err(issues), n_warn(issues)
    print(f"  總計: {e} error, {w} warning")
    print("  結果: " + ("規格通過校驗 ✓" if e == 0 else "規格未通過 ✗"))
    return 1 if e else 0
