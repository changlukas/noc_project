"""Shared helpers used by ≥2 generator sub-modules (packet/signals/registers).

Keep this file MD-format-agnostic — only generic markdown table / section
slicing primitives live here. Domain-specific parsers belong in the
sub-module that owns them.
"""

from __future__ import annotations
import re
import warnings
from typing import List, Optional, Tuple


# ---------- regex primitives ----------

_RANGE_RE = re.compile(r"\[(\d+)(?::(\d+))?\]")
# 整數解析容忍 backtick：`8` → 8
_INT_TOKEN_RE = re.compile(r"-?\d+")


def _parse_bit_range(s: str) -> Optional[Tuple[int, int]]:
    """`[3:0]` → (0, 3)；`[26]` → (26, 26)；失敗 None。回 (lsb, msb)。"""
    m = _RANGE_RE.search(s)
    if not m:
        return None
    msb = int(m.group(1))
    lsb = int(m.group(2)) if m.group(2) is not None else msb
    return (lsb, msb)


def _strip_cell(c: str) -> str:
    """剝掉 markdown 強調符號（`、*）跟空白。"""
    return c.strip().strip("`").strip("*").strip("`").strip()


def _parse_int_cell(s: str) -> Optional[int]:
    """從 cell 字串裡抓第一個整數（容忍 backtick / 描述文字）。"""
    s = _strip_cell(s)
    m = _INT_TOKEN_RE.search(s)
    return int(m.group(0)) if m else None


def _section_slice(md_text: str, heading_pattern: str) -> Optional[str]:
    """切出 heading 後到下一個同層或淺層 heading 之前的文字。找不到 heading 回 None。

    自動偵測 heading 層級——例如 `### Foo` (level 3) 結束於下一個 level ≤ 3 的 heading
    (i.e. `###` / `##` / `#`)，**不**結束於更深的 `####` (level 4)。這讓 sub-section
    parser 可以放心抓母 section 完整內容。
    """
    pat = re.compile(heading_pattern, re.MULTILINE)
    m = pat.search(md_text)
    if not m:
        return None
    # 計算這個 matched heading 的 # 個數
    matched = m.group(0).lstrip()
    level = 0
    for ch in matched:
        if ch == "#":
            level += 1
        else:
            break
    if level == 0:
        # Caller bug: heading_pattern matched something that isn't a markdown
        # heading. Previously silently fell back to level=6; now surface so the
        # caller fixes the regex instead of producing a sloppy section slice.
        raise ValueError(
            f"_section_slice received non-heading match for pattern "
            f"{heading_pattern!r}: {matched!r}"
        )
    after = md_text[m.end():]
    next_re = re.compile(rf"^#{{1,{level}}}\s", re.MULTILINE)
    nxt = next_re.search(after)
    return after[: nxt.start()] if nxt else after


def _extract_table(section_text: str) -> Tuple[List[str], List[List[str]]]:
    """從 section 文字裡抓第一張 markdown table。回 (header_cells, [data_rows])。

    自動跳過分隔列 (|---|---|) 跟空 first-column 的 summary 列。
    支援 escaped pipe (\\|) — 不會被誤切。
    """
    lines = [l for l in section_text.splitlines() if l.lstrip().startswith("|")]
    if len(lines) < 3:
        return [], []
    header = _split_table_row(lines[0])
    rows: List[List[str]] = []
    for raw in lines[2:]:
        cells = _split_table_row(raw)
        if not cells or not cells[0]:
            continue  # summary 列
        rows.append(cells)
    return header, rows


def _col_idx(header: List[str], name: str) -> Optional[int]:
    try:
        return header.index(name)
    except ValueError:
        return None


# ---------- table-row splitter (escaped-pipe safe) ----------

_ESC_PIPE = "\x00ESC_PIPE\x00"


def _split_table_row(raw: str) -> List[str]:
    """安全切 markdown table 列：先用 placeholder 保護 escaped pipe (\\|)，split 後還原。"""
    protected = raw.replace(r"\|", _ESC_PIPE)
    cells = [_strip_cell(c).replace(_ESC_PIPE, "|") for c in protected.strip().strip("|").split("|")]
    return cells


def _extract_all_tables(section_text: str) -> List[Tuple[List[str], List[List[str]]]]:
    """Section 內所有 markdown table（不只第一張）。回 [(header, rows), ...]。"""
    tables = []
    lines = section_text.splitlines()
    i = 0
    while i < len(lines):
        if not lines[i].lstrip().startswith("|"):
            i += 1
            continue
        block = []
        while i < len(lines) and lines[i].lstrip().startswith("|"):
            block.append(lines[i])
            i += 1
        if len(block) >= 3:
            header = _split_table_row(block[0])
            rows = []
            for raw in block[2:]:
                cells = _split_table_row(raw)
                if not cells or not cells[0]:
                    continue
                rows.append(cells)
            tables.append((header, rows))
    return tables


# ---------- dash placeholder detection (used by register-map parser) ----------

# Em-dash characters that appear in reserved row Access/Reset cells.
# Codepoints: U+2014 em-dash (—), U+2013 en-dash (–), U+002D hyphen-minus (-).
# The original set had two U+2014 entries; deduplicated here.
_EM_DASH_VARIANTS = {
    "—",  # U+2014 em-dash
    "–",  # U+2013 en-dash
    "-",  # U+002D hyphen-minus
}


def _is_dash(cell: str) -> bool:
    """True if cell content is an em-dash / en-dash / hyphen placeholder.

    Accept plain hyphen (U+002D) too — some MD authors use it as placeholder;
    safe because real hex offsets / RW1C tokens never start with bare hyphen.
    """
    stripped = cell.strip().strip("`")
    return stripped in _EM_DASH_VARIANTS
