import pytest
import json

import dependent_dma_plan as d
import gen_tb_top


def test_layer_collectives_require_all_contributors_and_received_payloads():
    from dataclasses import replace
    from composite_dma_plan import layer_sequence, validate_reductions

    topo = gen_tb_top.load_topology("mesh_4x4_dual_edge_large")
    for operation in ("tp_layer_all_reduce", "tp_layer_sequence"):
        whole = layer_sequence(topo, 1024 * 1024, "write", operation)
        plan = d.build_plan(topo, 1024 * 1024, "write", operation)
        assert d.delivered_payload_bytes(plan) == 48 * 1024 * 1024
        assert len(plan.transfers) == 96
        assert len(dict.fromkeys(t.phase for t in plan.transfers)) == 4
        phases = list(dict.fromkeys(t.phase for t in whole.transfers))
        retired, producers = {}, {}
        for transfer in whole.transfers:
            retired[transfer.issuer] = retired.get(transfer.issuer, 0) + 1
            producers[transfer.destination] = (transfer.issuer, retired[transfer.issuer])
        for transfer in whole.transfers:
            phase_index = phases.index(transfer.phase)
            if transfer.local_partial and transfer.source in whole.initial and phase_index:
                previous = phases[phase_index - 1]
                assert previous.endswith("all_gather")
                waits = dict(d.requirements(transfer))
                for region, (producer, count) in producers.items():
                    if region.node not in (transfer.source.node, transfer.destination.node):
                        continue
                    if any(t.phase == previous and t.destination == region for t in whole.transfers):
                        assert waits.get(producer, 0) >= count
        if operation == "tp_layer_sequence":
            mlp_inputs = [t.source for t in whole.transfers
                          if t.phase == "prefill_mlp_all_gather"][:4]
            attention_outputs = [t.destination for t in whole.transfers
                                 if t.phase == "prefill_attention_reduce_scatter"][-4:]
            assert set(mlp_inputs) == set(attention_outputs)
        # All transferred regions retain their original chunk byte identity.
        memory = {r: r.shard for r in whole.initial}
        for transfer in whole.transfers:
            assert transfer.destination not in memory
            memory[transfer.destination] = memory[transfer.source]
        assert all(memory[r] == r.shard for r in whole.final)
        index = next(i for i, t in enumerate(whole.transfers) if t.local_partial)
        broken = list(whole.transfers)
        broken[index] = replace(broken[index], local_partial=None)
        with pytest.raises(ValueError, match="incomplete Reduce-Scatter"):
            validate_reductions(replace(whole, transfers=tuple(broken)))
        index = next(i for i, t in enumerate(whole.transfers)
                     if t.source not in whole.initial)
        broken = list(whole.transfers)
        broken[index] = replace(broken[index], prerequisite=None, barrier=())
        with pytest.raises(ValueError, match="missing forwarding"):
            validate_reductions(replace(whole, transfers=tuple(broken)))


def test_prefill_decode_passive_memory_and_phase_dependencies():
    from collections import Counter
    from composite_dma_plan import phase_results

    topo = gen_tb_top.load_topology("mesh_4x4_dual_edge_large")
    plan = d.build_plan(topo, 1024 * 1024, "write", "prefill_decode")
    sizes = Counter()
    for transfer in plan.transfers:
        sizes[transfer.phase] += transfer.source.length
        assert transfer.issuer in (8, 9, 12, 13)
    assert sizes["prefill_kv_store"] == sizes["decode_kv_read"] == 2 * 1024 * 1024
    assert sizes["decode_kv_append"] == 4096
    assert sum(v for k, v in sizes.items() if k.startswith("decode_") and "kv" not in k) == 96 * 1024
    stores = [t for t in plan.transfers if t.phase == "prefill_kv_store"]
    reads = [t for t in plan.transfers if t.phase == "decode_kv_read"]
    assert {t.source for t in reads} == {t.destination for t in stores}
    assert all(len(d.requirements(t)) == 4 for t in reads)
    assert all(t.issuer == t.destination.node and t.source.node == 16 for t in reads)
    counts, lines = Counter(), []
    for index, transfer in enumerate(plan.transfers):
        node, job = transfer.issuer, counts[transfer.issuer]
        counts[node] += 1
        for event, cycle in (("issue", index * 3), ("retire", index * 3 + 1)):
            lines.append(f"[dma_phase] node={node} job={job} phase={transfer.phase} event={event} cycle={cycle}")
    observed = phase_results("\n".join(lines), plan)
    assert {k: v["delivered_payload_bytes"] for k, v in observed.items()} == sizes
    with pytest.raises(ValueError, match="missing DMA phase"):
        phase_results("\n".join(lines[:-1]), plan)


@pytest.mark.parametrize("shared", [True, False])
def test_data_parallel_weights_preserve_replica_shards_and_read_issuers(tmp_path, shared):
    topo = gen_tb_top.load_topology("mesh_4x4_dual_edge_large")
    size = 2 * 1024 * 1024
    operation = "dp_shared_weights" if shared else "dp_separate_weights"
    whole = d.data_parallel_weights(topo, size, "read", shared=shared)
    plan = d.build_plan(topo, size, "read", operation)
    groups, bases, sizes = d.backing_layout(topo)
    assert len(whole.initial) == (4 if shared else 16)
    assert len(whole.transfers) == 16 and len(plan.transfers) == 64
    assert d.result_header(plan)["logical_bytes"] == 4 * size
    assert d.delivered_payload_bytes(plan) == 16 * size
    for group, (_, _, consumers) in enumerate(groups):
        memory_node, memory, _ = groups[0 if shared else group]
        for rank, (node, cid) in enumerate(consumers):
            transfer = whole.transfers[group * 4 + rank]
            offset = 0x1000 + rank * size
            assert transfer.source == d.Region(memory_node, memory["base"] + offset, size, rank)
            assert transfer.destination == d.Region(node, bases[cid] + offset, size, rank)
            assert transfer.source in whole.initial and transfer.destination not in whole.initial
            assert offset + size <= min(memory["size"], sizes[cid])
            parts = [t for t in plan.transfers if t.issuer == node]
            assert sum(t.source.length for t in parts) == size
            for index, part in enumerate(parts):
                assert part.source.address == transfer.source.address + index * (size // 4)
                assert part.destination.address == transfer.destination.address + index * (size // 4)
                assert part.source.shard == part.destination.shard == rank
                assert not d.requirements(part) and not part.replicas and part.user == 0
    d.emit(plan, tmp_path, 20)
    for node in range(16, 20):
        assert (tmp_path / f"node{node}/jobs.txt").read_text() == ""
    with pytest.raises(ValueError, match="tile-issued read"):
        d.build_plan(topo, size, "write", operation)
    with pytest.raises(ValueError, match="memory window"):
        d.build_plan(topo, 4 * size, "read", operation)


@pytest.mark.parametrize("direction", ["read", "write"])
def test_pipeline_batches_preserve_bytes_and_full_batch_waits(tmp_path, direction):
    topo = gen_tb_top.load_topology("mesh_4x4")
    size = 1024 * 1024
    serial = d.build_plan(topo, size, direction, "pp_serial")
    overlap = d.build_plan(topo, size, direction, "pp_overlap")
    assert serial.initial == overlap.initial and serial.final == overlap.final
    assert len(serial.initial) == 16 and len(serial.final) == 64
    assert len(serial.transfers) == len(overlap.transfers) == 96
    assert d.delivered_payload_bytes(serial) == 48 * size
    assert [(t.issuer, t.source, t.destination, t.prerequisite) for t in serial.transfers] == [
        (t.issuer, t.source, t.destination, t.prerequisite) for t in overlap.transfers]
    assert not any(t.barrier for t in overlap.transfers)
    final_issuers = {t.issuer for t in serial.transfers[16:24]}
    for batch in range(d.PP_BATCH_COUNT):
        for t in serial.transfers[batch * 24:batch * 24 + 8]:
            assert set(t.barrier) == ({(node, batch * 2) for node in final_issuers} if batch else set())
        for t in serial.transfers[batch * 24 + 8:(batch + 1) * 24]:
            assert not t.barrier
            assert t.prerequisite[1] == (batch + 1) * 2
    d.emit(serial, tmp_path, 16)
    for node in range(16):
        records = (tmp_path / f"node{node}/dependencies.txt").read_text().splitlines()
        for line, t in zip(records, (t for t in serial.transfers if t.issuer == node)):
            fields = list(map(int, line.split()))
            assert len(fields) == 1 + fields[0] * 2
            assert tuple(zip(fields[1::2], fields[2::2])) == d.requirements(t)


def test_kv_tile_roundtrip_uses_passive_memory_and_preserves_dependencies(tmp_path):
    topo = gen_tb_top.load_topology("mesh_4x4_dual_edge")
    plan = d.build_plan(topo, 4096, "write", "kv_tile_roundtrip")
    previous = d.build_plan(topo, 4096, "write", "kv_roundtrip")
    assert plan.initial == previous.initial and plan.final == previous.final
    assert len(plan.transfers) == 32
    for offload, restore in zip(plan.transfers[:16], plan.transfers[16:]):
        assert offload.issuer == offload.source.node < 16
        assert offload.destination.node >= 16
        assert restore.issuer == restore.destination.node == offload.issuer
        assert restore.source == offload.destination
        assert restore.destination != offload.source
        assert restore.prerequisite == (offload.issuer, 1)
    d.emit(plan, tmp_path, 20)
    for node in range(16):
        assert (tmp_path / f"node{node}/dependencies.txt").read_text().splitlines() == [
            "0", f"1 {node} 1"]
    for node in range(16, 20):
        assert (tmp_path / f"node{node}/jobs.txt").read_text() == ""
    with pytest.raises(ValueError, match="starts with write"):
        d.build_plan(topo, 4096, "read", "kv_tile_roundtrip")


@pytest.mark.parametrize("operation", ["shared_independent", "shared_fetch_unicast", "shared_fetch_multicast"])
def test_large_shared_descriptors_preserve_whole_object_dependencies(operation):
    topo = gen_tb_top.load_topology("mesh_4x4_dual_edge_large")
    for size in (1048575, 1048576, 8388608):
        original = d.OPERATIONS[operation](topo, size, "read")
        plan = d.build_plan(topo, size, "read", operation)
        assert (plan.initial, plan.final) == (original.initial, original.final)
        assert d.delivered_payload_bytes(plan) == d.delivered_payload_bytes(original)
        remaining = iter(plan.transfers)
        original_counts, descriptor_counts, completion = {}, {}, {}
        for transfer in original.transfers:
            offset = 0
            expected_dep = completion[transfer.prerequisite] if transfer.prerequisite else None
            while offset < size:
                descriptor = next(remaining)
                assert 0 < descriptor.source.length <= 1048575
                assert descriptor.issuer == transfer.issuer
                assert descriptor.prerequisite == expected_dep
                assert descriptor.user == transfer.user
                assert len(descriptor.replicas) == len(transfer.replicas)
                for part, whole in zip((descriptor.source, descriptor.destination) + descriptor.replicas,
                                       (transfer.source, transfer.destination) + transfer.replicas):
                    assert part == d.replace(whole, address=whole.address + offset,
                                             length=descriptor.source.length)
                offset += descriptor.source.length
                descriptor_counts[transfer.issuer] = descriptor_counts.get(transfer.issuer, 0) + 1
            assert offset == size
            original_counts[transfer.issuer] = original_counts.get(transfer.issuer, 0) + 1
            completion[transfer.issuer, original_counts[transfer.issuer]] = (
                transfer.issuer, descriptor_counts[transfer.issuer])
        assert next(remaining, None) is None


@pytest.mark.parametrize("direction", ["read", "write"])
@pytest.mark.parametrize("shard_bytes", [1, 64, 4096])
@pytest.mark.parametrize("operation", ["all_gather", "all_reduce_transport"])
def test_ring_copies_actual_predecessor_data(direction, shard_bytes, operation):
    plan = d.build_plan(gen_tb_top.load_topology("mesh_4x4"), shard_bytes, direction, operation)
    passes = 2 if operation == "all_reduce_transport" else 1
    assert len(plan.initial) == 4
    assert len(plan.transfers) == 12 * passes
    assert len(plan.final) == 4 + 12 * passes
    memory = {(r.node, r.address): bytes([r.shard + 1]) * r.length for r in plan.initial}
    completion = {}
    produced = {}
    edges = {(8, 9), (9, 13), (13, 12), (12, 8)}
    for index, transfer in enumerate(plan.transfers):
        src, dst = transfer.source, transfer.destination
        assert (src.node, dst.node) in edges
        assert transfer.issuer == (src.node if direction == "write" else dst.node)
        assert src.shard == dst.shard
        source_key = (src.node, src.address)
        if index < 4:
            assert transfer.prerequisite is None
        else:
            # The prerequisite must name the job that wrote this exact slot.
            assert transfer.prerequisite == produced[source_key]
            node, count = transfer.prerequisite
            assert completion[node] >= count
        memory[dst.node, dst.address] = memory[source_key]
        completion[transfer.issuer] = completion.get(transfer.issuer, 0) + 1
        produced[dst.node, dst.address] = (transfer.issuer, completion[transfer.issuer])
    for region in plan.final:
        assert memory[region.node, region.address] == bytes([region.shard + 1]) * shard_bytes
    assert completion == {node: 3 * passes for node in (8, 9, 12, 13)}
    if passes == 2:
        assert {t.source for t in plan.transfers[12:16]} == {
            t.destination for t in plan.transfers[8:12]}
        assert {t.destination for t in plan.transfers[12:]}.isdisjoint(
            {t.destination for t in plan.transfers[:12]} | set(plan.initial))


@pytest.mark.parametrize("direction", ["read", "write"])
def test_expert_roundtrip_preserves_owner_and_waits_for_dispatch(direction):
    plan = d.build_plan(gen_tb_top.load_topology("mesh_4x4"), 4096, direction,
                        "expert_roundtrip")
    assert [(t.source.node, t.destination.node) for t in plan.transfers] == [
        (8, 10), (8, 2), (9, 10), (9, 2), (10, 8), (2, 8), (10, 9), (2, 9)]
    memory = {r: r.shard for r in plan.initial}
    producers, counts = {}, {}
    for transfer in plan.transfers:
        assert transfer.issuer == (transfer.source.node if direction == "write"
                                   else transfer.destination.node)
        assert transfer.prerequisite == producers.get(transfer.source)
        assert transfer.destination not in memory
        memory[transfer.destination] = memory[transfer.source]
        counts[transfer.issuer] = counts.get(transfer.issuer, 0) + 1
        producers[transfer.destination] = (transfer.issuer, counts[transfer.issuer])
    assert all(memory[r] == r.shard for r in plan.final)
    assert len(plan.initial) == 4 and len(plan.final) == 12
    assert d.delivered_payload_bytes(plan) == 8 * 4096
    with pytest.raises(ValueError, match="memory window"):
        d.expert_roundtrip(gen_tb_top.load_topology("mesh_4x4"), 0x400000, direction)


def test_read_write_preserve_payload_edges_and_storage():
    topo = gen_tb_top.load_topology("mesh_4x4")
    read = d.all_gather(topo, 64, "read")
    write = d.all_gather(topo, 64, "write")
    assert read.initial == write.initial
    assert read.final == write.final
    assert [(t.source, t.destination) for t in read.transfers] == [
        (t.source, t.destination) for t in write.transfers]


@pytest.mark.parametrize("size", [0, -1, True, 1.5, 0x2000000])
def test_rejects_invalid_or_overflowing_shards(size):
    with pytest.raises(ValueError):
        d.all_gather(gen_tb_top.load_topology("mesh_4x4"), size, "write")


def test_rejects_other_mapping_and_direction():
    with pytest.raises(ValueError, match="4x4"):
        d.all_gather(gen_tb_top.load_topology("mesh_2x2"), 64, "write")
    with pytest.raises(ValueError, match="direction"):
        d.all_gather(gen_tb_top.load_topology("mesh_4x4"), 64, "both")


@pytest.mark.parametrize("direction", ["read", "write"])
def test_emitted_jobs_and_dependencies_match_plan(tmp_path, direction):
    d.main(["--out", str(tmp_path), "--shard-bytes", "0x40", "--direction", direction])
    plan = d.all_gather(gen_tb_top.load_topology("mesh_4x4"), 64, direction)
    for node in range(16):
        jobs = (tmp_path / f"node{node}" / "jobs.txt").read_text().split()
        dependencies = (tmp_path / f"node{node}" / "dependencies.txt").read_text().splitlines()
        expected = [t for t in plan.transfers if t.issuer == node]
        assert len(jobs) == len(expected) * 11
        assert len(dependencies) == len(expected)
        for index, transfer in enumerate(expected):
            fields = jobs[index * 11:(index + 1) * 11]
            assert [int(value, 0) for value in fields[:3]] == [
                64, transfer.source.address, transfer.destination.address]
            assert tuple(map(int, dependencies[index].split())) == (
                (1, *transfer.prerequisite) if transfer.prerequisite else (0,))
    manifest = json.loads((tmp_path / "dependent_plan.json").read_text())
    assert len(manifest["initial"]) == 4
    assert len(manifest["final"]) == 16


def _result():
    return {"schema": 1, "operation": "all_gather", "test_layer": "L2",
            "start_event": "first_job_valid", "end_event": "last_dma_response",
            "logical_bytes": 256, "delivered_payload_bytes": 768,
            "expected_transfers": 12, "checked_bytes": 1024, "status": "PASS",
            "start_cycle": 3, "end_cycle": 12, "duration_cycles": 10}


def test_operation_bandwidth_uses_delivery_bytes_and_operation_window():
    plan = d.all_gather(gen_tb_top.load_topology("mesh_4x4"), 64, "write")
    assert d.validate_result(_result(), plan) == 76.8


@pytest.mark.parametrize("key,value", [
    ("start_cycle", -1), ("start_cycle", True), ("end_cycle", 1),
    ("duration_cycles", 0), ("duration_cycles", 9),
    ("logical_bytes", 768), ("delivered_payload_bytes", 1024),
    ("checked_bytes", 0), ("expected_transfers", 0), ("status", "FAIL"),
    ("start_event", "reset"), ("end_event", "simulation_end"), ("schema", True),
])
def test_rejects_invalid_operation_measurements(key, value):
    result = _result()
    result[key] = value
    plan = d.all_gather(gen_tb_top.load_topology("mesh_4x4"), 64, "write")
    with pytest.raises(ValueError):
        d.validate_result(result, plan)


def test_router_window_has_same_sample_count_as_operation():
    perf = {"window": {"start_cyc": 3, "end_cyc": 13},
            "noc": {"links": [{"name": "dat_8to9", "flit_count": 4, "stall_cyc": 2}]}}
    d.validate_perf_window(_result(), perf)
    perf["window"]["end_cyc"] = 12
    with pytest.raises(ValueError, match="different operation window"):
        d.validate_perf_window(_result(), perf)


@pytest.mark.parametrize("flits,stalls", [(0, 0), (11, 0), (1, -1), (True, 1)])
def test_rejects_vacuous_or_out_of_window_link_counters(flits, stalls):
    perf = {"window": {"start_cyc": 3, "end_cyc": 13},
            "noc": {"links": [{"flit_count": flits, "stall_cyc": stalls}]}}
    with pytest.raises(ValueError):
        d.validate_perf_window(_result(), perf)


@pytest.mark.parametrize("direction", ["read", "write"])
def test_pipeline_preserves_matching_shards_and_waits_for_producer(direction):
    plan = d.pipeline(gen_tb_top.load_topology("mesh_4x4"), 64, direction)
    assert plan.operation == "pp_forward"
    expected = [(8, 10), (9, 11), (12, 14), (13, 15),
                (10, 2), (11, 3), (14, 6), (15, 7),
                (2, 0), (3, 1), (6, 4), (7, 5)]
    assert [(t.source.node, t.destination.node) for t in plan.transfers] == expected
    memory = {(r.node, r.address): r.shard for r in plan.initial}
    producer = {}
    for index, transfer in enumerate(plan.transfers):
        src, dst = transfer.source, transfer.destination
        key = (src.node, src.address)
        assert transfer.issuer == (src.node if direction == "write" else dst.node)
        assert transfer.prerequisite == (None if index < 4 else producer[key])
        memory[dst.node, dst.address] = memory[key]
        producer[dst.node, dst.address] = (transfer.issuer, 1)
    assert len(plan.final) == 16
    for region in plan.final:
        assert memory[region.node, region.address] == region.shard


@pytest.mark.parametrize("direction", ["read", "write"])
def test_kv_handoff_copies_matching_layers_and_preserves_source(direction):
    plan = d.kv_handoff(gen_tb_top.load_topology("mesh_4x4"), 64, direction)
    assert plan.operation == "kv_handoff"
    expected = [(8, 0), (9, 1), (12, 4), (13, 5),
                (10, 2), (11, 3), (14, 6), (15, 7)]
    assert [(t.source.node, t.destination.node) for t in plan.transfers] == expected
    assert len(plan.initial) == 8
    assert len(plan.final) == 16
    memory = {(r.node, r.address): r.shard for r in plan.initial}
    originals = memory.copy()
    for transfer in plan.transfers:
        src, dst = transfer.source, transfer.destination
        assert transfer.prerequisite is None
        assert transfer.issuer == (src.node if direction == "write" else dst.node)
        memory[dst.node, dst.address] = memory[src.node, src.address]
    for key, value in originals.items():
        assert memory[key] == value
    for region in plan.final:
        assert memory[region.node, region.address] == region.shard


@pytest.mark.parametrize("direction", ["read", "write"])
def test_weight_load_routes_distinct_backing_shards(tmp_path, direction):
    topo = gen_tb_top.load_topology("mesh_4x4_dual_edge")
    plan = d.weight_load(topo, 4096, direction)
    groups = ((8, 9, 12, 13), (10, 11, 14, 15),
              (2, 3, 6, 7), (0, 1, 4, 5))
    assert plan.operation == "weight_load"
    assert len(plan.initial) == len(plan.transfers) == 16
    assert len(plan.final) == 32
    for shard, transfer in enumerate(plan.transfers):
        source, destination = transfer.source, transfer.destination
        assert source.node == 16 + shard // 4
        assert destination.node == groups[shard // 4][shard % 4]
        assert source.shard == destination.shard == shard
        assert source.length == destination.length == 4096
        assert transfer.issuer == (source.node if direction == "write" else destination.node)
        assert transfer.prerequisite is None
    d.main(["--topology", "mesh_4x4_dual_edge", "--operation", "weight_load",
            "--out", str(tmp_path), "--shard-bytes", "4096", "--direction", direction])
    for node in range(20):
        expected = sum(t.issuer == node for t in plan.transfers)
        assert len((tmp_path / f"node{node}" / "jobs.txt").read_text().split()) == expected * 11
        assert len((tmp_path / f"node{node}" / "dependencies.txt").read_text().splitlines()) == expected


def test_weight_load_rejects_unapproved_boundary_mapping():
    for config in ("mesh_4x4", "mesh_4x4_periph4"):
        with pytest.raises(ValueError, match="dual-edge"):
            d.weight_load(gen_tb_top.load_topology(config), 64, "read")


@pytest.mark.parametrize("operation", ["shared_independent", "shared_fetch_unicast", "shared_fetch_multicast"])
def test_shared_input_uses_one_backing_slot_per_group(operation):
    topo = gen_tb_top.load_topology("mesh_4x4_dual_edge")
    plan = d.build_plan(topo, 512 * 1024, "read", operation)
    assert len(plan.initial) == 4
    assert all(region.length == 512 * 1024 for region in plan.initial)
    with pytest.raises(ValueError, match="memory window"):
        d.build_plan(topo, 1024 * 1024, "read", operation)


def test_weight_load_checks_boundary_aperture():
    with pytest.raises(ValueError, match="memory window"):
        d.weight_load(gen_tb_top.load_topology("mesh_4x4_dual_edge"), 0x100000, "read")


@pytest.mark.parametrize("resident", [(), (0, 4, 8, 12), tuple(range(16))])
def test_weight_residency_loads_only_missing_shards(tmp_path, resident):
    topo = gen_tb_top.load_topology("mesh_4x4_dual_edge")
    plan = d.build_plan(topo, 4096, "read", "weight_load", resident)
    assert {t.destination.shard for t in plan.transfers} == set(range(16)) - set(resident)
    assert len(plan.initial) == 16 + len(resident)
    assert len(plan.final) == 32
    header = d.result_header(plan)
    assert header["logical_bytes"] == 65536
    assert header["delivered_payload_bytes"] == (16 - len(resident)) * 4096
    d.emit(plan, tmp_path, 20)
    for node in range(20):
        expected_jobs = sum(t.issuer == node for t in plan.transfers)
        assert len((tmp_path / f"node{node}" / "dependencies.txt").read_text().splitlines()) == expected_jobs
    if resident:
        assert header["resident_shards"] == list(resident)


def test_resident_ready_is_checked_without_network_work():
    plan = d.build_plan(gen_tb_top.load_topology("mesh_4x4_dual_edge"),
                        4096, "read", "weight_load", tuple(range(16)))
    result = {**d.result_header(plan), "start_cycle": 1, "end_cycle": 1,
              "duration_cycles": 1, "checked_bytes": 131072, "status": "PASS"}
    assert result["start_event"] == "residency_check"
    assert result["end_event"] == "resident_ready"
    assert d.validate_result(result, plan) == 0
    perf = {"window": {"start_cyc": 1, "end_cyc": 2},
            "noc": {"links": [{"flit_count": 0, "stall_cyc": 0}]}}
    d.validate_perf_window(result, perf)
    perf["noc"]["links"][0]["flit_count"] = 1
    with pytest.raises(ValueError, match="traffic disagrees"):
        d.validate_perf_window(result, perf)


@pytest.mark.parametrize("resident", [(0, 0), (-1,), (16,), (True,)])
def test_rejects_invalid_resident_shards(resident):
    with pytest.raises(ValueError, match="distinct valid"):
        d.build_plan(gen_tb_top.load_topology("mesh_4x4_dual_edge"),
                     4096, "read", "weight_load", resident)


@pytest.mark.parametrize("direction", ["read", "write"])
def test_kv_roundtrip_restores_received_bytes_after_offload(direction):
    plan = d.kv_roundtrip(gen_tb_top.load_topology("mesh_4x4_dual_edge"), 4096, direction)
    assert len(plan.initial) == 16
    assert len(plan.transfers) == 32
    assert len(plan.final) == 48
    memory = {(r.node, r.address): r.shard for r in plan.initial}
    originals = set(memory)
    counts, producers = {}, {}
    for index, transfer in enumerate(plan.transfers):
        src, dst = transfer.source, transfer.destination
        source_key = (src.node, src.address)
        destination_key = (dst.node, dst.address)
        assert destination_key not in originals
        assert transfer.issuer == (src.node if direction == "write" else dst.node)
        assert transfer.prerequisite == (None if index < 16 else producers[source_key])
        if index >= 16:
            assert dst.node == plan.initial[index - 16].node
            assert dst.shard == plan.initial[index - 16].shard
        memory[destination_key] = memory[source_key]
        counts[transfer.issuer] = counts.get(transfer.issuer, 0) + 1
        producers[destination_key] = (transfer.issuer, counts[transfer.issuer])
    for region in plan.final:
        assert memory[region.node, region.address] == region.shard


@pytest.mark.parametrize("direction", ["read", "write"])
def test_consumer_waits_for_actual_restore_and_excludes_local_bytes(direction):
    plan = d.kv_restore_consume(gen_tb_top.load_topology("mesh_4x4_dual_edge"), 4096, direction)
    assert len(plan.transfers) == 48
    assert len(plan.final) == 64
    assert d.delivered_payload_bytes(plan) == 32 * 4096
    counts, producers = {}, {}
    memory = {(r.node, r.address): r.shard for r in plan.initial}
    for index, transfer in enumerate(plan.transfers):
        src, dst = transfer.source, transfer.destination
        if index >= 32:
            assert src.node == dst.node == transfer.issuer
            assert transfer.prerequisite == producers[src]
            assert dst.address != src.address
        memory[dst.node, dst.address] = memory[src.node, src.address]
        counts[transfer.issuer] = counts.get(transfer.issuer, 0) + 1
        producers[dst] = (transfer.issuer, counts[transfer.issuer])
    for region in plan.final:
        assert memory[region.node, region.address] == region.shard


@pytest.mark.parametrize("direction", ["read", "write"])
def test_shared_fetch_preserves_inputs_and_consumers(direction):
    topo = gen_tb_top.load_topology("mesh_4x4_dual_edge")
    independent = d.shared_independent(topo, 4096, direction)
    shared = d.shared_fetch_unicast(topo, 4096, direction)
    assert independent.initial == shared.initial
    assert independent.final == shared.final
    assert len(shared.initial) == 4
    assert len(shared.final) == 20
    assert len(independent.transfers) == len(shared.transfers) == 16
    bases, _ = d.address_map.pack_config(topo)
    nodes, _, _ = gen_tb_top._nodes(topo)
    node_bases = {index: bases[cid] for index, _x, _y, cid in nodes}
    for shard in range(4):
        consumers = [r for r in shared.final if r.node < 16 and r.shard == shard]
        assert len({r.address - node_bases[r.node] for r in consumers}) == 1
    for plan, backing_copies in ((independent, 16), (shared, 4)):
        assert sum(t.source.node >= 16 for t in plan.transfers) == backing_copies
        memory = {(r.node, r.address): r.shard for r in plan.initial}
        counts, producers = {}, {}
        for transfer in plan.transfers:
            src, dst = transfer.source, transfer.destination
            assert transfer.issuer == (src.node if direction == "write" or src.node < 16 else dst.node)
            assert transfer.prerequisite == (None if src.node >= 16 else producers[src])
            memory[dst.node, dst.address] = memory[src.node, src.address]
            counts[transfer.issuer] = counts.get(transfer.issuer, 0) + 1
            producers[dst] = (transfer.issuer, counts[transfer.issuer])
        for region in plan.final:
            assert memory[region.node, region.address] == region.shard


def _memory_log():
    lines = []
    for node in range(20):
        for target in range(2):
            active = node >= 16 and target == 0
            lines.append(f"[memory_reads] node{node} target{target}: requests={4 if active else 0} "
                         f"requested_bytes={16384 if active else 0} response_beats={256 if active else 0}")
    return "\n".join(lines)


def test_shared_multicast_delivers_full_group_including_issuer(tmp_path):
    topo = gen_tb_top.load_topology("mesh_4x4_dual_edge")
    plan = d.shared_fetch_multicast(topo, 4096, "read")
    baseline = d.shared_fetch_unicast(topo, 4096, "read")
    assert plan.initial == baseline.initial
    assert plan.final == baseline.final
    assert len(plan.transfers) == 4
    assert d.delivered_payload_bytes(plan) == d.delivered_payload_bytes(baseline) == 65536
    nodes, _, _ = gen_tb_top._nodes(topo)
    bases, _ = d.address_map.pack_config(topo)
    node_bases = {index: bases[cid] for index, _x, _y, cid in nodes}
    memory = {(r.node, r.address): r.shard for r in plan.initial}
    for transfer in plan.transfers:
        destinations = (transfer.destination,) + transfer.replicas
        if transfer.user:
            assert transfer.source.node >= 16
            assert transfer.issuer != transfer.destination.node
            mask = transfer.user >> 10
            delivered = {node for node, base in node_bases.items()
                         if base & ~mask == node_bases[transfer.destination.node] & ~mask}
            assert delivered == {r.node for r in destinations}
            assert len(delivered) == 4
            assert transfer.issuer in delivered
            assert transfer.prerequisite is None
        for destination in destinations:
            memory[destination.node, destination.address] = memory[transfer.source.node, transfer.source.address]
    for region in plan.final:
        assert memory[region.node, region.address] == region.shard
    d.emit(plan, tmp_path, 20)
    for node in range(20):
        assert list(map(int, (tmp_path / f"node{node}" / "users.txt").read_text().split())) == [
            t.user for t in plan.transfers if t.issuer == node]


def test_shared_multicast_rejects_memory_issued_write():
    topo = gen_tb_top.load_topology("mesh_4x4_dual_edge")
    with pytest.raises(ValueError, match="tile-issued read"):
        d.shared_fetch_multicast(topo, 4096, "write")


def test_memory_read_accounting_checks_actual_sources(monkeypatch):
    topo = gen_tb_top.load_topology("mesh_4x4_dual_edge")
    plan = d.shared_independent(topo, 4096, "read")
    monkeypatch.setattr(gen_tb_top, "_constant", lambda *_: 512)
    assert d.validate_memory_reads(_memory_log(), plan, topo) == {
        "backing_requested_bytes": 65536, "tile_requested_bytes": 0}


def test_operation_collector_reads_counters_and_rejects_bad_provenance(tmp_path):
    import emit_result_manifest as provenance
    import perf_report

    plan = d.shared_independent(gen_tb_top.load_topology("mesh_4x4_dual_edge"), 4096, "read")
    result = {**d.result_header(plan), "start_cycle": 1, "end_cycle": 10000,
              "duration_cycles": 10000, "checked_bytes": 81920, "status": "PASS"}
    perf = {"window": {"start_cyc": 1, "end_cyc": 10001}, "noc": {"links": [
        {"name": "dat_node1.y_to_node1.router", "flit_count": 256, "stall_cyc": 0},
        {"name": "dat_1to5", "flit_count": 128, "stall_cyc": 0},
        {"name": "dat_eject_5", "flit_count": 64, "stall_cyc": 0}]}}
    (tmp_path / "source.patch").write_text("fixture source snapshot\n")
    manifest = {
        "exact_command": "make sim DMA=1 DMA_DEPENDENT=1 CONFIG=mesh_4x4_dual_edge "
                         "DMA_LENGTH=4096 DMA_RW=read DMA_OPERATION=shared_independent "
                         "DMA_BACKPRESSURE=1 SEED=1",
        "seed": 1,
        "config_file_sha256": provenance._sha256(gen_tb_top.ROOT / "sim/configs/mesh_4x4_dual_edge.yml"),
        "generated_parameter_sha256": provenance._parameter_sha256(gen_tb_top.ROOT),
        "source_patch_sha256": provenance._sha256(tmp_path / "source.patch")}
    for name, data in (("operation", result), ("perf", perf), ("manifest", manifest)):
        (tmp_path / f"{name}.json").write_text(json.dumps(data))
    (tmp_path / "run.log").write_text(d.pass_marker(plan) + "\n" + _memory_log())
    row, = perf_report.collect_operations([tmp_path])
    assert row["backing_requested_bytes"] == 65536
    assert row["dat_injected_flits"] == 256
    assert row["dat_flit_hops"] == 128
    export = tmp_path / "export" / "records.json"
    perf_report.main(["--operations", str(tmp_path), "-o", str(export)])
    exported = json.loads(export.read_text())
    record, = exported["records"]
    assert (record["backing_requested_bytes"], record["dat_injected_flits"],
            record["dat_flit_hops"]) == (65536, 256, 128)
    assert (export.parent / record["directory"]).resolve() == tmp_path.resolve()
    accepted_export = export.read_bytes()
    (tmp_path / "source.patch").write_text("changed snapshot\n")
    with pytest.raises(ValueError, match="provenance mismatch"):
        perf_report.collect_operations([tmp_path])
    with pytest.raises(ValueError, match="provenance mismatch"):
        perf_report.main(["--operations", str(tmp_path), "-o", str(export)])
    assert export.read_bytes() == accepted_export


@pytest.mark.parametrize("fault", ["missing", "duplicate", "bytes", "beats", "requests", "malformed"])
def test_memory_read_accounting_rejects_invalid_evidence(monkeypatch, fault):
    topo = gen_tb_top.load_topology("mesh_4x4_dual_edge")
    plan = d.shared_independent(topo, 4096, "read")
    monkeypatch.setattr(gen_tb_top, "_constant", lambda *_: 512)
    log = _memory_log()
    if fault == "missing":
        log = "\n".join(log.splitlines()[1:])
    elif fault == "duplicate":
        log += "\n" + log.splitlines()[0]
    elif fault == "bytes":
        log = log.replace("requested_bytes=16384", "requested_bytes=4096", 1)
    elif fault == "beats":
        log = log.replace("response_beats=256", "response_beats=255", 1)
    elif fault == "requests":
        log = log.replace("requests=4", "requests=0", 1)
    else:
        log = log.replace("requests=4", "requests=-1", 1)
    with pytest.raises(ValueError):
        d.validate_memory_reads(log, plan, topo)


@pytest.mark.parametrize("tensor_bytes", [64, 4 * 1024 * 1024])
@pytest.mark.parametrize("operation,factor,objects", [
    ("broadcast", 3, 1), ("scatter", 0.75, 1), ("gather", 0.75, 1),
    ("collective_all_gather", 3, 1), ("reduce", 3, 1),
    ("all_reduce", 6, 1), ("reduce_scatter", 3, 1), ("point_to_point", 4, 4),
])
def test_standalone_operation_ownership_and_accounting(tensor_bytes, operation, factor, objects):
    topo = gen_tb_top.load_topology("mesh_4x4_dual_edge_large")
    whole = d.standalone_operation(topo, tensor_bytes, "write", operation)
    plan = d.build_plan(topo, tensor_bytes, "write", operation)
    assert d.result_header(plan)["logical_bytes"] == tensor_bytes * objects
    assert d.delivered_payload_bytes(plan) == tensor_bytes * factor
    assert all(t.issuer == t.source.node for t in plan.transfers)
    assert all(t.source.length < (1 << 20) for t in plan.transfers)
    # Independent ownership simulation. Every forwarded source must already
    # exist and wait for its producer, and a contribution can be consumed once.
    owners = {r: {r.node} for r in whole.initial}
    produced, counts = {}, {}
    for t in whole.transfers:
        assert t.source in owners
        assert t.destination not in owners
        if t.source in produced:
            node, count = produced[t.source]
            assert dict(d.requirements(t)).get(node, 0) >= count
        contributors = owners[t.source].copy()
        if t.local_partial:
            assert not contributors & owners[t.local_partial]
            contributors |= owners[t.local_partial]
        owners[t.destination] = contributors
        counts[t.issuer] = counts.get(t.issuer, 0) + 1
        produced[t.destination] = t.issuer, counts[t.issuer]
    assert set(whole.final) <= owners.keys()
    participants = {8, 9, 12, 13}
    if operation in ("all_reduce", "reduce_scatter", "reduce"):
        complete = [r for r, sources in owners.items() if sources == participants]
        expected = {8} if operation == "reduce" else participants
        assert {r.node for r in complete} == expected
        for node in expected:
            assert sum(r.length for r in complete if r.node == node) == (
                tensor_bytes // 4 if operation == "reduce_scatter" else tensor_bytes)
    elif operation in ("broadcast", "collective_all_gather"):
        for node in participants:
            assert sum(r.length for r in whole.final if r.node == node) == tensor_bytes
    elif operation == "gather":
        assert sum(r.length for r in whole.final if r.node == 8) == tensor_bytes
    elif operation == "scatter":
        assert all(t.source.shard == t.destination.shard for t in whole.transfers)
        assert {t.destination.node for t in whole.transfers} == {9, 12, 13}
    else:
        assert {(t.source.node, t.destination.node) for t in whole.transfers} == {
            (2, 0), (3, 1), (6, 4), (7, 5)}
        assert not any(d.requirements(t) for t in whole.transfers)


@pytest.mark.parametrize("operation", ["reduce", "reduce_scatter", "all_reduce"])
def test_standalone_reduction_rejects_missing_contribution_and_forward_wait(operation):
    from dataclasses import replace
    from composite_dma_plan import validate_reductions
    topo = gen_tb_top.load_topology("mesh_4x4_dual_edge_large")
    plan = d.standalone_operation(topo, 64, "write", operation)
    for field in ("partial", "wait"):
        transfers = list(plan.transfers)
        i = next(i for i, t in enumerate(transfers)
                 if (t.local_partial if field == "partial" else t.prerequisite))
        transfers[i] = (replace(transfers[i], local_partial=None) if field == "partial"
                        else replace(transfers[i], prerequisite=None, barrier=()))
        with pytest.raises(ValueError, match="incomplete Reduce-Scatter|missing forwarding"):
            validate_reductions(replace(plan, transfers=tuple(transfers)), {8, 9, 12, 13})


@pytest.mark.parametrize("length,direction", [(0, "write"), (True, "write"),
                                              (63, "write"), (64, "read")])
def test_standalone_rejects_ambiguous_tensor_or_driver(length, direction):
    with pytest.raises(ValueError):
        d.standalone_operation(gen_tb_top.load_topology("mesh_4x4_dual_edge_large"),
                               length, direction, "scatter")
