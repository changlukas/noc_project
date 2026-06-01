"""Layer 1 + Layer 2 校驗。每個 check 回傳 Issue list，不 print。

Layer 2 不變量同時是 C-model packer 的 runtime assertion 來源：
- per-channel width sum: payload_width 必須等於 channel fields 寬度總和
- SECDED bound: ECC 寬度必須滿足 Hamming inequality
- route_par_coverage 參照的 header 欄位必須存在

PP-10 (post-PP-6): JSON 已不再儲存 lsb/msb/width/derived，欄位位置由
``ni_spec.constants`` 的 *_resolved helpers 在 read time 推算；所以
原來檢查「stored vs computed 不一致」的不變量被刪除（恆等式）。
"""

from __future__ import annotations
from dataclasses import dataclass
from typing import List, Optional

from . import constants as C
from .exceptions import SpecResolveError


@dataclass
class Issue:
    severity: str  # "ERROR" or "WARN"
    check: str
    message: str


def _err(check, msg) -> Issue: return Issue("ERROR", check, msg)
def _warn(check, msg) -> Issue: return Issue("WARN", check, msg)


def check_schema(packet_spec, packet_schema) -> List[Issue]:
    """Layer 1。"""
    if packet_schema is None:
        return [_warn("L1-SCHEMA", "schema 未提供，略過 Layer 1")]
    import jsonschema
    validator = jsonschema.Draft202012Validator(packet_schema)
    issues = []
    for e in sorted(validator.iter_errors(packet_spec), key=lambda e: list(e.absolute_path)):
        loc = "/".join(str(p) for p in e.absolute_path) or "(root)"
        issues.append(_err("L1-SCHEMA", f"{loc}: {e.message}"))
    return issues


def check_flit_arithmetic(packet_spec) -> List[Issue]:
    """Layer 2 packet arithmetic. Reads via ``ni_spec.constants`` helpers
    rather than legacy lsb/msb/width/derived JSON fields (dropped by PP-6)."""
    TAG = "L2-FLIT"
    issues: List[Issue] = []
    flit = packet_spec["flit"]
    fw = flit.get("field_widths", {})
    hdr = flit["header_fields"]

    # 1) Every header field's width_param must resolve cleanly.
    #    Resolve failures are demoted to WARN — the schema (Layer 1) is the
    #    authoritative gate for name validity; we only flag what we can compute.
    for f in hdr:
        try:
            w = C.header_field_width(packet_spec, f["name"])
        except SpecResolveError as e:
            issues.append(_warn(TAG, f"header '{f['name']}': width_param "
                                     f"'{f.get('width_param')}' unresolvable: {e}"))
            continue
        if w < 0:
            issues.append(_err(TAG, f"header '{f['name']}': resolved width {w} is negative"))

    # 2) Per-channel payload_width must equal the sum of its resolved field widths
    #    (catches authoring drift between channel-level total and per-field budget).
    #    If any field width is unresolvable, the sum check is skipped (warn-only).
    chan_widths = {}
    for ch in flit["payload_channels"]:
        scope = f"payload[{ch['name']}]"
        pw = int(ch["payload_width"])
        chan_widths[ch["name"]] = pw
        field_sum = 0
        sum_known = True
        for f in ch["fields"]:
            try:
                w = C.payload_field_width(packet_spec, ch["name"], f["name"])
            except SpecResolveError as e:
                issues.append(_warn(TAG, f"{scope} '{f['name']}': width unresolvable: {e}"))
                sum_known = False
                continue
            if w < 0:
                issues.append(_err(TAG, f"{scope} '{f['name']}': resolved width {w} is negative"))
                sum_known = False
            field_sum += w
        if sum_known and field_sum != pw:
            issues.append(_err(TAG, f"{scope}: field width sum {field_sum} != payload_width {pw}"))

    # 3) SECDED Hamming bound for the FLIT-level ECC.
    ecc = fw.get("FLIT_ECC_WIDTH")
    if ecc is not None:
        try:
            flit_data = C.flit_data_width_resolved(packet_spec)
        except SpecResolveError as e:
            issues.append(_err(TAG, f"FLIT_DATA_WIDTH resolve failed: {e}"))
            flit_data = None
        if flit_data is not None and ecc > 0:
            lhs = 2 ** (ecc - 1)
            rhs = flit_data + ecc + 1
            if lhs < rhs:
                issues.append(_err(TAG, f"SECDED bound violated: 2^({ecc}-1)={lhs} < "
                                        f"FLIT_DATA_WIDTH+ECC+1={rhs}"))
            else:
                margin = ecc
                while 2 ** (margin - 2) >= flit_data + (margin - 1) + 1:
                    margin -= 1
                if margin < ecc:
                    issues.append(_warn(TAG, f"FLIT_ECC_WIDTH={ecc} is conservative; "
                                             f"theoretical minimum is {margin}"))

    # 4) route_par_coverage references must exist in the header field list.
    hdr_names = {f["name"] for f in hdr}
    for cov in flit.get("route_par_coverage", []):
        if cov not in hdr_names:
            issues.append(_err(TAG, f"route_par_coverage references '{cov}' "
                                    f"which is not a header field"))

    # 5) Header derived padding: at most one field may declare width_param='derived'.
    #    The derived field anchors a fixed-size header (HEADER_TOTAL_WIDTH); allowing
    #    multiple derived fields would make the remainder split ambiguous.
    derived_hdr = [f["name"] for f in hdr if f.get("width_param") == "derived"]
    if len(derived_hdr) > 1:
        issues.append(_err(TAG, f"header: multiple 'derived' fields {derived_hdr}; "
                                f"at most one allowed (anchors HEADER_TOTAL_WIDTH)"))

    return issues


def check_signals_reset_domains(signals_spec) -> List[Issue]:
    """L2: every signal's reset_behavior.domain must be in meta.reset_signals."""
    issues: List[Issue] = []
    legal = set(signals_spec.get("meta", {}).get("reset_signals", []))
    for iface in signals_spec.get("interfaces", []):
        for ch in iface.get("channels", []):
            for sig in ch.get("signals", []):
                _check_one_reset(sig, legal, issues)
        for sig in iface.get("signals", []):
            _check_one_reset(sig, legal, issues)
    return issues


def _check_one_reset(sig: dict, legal_domains: set, issues: List[Issue]) -> None:
    rb = sig.get("reset_behavior")
    if rb is None:
        issues.append(_err("L2-SIG-RST", f"signal {sig.get('pin_name')} missing reset_behavior"))
        return
    if rb.get("kind") == "external_driven":
        if "value" in rb:
            issues.append(_err("L2-SIG-RST",
                f"signal {sig.get('pin_name')}: external_driven must not carry value"))
        return
    domain = rb.get("domain")
    if not domain:
        issues.append(_err("L2-SIG-RST",
            f"signal {sig.get('pin_name')}: non-external_driven must specify domain"))
    elif domain not in legal_domains:
        issues.append(_err("L2-SIG-RST",
            f"signal {sig.get('pin_name')}: reset domain {domain!r} not in meta.reset_signals"))


def check_signals_pin_uniqueness(signals_spec) -> List[Issue]:
    """L2: every signal must have a non-null, unique pin_name."""
    issues: List[Issue] = []
    seen: dict = {}
    for iface in signals_spec.get("interfaces", []):
        for ch in iface.get("channels", []):
            for sig in ch.get("signals", []):
                _check_pin_unique(sig, seen, issues)
        for sig in iface.get("signals", []):
            _check_pin_unique(sig, seen, issues)
    return issues


def _check_pin_unique(sig: dict, seen: dict, issues: List[Issue]) -> None:
    pin = sig.get("pin_name")
    if pin is None:
        issues.append(_err("L2-SIG-PIN", f"signal (pin_name=null) has null pin_name"))
        return
    if pin in seen:
        issues.append(_err("L2-SIG-PIN",
            f"pin_name {pin!r} duplicated (also seen at {seen[pin]})"))
    else:
        seen[pin] = pin


def check_csr_offset_alignment(regs_spec) -> List[Issue]:
    """L2: offset must be 4-byte aligned (32-bit registers)."""
    issues: List[Issue] = []
    for r in regs_spec.get("registers", []):
        if r.get("kind") != "register":
            continue
        try:
            ofs = int(r["offset"], 16)
        except (ValueError, KeyError):
            continue
        if ofs % 4 != 0:
            issues.append(_err("L2-REG-ALIGN",
                f"{r.get('name')}: offset {r['offset']} not 4-byte aligned"))
    return issues


def check_csr_offset_unique(regs_spec) -> List[Issue]:
    """L2: no two registers share an offset."""
    issues: List[Issue] = []
    seen: dict = {}
    for r in regs_spec.get("registers", []):
        ofs = r.get("offset")
        if ofs is None:
            continue
        if ofs in seen:
            issues.append(_err("L2-REG-OFS",
                f"offset {ofs} duplicated ({seen[ofs]} and {r.get('name')})"))
        else:
            seen[ofs] = r.get("name")
    return issues


def check_field_bit_tiling(regs_spec) -> List[Issue]:
    """L2: bit ranges within each register must not overlap."""
    issues: List[Issue] = []
    for r in regs_spec.get("registers", []):
        used: dict = {}
        for f in r.get("fields", []):
            try:
                hi, lo = int(f["bit_high"]), int(f["bit_low"])
            except (KeyError, ValueError, TypeError):
                continue
            if hi < lo:
                issues.append(_err("L2-REG-TILE",
                    f"{r.get('name')}.{f.get('name')}: bit_high {hi} < bit_low {lo}"))
                continue
            for b in range(lo, hi + 1):
                if b in used:
                    issues.append(_err("L2-REG-TILE",
                        f"{r.get('name')}: field {f.get('name')} bit {b} overlaps with {used[b]}"))
                else:
                    used[b] = f.get("name")
    return issues


def check_reset_in_data_width(regs_spec, data_width: int = 32) -> List[Issue]:
    """L2: reset_expr literal (when integer) must fit in data_width bits."""
    issues: List[Issue] = []
    for r in regs_spec.get("registers", []):
        if r.get("kind") != "register":
            continue
        rst = r.get("reset_expr")
        if not rst:
            continue
        try:
            val = int(rst, 0)
        except (ValueError, TypeError):
            continue  # symbolic reset (e.g. param expr) is fine for L2
        if val >= (1 << data_width):
            issues.append(_err("L2-REG-RESET",
                f"{r.get('name')}: reset {rst} exceeds {data_width}-bit width"))
    return issues


def check_blocks_xref_packet(fb_spec, pkt_spec) -> List[Issue]:
    """L2: every uses_packet_fields entry must exist in ni_packet.json."""
    issues: List[Issue] = []
    legal = {f["name"] for f in pkt_spec["flit"]["header_fields"]}
    legal |= {c["name"] for c in pkt_spec["flit"]["payload_channels"]}
    for block in fb_spec.get("blocks", []):
        for feat in block.get("features", []):
            for ref in feat.get("uses_packet_fields", []):
                if ref not in legal:
                    issues.append(_err("L2-FB-XREF-PKT",
                        f"{feat['id']}: uses_packet_fields {ref!r} not in ni_packet.json"))
    return issues


def check_blocks_xref_registers(fb_spec, regs_spec) -> List[Issue]:
    """L2: every configured_by entry's register name must exist in ni_registers.json."""
    issues: List[Issue] = []
    legal = {r["name"] for r in regs_spec.get("registers", []) if r.get("kind") == "register"}
    for block in fb_spec.get("blocks", []):
        for feat in block.get("features", []):
            for ref in feat.get("configured_by", []):
                # configured_by may include "REG.field" form — match register name part
                reg_name = ref.split(".")[0]
                if reg_name not in legal:
                    issues.append(_err("L2-FB-XREF-REG",
                        f"{feat['id']}: configured_by {ref!r} register not in ni_registers.json"))
    return issues


def check_blocks_param_uniqueness(fb_spec) -> List[Issue]:
    """L2: compile_time_params name must be unique across all features."""
    issues: List[Issue] = []
    seen: dict = {}
    for block in fb_spec.get("blocks", []):
        for feat in block.get("features", []):
            for pname in feat.get("compile_time_params", {}):
                if pname in seen:
                    issues.append(_err("L2-FB-PARAM",
                        f"{pname!r} defined in both {seen[pname]} and {feat['id']}"))
                else:
                    seen[pname] = feat["id"]
    return issues


def check_blocks_related_features_symmetric(fb_spec) -> List[Issue]:
    """L2: if A.related_features contains B, then B.related_features should contain A.
    Issues WARN (not ERROR) since one-way pointers may be intentional."""
    issues: List[Issue] = []
    all_feats: dict = {}  # id -> related_features set
    for block in fb_spec.get("blocks", []):
        for feat in block.get("features", []):
            all_feats[feat["id"]] = set(feat.get("related_features", []))
    for fid, refs in all_feats.items():
        for ref in refs:
            if ref not in all_feats:
                issues.append(_err("L2-FB-REL",
                    f"{fid}: related_features {ref!r} doesn't exist"))
                continue
            if fid not in all_feats[ref]:
                issues.append(_warn("L2-FB-REL",
                    f"{fid} -> {ref} is one-way (not symmetric)"))
    return issues


def check_protocol_rules_id_uniqueness(rule_spec) -> List[Issue]:
    """L2: no two rules share the same id."""
    issues = []
    seen: dict = {}
    for r in rule_spec.get("rules", []):
        rid = r["id"]
        if rid in seen:
            issues.append(_err("L2-PROTO-ID",
                f"rule id {rid!r} duplicated (first at line {seen[rid]}, also at line {r['source_line']})"))
        else:
            seen[rid] = r["source_line"]
    return issues


def check_all(bundle, md_dir: Optional[str] = None) -> List[Issue]:
    """Path B：跑 Layer 1 (schema) + Layer 2 (arithmetic)。

    Layer 3 (cross-check) 在 Path B 下不再存在 — generator 把 MD 直接
    產成 JSON，沒有兩份要對拍。md_dir 參數保留簽名但已無作用。
    """
    issues: List[Issue] = []
    issues += check_schema(bundle.packet, bundle.packet_schema)
    issues += check_flit_arithmetic(bundle.packet)
    return issues
