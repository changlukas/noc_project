import gen_test_patterns as g


def test_axi_widths_follow_constants_ssot():
    w = g.axi_widths()
    assert w == {"id": 8, "addr": 64, "data": 256}
