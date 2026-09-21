"""Approved one-layer transport schedules, without numerical kernels.

Ring Reduce-Scatter schedule: https://www.cs.fsu.edu/~xyuan/paper/09jpdc.pdf,
section 4.1. Each received partial consumes the matching local partial before
forwarding. Contributor sets are checked symbolically. Payload bytes retain
chunk identity and are not numerical reduction results.
"""

import dependent_dma_plan as dma


def _waits(*groups):
    latest = {}
    for group in groups:
        for node, count in group:
            latest[node] = max(latest.get(node, 0), count)
    return tuple(sorted(latest.items()))


def validate_reductions(plan, participants=None):
    """Check whole-object ownership before splitting into DMA descriptors."""
    if plan.contract:
        from xy_collective_plan import validate
        return validate(plan)
    owners = {r: frozenset((r.node,)) for r in plan.initial}
    producers, retired = {}, {}
    reduced = {}
    if participants is None:
        participants = {t.destination.node for t in plan.transfers if t.local_partial}
    for transfer in plan.transfers:
        source, destination = transfer.source, transfer.destination
        if (source.shard, source.length) != (destination.shard, destination.length):
            raise ValueError("changed transport payload identity")
        if source not in owners or destination in owners:
            raise ValueError("unavailable source or overwritten destination")
        waits = dict(dma.requirements(transfer))
        if source in producers:
            node, count = producers[source]
            if waits.get(node, 0) < count:
                raise ValueError("missing forwarding dependency")
        contributors = owners[source]
        partial = transfer.local_partial
        if partial:
            if (partial not in plan.initial or partial.node != destination.node
                    or partial.shard != source.shard or partial.length != source.length
                    or contributors & owners[partial]):
                raise ValueError("invalid local reduction contribution")
            contributors |= owners[partial]
            reduced[transfer.phase, destination.shard] = contributors
        owners[destination] = contributors
        retired[transfer.issuer] = retired.get(transfer.issuer, 0) + 1
        producers[destination] = (transfer.issuer, retired[transfer.issuer])
    if not reduced or any(c != participants for c in reduced.values()):
        raise ValueError("incomplete Reduce-Scatter contributors")
    for region in plan.final:
        if region not in owners:
            raise ValueError("unwritten final region")


def layer_sequence(topo, shard_bytes, direction, operation):
    """Build the approved 512-token input, not an arbitrary workload sweep."""
    standalone = operation in ("reduce_scatter", "all_reduce")
    if direction != "write" or (not standalone and shard_bytes != 1024 * 1024):
        raise ValueError("approved layer input requires 1 MiB shards and write collectives")
    ring_plan = dma.all_gather(topo, shard_bytes, direction)
    ring = [r.node for r in ring_plan.initial]
    count = len(ring)
    groups, bases, sizes = dma.backing_layout(topo)
    nodes, _, _ = dma.gen_tb_top._nodes(topo)
    windows = {node: (bases[cid], sizes[cid]) for node, _, _, cid in nodes}
    memory_node, memory, _ = groups[0]
    windows[memory_node] = (memory["base"], memory["size"])
    cursor = {node: 0x1000 for node in windows}
    initial, final, transfers = [], [], []
    ready, counts = {}, {}
    next_identity = 0

    def identities():
        nonlocal next_identity
        result = list(range(next_identity, next_identity + count))
        next_identity += count
        return result

    def allocate(node, length, shard, *, original=False, after=()):
        base, size = windows[node]
        offset = cursor[node]
        if offset + length > size:
            raise ValueError("composite payload exceeds the existing memory window")
        cursor[node] += length
        region = dma.Region(node, base + offset, length, shard)
        final.append(region)
        if original:
            initial.append(region)
            ready[region] = after
        return region

    def send(source, destination, phase, *, local_partial=None, issuer=None, after=()):
        issuer = source.node if issuer is None else issuer
        waits = _waits(ready[source], ready.get(local_partial, ()), after)
        transfers.append(dma.Transfer(issuer, source, destination,
                                      waits[0] if waits else None,
                                      barrier=waits[1:], phase=phase,
                                      local_partial=local_partial))
        counts[issuer] = counts.get(issuer, 0) + 1
        ready[destination] = ((issuer, counts[issuer]),)

    def boundary(outputs):
        return {node: _waits(*(ready[r] for r in outputs if r.node == node))
                for node in ring}

    def gather(owned, phase):
        available = {(rank, rank): region for rank, region in enumerate(owned)}
        for step in range(count - 1):
            for rank, node in enumerate(ring):
                shard = (rank - step) % count
                source = available[rank, shard]
                receiver = (rank + 1) % count
                destination = allocate(ring[receiver], source.length, source.shard)
                send(source, destination, phase)
                available[receiver, shard] = destination
        return list(available.values())

    def scatter(length, phase, previous):
        ids = identities()
        partials = {(rank, shard): allocate(node, length, ids[shard], original=True,
                                           after=previous[node])
                    for rank, node in enumerate(ring) for shard in range(count)}
        available = dict(partials)
        for step in range(1, count):
            for rank in range(count):
                shard = (rank - step) % count
                source = available[rank, shard]
                receiver = (rank + 1) % count
                destination = allocate(ring[receiver], length, ids[shard])
                send(source, destination, phase, local_partial=partials[receiver, shard])
                available[receiver, shard] = destination
        return [available[rank, rank] for rank in range(count)]

    def layer(length, prefix, previous, sequence=False):
        owned = None
        for block in ("attention", "mlp"):
            name = f"{prefix}_{block}"
            if sequence:
                if owned is None:
                    ids = identities()
                    owned = [allocate(node, length, ids[rank], original=True,
                                      after=previous[node]) for rank, node in enumerate(ring)]
                previous = boundary(gather(owned, name + "_all_gather"))
            output = scatter(length, name + "_reduce_scatter", previous)
            owned = output
            if not sequence:
                output = gather(output, name + "_all_gather")
            previous = boundary(output)
        return previous

    previous = {node: () for node in ring}
    if standalone:
        outputs = scatter(shard_bytes, "reduce_scatter", previous)
        if operation == "all_reduce":
            outputs = gather(outputs, "all_gather")
        plan = dma.Plan(tuple(initial), tuple(transfers), tuple(final), operation)
        validate_reductions(plan)
        return plan
    previous = layer(shard_bytes, "prefill", previous, operation == "tp_layer_sequence")
    if operation == "prefill_decode":
        # Fixed approved model inputs: 8 KV heads, dimension 128, 16-bit K/V.
        tokens, kv_heads, head_dimension, element_bytes = 512, 8, 128, 2
        token_kv_bytes = kv_heads * head_dimension * 2 * element_bytes
        kv_shard_bytes = tokens * token_kv_bytes // count
        ids = identities()
        backing = []
        for rank, node in enumerate(ring):
            source = allocate(node, kv_shard_bytes, ids[rank], original=True,
                              after=previous[node])
            destination = allocate(memory_node, kv_shard_bytes, ids[rank])
            send(source, destination, "prefill_kv_store")
            backing.append(destination)
        # Decode starts after the complete prefill layer and all KV stores.
        prefill_done = _waits(*(ready[r] for r in backing))
        restored = []
        for node, source in zip(ring, backing):
            destination = allocate(node, source.length, source.shard)
            send(source, destination, "decode_kv_read", issuer=node, after=prefill_done)
            restored.append(destination)
        previous = boundary(restored)
        ids = identities()
        appended = []
        for rank, node in enumerate(ring):
            source = allocate(node, token_kv_bytes // count, ids[rank], original=True,
                              after=previous[node])
            destination = allocate(memory_node, source.length, source.shard)
            send(source, destination, "decode_kv_append")
            appended.append(destination)
        # Inputs at the compute boundary are prepared externally, released only
        # after this rank's KV reads and append retire. No compute delay is added.
        previous = {node: ready[r] for node, r in zip(ring, appended)}
        layer(shard_bytes // tokens, "decode", previous)
    plan = dma.Plan(tuple(initial), tuple(transfers), tuple(final), operation)
    validate_reductions(plan)
    return plan


def phase_results(log, plan):
    """Pair every issued/retired descriptor and retain observed phase windows."""
    import re

    expected, counts = {}, {}
    for transfer in plan.transfers:
        index = counts.get(transfer.issuer, 0)
        expected[transfer.issuer, index] = transfer
        counts[transfer.issuer] = index + 1
    events = {}
    pattern = r"\[dma_phase\] node=(\d+) job=(\d+) phase=(\w+) event=(issue|retire) cycle=(\d+)"
    for line in log.splitlines():
        if not line.startswith("[dma_phase]"):
            continue
        match = re.fullmatch(pattern, line)
        if not match:
            raise ValueError("malformed DMA phase event")
        node, index, phase, event, cycle = match.groups()
        key = (int(node), int(index))
        if key not in expected or phase != expected[key].phase or (key, event) in events:
            raise ValueError("unexpected DMA phase event")
        events[key, event] = int(cycle)
    if len(events) != 2 * len(expected):
        raise ValueError("missing DMA phase event")
    phases = {}
    for key, transfer in expected.items():
        start, end = events[key, "issue"], events[key, "retire"]
        if end < start or any(events[(node, count - 1), "retire"] >= start
                              for node, count in dma.requirements(transfer)):
            raise ValueError("phase transport dependency violated")
        result = phases.setdefault(transfer.phase, {"start_cycle": start, "end_cycle": end,
                                                   "delivered_payload_bytes": 0, "descriptors": 0})
        result["start_cycle"] = min(result["start_cycle"], start)
        result["end_cycle"] = max(result["end_cycle"], end)
        result["delivered_payload_bytes"] += transfer.source.length
        result["descriptors"] += 1
    for result in phases.values():
        result["duration_cycles"] = result["end_cycle"] - result["start_cycle"] + 1
    return phases
