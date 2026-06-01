"""CLI 入口: python -m ni_spec <md_dir>

Path B 流程：
  1. generator 讀 <md_dir>/packet_format.md → generated/json/ni_packet.json
  2. generator 讀 <md_dir>/signal_interface.md → generated/json/ni_signals.json
  3. Layer 1 (JSON Schema) 驗各 generated JSON
  4. Layer 2 (semantic / arithmetic) 驗
  5. 印 report、回傳 exit code
"""

from __future__ import annotations
import sys
from pathlib import Path

from .generator import write_generated_json, write_generated_signals_json, generate_ni_registers_json, generate_ni_protocol_rule_index_json
from .loader import load_doc
from .invariants import (Issue,
                         check_schema, check_flit_arithmetic,
                         check_signals_reset_domains, check_signals_pin_uniqueness,
                         check_csr_offset_alignment, check_csr_offset_unique,
                         check_field_bit_tiling, check_reset_in_data_width,
                         check_blocks_xref_packet, check_blocks_xref_registers,
                         check_blocks_param_uniqueness,
                         check_blocks_related_features_symmetric,
                         check_protocol_rules_id_uniqueness)
from .report import print_report

for _s in (sys.stdout, sys.stderr):
    if hasattr(_s, "reconfigure"):
        _s.reconfigure(encoding="utf-8", errors="replace")


SPECGEN_ROOT = Path(__file__).resolve().parent.parent
GENERATED_DIR = SPECGEN_ROOT / "generated" / "json"

PACKET_JSON = GENERATED_DIR / "ni_packet.json"
PACKET_SCHEMA = GENERATED_DIR / "ni_packet.schema.json"

SIGNALS_JSON = GENERATED_DIR / "ni_signals.json"
SIGNALS_SCHEMA = GENERATED_DIR / "ni_signals.schema.json"

REGISTERS_JSON = GENERATED_DIR / "ni_registers.json"
REGISTERS_SCHEMA = GENERATED_DIR / "ni_registers.schema.json"

SOURCE_DIR = SPECGEN_ROOT / "source"
FB_JSON = SOURCE_DIR / "ni_function_blocks.json"
FB_SCHEMA = SOURCE_DIR / "ni_function_blocks.schema.json"

PROTO_JSON = GENERATED_DIR / "ni_protocol_rule_index.json"
PROTO_SCHEMA = GENERATED_DIR / "ni_protocol_rule_index.schema.json"


def main() -> int:
    if len(sys.argv) < 2:
        print("用法: python -m ni_spec <md_dir>", file=sys.stderr)
        print("  md_dir: 含 packet_format.md + signal_interface.md 的目錄", file=sys.stderr)
        print(f"           例（從 noc_project/specgen/ 跑）: ../../spec/ni/doc", file=sys.stderr)
        print(f"  輸出: {GENERATED_DIR.relative_to(SPECGEN_ROOT)}/ni_*.json", file=sys.stderr)
        return 2

    md_dir = sys.argv[1]

    # Step 1: generate packet JSON
    try:
        packet = write_generated_json(md_dir, PACKET_JSON)
    except FileNotFoundError as e:
        print(f"[FATAL] packet generator: {e}", file=sys.stderr)
        return 2

    # Step 2: generate signals JSON
    try:
        signals = write_generated_signals_json(md_dir, SIGNALS_JSON)
    except FileNotFoundError as e:
        print(f"[FATAL] signals generator: {e}", file=sys.stderr)
        return 2

    # Step 3: generate registers JSON
    try:
        registers = generate_ni_registers_json(md_dir, REGISTERS_JSON)
    except (FileNotFoundError, ValueError) as e:
        print(f"[FATAL] registers generator: {e}", file=sys.stderr)
        return 2

    # Step 4: validate each
    issues = []

    packet_schema = load_doc(PACKET_SCHEMA) if PACKET_SCHEMA.exists() else None
    issues += check_schema(packet, packet_schema)
    issues += check_flit_arithmetic(packet)

    signals_schema = load_doc(SIGNALS_SCHEMA) if SIGNALS_SCHEMA.exists() else None
    # Layer 1 for signals schema
    if signals_schema is not None:
        import jsonschema
        validator = jsonschema.Draft202012Validator(signals_schema)
        for e in sorted(validator.iter_errors(signals), key=lambda e: list(e.absolute_path)):
            loc = "/".join(str(p) for p in e.absolute_path) or "(root)"
            issues.append(Issue("ERROR", "L1-SIG-SCHEMA", f"{loc}: {e.message}"))

    # Layer 2 for signals: reset domains + pin uniqueness
    issues += check_signals_reset_domains(signals)
    issues += check_signals_pin_uniqueness(signals)

    # Layer 1 for registers schema
    regs_schema = load_doc(REGISTERS_SCHEMA) if REGISTERS_SCHEMA.exists() else None
    if regs_schema is not None:
        import jsonschema
        for e in sorted(jsonschema.Draft202012Validator(regs_schema).iter_errors(registers),
                        key=lambda e: list(e.absolute_path)):
            loc = "/".join(str(p) for p in e.absolute_path) or "(root)"
            issues.append(Issue("ERROR", "L1-REG-SCHEMA", f"{loc}: {e.message}"))

    # Layer 2 for registers
    issues += check_csr_offset_alignment(registers)
    issues += check_csr_offset_unique(registers)
    issues += check_field_bit_tiling(registers)
    issues += check_reset_in_data_width(registers)

    # Step: generate protocol rule index JSON
    try:
        proto_rules = generate_ni_protocol_rule_index_json(md_dir, PROTO_JSON)
    except (FileNotFoundError, ValueError) as e:
        print(f"[FATAL] protocol_rules generator: {e}", file=sys.stderr)
        return 2

    # Layer 1 for protocol rules schema
    proto_schema = load_doc(PROTO_SCHEMA) if PROTO_SCHEMA.exists() else None
    if proto_schema is not None:
        import jsonschema
        for e in sorted(jsonschema.Draft202012Validator(proto_schema).iter_errors(proto_rules),
                        key=lambda e: list(e.absolute_path)):
            loc = "/".join(str(p) for p in e.absolute_path) or "(root)"
            issues.append(Issue("ERROR", "L1-PROTO-SCHEMA", f"{loc}: {e.message}"))

    # Layer 2 for protocol rules: id uniqueness
    issues += check_protocol_rules_id_uniqueness(proto_rules)

    # Function blocks: Layer 1 (schema) + Layer 2 (cross-domain)
    fb = None
    if FB_JSON.exists():
        fb = load_doc(FB_JSON)
        fb_schema = load_doc(FB_SCHEMA) if FB_SCHEMA.exists() else None
        if fb_schema is not None:
            import jsonschema
            for e in sorted(jsonschema.Draft202012Validator(fb_schema).iter_errors(fb),
                            key=lambda e: list(e.absolute_path)):
                loc = "/".join(str(p) for p in e.absolute_path) or "(root)"
                issues.append(Issue("ERROR", "L1-FB-SCHEMA", f"{loc}: {e.message}"))
        issues += check_blocks_xref_packet(fb, packet)
        issues += check_blocks_xref_registers(fb, registers)
        issues += check_blocks_param_uniqueness(fb)
        issues += check_blocks_related_features_symmetric(fb)

    has_l1_err = any(i.check in ("L1-SCHEMA", "L1-SIG-SCHEMA", "L1-REG-SCHEMA") and i.severity == "ERROR" for i in issues)
    has_l1_skip = (signals_schema is None or
                   any(i.check == "L1-SCHEMA" and i.severity == "WARN" for i in issues))
    has_l1_reg_skip = regs_schema is None
    has_l2_packet_err = any(i.check == "L2-FLIT" and i.severity == "ERROR" for i in issues)
    has_l2_sig_err = any(i.check.startswith("L2-SIG") and i.severity == "ERROR" for i in issues)
    has_l2_reg_err = any(i.check.startswith("L2-REG") and i.severity == "ERROR" for i in issues)
    has_l1_fb_err = any(i.check == "L1-FB-SCHEMA" and i.severity == "ERROR" for i in issues)
    has_l2_fb_err = any(i.check.startswith("L2-FB") and i.severity == "ERROR" for i in issues)
    has_l1_proto_skip = proto_schema is None
    has_l1_proto_err = any(i.check == "L1-PROTO-SCHEMA" and i.severity == "ERROR" for i in issues)
    has_l2_proto_err = any(i.check == "L2-PROTO-ID" and i.severity == "ERROR" for i in issues)

    layers = {
        "Generator (MD -> JSON)":            "OK (ni_packet.json + ni_signals.json + ni_registers.json + ni_protocol_rule_index.json)",
        "Layer 1 (JSON Schema, packet+sig)": "SKIPPED" if has_l1_skip else ("FAIL" if has_l1_err else "PASS"),
        "Layer 1 (registers)":               "SKIPPED" if has_l1_reg_skip else ("FAIL" if any(i.check == "L1-REG-SCHEMA" and i.severity == "ERROR" for i in issues) else "PASS"),
        "Layer 1 (function_blocks)":         "SKIPPED" if fb is None else ("FAIL" if has_l1_fb_err else "PASS"),
        "Layer 1 (protocol_rules)":          "SKIPPED" if has_l1_proto_skip else ("FAIL" if has_l1_proto_err else "PASS"),
        "Layer 2 (packet arithmetic)":       "FAIL" if has_l2_packet_err else "PASS",
        "Layer 2 (signals reset)":           "FAIL" if has_l2_sig_err else "PASS",
        "Layer 2 (registers)":               "FAIL" if has_l2_reg_err else "PASS",
        "Layer 2 (function_blocks)":         "SKIPPED" if fb is None else ("FAIL" if has_l2_fb_err else "PASS"),
        "Layer 2 (protocol_rules)":          "FAIL" if has_l2_proto_err else "PASS",
    }
    return print_report(issues,
                        target_name="ni_packet.json + ni_signals.json + ni_registers.json + ni_function_blocks.json + ni_protocol_rule_index.json",
                        show_layers=layers)


if __name__ == "__main__":
    sys.exit(main())
