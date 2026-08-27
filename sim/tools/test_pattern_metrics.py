import pattern_metrics as pm


def test_uniform_random_matches_textbook_4_over_k():
    m = pm.metrics("uniform_random", 4, 4)
    # Table 7.2 / the 4 / k mesh rule, k = 4. The bisection channel carries
    # exactly k / 4 = 1 flit per node per cycle.
    assert abs(m["ideal_flits_per_node_cycle"] - 1.0) < 1e-9
    # ENUMERATED, not the textbook (2/3) k = 2.667. The exact mean Manhattan
    # distance over all ordered pairs of a k x k mesh with self permitted is
    # 2 (k^2 - 1) / (3 k) = 2.5 for k = 4. (2/3) k is its leading term, and the
    # missing 2 / (3 k) = 0.167 term is larger than the brief's 0.01 tolerance.
    assert abs(m["avg_hops"] - 2.5) < 1e-9


def test_all_to_all_is_two_thirds_k():
    # Same distance sum as uniform_random (self contributes 0) over n - 1
    # destinations instead of n, so the exact value is (2/3) k.
    m = pm.metrics("all_to_all", 4, 4)
    assert abs(m["avg_hops"] - 8 / 3) < 1e-9


def test_neighbor_is_one_flow_per_link():
    m = pm.metrics("neighbor", 4, 4)
    assert m["ideal_flits_per_node_cycle"] == 1.0
    # ENUMERATED, not the brief's 2.0. neighbor wraps (+1 mod k per dimension)
    # but a mesh has no wrap link, so the k - 1 edge nodes per dimension route
    # the long way back: mean per dimension = (1 + 1 + 1 + 3) / 4 = 1.5, twice
    # that over two dimensions. 2.0 is the torus value.
    assert m["avg_hops"] == 3.0


def test_bit_complement_bisection_bound():
    m = pm.metrics("bit_complement", 4, 4)
    assert m["ideal_flits_per_node_cycle"] == 0.5


def test_self_traffic_costs_no_hop_and_no_link():
    # transpose maps the diagonal to itself. 4 of 16 sources are self, the
    # other 12 pair up across the diagonal.
    m = pm.metrics("transpose", 4, 4)
    assert m["avg_hops"] == sum(2 * abs(i % 4 - i // 4) for i in range(16)) / 16


def test_self_fraction_is_the_share_that_never_enters_the_noc():
    # uniform_random and hotspot both permit self (1 of 16 sources addresses
    # itself); transpose maps its 4 diagonal nodes to themselves; all_to_all
    # excludes self by construction. The tile crossbar answers that share, so
    # no monitor ever sees it.
    assert pm.metrics("uniform_random", 4, 4)["self_fraction"] == 1 / 16
    assert pm.metrics("hotspot", 4, 4)["self_fraction"] == 1 / 16
    assert pm.metrics("all_to_all", 4, 4)["self_fraction"] == 0.0
    # The permutations that have fixed points: transpose fixes its diagonal,
    # bit_reverse the 4 palindromes of 4 bits, shuffle and bit_rotation the two
    # constant words 0000 and 1111.
    assert pm.metrics("transpose", 4, 4)["self_fraction"] == 4 / 16
    assert pm.metrics("bit_reverse", 4, 4)["self_fraction"] == 4 / 16
    assert pm.metrics("shuffle", 4, 4)["self_fraction"] == 2 / 16
    assert pm.metrics("bit_rotation", 4, 4)["self_fraction"] == 2 / 16


def test_every_pattern_reports_a_finite_ideal():
    for p in pm.PATTERNS:
        m = pm.metrics(p, 4, 4)
        assert m["max_channel_load"] > 0
        assert m["ideal_flits_per_node_cycle"] == 1.0 / m["max_channel_load"]
