import os

import gen_test_patterns as g


def test_axi_widths_follow_constants_ssot():
    w = g.axi_widths()
    assert w == {"id": 8, "addr": 64, "data": 256}


def test_encode_write_beats_full_width_addr_in_data():
    # DW=256 -> 32 bytes/beat. One full-width beat at 0x1000.
    beats = g.encode_write_beats(0x1000, axi_size=5, axi_len=0, data_width=256)
    assert len(beats) == 1
    data_hex, strb_hex, user = beats[0].split()
    assert user == "0"
    assert strb_hex == "0x" + "f" * 8              # 32 lanes all active
    # byte j (little-endian) == (0x1000 + j) & 0xff
    data = int(data_hex, 16)
    for j in range(32):
        assert (data >> (8 * j)) & 0xFF == (0x1000 + j) & 0xFF


def test_encode_write_beats_multibeat_incr():
    beats = g.encode_write_beats(0x2000, axi_size=5, axi_len=3, data_width=256)
    assert len(beats) == 4                          # len+1 beats
    # beat 2 starts at 0x2000 + 2*32
    data = int(beats[2].split()[0], 16)
    assert data & 0xFF == (0x2000 + 2 * 32) & 0xFF


def _parse_write(path):
    """Parse a write.txt back into txns. Mirrors axi_file_master.parse_write field order."""
    toks = open(path).read().split("\n")
    it = iter([t for t in toks if t != ""])
    txns = []
    while True:
        try:
            axid = int(next(it))
        except StopIteration:
            break
        addr = int(next(it), 16)
        length = int(next(it)); size = int(next(it)); burst = int(next(it))
        for _ in range(6):  # lock cache prot qos region atop
            next(it)
        next(it)            # user
        beats = [next(it) for _ in range(length + 1)]
        txns.append({"id": axid, "addr": addr, "len": length, "size": size,
                     "burst": burst, "beats": beats})
    return txns


def test_emit_file_master_node_format_and_partition(tmp_path):
    d = str(tmp_path / "node0")
    g.emit_file_master_node(d, src_idx=0, dst_cids=[1, 1], n_nodes=16,
                            base_local=0x1000, memory_size=0x40000,
                            axi_size=5, axi_len=0, data_width=256)
    w = _parse_write(os.path.join(d, "write.txt"))
    assert len(w) == 2
    for t in w:
        assert t["burst"] == 1 and t["size"] == 5 and t["len"] == 0
        assert (t["addr"] >> 32) == 1               # dst tile in addr[63:32]
        assert len(t["beats"]) == 1
    assert w[0]["addr"] != w[1]["addr"]             # disjoint offsets
    rlines = [l for l in open(os.path.join(d, "read.txt")).read().split("\n") if l != ""]
    assert len(rlines) == 2 * 11                    # 11 ax fields, no atop, no beats
