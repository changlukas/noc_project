# Response-ordering trade-off record

Why the NI orders AXI responses the way it does: the problem, the textbook solution
space, and the compromise this design ships. `docs/spec.md` (Response ordering, Virtual
networks) is the as-built reference this record argues for.

## The problem

A lossless packet-switched NoC guarantees delivery, not ordered delivery (on-chip-networks
Ch 2). AXI requires same-ID responses in issue order toward the master, so ordering must be
engineered, either in the fabric or at the endpoint. Reordering enters through three doors:

| source | mechanism |
|---|---|
| latency variation | different destinations, or one destination under congestion, return with different round-trip times |
| path divergence | adaptive or multi-path routing lets two packets of one flow take different routes, so the later-issued one can arrive first |
| cross-VC overtaking | packets of one flow split across VCs sit in separate queues, and the common arbiters (round-robin, matrix, separable, wavefront) serve without regard to arrival order (Ch 6) |

Two ordering levels exist (Ch 2/4). Point-to-point order (same source and destination
delivered in issue order) follows from deterministic routing plus never splitting a flow
across VCs. Global order (one total order observed by all nodes) needs destination
reordering or an ordering network. This design needs AXI same-ID order only, which is
point-to-point order plus a cross-destination window, and no global order.

One clarification carries most of the design. The requirement is "do not split a flow
across VCs", not "keep one VC identifier end to end". A packet's own flits never reorder:
the head allocates the VC, body and tail inherit it, and the wormhole hold keeps them
contiguous (Ch 5). Order between packets of one flow survives exactly when the VC
assignment is deterministic and never spills to another VC on a full queue. Per-hop VC
changes stay legal as long as every packet of the flow changes identically.

## Textbook solutions

| scheme | ordering mechanism | cost |
|---|---|---|
| stall | one VC per flow, injection stalls until the prior same-ID transaction drains | zero reorder storage; cross-destination transactions serialize |
| in-order arbitration | fabric arbiters serve by arrival age across VCs | re-serializes the flow, returning the throughput multi-VC borrowed; per-port age tracking; rare in practice |
| endpoint reorder buffer | fabric reorders freely, an endpoint RoB re-sorts by sequence number | largest buffer: it must cover the intra-destination window as well as the cross-destination one, and its space must be reserved before injection |

The underlying tension: multiple VCs exist to allow overtaking (that is how they relieve
head-of-line blocking), and ordering forbids overtaking within the flow. Every scheme
picks which side pays.

## The shipped compromise

Not one of the three. A hybrid motivated by hardware area cost and performance: the
fabric keeps same-destination order, a small NMU RoB absorbs only the cross-destination
window, and a bypass path keeps in-order traffic out of the buffer entirely.

- Fabric side: XY deterministic routing plus the fixed VC id. The request side reuses the
  last same-channel VC per `(dst_id, id)` (`nmu::VcArbiter`); the response side hashes
  `vnet[(dst_id ^ id) % size]`, a pure function with zero state (`nsu::VcArbiter`). A
  same-`(dst, id)` stream therefore never splits across VCs and returns in order.
- Endpoint side: `nmu::Rob` reorders only same-ID responses interleaved across
  destinations. Only the NMU knows its own issue order, so ordering toward the master
  lives there. `nsu::MetaBuffer` restores identity and performs no reordering. `rob_idx`
  and `rob_req` ride the wire as header fields the fabric never reads.
- Bypass path, the load-bearing piece of the area case: an in-order response is by
  definition the next to release, so it can drain immediately without storage. Idle-ID
  and same-destination-streak requests are marked `rob_req = 0` and skip slot allocation
  end to end. This shrinks the RoB to the actual cross-destination reordering window
  instead of the outstanding-transaction count, and it keeps a long same-`(id, dst)`
  burst from clogging the pool: the streak flows through while slots stay free for the
  traffic that needs them.
- Degenerate mode: `RobMode::Disabled` is the stall scheme, one flag per ID
  (`read_outstanding_`), zero reorder storage, strictly less throughput. The B side has no
  mode: a B slot is metadata only, so its RoB is unconditionally on.

| mode | fabric guarantee | endpoint storage | stalls |
|---|---|---|---|
| `RobMode::Enabled` | same-`(dst, id)` in order | RoB sized to the cross-destination window; bypass traffic takes no slot | only on slot exhaustion |
| `RobMode::Disabled` | same | none | every same-ID transaction waits out its predecessor |

## Invariants

- The ordering guarantee lives on the response direction, the direction the master
  observes. Request-side determinism alone proves nothing.
- `rob_req = 1` traffic never enters the network without a reserved slot. A full RoB
  gates admission instead of stalling the network: a response with nowhere to go blocks
  every flit behind it. This gate is the fabric's deadlock guarantee, not a removable
  coupling.
- The same-destination bypass is sound only while same-`(dst, id)` responses cannot split
  across VCs. The fixed VC id provides that. Round-robin VC spread breaks it, which was
  found as a risk and closed by fixing a bypass streak to one VC per network.
- There is no safe combination of "the fabric may reorder same-destination responses"
  and "no endpoint reordering". Drop the fixed VC id and the RoB must grow to scheme
  three's size.

## Open costs

- A flow cannot spread across VCs, so head-of-line blocking within a flow is the standing
  price of the small RoB.
- `max_txns_per_id` (default 32, `[TBD]`) and `r_rob_depth` (default 32, expressible to
  256 via `R_ROB_DEPTH`) are unswept.
- `RobMode::Disabled` refuses any second same-ID push while one is outstanding. The
  upstream counter-based scheme admits same-destination successors; the throughput cost
  of the simpler flag has never been measured.
