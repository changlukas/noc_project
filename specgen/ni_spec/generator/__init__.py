"""MD → JSON generator package. Path B 的核心：把 spec/ni MD 解析成 ni_*.json。

對齊業界做法（SystemRDL / Protocol Buffers）：人類只改 source（MD），
工具產衍生品（JSON），下游消費衍生品。

Split into per-domain sub-modules:
  - packet            — packet_format.md (flit JSON)
  - signals           — signal_interface.md + pin_level_reset.md (signals JSON)
  - registers         — registers.md (CSR JSON)
  - protocol_rules    — protocol_rules.md (rule-index JSON)
  - _common           — shared markdown table / section primitives

The package re-exports every name from the pre-split generator.py so
existing ``from ni_spec.generator import ...`` import sites keep working.
"""

from __future__ import annotations

# ----- shared primitives (re-exported for legacy callers/tests) -----
from ._common import (
    _RANGE_RE,
    _INT_TOKEN_RE,
    _parse_bit_range,
    _strip_cell,
    _parse_int_cell,
    _section_slice,
    _extract_table,
    _col_idx,
    _ESC_PIPE,
    _split_table_row,
    _extract_all_tables,
    _EM_DASH_VARIANTS,
    _is_dash,
)

# ----- packet domain -----
from .packet import (
    _is_zero_width_range,
    parse_header_fields,
    _CHANNEL_TO_NETWORK,
    _parse_payload_section,
    parse_payload_channels,
    _FIELD_WIDTHS_GROUPS,
    parse_field_widths,
    _DERIVED_SKIP,
    parse_derived,
    generate_ni_packet_json,
    write_generated_json,
)

# ----- signals domain -----
from .signals import (
    parse_signal_parameters,
    _AXI_CHANNELS_SLAVE,
    _AXI_CHANNELS_MASTER,
    _port_type_of,
    _build_axi_channels,
    _AXI_CHANNEL_SIGNALS,
    _NOC_INTERFACE_SIGNALS,
    _INTERFACE_PARAMS,
    _AXI_PORT_PARAMS,
    _build_noc_signals,
    _BLOCK_ENABLE_PARAMS,
    _IFACE_NAME_MAP,
    _PROTOCOL_MAP,
    _build_params_namespace,
    _select_params,
    parse_top_level_interfaces,
    generate_ni_signals_json,
    write_generated_signals_json,
    parse_pin_level_reset,
    _derive_pin_name,
    _default_reset_for,
    extract_reset_signals,
)

# ----- registers domain -----
from .registers import (
    parse_csr_policy,
    parse_register_map,
    parse_register_fields,
    _REGISTERS_WITH_FIELDS,
    generate_ni_registers_json,
)

# ----- protocol_rules domain -----
from .protocol_rules import (
    _PROTO_MAP,
    _SECTION_PAT,
    _ROW_PAT,
    _infer_proto,
    parse_protocol_rule_index,
    generate_ni_protocol_rule_index_json,
)
