import pattern_metrics as pm


def test_uniform_random_matches_textbook_4_over_k():
    m = pm.metrics("uniform_random", 4, 4)
    # Table 7.2 / the 4 / k mesh rule, k = 4. The bisection channel carries
    # exactly k / 4 = 1 flit per node per cycle.
    #
    # The combined matrix leaves this untouched. Load is
    #   L'(e) = sum over (s, d) of w(s, d) * (34/67 on P(s, d) + 33/67 on P(d, s))
    # and uniform_random is symmetric, w(s, d) = w(d, s) = 1/n, so relabelling
    # the second term turns it back into the first: L' = (34/67 + 33/67) L = L.
    # Every pattern with a symmetric matrix keeps its request only bound, which
    # here is exactly 1.0.
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
    # An involution, so its matrix is symmetric and the reverse path traffic
    # lands on the links the paired flow already used. Unchanged at 0.5.
    assert m["ideal_flits_per_node_cycle"] == 0.5


def _request_only_max_load(pattern, x_dim, y_dim):
    """The pre read reply bound: request path only, full weight per flow."""
    load = {}
    for src, dst, weight in pm._flows(pattern, x_dim, y_dim, list(pm._DEFAULT_HOTSPOTS)):
        for link in pm._xy_links(src, dst):
            load[link] = load.get(link, 0.0) + weight
    return max(load.values())


def test_symmetric_patterns_keep_their_request_only_bound():
    # transpose, bit_complement, bit_reverse and uniform_random all have a
    # symmetric traffic matrix, so the 34/67 forward and 33/67 reverse split
    # sums back to the request only load on every link.
    for p in ("transpose", "bit_complement", "bit_reverse", "uniform_random",
              "all_to_all", "neighbor"):
        m = pm.metrics(p, 4, 4)
        assert abs(m["max_channel_load"] - _request_only_max_load(p, 4, 4)) < 1e-9, p


def test_transpose_busiest_link_matches_the_measured_flit_count():
    """The bound checked against a wire, not against another formula.

    `output/continuous_mesh_4x4_transpose_r0.0179_s1/perf.json` measures four DAT
    links at 40200 flits over a 40347 cycle window, 99.6 percent, the busiest in
    the mesh: `dat_1to0`, `dat_0to4`, `dat_14to15` and `dat_15to11`. Those are
    exactly the four links this model puts at the top, and the count decomposes
    to the flit: each active node ran 200 transactions at AxLEN 32, so a write
    flow carries 200 * 34 = 6800 flits and a read reply flow 200 * 33 = 6600.
    `dat_1to0` takes 3 write flows and 3 read reply flows,

        3 * 6800 + 3 * 6600 = 40200

    which is the measured number. Counting one direction only would have
    predicted 20400 and missed by a factor of two.
    """
    fwd, rev = ((1, 0), (0, 0)), ((1, 0), (0, 0))
    n_fwd = n_rev = 0
    for src, dst, _w in pm._flows("transpose", 4, 4, [5]):
        n_fwd += pm._xy_links(src, dst).count(fwd)
        n_rev += pm._xy_links(dst, src).count(rev)
    assert (n_fwd, n_rev) == (3, 3)
    assert n_fwd * 200 * 34 + n_rev * 200 * 33 == 40200
    # Same link, same split, expressed as the model's own weights.
    assert abs(pm.metrics("transpose", 4, 4)["max_channel_load"]
               - (n_fwd * pm._FORWARD_SHARE + n_rev * pm._REVERSE_SHARE)) < 1e-9


def test_asymmetric_patterns_spread_the_load_over_two_directions():
    # shuffle, bit_rotation and hotspot concentrate their request paths on a few
    # links. Moving 33 of every 67 flits onto the reverse path, which for these
    # patterns is a different link set, lowers the busiest link and so raises
    # the bound above the request only figure.
    for p in ("shuffle", "bit_rotation", "hotspot"):
        m = pm.metrics(p, 4, 4)
        assert m["max_channel_load"] < _request_only_max_load(p, 4, 4), p


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
