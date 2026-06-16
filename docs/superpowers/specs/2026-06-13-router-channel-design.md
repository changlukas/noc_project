# RouterChannel Integration Design

Date: 2026-06-13
Status: Approved pending user review
Scope: a production `RouterChannel` that wires the existing `Router` into the NMU↔NSU
datapath as a 2-node, 1-hop, full-duplex fabric segment, validated end-to-end by a
bidirectional loopback reusing the existing scoreboard. Sub-project B of the NI+router
integration. Multi-hop / NxM mesh / >2 NSU / cosim are out of scope (later rounds / C).

## 1. Motivation

The `Router` (sub-project, merged) has only ever been tested standalone with synthetic
flits; the NMU↔NSU integration path still uses the `ChannelModel` stub. This wires the real
`Router` into that path. Sub-project A (NMU VcArbiter sticky `(channel,id)→vc` binding,
merged) makes multi-VC ordering safe, so the integration can run the full `num_vc` grid.

## 2. Node model and topology

Two symmetric nodes `(0,0)` and `(1,0)`, each a full Network Interface (NMU + NSU). Both
nodes can issue requests and receive responses.

- 4 `Router` instances: REQ network 2, RSP network 2 (REQ/RSP are separate physical
  networks — protocol-deadlock separation). Each network's two routers are joined by a
  full-duplex inter-router link (two unidirectional segments, independent per-VC credit).
- Per node, per network, the `LOCAL` port is bidirectional and serves both local NI units:

| Network | `LOCAL` input (inject) | `LOCAL` output (eject) |
|---|---|---|
| REQ | local NMU injects requests | local NSU receives requests |
| RSP | local NSU injects responses | local NMU receives responses |

- 8 LOCAL adapters total (inject + eject, × 2 networks, × 2 nodes).

## 3. Port wiring

`(0,0)` is west of `(1,0)` (lower X). A router's WEST port faces lower X, EAST faces higher
X, so adjacent routers join WEST↔EAST. `route_compute` matches `nmu::addr_trans::xy_route`
(X in low `dst_id` bits): `dst_x > cfg.x → EAST`, `dst_x < cfg.x → WEST`.

REQ network (full-duplex):

```
R_req(1,0).WEST_out ──► R_req(0,0).EAST_in     request (1,0)→(0,0): NMU(1,0) → NSU(0,0)
R_req(0,0).EAST_out ──► R_req(1,0).WEST_in     request (0,0)→(1,0): NMU(0,0) → NSU(1,0)
```

RSP network is a second symmetric pair carrying responses back to each request's source.

Routing traces (verify intent):
- NMU(1,0) issues `dst=(0,0)`: LOCAL_in → `dst_x 0<1` → WEST_out → link → (0,0).EAST_in →
  `dst==self` → LOCAL_out → NSU(0,0).
- NMU(0,0) issues `dst=(1,0)`: LOCAL_in → `dst_x 1>0` → EAST_out → link → (1,0).WEST_in →
  `dst==self` → LOCAL_out → NSU(1,0).

## 4. Adapters (production link contract)

The NoC interfaces (`NocReqOut/In`, `NocRspOut/In`) use a retryable push + credit-query +
pull model; the `Router` link contract is void-push + registered credit-return pulse. Three
adapter types bridge them.

**InjectAdapter** (implements `NocReqOut` / `NocRspOut`):
- Holds a per-VC credit mirror, seeded at construction to the downstream router LOCAL input
  VC FIFO depth.
- `push_flit(flit) → bool`: returns `false` (backpressure) if the mirror has no credit for
  the flit's VC **or** a flit was already pushed to this LOCAL input this tick
  (landing-register guard — the router accepts one flit per port per cycle and asserts on a
  second). Otherwise forwards to `router.input(LOCAL).push_flit(flit)`, decrements the
  mirror, returns `true`.
- `credit_avail(vc) → bool`: mirror has credit AND not yet pushed this tick.
- Is the router's `RouterCreditSink` (`set_upstream_credit(LOCAL, adapter)`); a credit pulse
  replenishes the mirror. The per-tick push flag resets each `tick()`.

**EjectAdapter** (implements `NocReqIn` / `NocRspIn`):
- Is the router's `downstream(LOCAL)` `RouterLink`; `push_flit` buffers ejected flits in a
  bounded queue.
- `pop_flit() → optional<Flit>`: serves from the queue; on consume, calls
  `router.receive_credit(LOCAL_out_port, vc)` to return the slot.
- Invariant (mandatory): the eject queue is SHARED across all VCs, so its depth must equal the
  AGGREGATE LOCAL-output credit = `num_vc * vc_depth` (the per-VC seed alone is insufficient when
  `num_vc>1`: the router has `num_vc` independent LOCAL-output credit counters each seeded to
  `vc_depth`, so with the NSU stalled it can grant up to `num_vc * vc_depth` flits into the one
  shared queue). `RouterLink::push_flit` is `void` and cannot backpressure, so this depth is what
  guarantees no overflow — the router only ejects when it holds LOCAL-output credit, and one
  ejected flit consumes exactly one queue slot. The credit-conservation unit test (§7) asserts
  this; there is no natural backpressure to fall back on.

**CreditRelay** (`RouterCreditSink`): forwards a downstream router's input credit pulse to
the upstream router's `receive_credit(port, vc)` for the matching inter-router port pair
(per link direction).

## 5. tick orchestration

`RouterChannel.tick()` advances one cycle: it resets each InjectAdapter's per-tick push flag,
then ticks the 4 routers and drains adapters in a fixed order. Each `Router::tick()` is
evaluate-then-commit (reverse stage order, one stage advanced per tick). The fixed router
order makes one traversal direction one tick faster than the other (directional latency
asymmetry); this affects timing only — the scoreboard checks data, not cycle counts.

Tick-boundary contract (required for the InjectAdapter landing-register guard): the
`pushed_this_tick` flag spans the whole producer injection window of a cycle. The test loop
calls each producer's `tick()` (which injects at most one flit per port via the NMU/NSU VC
arbiter) and `RouterChannel.tick()` exactly once per cycle; `RouterChannel.tick()` resetting
the flag is the cycle boundary. A producer must not push twice between two
`RouterChannel.tick()` calls — which the NMU/NSU naturally satisfy (one drained flit per
tick), but the guard enforces it regardless.

## 6. Ordering

Same-`(channel, axi_id)` traffic keeps one VC (sub-project A binding) and one deterministic
route, so it stays in one per-VC FIFO chain end-to-end and cannot be overtaken. The router
preserves `vc_id`. This is the structural ordering guarantee for the loopback's same-ID
traffic across the per-VC fabric.

Cross-flow isolation (the two simultaneous directions) does NOT rely on the flows using
different VC numbers — they may share VC ids. Isolation comes from the structure: REQ and RSP
are separate router instances, and the two request directions traverse opposite unidirectional
link segments and different `(input port → output port)` pairs within each router (flow A:
`LOCAL_in → WEST_out`; flow B: `WEST_in → LOCAL_out`). Different `(input port, vc)` FIFOs, so
no cross-flow overtaking.

## 7. Verification

**RouterChannel unit tests** (`c_model/tests/noc/test_router_channel.cpp`):
1. Single-flit end-to-end: a flit injected at one node's NMU-facing REQ port is ejected at
   the other node's NSU-facing REQ port (and the RSP reverse), through both routers.
2. Credit conservation: per `(network, link, vc)`, available credit + in-flight +
   buffered-downstream is invariant across every tick; asserted explicitly (not scoreboard-
   inferred).
3. Full backpressure: with the downstream NI not popping, after the mirror credit drains the
   InjectAdapter's `credit_avail`/`push_flit` return false (no flit lost, no router assert).

**Integration loopback (bidirectional)** (`c_model/tests/integration/test_router_loopback.cpp`):
- This is a NEW harness, not a drop-in reuse of `test_request_response_loopback.cpp`. That
  test is single-master/single-slave (one `Nmu`, one `AxiMaster`, one `AxiSlave`, one
  scoreboard, one `master.tick()`/`nmu.tick()` in the loop). The bidirectional test needs two
  full NI nodes — two `Nmu` + two `AxiMaster` + two `NSU`/`AxiSlave` + two response paths —
  driven in one tick loop alongside `RouterChannel.tick()`.
- Oracle reuse is at the COMPONENT level: the existing scoreboard class is the per-flow
  oracle, instantiated once per flow (two instances), each attached to its own master's
  callbacks. The harness wiring, the two-master tick loop, and the per-flow address ranges
  are new code; only the scoreboard/oracle class is reused.
- NMU(1,0) drives low-address scenarios (`dst=(0,0)`); NMU(0,0) drives high-address scenarios
  (`dst=(1,0)`); both run simultaneously. Each scoreboard asserts zero mismatch on its flow.
- Parameterized over `num_vc ∈ {1, 2, 4, 8}` (A binding holds same-ID order through the
  per-VC fabric). ReadWriteSplit candidate sets used for multi-VC.

## 8. Errors and reset

- Node coordinates and scenario address ranges keep all `dst_id` in mesh, so the router's
  out-of-mesh and `vc_id`-range aborts are not triggered in normal operation.
- Reset is not modeled (the router's "construction = reset" stands; no partial reset).

## 9. File structure

| File | Responsibility |
|---|---|
| `c_model/include/noc/router_channel.hpp` | `RouterChannel` + InjectAdapter / EjectAdapter / CreditRelay; owns the 4 routers + 8 adapters; exposes per-node `nmu_req_out(node)`, `nmu_rsp_in(node)`, `nsu_req_in(node)`, `nsu_rsp_out(node)` (idx form for future multi-NSU) + `tick()` |
| `c_model/tests/noc/test_router_channel.cpp` | the 3 unit tests (§7) |
| `c_model/tests/integration/test_router_loopback.cpp` | the bidirectional loopback (§7) |

## 10. Scope boundary

In scope: production `RouterChannel` as a 2-node, 1-hop, full-duplex segment; symmetric
full-NI nodes; bidirectional loopback over the `num_vc` grid. Out of scope: multi-hop / NxM
mesh / >2 NSU per channel; per-NSU latency/route programming (`ChannelModel`'s
`set_dst_route`/`set_nsu_latency` are not mirrored); cosim wrapper (sub-project C); reset.
The per-node `idx`-style accessors leave room for multi-NSU without an interface break.
