# Architecture overview

This document describes the system context, component map, co-sim boundary,
verification layers, and AXI4 conformity scope for noc_project.

For build instructions and workflow conventions, see `docs/development.md`.

## Table of contents

1. [System context](#1-system-context)
2. [NI components -- NMU, NSU, NoC fabric stub](#2-ni-components----nmu-nsu-noc-fabric-stub)
3. [c_model component flow and tick discipline](#3-cmodel-component-flow-and-tick-discipline)
   - 3.1 [Component map](#31-component-map)
   - 3.2 [Tick semantics](#32-tick-semantics)
   - 3.3 [Extension boundaries](#33-extension-boundaries)
4. [Cosim and Verilator boundary](#4-cosim-and-verilator-boundary)
5. [Verification layers](#5-verification-layers)
6. [AXI4 conformity scope](#6-axi4-conformity-scope)
7. [References](#7-references)

---

## 1. System context

noc_project is a research-grade behavioural C++ model (c_model) of an
AXI4 Network-on-Chip Interface, paired with a Verilator co-simulation
harness that drives the c_model through a wire-level testbench. The goal
is AXI4 IHI 0022H conformity verification at the NI boundary before
committing to a silicon implementation.

System boundary:

~~~
 +-------------------+     +-------------------+
 |    AXI4 Master    |     |    AXI4 Slave      |
 +--------+----------+     +----------+---------+
          |  AW/W/AR/B/R              |  AW/W/AR/B/R
          v                           v
 +--------+----------+     +----------+---------+
 |       NMU         |     |       NSU           |
 | (Network Manager  |     | (Network Subordinate|
 |   Unit)           |     |   Unit)             |
 +--------+----------+     +----------+---------+
          |  NoC flits                |  NoC flits
          +-----------> [NoC] <-------+
~~~

The NMU and NSU are the primary subjects under test. The NoC fabric
between them is represented by a simple channel stub in the c_model
(ChannelModel) and is not under AXI4 conformity verification.

The c_model is the conformity verification vehicle at the AXI4 pin
boundary of the NMU and NSU.

---

## 2. NI components -- NMU, NSU, NoC fabric stub

### NMU (Network Manager Unit)

The NMU sits on the AXI-master ingress side. Its responsibilities:

- Accepts AW, W, AR beats from the AXI4 master.
- Packs AXI4 transactions into NoC flits and injects them into the
  request network.
- Zero-fills the `noc_qos`, `route_par`, and `flit_ecc` header fields in
  outbound flits (all three are deferred; see
  `c_model/include/nmu/packetize.hpp` lines 24-26).
- Manages a Reorder Buffer (RoB) for incoming B and R responses so that
  out-of-order network delivery is re-serialized per AXI4 ID ordering rules.
- Address translation is a System Address Map (SAM) lookup, not a
  bit-slice: a per-tile `{base, size, dst_id}` table
  (`addr_trans::SamTable`, `c_model/include/nmu/addr_trans.hpp`), loaded
  from the topology YAML `address_map` block
  (`c_model/include/nmu/sam_yaml.hpp`) -- the single source of truth
  shared by the c_model, the stimulus generator, and the testbench. At
  packetize time the address is range-matched to a tile, giving
  `dst_id` (from the table) and `local_addr = addr - tile base`
  (rebase: the subordinate sees a 0-based local address, not a
  coordinate-bearing global one). A miss (address maps to no tile)
  asserts (model policy, not AXI-faithful). A real interconnect would
  return DECERR, tracked as a separate feature. Tiles are 4 KB-granular (`base` and
  `size` both 4 KB multiples), so an AXI-legal burst (<=4 KB, cannot
  cross a 4 KB boundary) never crosses a tile boundary -- the NI does
  not split bursts across tiles. The wire flit format is unchanged:
  `dst_id` is already a header field and `awaddr` / `araddr` is already
  the payload address; SAM only changes which value each carries.

The RoB holds in-flight response slots indexed by `rob_idx` -- a header
field the NSU echoes back, not an AXI ID. A separate per-ID order list
records the program order of that ID's transactions; a response is
released only once every earlier transaction of the same ID has
released. `RobMode::Disabled`, the shipped default, allocates no slots
at all and instead permits one outstanding transaction per AXI ID. In
`RobMode::Enabled`, a transaction whose AXI ID has nothing in flight
allocates no slot either -- its response is forwarded directly -- which
is why a read burst longer than the pool is admissible.

Structure-by-structure, including the sizes and allocator choices the
C++ makes implicitly, and the FlooNoC parameters they correspond to:
`docs/nmu-rob-microarchitecture.md`.

### NSU (Network Subordinate Unit)

The NSU sits on the AXI-subordinate egress side. Its responsibilities:

- Receives request flits from the NoC, unpacks them, and drives AW/W/AR
  to the downstream AXI4 slave.
- Captures B and R responses from the slave, packs them into NoC flits
  (the `noc_qos`, `route_par`, and `flit_ecc` header fields are
  zero-filled, deferred), and injects them into the response network.
- The ECC CSR counters (`ECC_CORR_ERR_CNT`, `ECC_UNCORR_ERR_CNT`) are
  present in the register file but the ECC check logic is not yet
  implemented.

The NSU is asymmetric with the NMU: it does not have a RoB (response
ordering is the master's responsibility).

#### Meta buffer and downstream AXI ID remap

Ported from FlooNoC's `floo_meta_buffer` (spec
`docs/superpowers/specs/2026-07-09-nsu-meta-buffer-floonoc-alignment-design.md`).

A request flit carries the manager's AXI ID and, separately, the requesting tile
in the header field `src_id`. The NSU stores `{src_id, upstream_id, rob_req,
rob_idx}` in a **meta buffer** keyed by the ID it presents downstream, and
restores the manager's original ID into the B/R response
(`nsu/meta_buffer.hpp`, `nsu/packetize.hpp:93,108`). The response finds its way
home through `src_id`, never through the AXI ID.

| parameter | meaning | default |
|---|---|---|
| `max_outstanding` | shared pool depth per direction. One entry per in-flight transaction. This is a physical buffer, hence area. FlooNoC's `MaxTxns`. | 32 |
| `max_unique_ids` | count of distinct AXI IDs presented downstream. `1` collapses every request onto the all-ones ID; `AXI_ID_SPACE` passes the manager's ID through. No other value is legal. | 1 |

`remap_downstream_id` is a function of `upstream_id` alone. Feeding it `src_id`
would let two sources' read bursts sharing a restored `rid` be interleaved by
the subordinate, which would contend `nsu::VcArbiter::r_burst_vc_`.

#### What `max_unique_ids` is, and is not

**It is a statement about the downstream subordinate's ability to return
responses out of order**, and about the storage that ability forces on the NI.

- `max_unique_ids = 1`: one downstream ID, so AXI compels the subordinate to
  return responses in request order. The NI's metadata store can therefore be a
  plain FIFO. FlooNoC's `ChimneyDefaultCfg`.
- `max_unique_ids > 1`: several downstream IDs, so responses may return out of
  order across IDs. The metadata store must be an ID-addressable queue
  (FlooNoC's `id_queue`), which costs area.

FlooNoC states this directly (`docs/floonoc/chimneys.md:50`): "The serialization
should not cause any big performance problems if the downstream AXI subordinates
cannot handle out-of-order transactions. Alternatively ... `MaxUniqueIds` ...
comes at a higher cost, since the `src_id` and additional information need to be
stored in an ID queue, that allows out-of-order access."

**It is not a throughput knob.** FlooNoC nowhere claims that a single downstream
ID lowers a subordinate's achievable bandwidth, and for a single-port
constant-latency subordinate it cannot: the port is busy every cycle regardless
of how many distinct IDs the requests carry. Two independent surveys
(2026-07-09) confirmed this against the FlooNoC source and against our own
co-sim, whose subordinate is a zero-wait `MAPPED` pulp `axi_rand_slave`
(`sim/tb/user_node_endpoint.sv:193-196,242`). Under uniform service latency,
in-order same-ID return costs nothing.

The measurable effects of collapsing are confined to ordering: the subordinate
loses the freedom to interleave bursts of different IDs, and the response path
sees a different arrival order. Whether that shifts a saturation curve is a
question to measure, not to assume.

**It is unrelated to the RoB.** The two sit on opposite sides of the NI. A RoB
reorders responses arriving from the NoC toward a local manager, and its knob is
FlooNoC's `MaxTxnsPerId`. `max_unique_ids` governs the IDs the NI emits toward a
downstream subordinate. FlooNoC's `MaxTxns` -- the meta buffer depth -- likewise
belongs to the subordinate face, guarded by `EnSbrPort`
(`floo_axi_chimney.sv:811-816`), and a chimney without a manager port carries no
RoB at all (`floo_axi_chimney.sv:872-873`).

On the manager side, RoB absence is what limits a single AXI ID's outstanding
transactions. FlooNoC's `NoRoB` stalls only same-`txnID` transactions **going to
a different destination**, and admits further same-destination ones up to a
per-ID counter (`floo_rob_wrapper.sv:139`). Our `RobMode::Disabled` stalls on
same-ID regardless of destination -- strictly more conservative. The gap is
unmeasured. See `docs/nmu-rob-microarchitecture.md` section 2.

**Not modelled.** The C++ meta buffer keeps its `AXI_ID_SPACE`-wide bucket array
under both settings, so the model reproduces neither the FIFO's area saving nor
the `id_queue`'s cost. `max_unique_ids` is carried as a configuration point, not
as a mechanism the behaviour model exploits.

### NoC fabric stub

The c_model contains no router class. The only NoC component is the
`ChannelModel` stub (`c_model/tests/common/channel_model.hpp`), a
testbench-only NoC bridge that conducts NMU TX flits to NSU RX. By
default it is zero-delay; the test fixture can set a per-NSU response
latency via `set_nsu_latency` / `set_nsu_latency_range`, or a global
request / response delay via `set_req_delay` / `set_rsp_delay` (single-
NSU mode only). Destination derivation is a SAM lookup on `awaddr` /
`araddr` (see NMU above), performed at NMU packetize time via
`addr_trans::SamTable::translate` (`c_model/include/nmu/addr_trans.hpp`),
not at the NoC level. A router model can replace `ChannelModel` by
implementing the four `NocReqOut` / `NocRspIn` / `NocReqIn` /
`NocRspOut` abstract interfaces declared in `c_model/include/router/`.

### AXI4 endpoint model

The c_model test harness uses cocotbext-axi-derived C++ classes for the
AXI4 master and slave endpoints. These are ported to C++17 under
`c_model/include/axi/`. Attribution: `c_model/include/axi/ATTRIBUTION.md`.

---

## 3. c_model component flow and tick discipline

### 3.1 Component map

The full c_model pipeline for a single NMU/NSU loopback transaction:

~~~
AxiMaster
    |  AW/W/AR drives
    v
AxiSlavePort        (accepts AXI4 beats from the master-side driver)
    |  push_aw() / push_w() / push_ar()
    v
Nmu                 (packetizes; zero-fills noc_qos + flit_ecc; manages RoB)
    |  flit inject
    v
ChannelModel        (zero-latency stub; connects NMU TX to NSU RX)
    |  flit deliver
    v
Nsu                 (unpacks; drives AXI4 to downstream slave)
    |  AW/W/AR drives
    v
AxiMasterPort       (drives AXI4 beats toward the slave-side receiver)
    |  B/R back
    v
AxiSlave + Memory   (responds with B/R; memory array holds written data)
~~~

Each component in the pipeline is a separate C++ class in `c_model/`.
Components communicate through typed queues or method calls -- never
through shared global state.

### Per-instance handle ABI

Each wrap (`*Wrap`) is instantiated per call to
`cmodel_<component>_create(name)`. The function returns a 64-bit integer
handle (`unsigned long long`; SV `longint unsigned`) that encodes a
pointer to a typed `HandleBlock` (cast back at the DPI boundary; a plain
integer is used rather than SV `chandle` because VCS rejects `chandle`
as a module port):

```cpp
struct HandleBlock {
    uint32_t    magic;     // WrapType-derived sentinel
    WrapType    type;
    HandleState state;
    std::string name;
    std::unique_ptr<void, void(*)(void*)> adapter;  // type-erased
};
```

All live handles are tracked in `g_handle_registry`. Cycle handlers
(`cmodel_<component>_set_inputs/tick/get_outputs`) validate via
`REQUIRE_HANDLE(ctx, expected_type, fn_name)`, which checks: session
state != Uninitialized, registry membership, magic/type self-
consistency, and Live state.

`cmodel_finalize` walks the registry and destroys each `HandleBlock`,
which in turn invokes the per-handle type-erased deleter to clean up
the underlying adapter.

See `docs/internal/superpowers/specs/2026-06-09-multi-instance-dpi-design.md`
for the full design.

### 3.2 Tick semantics

The c_model uses a beta tick discipline: one call to `tick()` on a
component corresponds to one rising-edge evaluation of that component's
registered state. This matches the Verilator clk_i cycle semantics.

Key properties:

- 1-cycle latency per pipeline hop -- a beat pushed into NMU at tick N
  appears at the ChannelModel input at tick N+1.
- Registered handshake -- `can_accept_*()` queries reflect state latched
  at the previous tick, not combinational lookahead.
- Verilator clk_i match -- `tb_top.sv` drives its own clock (self-clocked
  via `always #5` or equivalent); `main.cpp` runs a minimal event-loop
  (`eval()` + time advance) and does not toggle `clk_i`. One posedge
  equals one c_model tick. The SV side owns the cycle-by-cycle wire
  propagation within that clock edge. This is the timing model for
  Stage 5b.
- C++ vs SV timing nuance -- in the C++ model, state updates are
  immediate within tick(). In SV, the registered outputs settle after the
  clock edge and are visible one delta later. The DPI wrap adapters
  absorb this difference: they read input wires before the tick() call
  and write output wires after, so the wire bundle seen on the SV side
  is consistent with the registered model.

### 3.3 Extension boundaries

The c_model separates concerns so that the NoC stub and the NI
components can evolve independently:

- VcArbiter modes -- `VcArbiter::read_write_split` and
  `VcArbiter::multi_candidate` are static factory methods on
  `nmu::VcArbiter` and `nsu::VcArbiter` (the constructors are private).
  The mode is selected at construction time via `NmuConfig::vc_mode` /
  `NsuConfig::vc_mode`.
- ChannelModel substitution -- `ChannelModel` (`c_model/tests/common/`)
  implements the `NocReqOut` / `NocRspIn` / `NocReqIn` / `NocRspOut`
  abstract interfaces defined in `c_model/include/router/`. Replacing it
  with a real router model requires implementing those four interfaces.

---

## 4. Cosim and Verilator boundary

### Stage 5b wire-wrap architecture

Stage 5b introduces a DPI wire-wrap layer that connects the c_model
components to the Verilator-compiled SV testbench. The layer has three steps:

1. Each `*_wrap.sv` module calls its per-wrap DPI imports at every
   posedge `clk_i`: `cmodel_<component>_set_inputs` ->
   `cmodel_<component>_tick` -> `cmodel_<component>_get_outputs` (see
   `sim/c/cmodel_dpi.h` for the four tick functions:
   `cmodel_master_tick`, `cmodel_nmu_tick`,
   `cmodel_nsu_tick`, `cmodel_slave_tick`).
2. The DPI implementation reads the SV input wire bundle, calls the
   appropriate `*_wrap.hpp::tick()`, and writes the SV output wire bundle.
3. SV propagates output wires to the next stage's input ports.

Four wraps mediate the boundary:

- `NmuWrap` -- NMU AXI slave port input / flit output.
- `NsuWrap` -- flit input / AXI master port output.
- `MasterWrap` -- AXI master driver output / NMU slave port input.
- `SlaveWrap` -- NSU master port output / slave receiver input.

Wrap responsibility invariant: `<comp>_wrap.hpp::tick()` is
allowed only to:

- Read the input wire latch and check `can_accept_*()` capacity, then
  push a beat into the c_model component.
- Call `<comp>_->tick()` exactly once.
- Read c_model output state into the output wire latch.

Forbidden: any business logic inside the adapter that belongs in the
c_model (packetization, routing, ROB reordering). If adapter logic is
needed, the c_model component is missing an API -- extend the c_model
header instead.

### Vtb_top binary

The Verilator binary `Vtb_top` is built per topology and run class in
`build/verilator/obj_dir_<TOPOLOGY>_<RUN_CLASS>/` using `--timing`
(`RUN_CLASS=constrained_random` is the default flavor; `RUN_CLASS=directed`
compiles the endpoint's `TB_DIRECTED` flavor). The generated
`tb_top_<TOPOLOGY>.sv` is self-clocked (clock, reset, and timeout are
internal); `sim/verilator/main.cpp` is a minimal event-loop entry that
calls `eval()` and advances time until `$finish`. The same
`tb_top_<TOPOLOGY>.sv` is used by VCS (`-top tb_top`); waveforms are
captured via VCS/FSDB. Per-run correctness is established by the c_model
scoreboard (per-transaction write->readback data compare) and the model's
own internal checks.

---

## 5. Verification layers

The project uses four overlapping verification layers:

**Layer 1 -- c_model unit tests**
GoogleTest tests under `c_model/tests/` exercise individual components
(Nmu, Nsu, AxiSlavePort, scoreboard, etc.) in isolation. Run with
`make test`.

**Layer 2 -- c_model integration test**
`c_model/tests/integration/test_request_response_loopback.cpp` drives the
full NMU + NSU packetize/depacketize pipeline through ChannelModel and
checks the scoreboard. Its fixture is the ORD-003 same-id-multi-dst reorder
gate, built inline in the test source (no external scenario files).

**Layer 3 -- cosim smoke**
`make sim TB=mesh_4x4_vc1 PATTERN=neighbor` runs a neighbor-pattern
benchmark through the Verilator wire-level harness and gates on the PASS
marker. The curated AX4 bidirectional sweep is deferred.

**Layer 4 -- scoped / targeted tests**
Two hand-curated test suites exercise specific protocol invariants:

- `c_model/tests/integration/test_port_pair_loopback.cpp` -- 4 scenarios
  x delay sweep.
- `c_model/tests/integration/test_request_response_loopback.cpp` -- 7
  scenarios x num_vc variants.

`make test` builds the c_model and runs the full ctest suite (Layers
1-2, 4). Layer 3 is a separate cosim run: `make sim TB=<topo> PATTERN=<p>`.

---

## 6. AXI4 conformity scope

### Covered (IHI 0022H)

- sec. A3.2: Basic VALID/READY handshake protocol.
- sec. A3.2: Handshake stall and backpressure (HSH category).
- sec. A3.4.1: INCR, WRAP, FIXED burst types and beat-length (BUR
  category).
- sec. A3.4.1: Alignment rules and 4 KB boundary crossing (BND
  category).
- sec. A5, A6: ID-based response ordering with multi-ID traffic (ORD
  category).
- sec. A3.4.5: Error response codes DECERR (RSP category; RSP-001 and
  RSP-002 exercise out-of-bounds DECERR paths via
  `c_model/include/axi/memory.hpp:100,119`).

The BAS category covers basic serialized single-beat write and read
transfers.

### Excluded in Stage 5b

- sec. A7.2.4: Exclusive access monitor -- NSU does not implement an
  Exclusive Monitor in Stage 5b; EXC scenarios are present in the
  scenario tree but not passing through the cosim layer.
- SLVERR response -- not exercised by any scenario in Stage 5b.
- Dual-clock CDC -- the c_model uses a single-clock approximation; CDC
  is a property of the RTL implementation, not the behavioural model.

---

## 7. References

- `sim/test_patterns/README.md` -- scenario naming convention, YAML schema,
  IHI 0022H section mapping per category.
- `c_model/include/axi/ATTRIBUTION.md` -- cocotbext-axi MIT attribution.
- `docs/development.md` -- build system, workflow, contributing guide.
- IHI 0022H (AMBA AXI4 protocol specification, ARM Ltd.) -- cited inline
  by section number throughout this document.
