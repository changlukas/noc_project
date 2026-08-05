# Response-ordering trade-off record

Rationale for the NI's AXI response ordering. `docs/nmu-spec.md` (RoB, virtual
networks) and `docs/nsu-spec.md` (meta buffer, response VC selection) specify the
as-built mechanisms. This record explains the choice behind them.

## The problem

AXI requires same-ID responses in issue order toward the master. A lossless
packet-switched NoC guarantees that a packet arrives, and nothing about when or
in what order, so the design must add the ordering itself, in the fabric or at
the endpoint. Reordering has three sources:

| source | mechanism |
|---|---|
| latency variation | round-trip time differs across destinations, and across time for one destination under congestion |
| path divergence | adaptive or multi-path routing gives two packets of one flow different routes, and the later-issued one can arrive first |
| cross-VC overtaking | a flow split across VCs sits in separate queues, and the common arbiters (round-robin, matrix, separable, wavefront) grant without regard to arrival order (on-chip-networks Ch 6) |

AXI same-ID order decomposes into point-to-point order (same source and
destination, issue order) plus a cross-destination window. Point-to-point order
follows from deterministic routing plus an unsplit flow (on-chip-networks
Ch 7). Global order, a single total order seen by all nodes, is out of scope.

The precise requirement is that a flow must not split across VCs. The stronger
reading, one VC identifier end to end, over-constrains. A packet's own flits
cannot reorder: the head allocates the VC, body and tail inherit it, and the
wormhole lock keeps them contiguous (on-chip-networks Ch 5). Order between
packets of one flow survives whenever the VC assignment is deterministic and
refuses on a full queue instead of spilling to another VC. Per-hop VC changes
stay legal as long as all packets of the flow change the same way at each hop.

## Textbook solutions

| scheme | ordering mechanism | cost |
|---|---|---|
| stall | one VC per flow, injection waits until the prior same-ID transaction drains | zero reorder storage, cross-destination transactions serialize |
| in-order arbitration | fabric arbiters grant by arrival age across VCs | re-serializes the flow and gives back the throughput multi-VC bought, plus per-port age tracking, rare in practice |
| endpoint reorder buffer | the fabric applies no ordering, an endpoint RoB re-sorts by sequence number | largest buffer, covering the intra-destination window as well as the cross-destination one, and the endpoint must reserve its space before injection |

VCs relieve head-of-line blocking by letting a later packet overtake a blocked
one. Ordering forbids overtaking within a flow. A flow therefore cannot spread
across VCs for throughput and stay ordered, unless an in-order arbiter
re-serializes it and returns most of the throughput the extra VCs bought.

## The shipped compromise

The shipped design sits outside this menu. Hardware area cost and performance
drove it: the fabric keeps same-destination order in-network, and `nmu::Rob`
with a bypass path handles cross-destination reordering at the endpoint.

| piece | mechanism |
|---|---|
| routing | XY dimension-order, one fixed path per source-destination pair |
| fixed VC id, request | `nmu::VcAllocator` reuses the ID's recorded VC when an `ordering_req = 0` flit repeats its `(dst_id, id)`. A blocked fixed VC waits instead of rerouting |
| fixed VC id, response | `nsu::VcAllocator` maps R to `(dst_id ^ rid) % num_vc`, stateless. Full or no-credit refuses, never spills |
| endpoint reorder | `nmu::Rob` re-sorts only same-ID responses interleaved across destinations. Ordering toward the master lives in the NMU because only the NMU knows its own issue order |
| identity echo | `nsu::MetaBuffer` holds `{src_id, upstream_id, ordering_req, ordering_tag}` per response; `nsu::Packetize` stamps them onto B/R. Neither reorders, and the fabric never reads either field |

The bypass path does the area work. An in-order response is by construction the
next to release, so it drains without taking a slot: idle-ID and
same-destination-streak requests leave with `ordering_req = 0` and skip slot
allocation end to end. This buys two things. The RoB shrinks beyond what
fabric-side same-destination ordering alone allows, because it holds only the
responses that can arrive out of order rather than one entry per outstanding
transaction. And a long same-`(id, dst)` burst flows through without clogging
the pool, so slots stay free for the cross-destination traffic that needs
reordering.

`RobMode::Disabled` is the stall scheme in this design: a per-ID flag
(`read_outstanding_`) refuses a second same-ID push while one is outstanding,
with zero reorder storage. The B side has no mode. A B slot holds metadata
only, so the B RoB stays on.

| mode | fabric guarantee | endpoint storage | stalls |
|---|---|---|---|
| `RobMode::Enabled` | same-`(dst, id)` responses in order | RoB sized to the cross-destination window, bypass traffic takes no slot | only on slot exhaustion |
| `RobMode::Disabled` | same | none | every same-ID transaction waits out its predecessor |

## Invariants

- The ordering guarantee must hold on the response network, the direction the
  master observes. Determinism on the request network alone proves nothing
  about R/B arrival order.
- `ordering_req = 1` traffic enters the network only with a reserved slot. Admission
  gates on RoB space up front, because a response stalled in-fabric with
  nowhere to land would block every flit behind it. Slot reservation before
  injection is the fabric's deadlock gate.
- The same-destination bypass stays sound only while same-`(dst, id)` responses
  cannot split across VCs. The fixed VC id provides that. A round-robin spread
  of the streak breaks it, which is why each streak stays on one VC per
  network.
- No safe design pairs a fabric that may reorder same-destination responses
  with an endpoint that performs no reordering. Dropping the fixed VC id grows
  the RoB back to the endpoint-reorder-buffer scheme's size.

## Open costs

- A flow cannot spread across VCs, so head-of-line blocking within a flow
  remains. This is the recurring cost of the small RoB.
- No sweep exists for `max_txns_per_id` (default 32, `[TBD]`) or `r_rob_depth`
  (default 32, expressible to 256 via `R_ROB_DEPTH`).
- `RobMode::Disabled` refuses a second same-ID push while one is outstanding.
  The upstream counter scheme admits same-destination successors. No
  measurement exists of the throughput the simpler flag gives up.
