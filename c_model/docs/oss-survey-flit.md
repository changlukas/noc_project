# OSS Survey — Flit bit-field manipulation

## Need
- Pack / unpack a fixed-width (FLIT_WIDTH bits) bit-array using codegen-supplied LSB/MSB
- Round-trip set_header_field / get_header_field
- Verify padding bits stay zero (check_padding_is_zero)
- Cannot hardcode widths — must reference ni::FLIT_WIDTH, ni::header::*_LSB/_MSB

## Candidates considered
(See plan task 9 step 1 table.)

## Decision
**std::array<uint8_t, WIDTH_BYTES> + hand-rolled shift/mask**

Rationale: the helpers in boost / bit_field_v3 add a dependency layer that
shadows the codegen symbols (LSB/MSB) rather than consuming them directly.
The shift-and-mask logic against `ni::header::*_LSB/_MSB` is 5-10 LOC per
accessor — adopting a library doesn't reduce code or risk.

`std::bitset<FLIT_WIDTH>` was considered but rejected because indexing past
64 bits requires byte-wise conversion to expose `set_header_field` returning
`uint64_t` — same complexity as `array<uint8_t, N>`.

## Future revisit trigger
If c_model grows to 5+ packet types each with their own bit layout, revisit
to see if a layout DSL is justified.
