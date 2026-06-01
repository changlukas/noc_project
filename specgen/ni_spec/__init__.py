"""ni_spec — Single source of truth for the NoC NI specification.

Both the validator (validate.py) and a future C-model consume this module's
APIs. Spec-derived constants live in `ni_spec.constants`; runtime invariants
in `ni_spec.invariants` double as the C-model packer's assertions.
"""

from .loader import load_doc, load_spec_bundle, load_spec_version, SpecBundle
from .invariants import (
    Issue,
    check_schema,
    check_flit_arithmetic,
    check_all,
)
from .generator import (
    generate_ni_packet_json,
    write_generated_json,
    parse_header_fields,
    parse_payload_channels,
    parse_field_widths,
    parse_derived,
)
from .report import n_err, n_warn, print_report
from . import constants

__all__ = [
    "load_doc",
    "load_spec_bundle",
    "load_spec_version",
    "SpecBundle",
    "Issue",
    "check_schema",
    "check_flit_arithmetic",
    "check_all",
    "generate_ni_packet_json",
    "write_generated_json",
    "parse_header_fields",
    "parse_payload_channels",
    "parse_field_widths",
    "parse_derived",
    "n_err",
    "n_warn",
    "print_report",
    "constants",
]
