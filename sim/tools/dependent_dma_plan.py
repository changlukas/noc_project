"""Dependent DMA transfers for the report's collective and PP mappings.

Only original shards are initialized. Later transfers read the destination
of an earlier transfer after that transfer retires. This module defines the
stimulus contract, not a simulator or a claim of TB support.
"""

import argparse
from dataclasses import asdict, dataclass, replace
import json
import re
from pathlib import Path

import address_map
import gen_tb_top
import gen_dma_jobs
import gen_test_patterns


@dataclass(frozen=True)
class Region:
    node: int
    address: int
    length: int
    shard: int
    offset: int = 0


@dataclass(frozen=True)
class Transfer:
    issuer: int
    source: Region
    destination: Region
    # None for original data, otherwise (issuer, retired job count).
    prerequisite: tuple[int, int] | None
    user: int = 0
    replicas: tuple[Region, ...] = ()
    # Additional retirements required before issuing, used by full-batch waits.
    barrier: tuple[tuple[int, int], ...] = ()
    phase: str = ""
    # Local partial consumed at a transport-only reduction boundary.
    local_partial: Region | None = None


@dataclass(frozen=True)
class Plan:
    initial: tuple[Region, ...]
    transfers: tuple[Transfer, ...]
    final: tuple[Region, ...]
    operation: str = "all_gather"
    resident_shards: tuple[int, ...] = ()
    outputs: tuple[Region, ...] = ()
    contract: dict | None = None


def all_gather(topo, shard_bytes, direction, *, base_offset=0x1000):
    """Build one operation on approved ring A0, A1, A3, A2.

    Read and Write preserve payload direction. Only the DMA issuer changes.
    Each shard retains its own slot at every participant, including its owner.
    """
    if isinstance(shard_bytes, bool) or not isinstance(shard_bytes, int) or shard_bytes <= 0:
        raise ValueError("shard_bytes must be a positive integer")
    if isinstance(base_offset, bool) or not isinstance(base_offset, int) or base_offset < 0:
        raise ValueError("base_offset must be a non-negative integer")
    if direction not in ("read", "write"):
        raise ValueError("direction must be read or write")
    nodes, x_dim, y_dim = gen_tb_top._nodes(topo)
    if (x_dim, y_dim) != (4, 4):
        raise ValueError("the approved TP mapping requires a 4x4 mesh")
    by_xy = {(x, y): (idx, cid) for idx, x, y, cid in nodes}
    ring = [by_xy[xy] for xy in ((0, 2), (1, 2), (1, 3), (0, 3))]
    bases, entries = address_map.pack_config(topo)
    sizes = {e["dst_id"]: e["size"] for e in entries if e["space"] == "memory"}
    count = len(ring)
    for _, cid in ring:
        if base_offset + count * shard_bytes > sizes[cid]:
            raise ValueError("All-Gather shard slots exceed the memory window")

    def region(rank, shard):
        node, cid = ring[rank]
        return Region(node, bases[cid] + base_offset + shard * shard_bytes,
                      shard_bytes, shard)

    initial = tuple(region(rank, rank) for rank in range(count))
    available = {(r.node, r.shard): None for r in initial}
    retired_counts = {}
    transfers = []
    for step in range(count - 1):
        delivered = {}
        for rank in range(count):
            shard = (rank - step) % count
            source = region(rank, shard)
            destination = region((rank + 1) % count, shard)
            issuer = source.node if direction == "write" else destination.node
            prerequisite = available[source.node, shard]
            transfers.append(Transfer(issuer, source, destination, prerequisite))
            retired_counts[issuer] = retired_counts.get(issuer, 0) + 1
            delivered[destination.node, shard] = (issuer, retired_counts[issuer])
        available.update(delivered)
    final = tuple(region(rank, shard)
                  for rank in range(count) for shard in range(count))
    return Plan(initial, tuple(transfers), final)


def all_reduce_transport(topo, shard_bytes, direction):
    """Two dependent ring passes with identity payloads, without reduction."""
    first = all_gather(topo, shard_bytes, direction)
    count = len(first.initial)
    second = all_gather(topo, shard_bytes, direction,
                        base_offset=0x1000 + count * shard_bytes)
    boundary = {t.destination.node: t for t in first.transfers[-count:]}
    counts = {}
    for t in first.transfers:
        counts[t.issuer] = counts.get(t.issuer, 0) + 1
    shard_map = {r.shard: boundary[r.node].destination.shard for r in second.initial}

    def mapped(region):
        return replace(region, shard=shard_map[region.shard])

    transfers = list(first.transfers)
    for t in second.transfers:
        if t.prerequisite is None:
            producer = boundary[t.source.node]
            source = producer.destination
            prerequisite = (producer.issuer, counts[producer.issuer])
        else:
            source = mapped(t.source)
            node, retired = t.prerequisite
            prerequisite = (node, counts[node] + retired)
        transfers.append(replace(t, source=source, destination=mapped(t.destination),
                                 prerequisite=prerequisite))
    final = first.final + tuple(mapped(r) for r in second.final if r not in second.initial)
    return Plan(first.initial, tuple(transfers), final, "all_reduce_transport")


def pipeline(topo, shard_bytes, direction):
    """Matching-shard PP transport through A, B, C, D, with identity stages."""
    # Reuse geometry and size checks without introducing a different aperture.
    all_gather(topo, shard_bytes, direction)
    nodes, _, _ = gen_tb_top._nodes(topo)
    bases, entries = address_map.pack_config(topo)
    sizes = {e["dst_id"]: e["size"] for e in entries if e["space"] == "memory"}
    by_xy = {(x, y): (idx, cid) for idx, x, y, cid in nodes}
    groups = [[by_xy[x + dx, y + dy] for dx, dy in ((0, 0), (1, 0), (0, 1), (1, 1))]
              for x, y in ((0, 2), (2, 2), (2, 0), (0, 0))]
    regions = []
    for group in groups:
        stage = []
        for rank, (node, cid) in enumerate(group):
            offset = 0x1000 + rank * shard_bytes
            if offset + shard_bytes > sizes[cid]:
                raise ValueError("PP shard slot exceeds the memory window")
            stage.append(Region(node, bases[cid] + offset, shard_bytes, rank))
        regions.append(stage)
    transfers = []
    producers = {}
    for previous, current in zip(regions, regions[1:]):
        for source, destination in zip(previous, current):
            issuer = source.node if direction == "write" else destination.node
            transfers.append(Transfer(issuer, source, destination, producers.get(source.node)))
            producers[destination.node] = (issuer, 1)
    return Plan(tuple(regions[0]), tuple(transfers),
                tuple(region for stage in regions for region in stage), "pp_forward")


PP_BATCH_COUNT = 4  # Approved finite prefill comparison, not a hardware default.


def pipeline_batches(topo, shard_bytes, direction, *, serial):
    """Four disjoint batches with matching sequence shards at every stage."""
    single = pipeline(topo, shard_bytes, direction)
    ranks = len(single.initial)
    _, entries = address_map.pack_config(topo)
    windows = [e for e in entries if e["space"] == "memory"]
    initial, final, transfers = [], [], []
    for batch in range(PP_BATCH_COUNT):
        def moved(region):
            return replace(region, address=region.address + batch * ranks * shard_bytes,
                           shard=region.shard + batch * ranks)

        initial.extend(map(moved, single.initial))
        final.extend(map(moved, single.final))
        for transfer in single.transfers:
            prerequisite = ((transfer.prerequisite[0], batch + 1)
                            if transfer.prerequisite else None)
            barrier = (tuple((t.issuer, batch) for t in single.transfers[-ranks:])
                       if serial and batch and prerequisite is None else ())
            transfers.append(replace(transfer, source=moved(transfer.source),
                                     destination=moved(transfer.destination),
                                     prerequisite=prerequisite, barrier=barrier))
    for region in final:
        if not any(e["base"] <= region.address and
                   region.address + region.length <= e["base"] + e["size"] for e in windows):
            raise ValueError("pipeline batches exceed the memory window")
    return Plan(tuple(initial), tuple(transfers), tuple(final),
                "pp_serial" if serial else "pp_overlap")


def requirements(transfer):
    return ((transfer.prerequisite,) if transfer.prerequisite else ()) + transfer.barrier


def kv_handoff(topo, shard_bytes, direction):
    """Copy matching layer shards from A/B to D/C and retain every source."""
    all_gather(topo, shard_bytes, direction)
    nodes, _, _ = gen_tb_top._nodes(topo)
    bases, entries = address_map.pack_config(topo)
    sizes = {e["dst_id"]: e["size"] for e in entries if e["space"] == "memory"}
    by_xy = {(x, y): (idx, cid) for idx, x, y, cid in nodes}
    initial, destinations, transfers = [], [], []
    for x, y in ((0, 2), (1, 2), (0, 3), (1, 3),
                 (2, 2), (3, 2), (2, 3), (3, 3)):
        shard = len(initial)
        src_node, src_cid = by_xy[x, y]
        dst_node, dst_cid = by_xy[x, y - 2]
        offset = 0x1000 + shard * shard_bytes
        if offset + shard_bytes > min(sizes[src_cid], sizes[dst_cid]):
            raise ValueError("KV shard slot exceeds the memory window")
        source = Region(src_node, bases[src_cid] + offset, shard_bytes, shard)
        destination = Region(dst_node, bases[dst_cid] + offset, shard_bytes, shard)
        initial.append(source)
        destinations.append(destination)
        issuer = src_node if direction == "write" else dst_node
        transfers.append(Transfer(issuer, source, destination, None))
    return Plan(tuple(initial), tuple(transfers), tuple(initial + destinations), "kv_handoff")


def backing_layout(topo):
    """Resolve the approved backing services and their four consumer groups."""
    nodes, x_dim, y_dim = gen_tb_top._nodes(topo)
    if (x_dim, y_dim) != (4, 4):
        raise ValueError("the approved backing mapping requires a 4x4 mesh")
    peripherals = gen_tb_top._peripherals(topo)
    if [(p["x"], p["y"], p["dir"]) for p in peripherals] != [
            (1, 0, "SOUTH"), (2, 0, "SOUTH"), (1, 3, "NORTH"), (2, 3, "NORTH")]:
        raise ValueError("weight_load requires the approved dual-edge memory placement")
    bases, entries = address_map.pack_config(topo)
    sizes = {e["dst_id"]: e["size"] for e in entries if e["space"] == "memory"}
    by_xy = {(x, y): (idx, cid) for idx, x, y, cid in nodes}
    groups = []
    for group, (x, y) in enumerate(((0, 2), (2, 2), (2, 0), (0, 0))):
        peripheral = peripherals[group]
        memory = next(e for e in entries if e["space"] == "peripheral" and
                      e["dst_id"] == peripheral["cid"] and e["port"] == peripheral["port"])
        consumers = [by_xy[x + dx, y + dy] for dx, dy in ((0, 0), (1, 0), (0, 1), (1, 1))]
        groups.append((len(nodes) + group, memory, consumers))
    return groups, bases, sizes


def weight_load(topo, shard_bytes, direction):
    """Transfer distinct backing shards M0..M3 to groups A..D."""
    all_gather(topo, shard_bytes, direction)
    groups, bases, sizes = backing_layout(topo)
    initial, destinations, transfers = [], [], []
    for memory_node, memory, consumers in groups:
        for node, cid in consumers:
            shard = len(initial)
            offset = 0x1000 + shard * shard_bytes
            if offset + shard_bytes > min(memory["size"], sizes[cid]):
                raise ValueError("weight shard slot exceeds a memory window")
            source = Region(memory_node, memory["base"] + offset, shard_bytes, shard)
            destination = Region(node, bases[cid] + offset, shard_bytes, shard)
            initial.append(source)
            destinations.append(destination)
            issuer = source.node if direction == "write" else destination.node
            transfers.append(Transfer(issuer, source, destination, None))
    return Plan(tuple(initial), tuple(transfers), tuple(initial + destinations), "weight_load")


def data_parallel_weights(topo, shard_bytes, direction, *, shared):
    """Load the same rank shards at independent replicas from passive memory."""
    if direction != "read":
        raise ValueError("data-parallel weight loading requires tile-issued read")
    all_gather(topo, shard_bytes, direction)
    groups, bases, sizes = backing_layout(topo)
    initial, destinations, transfers = {}, [], []
    for group, (_, _, consumers) in enumerate(groups):
        memory_node, memory, _ = groups[0 if shared else group]
        for rank, (node, cid) in enumerate(consumers):
            offset = 0x1000 + rank * shard_bytes
            if offset + shard_bytes > min(memory["size"], sizes[cid]):
                raise ValueError("replica weight shard exceeds a memory window")
            source = Region(memory_node, memory["base"] + offset, shard_bytes, rank)
            destination = Region(node, bases[cid] + offset, shard_bytes, rank)
            initial[source] = source
            destinations.append(destination)
            transfers.append(Transfer(node, source, destination, None))
    originals = tuple(initial.values())
    return Plan(originals, tuple(transfers), originals + tuple(destinations),
                "dp_shared_weights" if shared else "dp_separate_weights")


def kv_roundtrip(topo, shard_bytes, direction):
    """Offload each tile block, then restore its received bytes to a fresh slot."""
    loading = weight_load(topo, shard_bytes, direction)
    _, entries = address_map.pack_config(topo)
    windows = [e for e in entries if e["space"] == "memory"]
    initial, backing, restored, transfers = [], [], [], []
    counts = {}
    producers = {}
    for load in loading.transfers:
        source, destination = load.destination, load.source
        initial.append(source)
        backing.append(destination)
        issuer = source.node if direction == "write" else destination.node
        transfers.append(Transfer(issuer, source, destination, None))
        counts[issuer] = counts.get(issuer, 0) + 1
        producers[destination.node, destination.address] = (issuer, counts[issuer])
    for source, original in zip(backing, initial):
        destination = Region(original.node, original.address + len(initial) * shard_bytes,
                             shard_bytes, original.shard)
        if not any(e["base"] <= destination.address and
                   destination.address + shard_bytes <= e["base"] + e["size"]
                   for e in windows):
            raise ValueError("restored KV block exceeds the memory window")
        issuer = source.node if direction == "write" else destination.node
        transfers.append(Transfer(issuer, source, destination,
                                  producers[source.node, source.address]))
        restored.append(destination)
    return Plan(tuple(initial), tuple(transfers), tuple(initial + backing + restored),
                "kv_roundtrip")


def kv_tile_roundtrip(topo, shard_bytes, direction):
    """Each tile offloads one block, then reads it back after its write retires.

    The direction argument selects the initial write. Restore always uses a
    tile-issued read. Keep the older single-direction transport plans intact.
    """
    if direction != "write":
        raise ValueError("kv_tile_roundtrip starts with write, then restores with read")
    plan = kv_roundtrip(topo, shard_bytes, direction)
    count = len(plan.initial)
    offloads = plan.transfers[:count]
    restores = tuple(replace(restore, issuer=offload.issuer,
                             prerequisite=(offload.issuer, 1))
                     for offload, restore in zip(offloads, plan.transfers[count:]))
    return replace(plan, transfers=offloads + restores, operation="kv_tile_roundtrip")


def kv_restore_consume(topo, shard_bytes, direction):
    """Read restored blocks through local DMA after each restore retires."""
    copies = kv_roundtrip(topo, shard_bytes, direction)
    counts, producers = {}, {}
    for transfer in copies.transfers:
        counts[transfer.issuer] = counts.get(transfer.issuer, 0) + 1
        producers[transfer.destination] = (transfer.issuer, counts[transfer.issuer])
    _, entries = address_map.pack_config(topo)
    transfers = list(copies.transfers)
    final = list(copies.final)
    for restore in copies.transfers[len(copies.initial):]:
        source = restore.destination
        destination = Region(source.node, source.address + len(copies.initial) * shard_bytes,
                             shard_bytes, source.shard)
        if not any(e["space"] == "memory" and e["base"] <= destination.address and
                   destination.address + shard_bytes <= e["base"] + e["size"] for e in entries):
            raise ValueError("consumer output exceeds the memory window")
        transfers.append(Transfer(source.node, source, destination, producers[source]))
        final.append(destination)
    return Plan(copies.initial, tuple(transfers), tuple(final), "kv_restore_consume")


def delivered_payload_bytes(plan):
    return sum(t.source.length for t in plan.transfers
               for destination in (t.destination,) + t.replicas if t.source.node != destination.node)


def shared_tensor(topo, shard_bytes, direction, *, single_fetch):
    """Four groups consume one identical input per group from its backing service."""
    if type(shard_bytes) is not int or shard_bytes <= 0:
        raise ValueError("shard_bytes must be a positive integer")
    if direction not in ("read", "write"):
        raise ValueError("direction must be read or write")
    groups, bases, sizes = backing_layout(topo)
    initial, final, transfers = [], [], []
    counts = {}
    for group, (memory_node, memory, members) in enumerate(groups):
        offset = 0x1000
        if offset + shard_bytes > min(memory["size"], *(sizes[cid] for _, cid in members)):
            raise ValueError("shared input exceeds a memory window")
        backing = Region(memory_node, memory["base"] + offset, shard_bytes, group)
        initial.append(backing)
        # Collective destinations must share the same offset within each tile.
        consumers = [Region(node, bases[cid] + offset, shard_bytes, group) for node, cid in members]
        final.extend(consumers)
        producer = None
        for rank, destination in enumerate(consumers):
            source = consumers[0] if single_fetch and rank else backing
            issuer = source.node if direction == "write" or (single_fetch and rank) else destination.node
            transfers.append(Transfer(issuer, source, destination,
                                      producer if single_fetch and rank else None))
            counts[issuer] = counts.get(issuer, 0) + 1
            if rank == 0:
                producer = (issuer, counts[issuer])
    operation = "shared_fetch_unicast" if single_fetch else "shared_independent"
    return Plan(tuple(initial), tuple(transfers), tuple(initial + final), operation)


def shared_independent(topo, shard_bytes, direction):
    return shared_tensor(topo, shard_bytes, direction, single_fetch=False)


def shared_fetch_unicast(topo, shard_bytes, direction):
    return shared_tensor(topo, shard_bytes, direction, single_fetch=True)


def shared_fetch_multicast(topo, shard_bytes, direction):
    """One tile-issued DMA reads backing memory and multicasts to its full group."""
    if direction != "read":
        raise ValueError("shared multicast requires tile-issued read")
    layout = shared_independent(topo, shard_bytes, direction)
    nodes, _, _ = gen_tb_top._nodes(topo)
    cids = {index: cid for index, _x, _y, cid in nodes}
    bases, _ = address_map.pack_config(topo)
    transfers = []
    for group in range(4):
        fetch, first, second, third = layout.transfers[group * 4:(group + 1) * 4]
        destinations = (first.destination, fetch.destination, second.destination, third.destination)
        members = [cids[r.node] for r in destinations]
        mask = gen_test_patterns.collective_addr_mask(bases, members, members[0])
        # Name a remote tile in AWADDR so the tile crossbar sends the write to
        # the NMU. The wildcard mask also includes the issuing tile's storage.
        transfers.append(Transfer(fetch.destination.node, fetch.source, first.destination,
                         None, gen_test_patterns._awuser_multicast(mask), destinations[1:]))
    return Plan(layout.initial, tuple(transfers), layout.final, "shared_fetch_multicast")


def expert_roundtrip(topo, shard_bytes, direction):
    """Dispatch and identity-return on existing expert edges, without compute."""
    all_gather(topo, shard_bytes, direction)
    nodes, x_dim, y_dim = gen_tb_top._nodes(topo)
    bases, entries = address_map.pack_config(topo)
    node_bases = {index: bases[cid] for index, _x, _y, cid in nodes}
    sizes = {e["dst_id"]: e["size"] for e in entries if e["space"] == "memory"}
    node_sizes = {index: sizes[cid] for index, _x, _y, cid in nodes}
    targets = gen_test_patterns.mapped_payload_dsts("expert_dispatch", x_dim, y_dim, 1)
    edges = [(owner, expert) for owner, experts in targets.items() for expert in experts]
    initial, received, returned, transfers = [], [], [], []
    counts, producers = {}, {}

    def region(node, slot, shard):
        offset = 0x1000 + slot * shard_bytes
        if offset + shard_bytes > node_sizes[node]:
            raise ValueError("expert payload slot exceeds the memory window")
        return Region(node, node_bases[node] + offset, shard_bytes, shard)

    for shard, (owner, expert) in enumerate(edges):
        source = region(owner, shard, shard)
        destination = region(expert, shard, shard)
        initial.append(source)
        received.append(destination)
        returned.append(region(owner, len(edges) + shard, shard))
        issuer = source.node if direction == "write" else destination.node
        transfers.append(Transfer(issuer, source, destination, None))
        counts[issuer] = counts.get(issuer, 0) + 1
        producers[destination] = (issuer, counts[issuer])
    for source, destination in zip(received, returned):
        issuer = source.node if direction == "write" else destination.node
        transfers.append(Transfer(issuer, source, destination, producers[source]))
    return Plan(tuple(initial), tuple(transfers), tuple(initial + received + returned),
                "expert_roundtrip")



def standalone_operation(topo, tensor_bytes, direction, operation):
    """One complete operation. Input length is the full tensor, not a shard.

    Ownership follows MPI-4.1 collective semantics. Reduction uses symbolic
    contributions with no numerical kernel. Existing ring schedules are reused.
    """
    if type(tensor_bytes) is not int or tensor_bytes <= 0 or tensor_bytes % 4:
        raise ValueError("full tensor bytes must be positive and divisible by four")
    if direction != "write":
        raise ValueError("standalone collectives require source-issued write")
    if operation in ("reduce_scatter", "all_reduce"):
        from composite_dma_plan import layer_sequence
        return layer_sequence(topo, tensor_bytes // 4, direction, operation)
    layout = all_gather(topo, tensor_bytes // 4, direction)
    if operation == "collective_all_gather":
        return replace(layout, operation=operation)
    if operation == "point_to_point":
        boundary = pipeline(topo, tensor_bytes, direction).transfers[-4:]
        transfers = tuple(replace(t, prerequisite=None) for t in boundary)
        initial = tuple(t.source for t in transfers)
        return Plan(initial, transfers, initial + tuple(t.destination for t in transfers), operation)
    nodes = sorted(r.node for r in layout.initial)
    bases, entries = address_map.pack_config(topo)
    endpoints, _, _ = gen_tb_top._nodes(topo)
    node_bases = {node: bases[cid] for node, _, _, cid in endpoints}
    sizes = {e["dst_id"]: e["size"] for e in entries if e["space"] == "memory"}
    node_sizes = {node: sizes[cid] for node, _, _, cid in endpoints}

    def region(node, offset, length, shard):
        if 0x1000 + offset + length > node_sizes[node]:
            raise ValueError("operation payload exceeds the memory window")
        return Region(node, node_bases[node] + 0x1000 + offset, length, shard)

    root = nodes[0]
    transfers = []
    if operation == "broadcast":
        source = region(root, 0, tensor_bytes, 0)
        initial = (source,)
        for node in nodes[1:]:
            transfers.append(Transfer(root, source, region(node, 0, tensor_bytes, 0), None))
    elif operation in ("scatter", "gather"):
        length = tensor_bytes // len(nodes)
        initial = tuple(region(root if operation == "scatter" else node,
                               rank * length, length, rank)
                        for rank, node in enumerate(nodes))
        for rank, node in enumerate(nodes[1:], 1):
            source = initial[rank]
            destination = region(node if operation == "scatter" else root,
                                 rank * length, length, rank)
            transfers.append(Transfer(source.node, source, destination, None))
    elif operation == "reduce":
        # A1, A3, A2, A0: carry a fixed-size partial to root A0.
        chain = [r.node for r in layout.initial][1:] + [root]
        initial = tuple(region(node, 0, tensor_bytes, 0) for node in nodes)
        partials = {r.node: r for r in initial}
        source, prerequisite = partials[chain[0]], None
        for node in chain[1:]:
            destination = region(node, tensor_bytes, tensor_bytes, 0)
            transfers.append(Transfer(source.node, source, destination, prerequisite,
                                      phase="reduce", local_partial=partials[node]))
            prerequisite = (source.node, 1)
            source = destination
    else:
        raise ValueError(f"unknown standalone operation: {operation}")
    final = initial + tuple(t.destination for t in transfers)
    plan = Plan(initial, tuple(transfers), final, operation)
    if operation == "reduce":
        from composite_dma_plan import validate_reductions
        validate_reductions(plan, participants=set(nodes))
    return plan


OPERATIONS = {"all_gather": all_gather, "all_reduce_transport": all_reduce_transport,
              "pp_forward": pipeline,
              "pp_serial": lambda t, s, d: pipeline_batches(t, s, d, serial=True),
              "pp_overlap": lambda t, s, d: pipeline_batches(t, s, d, serial=False),
              "expert_roundtrip": expert_roundtrip,
              "kv_handoff": kv_handoff, "weight_load": weight_load,
              "dp_shared_weights": lambda t, s, d: data_parallel_weights(t, s, d, shared=True),
              "dp_separate_weights": lambda t, s, d: data_parallel_weights(t, s, d, shared=False),
              "kv_roundtrip": kv_roundtrip, "kv_restore_consume": kv_restore_consume,
              "kv_tile_roundtrip": kv_tile_roundtrip,
              "shared_independent": shared_independent, "shared_fetch_unicast": shared_fetch_unicast,
              "shared_fetch_multicast": shared_fetch_multicast}
OPERATION_LABELS = {"all_gather": "All-Gather", "pp_forward": "PP-forward",
                    "pp_serial": "PP-serial", "pp_overlap": "PP-overlap",
                    "all_reduce_transport": "All-Reduce-transport",
                    "expert_roundtrip": "Expert-roundtrip",
                    "kv_handoff": "KV-handoff", "weight_load": "Weight-load",
                    "dp_shared_weights": "DP-shared-weights", "dp_separate_weights": "DP-separate-weights",
                    "kv_roundtrip": "KV-roundtrip", "kv_restore_consume": "KV-restore-consume",
                    "kv_tile_roundtrip": "KV-tile-roundtrip",
                    "shared_independent": "Shared-independent", "shared_fetch_unicast": "Shared-fetch-unicast",
                    "shared_fetch_multicast": "Shared-fetch-multicast"}


def composite(topo, shard_bytes, direction, operation):
    from composite_dma_plan import layer_sequence
    return layer_sequence(topo, shard_bytes, direction, operation)


for _operation in ("tp_layer_all_reduce", "tp_layer_sequence", "prefill_decode"):
    OPERATIONS[_operation] = lambda t, s, d, op=_operation: composite(t, s, d, op)
    OPERATION_LABELS[_operation] = _operation.replace("_", "-")


for _operation in ("broadcast", "scatter", "gather", "collective_all_gather",
                   "reduce", "all_reduce", "reduce_scatter", "point_to_point"):
    OPERATIONS[_operation] = lambda t, b, d, op=_operation: standalone_operation(t, b, d, op)
    OPERATION_LABELS[_operation] = {"collective_all_gather": "All-Gather",
                                   "point_to_point": "Point-to-point"}.get(
                                       _operation, _operation.replace("_", "-").title())


# Explicit research cases keep the legacy DMA_LENGTH interpretation unchanged.
# For xy_* cases the length is per-PE bytes, documented in the contract.
def _xy(topo, length, direction, operation):
    from xy_collective_plan import build
    return build(topo, length, direction, operation)


for _count in (4, 16):
    for _kind in ("gather", "all_gather", "reduce_scatter", "reduce", "all_reduce"):
        _key = f"xy_{_kind}_p{_count}"
        OPERATIONS[_key] = lambda t, b, d, op=_key: _xy(t, b, d, op)
        OPERATION_LABELS[_key] = _kind.replace("_", "-").title()
for _count in (4, 16):
    for _kind in ("gather", "all_gather", "reduce_scatter", "reduce", "all_reduce"):
        _key = f"xy_direct_{_kind}_p{_count}"
        OPERATIONS[_key] = lambda t, b, d, op=_key: _xy(t, b, d, op)
        OPERATION_LABELS[_key] = _kind.replace("_", "-").title()
for _src, _dst in ((0, 1), (1, 0), (0, 4), (4, 0), (0, 2), (2, 0),
                   (0, 8), (8, 0), (0, 15), (15, 0)):
    _key = f"xy_cal_{_src}_{_dst}"
    OPERATIONS[_key] = lambda t, b, d, op=_key: _xy(t, b, d, op)
    OPERATION_LABELS[_key] = "Calibration"
for _key in ("xy_duplex_0_1", "xy_duplex_0_4", "xy_descriptor_chain"):
    OPERATIONS[_key] = lambda t, b, d, op=_key: _xy(t, b, d, op)
    OPERATION_LABELS[_key] = "Calibration" if "duplex" in _key else "Descriptor-chain"


def split_descriptors(plan):
    """Keep whole-object ownership and readiness across bounded DMA descriptors."""
    binding = Path(__file__).resolve().parents[1] / "tb/soc/idma_types_pkg.sv"
    match = re.search(r"localparam int unsigned TF_LEN_WIDTH\s*=\s*(\d+)\s*;", binding.read_text())
    if match is None or int(match[1]) < 2:
        raise ValueError("cannot resolve DMA transfer length width")
    limit = (1 << int(match[1])) - 1
    # Largest power of two within the descriptor field preserves page alignment.
    chunk_bytes = 1 << (int(match[1]) - 1)
    original_counts, descriptor_counts, completion = {}, {}, {}
    transfers = []
    for transfer in plan.transfers:
        issuer = transfer.issuer
        prerequisite = completion[transfer.prerequisite] if transfer.prerequisite else None
        barrier = tuple(completion[p] for p in transfer.barrier)
        stride = chunk_bytes if transfer.source.length > limit else transfer.source.length
        for offset in range(0, transfer.source.length, stride):
            length = min(stride, transfer.source.length - offset)

            def part(region):
                return replace(region, address=region.address + offset, length=length,
                               offset=region.offset + offset if plan.contract else region.offset)

            transfers.append(replace(transfer, source=part(transfer.source),
                                     destination=part(transfer.destination),
                                     replicas=tuple(part(r) for r in transfer.replicas),
                                     prerequisite=prerequisite, barrier=barrier))
            descriptor_counts[issuer] = descriptor_counts.get(issuer, 0) + 1
        original_counts[issuer] = original_counts.get(issuer, 0) + 1
        completion[issuer, original_counts[issuer]] = (issuer, descriptor_counts[issuer])
    return replace(plan, transfers=tuple(transfers))


def build_plan(topo, shard_bytes, direction, operation="all_gather", resident_shards=()):
    plan = OPERATIONS[operation](topo, shard_bytes, direction)
    if not resident_shards:
        return split_descriptors(plan)
    if operation != "weight_load":
        raise ValueError("resident shards require weight_load")
    valid = {r.shard for r in plan.initial}
    if (any(type(shard) is not int or shard not in valid for shard in resident_shards)
            or len(set(resident_shards)) != len(resident_shards)):
        raise ValueError("resident shards must be distinct valid weight shard IDs")
    resident = tuple(sorted(resident_shards))
    return split_descriptors(replace(plan,
                   initial=plan.initial + tuple(t.destination for t in plan.transfers
                                                if t.destination.shard in resident),
                   transfers=tuple(t for t in plan.transfers if t.destination.shard not in resident),
                   resident_shards=resident))


def parse_resident_shards(value):
    """Explicit comma-separated weight shard IDs, not a cache policy."""
    return tuple(int(shard) for shard in value.split(","))


def result_header(plan):
    header = {"schema": 1, "operation": plan.operation, "test_layer": "L2",
              "start_event": "first_job_valid" if plan.transfers else "residency_check",
              "end_event": "last_dma_response" if plan.transfers else "resident_ready",
              "logical_bytes": sum({r.shard: r.length for r in plan.initial}.values()),
              "delivered_payload_bytes": delivered_payload_bytes(plan),
              "expected_transfers": len(plan.transfers)}
    if plan.resident_shards:
        header["resident_shards"] = list(plan.resident_shards)
    return header


def pass_marker(plan):
    return (f"PASS: {OPERATION_LABELS[plan.operation]} retired {len(plan.transfers)} transfers, "
            f"checked {sum(r.length for r in plan.final)} bytes")


def emit(plan, out_root, node_count):
    """Write DMA jobs and count-prefixed prerequisite lists, one list per job.

    A zero list length means no dependency. Empty files identify inactive
    nodes. The manifest retains source initialization and final ownership for
    the generated TB, without preloading intermediate forwarding slots.
    """
    out_root = Path(out_root)
    for node in range(node_count):
        node_dir = out_root / f"node{node}"
        node_dir.mkdir(parents=True, exist_ok=True)
        jobs = []
        dependencies = []
        users = []
        for transfer in plan.transfers:
            if transfer.issuer != node:
                continue
            jobs.extend(gen_dma_jobs.job_lines(
                transfer.source.length, transfer.source.address,
                transfer.destination.address, gen_dma_jobs._AXI_ID))
            waits = requirements(transfer)
            dependencies.append(" ".join(map(str, [len(waits)] + [v for pair in waits for v in pair])))
            users.append(str(transfer.user))
        (node_dir / "jobs.txt").write_text("".join(f"{line}\n" for line in jobs))
        (node_dir / "dependencies.txt").write_text(
            "".join(f"{line}\n" for line in dependencies))
        (node_dir / "users.txt").write_text("".join(f"{line}\n" for line in users))
    (out_root / "dependent_plan.json").write_text(json.dumps(asdict(plan), indent=2) + "\n")
    if plan.contract:
        from xy_runtime_checker import emit as emit_checker
        emit_checker(plan, out_root)


def validate_result(result, plan):
    """Validate operation timing and payload accounting, not router counters."""
    expected = {**result_header(plan),
                "checked_bytes": sum(r.length for r in plan.final), "status": "PASS"}
    for key, value in expected.items():
        if type(result.get(key)) is not type(value) or result[key] != value:
            raise ValueError(f"operation result disagrees with plan: {key}")
    for key in ("start_cycle", "end_cycle", "duration_cycles"):
        if type(result.get(key)) is not int or result[key] < 0:
            raise ValueError(f"invalid operation counter: {key}")
    if result["end_cycle"] < result["start_cycle"] or result["duration_cycles"] != (
            result["end_cycle"] - result["start_cycle"] + 1):
        raise ValueError("inconsistent operation measurement window")
    return result["delivered_payload_bytes"] / result["duration_cycles"]


def validate_perf_window(result, perf):
    """The collector uses an exclusive end, the operation record an inclusive end."""
    window = perf.get("window", {})
    if window.get("start_cyc") != result["start_cycle"] or window.get("end_cyc") != result["end_cycle"] + 1:
        raise ValueError("router counters use a different operation window")
    links = perf.get("noc", {}).get("links", [])
    if not links:
        raise ValueError("operation has no link counters")
    has_traffic = any(link.get("flit_count", 0) > 0 for link in links)
    if has_traffic != bool(result["expected_transfers"]):
        raise ValueError("measured NoC traffic disagrees with operation transfers")
    for link in links:
        for key in ("flit_count", "stall_cyc"):
            if type(link.get(key)) is not int or not 0 <= link[key] <= result["duration_cycles"]:
                raise ValueError(f"invalid link counter: {key}")


def validate_memory_reads(log, plan, topo):
    """Check aligned full-width DMA source reads against memory-side counters."""
    beat_bytes = gen_tb_top._constant("axi", "DATA_WIDTH") // 8
    if not plan.contract and any(t.source.address % beat_bytes or t.source.length % beat_bytes for t in plan.transfers):
        raise ValueError("memory read accounting requires aligned full-width blocks")
    nodes, _, _ = gen_tb_top._nodes(topo)
    peripherals = gen_tb_top._peripherals(topo)
    endpoints = gen_tb_top._endpoints(nodes, peripherals)
    windows, _ = gen_tb_top.tile_targets(topo, endpoints)
    expected = {(node, target): 0 for node, targets in windows.items()
                for target in range(len(targets))}
    for transfer in plan.transfers:
        source = transfer.source
        matches = [index for index, window in enumerate(windows[source.node])
                   if window["base"] <= source.address and
                   source.address + source.length <= window["base"] + window["size"]]
        if len(matches) != 1:
            raise ValueError("DMA source does not select exactly one memory target")
        expected[source.node, matches[0]] += source.length
    expected_beats = {key: 0 for key in expected}
    for transfer in plan.transfers:
        source = transfer.source
        target = next(i for i, w in enumerate(windows[source.node])
                      if w["base"] <= source.address < w["base"] + w["size"])
        expected_beats[source.node, target] += (source.address % beat_bytes + source.length + beat_bytes - 1) // beat_bytes
    observed = {}
    pattern = r"\[memory_reads\] node(\d+) target(\d+): requests=(\d+) requested_bytes=(\d+) response_beats=(\d+)"
    for line in log.splitlines():
        if not line.startswith("[memory_reads]"):
            continue
        match = re.fullmatch(pattern, line)
        if not match:
            raise ValueError("malformed memory read counter")
        node, target, requests, requested, beats = map(int, match.groups())
        key = (node, target)
        if key in observed or key not in expected:
            raise ValueError("duplicate or unexpected memory read counter")
        if (beats != expected_beats[key] or
                (requested != expected[key] if not plan.contract else requested != beats * beat_bytes)):
            raise ValueError(f"memory read bytes disagree at {key}")
        if (requested == 0 and requests != 0) or (requested > 0 and not 0 < requests <= beats):
            raise ValueError("invalid memory read request count")
        observed[key] = requested
    if observed.keys() != expected.keys():
        raise ValueError("missing memory read counters")
    return {"backing_requested_bytes": sum(value for (node, _), value in observed.items() if node >= len(nodes)),
            "tile_requested_bytes": sum(value for (node, _), value in observed.items() if node < len(nodes))}


def checker_sv(plan, node_count, memory_targets):
    """Emit a byte oracle against original shard identity at every owner."""
    if plan.contract:
        from xy_runtime_checker import checker_sv as runtime_checker
        return runtime_checker(node_count, memory_targets)
    label = OPERATION_LABELS[plan.operation]
    if len(memory_targets) != node_count:
        raise ValueError("memory target count must match the endpoint count")
    lines = [f"    localparam int MEM_TARGET [{node_count}] = '{{{', '.join(map(str, memory_targets))}}};"]
    w = lines.append
    w(f"    logic [{node_count - 1}:0] operation_job_valid, operation_response_valid;")
    for node in range(node_count):
        w(f"    assign operation_job_valid[{node}] = g_endpoint[{node}].u_endpoint.dma_job_req_valid;")
        w(f"    assign operation_response_valid[{node}] = g_endpoint[{node}].u_endpoint.dma_job_rsp_valid;")
    w("    bit operation_started_reg = 0, operation_done_reg = 0;")
    w("    longint unsigned operation_start_reg = 0, operation_end_reg = 0;")
    w("    int unsigned operation_retired_reg = 0;")
    w("    always @(posedge clk_i or negedge rst_n_i) begin")
    w("        if (~rst_n_i) begin")
    w("            operation_started_reg <= 0;")
    w("            operation_done_reg <= 0;")
    w("            operation_start_reg <= 0;")
    w("            operation_end_reg <= 0;")
    w("            operation_retired_reg <= 0;")
    w("        end else begin")
    start_condition = " && |operation_job_valid" if plan.transfers else " && operation_window_started"
    w(f"            if (!operation_started_reg{start_condition}) begin")
    w("                operation_started_reg <= 1;")
    w("                operation_start_reg <= live_cyc;")
    w("            end")
    w("            operation_retired_reg <= operation_retired_reg + $countones(operation_response_valid);")
    ready_condition = "" if plan.transfers else "operation_window_started && "
    w(f"            if (!operation_done_reg && {ready_condition}operation_retired_reg + $countones(operation_response_valid) == {len(plan.transfers)}) begin")
    w("                operation_done_reg <= 1;")
    w("                operation_end_reg <= live_cyc;")
    w("            end")
    w("        end")
    w("    end")
    def mem(node):
        return f"g_endpoint[{node}].u_endpoint.g_tile_mem[MEM_TARGET[{node}]].i_mem.i_sim_mem.mem"
    w("    function automatic logic [7:0] shard_byte(input int shard, input int offset);")
    w("        return 8'(shard + 1) ^ 8'(offset) ^ 8'(offset >> 8) ^ 8'(offset >> 16);")
    w("    endfunction")
    w("    function automatic logic [7:0] read_shard(input int node, input logic [ADDR_WIDTH-1:0] address);")
    w("        case (node)")
    for node in range(node_count):
        w(f"            {node}: return {mem(node)}[address];")
    w('            default: $fatal(1, "invalid shard endpoint");')
    w("        endcase")
    w("        return 'x;")
    w("    endfunction")
    w("    initial begin")
    # Poison every non-original slot to reject a transfer that never wrote it.
    initial_keys = {(r.node, r.address) for r in plan.initial}
    for region in plan.final:
        invert = "" if (region.node, region.address) in initial_keys else "~"
        w(f"        for (int k = 0; k < {region.length}; k++)")
        w(f"            {mem(region.node)}[ADDR_WIDTH'(64'h{region.address:x}) + k] = "
          f"{invert}shard_byte({region.shard}, k + {region.offset});")
    w("    end")
    counts = [sum(t.issuer == node for t in plan.transfers) for node in range(node_count)]
    w(f"    localparam int EXPECTED_JOBS [{node_count}] = '{{{', '.join(map(str, counts))}}};")
    for node in range(node_count):
        issued = [t for t in plan.transfers if t.issuer == node]
        endpoint = f"g_endpoint[{node}].u_endpoint"
        w(f"    int unsigned checked_jobs_{node} = 0;")
        if any(t.phase for t in issued):
            w(f"    int unsigned phase_retired_{node}_reg = 0;")
            w("    always @(posedge clk_i or negedge rst_n_i) begin")
            w("        if (~rst_n_i) begin")
            w(f"            phase_retired_{node}_reg = 0;")
            w("        end else begin")
            w(f"            if ({endpoint}.dma_job_rsp_valid) begin")
            w(f"                case (phase_retired_{node}_reg)")
            for index, transfer in enumerate(issued):
                w(f'                    {index}: $display("[dma_phase] node={node} job={index} phase={transfer.phase} event=retire cycle=%0d", live_cyc);')
            w('                    default: $fatal(1, "unexpected phase response");')
            w("                endcase")
            w(f"                phase_retired_{node}_reg++;")
            w("            end")
            w("        end")
            w("    end")
        w("    always @(posedge clk_i) begin")
        w(f"        if (rst_n_i && {endpoint}.dma_job_req_valid && {endpoint}.dma_job_req_ready) begin")
        w(f"            case (checked_jobs_{node})")
        for index, transfer in enumerate(issued):
            w(f"                {index}: begin")
            if transfer.phase:
                w(f'                    $display("[dma_phase] node={node} job={index} phase={transfer.phase} event=issue cycle=%0d", live_cyc);')
            for predecessor, count in requirements(transfer):
                w(f"                    if (jobs_retired[{predecessor}] < {count})")
                w(f'                        $fatal(1, "{label} dependency violation node{node} job{index}");')
            w(f"                    if ({endpoint}.dma_job_req.src_addr != ADDR_WIDTH'(64'h{transfer.source.address:x}) ||")
            w(f"                        {endpoint}.dma_job_req.dst_addr != ADDR_WIDTH'(64'h{transfer.destination.address:x}) ||")
            w(f"                        {endpoint}.dma_job_req.length != {transfer.source.length} ||")
            w(f"                        {endpoint}.dma_job_req.user != idma_types_pkg::USER_WIDTH'(64'h{transfer.user:x}))")
            w(f'                        $fatal(1, "{label} request mismatch node{node} job{index}");')
            w("                end")
        w(f'                default: $fatal(1, "{label} unexpected job node{node}");')
        w("            endcase")
        w(f"            checked_jobs_{node}++;")
        w("        end")
        w("    end")
    w("    initial begin")
    w("        bit complete;")
    w("        longint unsigned checked_bytes;")
    w("        int operation_fd;")
    w('        string operation_path;')
    w("        checked_bytes = 0;")
    w("        do begin")
    w("            @(posedge clk_i);")
    w("            complete = rst_n_i && operation_done_reg;")
    w(f"            for (int node = 0; node < {node_count}; node++) begin")
    w('                if (jobs_issued[node] > EXPECTED_JOBS[node]) $fatal(1, "extra DMA job at node%0d", node);')
    w("                complete &= jobs_done[node] && jobs_retired[node] == EXPECTED_JOBS[node];")
    w("            end")
    w("        end while (!complete);")
    for node, count in enumerate(counts):
        w(f'        if (checked_jobs_{node} != {count}) $fatal(1, "{label} unchecked jobs node{node}");')
    for region in plan.final:
        w(f"        for (int k = 0; k < {region.length}; k++) begin")
        w(f"            if (read_shard({region.node}, ADDR_WIDTH'(64'h{region.address:x}) + k) "
          f"!== shard_byte({region.shard}, k + {region.offset}))")
        w(f'                $fatal(1, "{label} node{region.node} shard{region.shard} byte%0d mismatch", k);')
        w("            checked_bytes++;")
        w("        end")
    w('        if (!operation_done_reg || !operation_started_reg || operation_end_reg < operation_start_reg)')
    w('            $fatal(1, "invalid operation measurement window");')
    w('        if ($value$plusargs("operation_out=%s", operation_path)) begin')
    w('            operation_fd = $fopen(operation_path, "w");')
    w('            if (!operation_fd) $fatal(1, "cannot open operation output");')
    header = result_header(plan)
    prefix = json.dumps(header)[:-1] + ', "start_cycle": %0d, "end_cycle": %0d, "duration_cycles": %0d, "checked_bytes": %0d, "status": "PASS"}'
    sv_format = prefix.replace('"', '\\"')
    w(f'            $fdisplay(operation_fd, "{sv_format}", operation_start_reg, operation_end_reg,')
    w("                      operation_end_reg - operation_start_reg + 1, checked_bytes);")
    w("            $fclose(operation_fd);")
    w("        end")
    w(f'        $display("PASS: {label} retired {len(plan.transfers)} transfers, checked %0d bytes", checked_bytes);')
    w("        $finish(0);")
    w("    end")
    return lines


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--topology", default="mesh_4x4")
    parser.add_argument("--shard-bytes", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--direction", choices=("read", "write"), required=True)
    parser.add_argument("--operation", choices=OPERATIONS, default="all_gather")
    parser.add_argument("--resident-shards", type=parse_resident_shards, default=(),
                        help="weight_load only: comma-separated shard IDs already at their tile owners")
    parser.add_argument("--out")
    parser.add_argument("--validate-result", type=Path)
    parser.add_argument("--validate-memory-reads", action="store_true",
                        help="Require full-width aligned source-read accounting from run.log")
    args = parser.parse_args(argv)
    topo = gen_tb_top.load_topology(args.topology)
    plan = build_plan(topo, args.shard_bytes, args.direction, args.operation, args.resident_shards)
    if args.validate_result:
        log = (args.validate_result / "run.log").read_text()
        if pass_marker(plan) not in log.splitlines():
            raise ValueError("missing byte-checked operation completion")
        result = json.loads((args.validate_result / "operation.json").read_text())
        bandwidth = validate_result(result, plan)
        validate_perf_window(result, json.loads((args.validate_result / "perf.json").read_text()))
        if any(t.phase for t in plan.transfers):
            from composite_dma_plan import phase_results
            phases = phase_results(log, plan)
            (args.validate_result / "phases.json").write_text(json.dumps(phases, indent=2) + "\n")
        if args.validate_memory_reads:
            memory = validate_memory_reads(log, plan, topo)
            print("memory reads verified: " + json.dumps(memory, sort_keys=True))
        print(f"operation window verified: {result['duration_cycles']} cycles, {bandwidth:.6f} payload B/cycle")
        return
    if not args.out:
        parser.error("--out is required when generating jobs")
    nodes, _, _ = gen_tb_top._nodes(topo)
    emit(plan, args.out, len(nodes) + len(gen_tb_top._peripherals(topo)))


if __name__ == "__main__":
    main()
