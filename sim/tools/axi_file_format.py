"""Shared axi_file_master field/beat encoding (Python standard library only)."""

def encode_write_beats(addr, axi_size, axi_len, data_width):
    """file_master W-beat lines: "0x<data> 0x<strb> 0", INCR, full strobe,
    address-in-data (byte A = A & 0xFF). data/strb sized to the DW bus."""
    bus_bytes = data_width // 8
    beat_bytes = 1 << axi_size
    if beat_bytes > bus_bytes:
        raise ValueError(
            f"axi_size={axi_size} (beat {beat_bytes} B) exceeds the {data_width}-bit "
            f"data bus ({bus_bytes} B); narrow the burst or widen DATA_WIDTH")
    lines = []
    for b in range(axi_len + 1):
        beat_addr = addr + b * beat_bytes
        lane0 = beat_addr % bus_bytes            # byte-lane of the beat's first byte
        data = 0
        strb = 0
        for k in range(beat_bytes):
            lane = lane0 + k
            data |= ((beat_addr + k) & 0xFF) << (8 * lane)
            strb |= 1 << lane
        lines.append(f"0x{data:0{bus_bytes * 2}x} 0x{strb:0{bus_bytes // 4}x} 0")
    return lines


def _ax_fields(axid, addr, axi_len, axi_size, include_atop, user=0):
    """The AW/AR field lines in parse_write/parse_read order. Write includes atop
    (12 fields); read omits it (11 fields, matching axi_file_master.parse_read).

    user: AWUSER value (58 b, decimal in the file; axi_file_master parses %d).
    Nonzero only for collective writes: [9:8] collective_op, [57:10] the
    address mask (docs/noc-target-spec.md AWUSER layout). Reads keep 0."""
    lines = [str(axid), f"0x{addr:x}", str(axi_len), str(axi_size),
             "1", "0", "0", "0", "0", "0"]          # burst=INCR lock cache prot qos region
    if include_atop:
        lines.append("0")                            # atop (write only)
    lines.append(str(user))                          # user (AWUSER / ARUSER)
    return lines
