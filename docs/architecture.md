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
harness that connects the c_model to wb2axip protocol checkers. The goal
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
between them is represented by a simple loopback in the c_model
(LoopbackNoc) and is not under AXI4 conformity verification.

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
- Address translation extracts the destination ID from the upper address
  bits at packetize time (`nmu::addr_trans::xy_route` in
  `c_model/include/nmu/addr_trans.hpp`); the local address passed through
  the NoC is the full input address unmodified. There is no remap table
  in the c_model.

The RoB holds in-flight response slots indexed by AXI ID, releases them
in-order per ID, and asserts BVALID / RVALID only when the slot at the
head of each ID queue drains.

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

### NoC fabric stub

The c_model contains no router class. The only NoC component is the
`LoopbackNoc` stub (`c_model/tests/common/loopback_noc.hpp`), a
testbench-only NoC bridge that conducts NMU TX flits to NSU RX. By
default it is zero-delay; the test fixture can set a per-NSU response
latency via `set_nsu_latency` / `set_nsu_latency_range`, or a global
request / response delay via `set_req_delay` / `set_rsp_delay` (single-
NSU mode only). Destination derivation (XY bit-slice on `awaddr` /
`araddr`) is performed at NMU packetize time via
`nmu::addr_trans::xy_route` (`c_model/include/nmu/addr_trans.hpp`), not
at the NoC level. A router model can replace `LoopbackNoc` by
implementing the four `NocReqOut` / `NocRspIn` / `NocReqIn` /
`NocRspOut` abstract interfaces declared in `c_model/include/noc/`.

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
LoopbackNoc         (zero-latency stub; connects NMU TX to NSU RX)
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

Hermetic singleton invariant: each `*_shell_adapter.hpp` in
`c_model/include/cosim/` owns exactly ONE c_model component.
The DPI entry point `cosim/c/cmodel_dpi.cpp` instantiates one global
adapter per component and is the only file that may hold these globals.
The following cross-component references are forbidden:

- `cosim/c/cmodel_dpi.cpp` referencing adapter state from a different
  component's global.
- A `*_shell_adapter.hpp` including another shell's adapter header.
- A C++ component holding a reference or pointer to a different component.

The hermetic invariant is enforced by code review.

### 3.2 Tick semantics

The c_model uses a beta tick discipline: one call to `tick()` on a
component corresponds to one rising-edge evaluation of that component's
registered state. This matches the Verilator clk_i cycle semantics.

Key properties:

- 1-cycle latency per pipeline hop -- a beat pushed into NMU at tick N
  appears at the LoopbackNoc input at tick N+1.
- Registered handshake -- `can_accept_*()` queries reflect state latched
  at the previous tick, not combinational lookahead.
- Verilator clk_i match -- the Verilator harness calls
  `eval()` with clk_i=0 then clk_i=1 per C++ tick, producing exactly one
  SV clock edge per c_model tick. The C++ harness is the timing master at
  the Verilator level (main.cpp toggles clk_i and drives rst_ni). The SV
  side owns the cycle-by-cycle wire propagation within that clock edge.
  This is the timing model for Stage 5b.
- C++ vs SV timing nuance -- in the C++ model, state updates are
  immediate within tick(). In SV, the registered outputs settle after the
  clock edge and are visible one delta later. The DPI shell adapters
  absorb this difference: they read input wires before the tick() call
  and write output wires after, so the wire bundle seen by the SV
  checker is consistent with the registered model.

### 3.3 Extension boundaries

The c_model separates concerns so that the NoC stub and the NI
components can evolve independently:

- VcArbiter modes -- `VcArbiter::read_write_split` and
  `VcArbiter::multi_candidate` are static factory methods on
  `nmu::VcArbiter` and `nsu::VcArbiter` (the constructors are private).
  The mode is selected at construction time via `NmuConfig::vc_mode` /
  `NsuConfig::vc_mode`.
- LoopbackNoc substitution -- `LoopbackNoc` (`c_model/tests/common/`)
  implements the `NocReqOut` / `NocRspIn` / `NocReqIn` / `NocRspOut`
  abstract interfaces defined in `c_model/include/noc/`. Replacing it
  with a real router model requires implementing those four interfaces.

---

## 4. Cosim and Verilator boundary

### Stage 5b wire-wrap architecture

Stage 5b introduces a DPI wire-wrap layer that connects the c_model
components to Verilator-compiled SV checkers. The layer has three steps:

1. Each `*_wrap.sv` module calls its per-shell DPI imports at every
   posedge `clk_i`: `cmodel_<shell>_set_inputs` ->
   `cmodel_<shell>_tick` -> `cmodel_<shell>_get_outputs` (see
   `cosim/c/cmodel_dpi.h` for the five tick functions:
   `cmodel_master_tick`, `cmodel_nmu_tick`, `cmodel_loopback_noc_tick`,
   `cmodel_nsu_tick`, `cmodel_slave_tick`).
2. The DPI implementation reads the SV input wire bundle, calls the
   appropriate `*_shell_adapter.hpp::tick()`, and writes the SV output
   wire bundle.
3. SV propagates output wires to the connected wb2axip checker ports.

Five shell adapters mediate the boundary:

- `NmuShellAdapter` -- NMU AXI slave port input / flit output.
- `NsuShellAdapter` -- flit input / AXI master port output.
- `MasterShellAdapter` -- AXI master driver output / NMU slave port input.
- `SlaveShellAdapter` -- NSU master port output / slave receiver input.
- `LoopbackNocShellAdapter` -- NMU flit output to NSU flit input (zero-latency loopback stub).

Shell responsibility invariant: `<comp>_shell_adapter.hpp::tick()` is
allowed only to:

- Read the input wire latch and check `can_accept_*()` capacity, then
  push a beat into the c_model component.
- Call `<comp>_->tick()` exactly once.
- Read c_model output state into the output wire latch.

Forbidden: any business logic inside the adapter that belongs in the
c_model (packetization, routing, ROB reordering). If adapter logic is
needed, the c_model component is missing an API -- extend the c_model
header instead.

### wb2axip protocol checkers

`cosim/sv/wb2axip/` contains the ZipCPU/wb2axip AXI4 formal checker
files (Apache 2.0), adapted for Verilator simulation mode. The checker
observes AXI4 wire bundles on the NMU manager-facing side
(`faxi_slave.v`, treating the NMU as an AXI master) and on the NSU
memory-facing side (`faxi_master.v`, treating the NSU as an AXI slave).
See `cosim/sv/tb_top.sv` lines 12-13 for the role mapping and lines
208 / 279 for the bind sites.

The Verilator binary `Vtb_top` is built in `cosim/verilator/obj_dir/`.
It is driven by `cosim/verilator/main.cpp` via the DPI shell adapters.

### wb2axip structural limits

wb2axip carries internal simplifications for formal engine convergence
that are stricter than IHI 0022H. These determine which scenarios the
cosim integration test can exercise directly.

**faxi_slave.v lines 805-807 -- single-burst-at-a-time constraint**

~~~verilog
always @(*)
if (f_axi_wr_pending > 1)
    `SLAVE_ASSERT(!i_axi_awready);
~~~

This forces AWREADY=0 while a previous AW's W burst is still mid-flight
(wr_pending > 1, meaning more than one W beat still expected). It
prevents the slave from accepting a new AW while the current burst is
in progress.

This is not an AXI4 mandate. The wb2axip author note at faxi_slave.v
lines 583-587 states explicitly: "not strictly required by the
specification, but is required in order to make these properties work" --
it is a wb2axip-internal simplification for formal engine convergence.

AXI4 IHI 0022H sec. A3.3 and sec. A5.2.2 allow multi-outstanding AW
with pipelined W (W beats are serialized in AW order because there is
no WID field in AXI4). A slave may pre-assert AWREADY for a new AW
while the current AW's W burst is still in flight, as long as it tracks
AW order internally to demultiplex incoming W beats.

The c_model NMU is spec-compliant: `axi_slave_port.hpp` lines 101-103
`can_accept_aw()` returns pure queue-vacancy without the single-burst
ordering constraint.

**Impact on Stage 5b smoke set**: wb2axip-bound scenarios are limited to
single-beat configurations (AWLEN=0 throughout). Multi-beat and true
multi-outstanding scenarios are verified at the C++ adapter / scoreboard
layer only -- the T10 unit test (`test_nmu_shell_adapter.cpp:
NmuShellAdapter.multi_beat_w_burst_visible_per_cycle`) and per-shell
unit tests exercise these paths.

**Attempted workarounds (reverted)**: T15 (commit 0008c28) added
wr_pending_gt1 AWREADY suppression in NmuShellAdapter + SlaveShellAdapter
to silence the assertion. Side effect: `push_aw()` still consumed the
beat into the c_model queue while reporting AWREADY=0 on wire, causing
C++/SV state mismatch. Reverted in commit 9701cb5.

**Acceptable resolution paths (none done in Stage 5b)**:
- (a) Fork wb2axip with an `F_OPT_SINGLE_WRITE_BURST` parameter (Apache
  2.0 attribution must be updated). Set to 0 to bypass the assertion.
- (b) Replace wb2axip with a full AXI4 BFM without this constraint
  (candidates include OSVVM or a vendor-agnostic open-source checker).
- (c) Accept current scope limit (path taken in Stage 5b): wb2axip
  verifies single-beat traffic; multi-beat is C++-layer verified.

Do NOT modify `cosim/sv/wb2axip/faxi_slave.v` to silently remove the
assertion. That is an OSS attribution violation and a verification
integrity violation.

**faxi_wstrb.v -- permissive stub**

`cosim/sv/wb2axip/faxi_wstrb.v` was created as a permissive simulation
stub during the Stage 5a build-fix pass (commit 822a780). `o_valid` is
hardwired to `1'b1`, disabling WSTRB alignment checking. Stage 5b
carries this stub unchanged.

### cosim/tests/wb2axip_block.hpp runtime predicate

`wb2axip_block_reason()` inspects each scenario's parsed content against
wb2axip's structural limits and returns a SKIP reason on hit. The cosim
integration test uses this predicate so that scenarios exceeding wb2axip's
scope are skipped rather than falsely failed. No skip map is maintained;
when wb2axip is replaced with a full AXI4 BFM, deleting the helper body
activates all previously skipped scenarios.

---

## 5. Verification layers

The project uses four overlapping verification layers:

**Layer 1 -- c_model unit tests**
GoogleTest tests under `c_model/tests/` exercise individual components
(Nmu, Nsu, AxiSlavePort, scoreboard, etc.) in isolation. Run with
`make test`.

**Layer 2 -- c_model integration test**
`c_model/tests/axi/test_integration.cpp` runs all AX4-* scenarios
(excluding INF prefix) through the full c_model pipeline. The scenario
list is generated at CMake configure time from
`tests/scenarios/AX4-*/scenario.yaml` via `file(GLOB CONFIGURE_DEPENDS)`.

**Layer 3 -- cosim integration test with wb2axip**
`cosim/tests/test_cosim_integration.cpp` runs the same scenario list
through the Verilator + wb2axip harness. Scenarios that exceed wb2axip's
structural limits (multi-beat, multi-outstanding) are runtime-SKIPped by
`wb2axip_block_reason()`. INF-prefix scenarios are also excluded.

**Layer 4 -- scoped / targeted tests**
Three hand-curated test suites exercise specific protocol invariants:

- `c_model/tests/integration/test_port_pair_loopback.cpp` -- 4 scenarios
  x delay sweep.
- `c_model/tests/integration/test_request_response_loopback.cpp` -- 7
  scenarios x num_vc variants.
- `cosim/tests/test_checker_fires_on_violation.cpp` -- uses INF-001 to
  verify that the wb2axip checker fires on a deliberate protocol
  violation (bringup test for the checker itself).

`make check` runs lint_scenarios, lint_docs, builds the c_model and the
Verilator binary, and runs the full ctest suite (Layers 1-4). It does
not invoke `make sim` (sim is a separate manual target). Note: cosim
Layer 3 ctest registration requires `Vtb_top` to be present at CMake
configure time; `make check` ensures this by depending on
`build-verilator` before running ctest.

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
- Multi-beat INCR bursts through the cosim wb2axip layer -- covered only
  at the C++ adapter layer (see sec. 4, wb2axip structural limits).
- Dual-clock CDC -- the c_model uses a single-clock approximation; CDC
  is a property of the RTL implementation, not the behavioural model.

---

## 7. References

- `tests/scenarios/README.md` -- scenario naming convention, YAML schema,
  IHI 0022H section mapping per category.
- `c_model/include/axi/ATTRIBUTION.md` -- cocotbext-axi MIT attribution.
- `cosim/sv/wb2axip/ATTRIBUTION.md` -- ZipCPU/wb2axip Apache 2.0
  attribution and modification record.
- `docs/development.md` -- build system, workflow, contributing guide.
- IHI 0022H (AMBA AXI4 protocol specification, ARM Ltd.) -- cited inline
  by section number throughout this document.
