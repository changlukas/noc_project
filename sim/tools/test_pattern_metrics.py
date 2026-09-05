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
               - (n_fwd * 34 + n_rev * 33) / 67) < 1e-9


def test_beats_sets_the_forward_reverse_split():
    """`beats` is AxLEN + 1 and sets how one AX pair splits over the two
    directions. At AxLEN 0 the pair is 1 AW + 1 W forward and 1 R back, a 2/3
    forward share against 34/67, which moves the busiest link of an asymmetric
    pattern. A symmetric one cannot move, by the identity below."""
    assert (pm.metrics("shuffle", 4, 4, beats=1)["max_channel_load"]
            != pm.metrics("shuffle", 4, 4)["max_channel_load"])
    assert abs(pm.metrics("transpose", 4, 4, beats=1)["max_channel_load"]
               - pm.metrics("transpose", 4, 4)["max_channel_load"]) < 1e-9


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


def _resource_kind(resources, plane, kind):
    prefix = f"{plane}_"
    marker = f"_{kind}_"
    return {name: count for name, count in resources.items()
            if name.startswith(prefix) and marker in name}


def _resource_links(resources, plane):
    prefix = f"{plane}_"
    return {name: count for name, count in resources.items()
            if name.startswith(prefix)
            and "_inject_" not in name and "_eject_" not in name}


_ALL_DAT_EJECT_TWO = {
    "dat_eject_0": 2, "dat_eject_1": 2, "dat_eject_2": 2,
    "dat_eject_3": 2, "dat_eject_4": 2, "dat_eject_5": 2,
    "dat_eject_6": 2, "dat_eject_7": 2, "dat_eject_8": 2,
    "dat_eject_9": 2, "dat_eject_10": 2, "dat_eject_11": 2,
    "dat_eject_12": 2, "dat_eject_13": 2, "dat_eject_14": 2,
    "dat_eject_15": 2,
}
_ALL_RSP_INJECT_ONE = {
    "rsp_inject_0": 1, "rsp_inject_1": 1, "rsp_inject_2": 1,
    "rsp_inject_3": 1, "rsp_inject_4": 1, "rsp_inject_5": 1,
    "rsp_inject_6": 1, "rsp_inject_7": 1, "rsp_inject_8": 1,
    "rsp_inject_9": 1, "rsp_inject_10": 1, "rsp_inject_11": 1,
    "rsp_inject_12": 1, "rsp_inject_13": 1, "rsp_inject_14": 1,
    "rsp_inject_15": 1,
}


def test_pipeline_write_has_exact_native_dat_and_rsp_resources():
    resources = pm.ai_resource_flits("pipeline", "write", 2, 1, None)

    assert _resource_kind(resources, "dat", "inject") == {
        "dat_inject_0": 3, "dat_inject_1": 3, "dat_inject_2": 3,
        "dat_inject_3": 3, "dat_inject_4": 3, "dat_inject_5": 3,
        "dat_inject_6": 3, "dat_inject_7": 3, "dat_inject_8": 3,
        "dat_inject_9": 3, "dat_inject_10": 3, "dat_inject_11": 3,
        "dat_inject_13": 3, "dat_inject_14": 3, "dat_inject_15": 3,
    }
    assert _resource_links(resources, "dat") == {
        "dat_0to1": 3, "dat_1to2": 3, "dat_2to3": 3,
        "dat_3to7": 3, "dat_7to6": 3, "dat_6to5": 3,
        "dat_5to4": 3, "dat_4to8": 3, "dat_8to9": 3,
        "dat_9to10": 3, "dat_10to11": 3, "dat_11to15": 3,
        "dat_15to14": 3, "dat_14to13": 3, "dat_13to12": 3,
    }
    assert _resource_kind(resources, "dat", "eject") == {
        "dat_eject_1": 3, "dat_eject_2": 3, "dat_eject_3": 3,
        "dat_eject_4": 3, "dat_eject_5": 3, "dat_eject_6": 3,
        "dat_eject_7": 3, "dat_eject_8": 3, "dat_eject_9": 3,
        "dat_eject_10": 3, "dat_eject_11": 3, "dat_eject_12": 3,
        "dat_eject_13": 3, "dat_eject_14": 3, "dat_eject_15": 3,
    }
    assert _resource_kind(resources, "rsp", "inject") == {
        "rsp_inject_1": 1, "rsp_inject_2": 1, "rsp_inject_3": 1,
        "rsp_inject_4": 1, "rsp_inject_5": 1, "rsp_inject_6": 1,
        "rsp_inject_7": 1, "rsp_inject_8": 1, "rsp_inject_9": 1,
        "rsp_inject_10": 1, "rsp_inject_11": 1, "rsp_inject_12": 1,
        "rsp_inject_13": 1, "rsp_inject_14": 1, "rsp_inject_15": 1,
    }
    assert _resource_links(resources, "rsp") == {
        "rsp_1to0": 1, "rsp_2to1": 1, "rsp_3to2": 1,
        "rsp_7to3": 1, "rsp_6to7": 1, "rsp_5to6": 1,
        "rsp_4to5": 1, "rsp_8to4": 1, "rsp_9to8": 1,
        "rsp_10to9": 1, "rsp_11to10": 1, "rsp_15to11": 1,
        "rsp_14to15": 1, "rsp_13to14": 1, "rsp_12to13": 1,
    }
    assert _resource_kind(resources, "rsp", "eject") == {
        "rsp_eject_0": 1, "rsp_eject_1": 1, "rsp_eject_2": 1,
        "rsp_eject_3": 1, "rsp_eject_4": 1, "rsp_eject_5": 1,
        "rsp_eject_6": 1, "rsp_eject_7": 1, "rsp_eject_8": 1,
        "rsp_eject_9": 1, "rsp_eject_10": 1, "rsp_eject_11": 1,
        "rsp_eject_13": 1, "rsp_eject_14": 1, "rsp_eject_15": 1,
    }
    assert not any(name.startswith("req_") for name in resources)


def test_pipeline_read_reverses_req_but_returns_dat_on_payload_edges():
    resources = pm.ai_resource_flits("pipeline", "read", 2, 1, None)

    assert _resource_kind(resources, "req", "inject") == {
        "req_inject_1": 1, "req_inject_2": 1, "req_inject_3": 1,
        "req_inject_4": 1, "req_inject_5": 1, "req_inject_6": 1,
        "req_inject_7": 1, "req_inject_8": 1, "req_inject_9": 1,
        "req_inject_10": 1, "req_inject_11": 1, "req_inject_12": 1,
        "req_inject_13": 1, "req_inject_14": 1, "req_inject_15": 1,
    }
    assert _resource_links(resources, "req") == {
        "req_1to0": 1, "req_2to1": 1, "req_3to2": 1,
        "req_7to3": 1, "req_6to7": 1, "req_5to6": 1,
        "req_4to5": 1, "req_8to4": 1, "req_9to8": 1,
        "req_10to9": 1, "req_11to10": 1, "req_15to11": 1,
        "req_14to15": 1, "req_13to14": 1, "req_12to13": 1,
    }
    assert _resource_kind(resources, "req", "eject") == {
        "req_eject_0": 1, "req_eject_1": 1, "req_eject_2": 1,
        "req_eject_3": 1, "req_eject_4": 1, "req_eject_5": 1,
        "req_eject_6": 1, "req_eject_7": 1, "req_eject_8": 1,
        "req_eject_9": 1, "req_eject_10": 1, "req_eject_11": 1,
        "req_eject_13": 1, "req_eject_14": 1, "req_eject_15": 1,
    }
    assert _resource_kind(resources, "dat", "inject") == {
        "dat_inject_0": 2, "dat_inject_1": 2, "dat_inject_2": 2,
        "dat_inject_3": 2, "dat_inject_4": 2, "dat_inject_5": 2,
        "dat_inject_6": 2, "dat_inject_7": 2, "dat_inject_8": 2,
        "dat_inject_9": 2, "dat_inject_10": 2, "dat_inject_11": 2,
        "dat_inject_13": 2, "dat_inject_14": 2, "dat_inject_15": 2,
    }
    assert _resource_links(resources, "dat") == {
        "dat_0to1": 2, "dat_1to2": 2, "dat_2to3": 2,
        "dat_3to7": 2, "dat_7to6": 2, "dat_6to5": 2,
        "dat_5to4": 2, "dat_4to8": 2, "dat_8to9": 2,
        "dat_9to10": 2, "dat_10to11": 2, "dat_11to15": 2,
        "dat_15to14": 2, "dat_14to13": 2, "dat_13to12": 2,
    }
    assert _resource_kind(resources, "dat", "eject") == {
        "dat_eject_1": 2, "dat_eject_2": 2, "dat_eject_3": 2,
        "dat_eject_4": 2, "dat_eject_5": 2, "dat_eject_6": 2,
        "dat_eject_7": 2, "dat_eject_8": 2, "dat_eject_9": 2,
        "dat_eject_10": 2, "dat_eject_11": 2, "dat_eject_12": 2,
        "dat_eject_13": 2, "dat_eject_14": 2, "dat_eject_15": 2,
    }
    assert not any(name.startswith("rsp_") for name in resources)


def test_global_gather_write_concentrates_dat_at_root_and_rsp_at_sources():
    resources = pm.ai_resource_flits(
        "gather_global_root0", "write", 1, 1, None)

    assert _resource_kind(resources, "dat", "inject") == {
        "dat_inject_1": 2, "dat_inject_2": 2, "dat_inject_3": 2,
        "dat_inject_4": 2, "dat_inject_5": 2, "dat_inject_6": 2,
        "dat_inject_7": 2, "dat_inject_8": 2, "dat_inject_9": 2,
        "dat_inject_10": 2, "dat_inject_11": 2, "dat_inject_12": 2,
        "dat_inject_13": 2, "dat_inject_14": 2, "dat_inject_15": 2,
    }
    assert _resource_links(resources, "dat") == {
        "dat_1to0": 6, "dat_2to1": 4, "dat_3to2": 2,
        "dat_5to4": 6, "dat_6to5": 4, "dat_7to6": 2,
        "dat_9to8": 6, "dat_10to9": 4, "dat_11to10": 2,
        "dat_13to12": 6, "dat_14to13": 4, "dat_15to14": 2,
        "dat_4to0": 24, "dat_8to4": 16, "dat_12to8": 8,
    }
    assert _resource_kind(resources, "dat", "eject") == {"dat_eject_0": 30}
    assert _resource_kind(resources, "rsp", "inject") == {"rsp_inject_0": 15}
    assert _resource_links(resources, "rsp") == {
        "rsp_0to1": 12, "rsp_1to2": 8, "rsp_2to3": 4,
        "rsp_0to4": 3, "rsp_4to8": 2, "rsp_8to12": 1,
        "rsp_1to5": 3, "rsp_5to9": 2, "rsp_9to13": 1,
        "rsp_2to6": 3, "rsp_6to10": 2, "rsp_10to14": 1,
        "rsp_3to7": 3, "rsp_7to11": 2, "rsp_11to15": 1,
    }
    assert _resource_kind(resources, "rsp", "eject") == {
        "rsp_eject_1": 1, "rsp_eject_2": 1, "rsp_eject_3": 1,
        "rsp_eject_4": 1, "rsp_eject_5": 1, "rsp_eject_6": 1,
        "rsp_eject_7": 1, "rsp_eject_8": 1, "rsp_eject_9": 1,
        "rsp_eject_10": 1, "rsp_eject_11": 1, "rsp_eject_12": 1,
        "rsp_eject_13": 1, "rsp_eject_14": 1, "rsp_eject_15": 1,
    }


def test_broadcast_hardware_fork_and_collectb_join_edges_are_exact():
    expected = {
        "broadcast_row": (
            {"dat_inject_0": 2, "dat_inject_4": 2,
             "dat_inject_8": 2, "dat_inject_12": 2},
            {
                "dat_0to1": 2, "dat_1to2": 2, "dat_2to3": 2,
                "dat_4to5": 2, "dat_5to6": 2, "dat_6to7": 2,
                "dat_8to9": 2, "dat_9to10": 2, "dat_10to11": 2,
                "dat_12to13": 2, "dat_13to14": 2, "dat_14to15": 2,
            },
            {
                "rsp_1to0": 1, "rsp_2to1": 1, "rsp_3to2": 1,
                "rsp_5to4": 1, "rsp_6to5": 1, "rsp_7to6": 1,
                "rsp_9to8": 1, "rsp_10to9": 1, "rsp_11to10": 1,
                "rsp_13to12": 1, "rsp_14to13": 1, "rsp_15to14": 1,
            },
            {"rsp_eject_0": 1, "rsp_eject_4": 1,
             "rsp_eject_8": 1, "rsp_eject_12": 1},
        ),
        "broadcast_col": (
            {"dat_inject_0": 2, "dat_inject_1": 2,
             "dat_inject_2": 2, "dat_inject_3": 2},
            {
                "dat_0to4": 2, "dat_4to8": 2, "dat_8to12": 2,
                "dat_1to5": 2, "dat_5to9": 2, "dat_9to13": 2,
                "dat_2to6": 2, "dat_6to10": 2, "dat_10to14": 2,
                "dat_3to7": 2, "dat_7to11": 2, "dat_11to15": 2,
            },
            {
                "rsp_4to0": 1, "rsp_8to4": 1, "rsp_12to8": 1,
                "rsp_5to1": 1, "rsp_9to5": 1, "rsp_13to9": 1,
                "rsp_6to2": 1, "rsp_10to6": 1, "rsp_14to10": 1,
                "rsp_7to3": 1, "rsp_11to7": 1, "rsp_15to11": 1,
            },
            {"rsp_eject_0": 1, "rsp_eject_1": 1,
             "rsp_eject_2": 1, "rsp_eject_3": 1},
        ),
        "broadcast_submesh": (
            {"dat_inject_0": 2, "dat_inject_2": 2,
             "dat_inject_8": 2, "dat_inject_10": 2},
            {
                "dat_0to1": 2, "dat_0to4": 2, "dat_1to5": 2,
                "dat_2to3": 2, "dat_2to6": 2, "dat_3to7": 2,
                "dat_8to9": 2, "dat_8to12": 2, "dat_9to13": 2,
                "dat_10to11": 2, "dat_10to14": 2, "dat_11to15": 2,
            },
            {
                "rsp_1to0": 1, "rsp_4to0": 1, "rsp_5to4": 1,
                "rsp_3to2": 1, "rsp_6to2": 1, "rsp_7to6": 1,
                "rsp_9to8": 1, "rsp_12to8": 1, "rsp_13to12": 1,
                "rsp_11to10": 1, "rsp_14to10": 1, "rsp_15to14": 1,
            },
            {"rsp_eject_0": 1, "rsp_eject_2": 1,
             "rsp_eject_8": 1, "rsp_eject_10": 1},
        ),
        "broadcast_global": (
            {"dat_inject_0": 2},
            {
                "dat_0to1": 2, "dat_1to2": 2, "dat_2to3": 2,
                "dat_0to4": 2, "dat_4to8": 2, "dat_8to12": 2,
                "dat_1to5": 2, "dat_5to9": 2, "dat_9to13": 2,
                "dat_2to6": 2, "dat_6to10": 2, "dat_10to14": 2,
                "dat_3to7": 2, "dat_7to11": 2, "dat_11to15": 2,
            },
            {
                "rsp_1to0": 1, "rsp_2to1": 1, "rsp_3to2": 1,
                "rsp_5to4": 1, "rsp_6to5": 1, "rsp_7to6": 1,
                "rsp_9to8": 1, "rsp_10to9": 1, "rsp_11to10": 1,
                "rsp_13to12": 1, "rsp_14to13": 1, "rsp_15to14": 1,
                "rsp_4to0": 1, "rsp_8to4": 1, "rsp_12to8": 1,
            },
            {"rsp_eject_0": 1},
        ),
    }

    for mapping, (dat_inject, dat_links, rsp_links, rsp_eject) in expected.items():
        resources = pm.ai_resource_flits(mapping, "write", 1, 1, "hardware")
        assert _resource_kind(resources, "dat", "inject") == dat_inject
        assert _resource_links(resources, "dat") == dat_links
        assert _resource_kind(resources, "dat", "eject") == _ALL_DAT_EJECT_TWO
        assert _resource_kind(resources, "rsp", "inject") == _ALL_RSP_INJECT_ONE
        assert _resource_links(resources, "rsp") == rsp_links
        assert _resource_kind(resources, "rsp", "eject") == rsp_eject


def test_global_repeated_unicast_injects_one_stream_per_remote_member():
    hardware = pm.ai_resource_flits(
        "broadcast_global", "write", 1, 1, "hardware")
    unicast = pm.ai_resource_flits(
        "broadcast_global", "write", 1, 1, "repeated_unicast")

    assert hardware["dat_inject_0"] == 2
    assert unicast["dat_inject_0"] == 30
    assert _resource_links(unicast, "dat") == {
        "dat_0to1": 24, "dat_1to2": 16, "dat_2to3": 8,
        "dat_0to4": 6, "dat_4to8": 4, "dat_8to12": 2,
        "dat_1to5": 6, "dat_5to9": 4, "dat_9to13": 2,
        "dat_2to6": 6, "dat_6to10": 4, "dat_10to14": 2,
        "dat_3to7": 6, "dat_7to11": 4, "dat_11to15": 2,
    }
    assert _resource_kind(unicast, "dat", "eject") == {
        "dat_eject_1": 2, "dat_eject_2": 2, "dat_eject_3": 2,
        "dat_eject_4": 2, "dat_eject_5": 2, "dat_eject_6": 2,
        "dat_eject_7": 2, "dat_eject_8": 2, "dat_eject_9": 2,
        "dat_eject_10": 2, "dat_eject_11": 2, "dat_eject_12": 2,
        "dat_eject_13": 2, "dat_eject_14": 2, "dat_eject_15": 2,
    }
    assert _resource_kind(unicast, "rsp", "inject") == {
        "rsp_inject_1": 1, "rsp_inject_2": 1, "rsp_inject_3": 1,
        "rsp_inject_4": 1, "rsp_inject_5": 1, "rsp_inject_6": 1,
        "rsp_inject_7": 1, "rsp_inject_8": 1, "rsp_inject_9": 1,
        "rsp_inject_10": 1, "rsp_inject_11": 1, "rsp_inject_12": 1,
        "rsp_inject_13": 1, "rsp_inject_14": 1, "rsp_inject_15": 1,
    }
    assert _resource_links(unicast, "rsp") == {
        "rsp_1to0": 3, "rsp_2to1": 2, "rsp_3to2": 1,
        "rsp_5to4": 3, "rsp_6to5": 2, "rsp_7to6": 1,
        "rsp_9to8": 3, "rsp_10to9": 2, "rsp_11to10": 1,
        "rsp_13to12": 3, "rsp_14to13": 2, "rsp_15to14": 1,
        "rsp_4to0": 12, "rsp_8to4": 8, "rsp_12to8": 4,
    }
    assert _resource_kind(unicast, "rsp", "eject") == {"rsp_eject_0": 15}


def test_ideal_bound_uses_logical_delivered_bytes_over_busiest_resource():
    assert pm.ideal_throughput_bound(
        "pipeline", "write", 2, 1, None) == 15 * 2 * 64 / 3
    assert pm.ideal_throughput_bound(
        "pipeline", "read", 2, 1, None) == 15 * 2 * 64 / 2
    assert pm.ideal_throughput_bound(
        "gather_global_root0", "write", 1, 1, None) == 15 * 64 / 30
    assert pm.ideal_throughput_bound(
        "broadcast_global", "write", 1, 1, "hardware") == 16 * 64 / 2
    assert pm.ideal_throughput_bound(
        "broadcast_global", "write", 1, 1, "repeated_unicast") == 16 * 64 / 30
