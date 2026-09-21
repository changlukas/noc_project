"""Fixed-XY collective schedules and independent ownership/cost accounting.

Algorithm provenance and transfer assumptions: docs/perf_report_restructured.md,
chapter 2. Reduction payloads encode index identity, not numerical arithmetic.
"""
from collections import Counter
from dataclasses import replace
import re
import dependent_dma_plan as dma

CYCLES = {4: (0, 1, 5, 4),
          16: (0, 1, 2, 3, 7, 6, 5, 9, 10, 11, 15, 14, 13, 12, 8, 4)}


def route(source, destination):
    path = [source]
    while source % 4 != destination % 4:
        source += 1 if source % 4 < destination % 4 else -1
        path.append(source)
    while source // 4 != destination // 4:
        source += 4 if source // 4 < destination // 4 else -4
        path.append(source)
    return tuple(path)


def contains(parent, child):
    return (parent.node == child.node and parent.shard == child.shard
            and parent.address <= child.address
            and child.address + child.length <= parent.address + parent.length
            and child.offset - parent.offset == child.address - parent.address)


class Builder:
    def __init__(self, topo):
        nodes, x, y = dma.gen_tb_top._nodes(topo)
        if (x, y) != (4, 4):
            raise ValueError("XY collective cases require the existing 4x4 mesh")
        bases, entries = dma.address_map.pack_config(topo)
        sizes = {e["dst_id"]: e["size"] for e in entries if e["space"] == "memory"}
        self.windows = {n: (bases[cid], sizes[cid]) for n, _, _, cid in nodes}
        self.cursor = {n: 0x1000 for n in self.windows}
        self.initial, self.storage, self.transfers = [], [], []
        self.producers, self.counts = [], Counter()

    def allocate(self, node, size, shard=0, offset=0, original=False):
        base, capacity = self.windows[node]
        start = (self.cursor[node] + 7) // 8 * 8
        if start + size > capacity:
            raise ValueError("collective storage exceeds memory window")
        region = dma.Region(node, base + start, size, shard, offset)
        self.cursor[node] = start + size
        self.storage.append(region)
        if original:
            self.initial.append(region)
        return region

    def waits(self, region):
        return tuple(key for parent, key in self.producers if contains(parent, region))

    def send(self, source, node, phase, local=None):
        destination = self.allocate(node, source.length, source.shard, source.offset)
        waits = dict(self.waits(source))
        if local:
            for peer, count in self.waits(local):
                waits[peer] = max(waits.get(peer, 0), count)
        ordered = tuple(sorted(waits.items()))
        self.transfers.append(dma.Transfer(source.node, source, destination,
                              ordered[0] if ordered else None, barrier=ordered[1:],
                              phase=phase, local_partial=local))
        self.counts[source.node] += 1
        self.producers.append((destination, (source.node, self.counts[source.node])))
        return destination

    def gather(self, owned):
        return tuple(r if r.node == 0 else self.send(r, 0, "gather") for r in owned)

    def all_gather(self, owned, cycle):
        available = {(r.node, r.shard, r.offset): r for r in owned}
        for step in range(len(cycle) - 1):
            received = {}
            for rank, node in enumerate(cycle):
                original = owned[(rank - step) % len(cycle)]
                source = available[node, original.shard, original.offset]
                target = cycle[(rank + 1) % len(cycle)]
                dest = self.send(source, target, f"all_gather_s{step}")
                received[target, dest.shard, dest.offset] = dest
            available.update(received)
        return tuple(available.values())

    def reduce_scatter(self, initial, x, y, side):
        if side == 1:
            return {y * 4 + x: initial[y * 4 + x]}
        half = side // 2
        origins = ((x, y + half), (x + half, y + half),
                   (x + half, y), (x, y))
        children = [self.reduce_scatter(initial, a, b, half) for a, b in origins]
        offsets = sorted((r.offset, r.length) for r in children[0].values())
        output = {}
        for offset, length in offsets:
            originals = [next(r for r in child.values()
                              if (r.offset, r.length) == (offset, length)) for child in children]
            size = length // 4
            def chunk(region, index):
                return replace(region, address=region.address + index * size,
                               offset=region.offset + index * size, length=size)
            available = {(rank, index): chunk(region, index)
                         for rank, region in enumerate(originals) for index in range(4)}
            for step in range(3):
                received = {}
                for rank in range(4):
                    index = (rank - step) % 4
                    receiver = (rank + 1) % 4
                    dest = self.send(available[rank, index], originals[receiver].node,
                                     f"reduce_scatter_r{side.bit_length()-1}_s{step}",
                                     chunk(originals[receiver], index))
                    received[receiver, index] = dest
                available.update(received)
            for rank, original in enumerate(originals):
                output[original.node] = available[rank, (rank + 1) % 4]
        return output


def build_direct(topo, length, direction, operation):
    """Independent original inputs go directly to their final contribution owners."""
    if direction != "write" or type(length) is not int or length <= 0 or length % 4:
        raise ValueError("direct XY requires positive 32-bit-element input and DMA Write")
    match = re.fullmatch(r"xy_direct_(gather|all_gather|reduce_scatter|reduce|all_reduce)_p(4|16)", operation)
    if not match:
        raise ValueError("unknown direct operation")
    kind, count = match[1], int(match[2])
    if length % (4 * count):
        raise ValueError("per-PE element count must be divisible by participants")
    nodes = tuple(sorted(CYCLES[count]))
    root = min(nodes, key=lambda r: (sum(len(route(n,r))-1 for n in nodes),
                                     max(len(route(n,r))-1 for n in nodes), r))
    b = Builder(topo)
    initial = {n: b.allocate(n, length, shard=rank, original=True)
               for rank, n in enumerate(nodes)}
    outputs = []
    targets = (root,) if kind in ("gather", "reduce") else nodes
    # Ascending destination order per issuer is the unoptimized baseline order.
    # Contributions use separate slots so receivers can accept independently.
    for source in nodes:
        for target in targets:
            region = initial[source]
            if kind == "reduce_scatter":
                size = length // count
                offset = nodes.index(target) * size
                region = replace(region, address=region.address + offset,
                                 offset=offset, length=size)
            outputs.append(region if source == target else
                           b.send(region, target, "direct_delivery"))
    contract = dict(schema=2, kind=kind, participants=list(nodes),
                    root=root if kind in ("gather", "reduce") else None,
                    per_pe_bytes=length, elements_per_pe=length // 4,
                    readiness="original_inputs_ready", routing="XY", variant="direct_xy",
                    destination_order="ascending_node_id",
                    output_representation="contribution_slots" if kind in
                        ("reduce", "reduce_scatter", "all_reduce") else "original_slots")
    contract["allocated_bytes"] = dict(b.cursor)
    plan = dma.Plan(tuple(b.initial), tuple(b.transfers), tuple(b.storage), operation,
                    outputs=tuple(outputs), contract=contract)
    validate(plan)
    contract["costs"] = costs(plan)
    return plan


def validate_direct_outputs(plan, states, counts):
    """Check each destination/index has exactly one contribution from every source."""
    contract = plan.contract
    nodes = tuple(sorted(contract["participants"]))
    kind, q = contract["kind"], contract["per_pe_bytes"]
    targets = (contract["root"],) if kind in ("gather", "reduce") else nodes
    expected = set()
    for target in targets:
        size = q // len(nodes) if kind == "reduce_scatter" else q
        offset = nodes.index(target) * size if kind == "reduce_scatter" else 0
        for rank, source in enumerate(nodes):
            expected.add((target, rank, offset, size, source))
    actual = []
    for region in plan.outputs:
        found = [(owner, key) for parent, owner, key in states if contains(parent, region)]
        if len(found) != 1 or len(found[0][0]) != 1:
            raise ValueError("invalid direct contribution provenance")
        owners, key = found[0]
        if key and counts[key[0]] < key[1]:
            raise ValueError("unavailable direct output")
        actual.append((region.node, region.shard, region.offset, region.length, next(iter(owners))))
    if len(actual) != len(expected) or set(actual) != expected:
        raise ValueError("incomplete or duplicate direct output contribution coverage")
    received = Counter(t.destination for t in plan.transfers)
    expected_received = Counter(r for r in plan.outputs if not any(contains(i, r) for i in plan.initial))
    if received != expected_received:
        raise ValueError("direct transfers differ from final contribution slots")
    for transfer in plan.transfers:
        if dma.requirements(transfer) or transfer.local_partial or not any(
                contains(r, transfer.source) for r in plan.initial):
            raise ValueError("direct baseline requires independent original sources")


def build(topo, length, direction, operation):
    if operation.startswith("xy_direct_"):
        return build_direct(topo, length, direction, operation)
    if direction != "write" or type(length) is not int or length <= 0 or length % 4:
        raise ValueError("XY cases require positive 32-bit-element input and DMA Write")
    b = Builder(topo)
    match = re.fullmatch(r"xy_(gather|all_gather|reduce_scatter|reduce|all_reduce)_p(4|16)", operation)
    if match:
        kind, count = match[1], int(match[2])
        cycle = CYCLES[count]
        if length % (4 * count):
            raise ValueError("per-PE element count must be divisible by participants")
        if kind in ("gather", "all_gather"):
            owned = tuple(b.allocate(node, length, shard=rank, original=True)
                          for rank, node in enumerate(cycle))
            outputs = b.gather(owned) if kind == "gather" else b.all_gather(owned, cycle)
        else:
            initial = {node: b.allocate(node, length, original=True) for node in cycle}
            reduced = b.reduce_scatter(initial, 0, 0, 2 if count == 4 else 4)
            owned = tuple(reduced[node] for node in cycle)
            outputs = (owned if kind == "reduce_scatter" else b.gather(owned)
                       if kind == "reduce" else b.all_gather(owned, cycle))
        contract = dict(schema=1, kind=kind, participants=list(cycle), root=0,
                        per_pe_bytes=length, elements_per_pe=length // 4,
                        readiness="whole_transfer_response", routing="XY",
                        variant={"gather":"direct_xy", "all_gather":"adjacent_2d_ring",
                                 "reduce_scatter":"recursive_quadrants",
                                 "reduce":"recursive_then_direct_xy",
                                 "all_reduce":"recursive_then_adjacent_2d_ring"}[kind])
    else:
        if operation == "xy_descriptor_chain":
            pairs = [(0, 1), (1, 2)]
            initial = b.allocate(0, length, original=True)
            first = b.send(initial, 1, "descriptor_first")
            outputs = (b.send(first, 2, "descriptor_second"),)
        else:
            mode, src, dst = re.fullmatch(r"xy_(cal|duplex)_(\d+)_(\d+)", operation).groups()
            pairs = [(int(src), int(dst))]
            if mode == "duplex":
                pairs.append((int(dst), int(src)))
            outputs = tuple(b.send(b.allocate(src, length, shard=i, original=True), dst, "calibration")
                            for i, (src, dst) in enumerate(pairs))
        contract = dict(schema=1, kind="calibration", participants=sorted({n for pair in pairs for n in pair}),
                        pairs=pairs, per_pe_bytes=length, routing="XY",
                        readiness="whole_transfer_response", variant=operation)
    contract["allocated_bytes"] = dict(b.cursor)
    plan = dma.Plan(tuple(b.initial), tuple(b.transfers), tuple(b.storage), operation,
                    outputs=tuple(outputs), contract=contract)
    validate(plan)
    contract["costs"] = costs(plan)
    return plan


def validate(plan):
    """Track immutable interval versions and disjoint provenance on both operands."""
    states = [(r, frozenset((r.node,)), None) for r in plan.initial]
    counts = Counter()
    def lookup(region, waits):
        matches = [(owner, key) for parent, owner, key in states if contains(parent, region)]
        if len(matches) != 1:
            raise ValueError("unavailable or ambiguous input interval")
        owners, key = matches[0]
        if key and waits.get(key[0], 0) < key[1]:
            raise ValueError("missing operand producer dependency")
        return owners
    for transfer in plan.transfers:
        src, dst = transfer.source, transfer.destination
        if (src.length, src.shard, src.offset) != (dst.length, dst.shard, dst.offset):
            raise ValueError("changed payload index identity")
        if src.node != transfer.issuer or src.node == dst.node:
            raise ValueError("invalid XY DMA issuer/destination")
        waits = dict(dma.requirements(transfer))
        for peer, count in waits.items():
            if count <= 0 or count > counts[peer]:
                raise ValueError("dependency on unavailable producer")
        owners = lookup(src, waits)
        if transfer.local_partial:
            local = transfer.local_partial
            if (local.node != dst.node or (local.length, local.shard, local.offset) !=
                    (src.length, src.shard, src.offset)):
                raise ValueError("mismatched local contribution")
            other = lookup(local, waits)
            if owners & other:
                raise ValueError("duplicate contributor")
            owners |= other
        if any(r.node == dst.node and max(r.address, dst.address) <
               min(r.address + r.length, dst.address + dst.length) for r, _, _ in states):
            raise ValueError("overwritten storage")
        counts[transfer.issuer] += 1
        states.append((dst, owners, (transfer.issuer, counts[transfer.issuer])))
    if plan.contract.get("schema") == 2:
        validate_direct_outputs(plan, states, counts)
        return
    kind = plan.contract["kind"]
    participants = set(plan.contract["participants"])
    output = {}
    for region in plan.outputs:
        owners = lookup(region, counts)
        if kind in ("reduce", "reduce_scatter", "all_reduce") and owners != participants:
            raise ValueError("incomplete final contributors")
        output.setdefault(region.node, []).append((region.shard, region.offset, region.length))
    if kind == "calibration":
        return
    expected_nodes = {0} if kind in ("gather", "reduce") else participants
    if set(output) != expected_nodes:
        raise ValueError("wrong output owners")
    q = plan.contract["per_pe_bytes"]
    if kind == "reduce_scatter":
        spans = sorted((off, size) for items in output.values() for _, off, size in items)
        if len(spans) != len(participants) or spans != [(i*q//len(participants), q//len(participants))
                                                       for i in range(len(participants))]:
            raise ValueError("incomplete or overlapping result partition")
    else:
        for items in output.values():
            expected = ([(i, 0, q) for i in range(len(participants))]
                        if kind in ("gather", "all_gather") else
                        [(0, i*q//len(participants), q//len(participants)) for i in range(len(participants))])
            if sorted(items) != expected:
                raise ValueError("incomplete output coverage")


def costs(plan):
    """Payload-byte costs before packetization; critical paths include both inputs."""
    sends, receives, links, counts = Counter(), Counter(), Counter(), Counter()
    ready, records, events = [], [], {}
    beat_bytes = dma.gen_tb_top._constant("axi", "DATA_WIDTH") // 8
    def predecessor(region):
        return next((key for parent, key in ready if contains(parent, region)), None)
    for t in plan.transfers:
        path = route(t.source.node, t.destination.node)
        hops = len(path)-1
        sends[t.source.node] += t.source.length
        receives[t.destination.node] += t.source.length
        for edge in zip(path, path[1:]):
            links[edge] += t.source.length
        keys = [k for r in (t.source, t.local_partial) if r is not None
                for k in [predecessor(r)] if k]
        depth = 1 + max((events[k][0] for k in keys), default=0)
        source_key = predecessor(t.source)
        local_key = predecessor(t.local_partial) if t.local_partial else None
        distance = max(hops + (events[source_key][1] if source_key else 0),
                       events[local_key][1] if local_key else 0)
        counts[t.issuer] += 1
        key = (t.issuer, counts[t.issuer])
        events[key] = depth, distance
        ready.append((t.destination, key))
        service_bytes = max((r.address % beat_bytes + r.length + beat_bytes - 1) // beat_bytes
                            for r in (t.source, t.destination)) * beat_bytes
        records.append(dict(issuer=t.issuer, job=key[1], bytes=t.source.length,
                            service_bytes=service_bytes,
                            path=list(path), prerequisites=list(dma.requirements(t)),
                            phase=t.phase))
    return dict(C_bytes=max([*sends.values(), *receives.values()], default=0),
                E_byte_hops=sum(links.values()), N_directed_links=len(links),
                L_hops=max((x[1] for x in events.values()), default=0),
                traffic_depth=max((x[0] for x in events.values()), default=0),
                paper_D=None, paper_D_note="CE computation is not simulated",
                sends_bytes=dict(sends), receives_bytes=dict(receives),
                directed_link_bytes={f"{a}to{b}":v for (a,b),v in links.items()}, transfers=records)
