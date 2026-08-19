# Verification environment

Environment details for the DUT (NMU + NoC routers + NSU) testbench: how the
testbench is generated, what drives and checks it, and how to reproduce a run.
For DUT design content (SAM, RoB, VC allocation), see the block specs
`docs/nmu-spec.md`, `docs/nsu-spec.md`, and `docs/router-spec.md`.

## Conformity scope

Verification intent: demonstrate AXI4 (IHI 0022H) conformity at the NMU and
NSU pin boundaries. The router fabric is exercised as transport and checked
end to end by the write-to-readback scoreboard; AXI conformity claims attach
to the NI boundary, not to the fabric.

Covered (IHI 0022H):

| area | sections | where exercised |
|---|---|---|
| VALID/READY handshake, stall, backpressure | A3.2 | focused model-egress regression + wire-level co-sim (approved held-valid spill registers on model-facing REQ/RSP) |
| Burst types INCR/WRAP/FIXED, length, alignment, 4 KB boundary | A3.4.1 | `protocol_rules.hpp` checks + unit tests + burst stimulus |
| Per-ID response ordering: same-ID R returns in AR-issue order; W follows AW (no WID in AXI4) | A5.3 | RoB / interlock design + integration tests + co-sim scoreboard |
| Response codes; DECERR on out-of-bounds access | A3.4.4 | memory model returns DECERR past its bounds; unit tests |
| Exclusive access rules and monitor | A7.2.4 | `protocol_rules.hpp` + the C++ `AxiSlave` exclusive monitor, unit tests only |

Excluded, with reasons:

| exclusion | reason |
|---|---|
| Exclusive access through the fabric | co-sim stimulus issues no exclusive transactions; monitor coverage is unit-level |
| SLVERR | no component generates it and no scenario provokes it |
| Atomic transactions, user signals | out of scope; the endpoint bridge ties `aw_atop` and `*_user` off |
| Dual-clock CDC | single-clock model; CDC is a property of the RTL implementation |
| NoC-layer QoS | `NOC_QOS_WIDTH = 0`; `awqos`/`arqos` ride the AXI payload but drive nothing |

## Testbench architecture

The testbench sources sit in two directories, one per endpoint flavour, plus
what both compile:

| directory | endpoint | what it is |
|---|---|---|
| `sim/tb/test/` | `user_node_endpoint` | a pulp `axi_file_master` replaying generated stimulus against a tile crossbar of `axi_sim_mem` targets. The DV layer: the traffic is what a pattern named, so a run measures the fabric |
| `sim/tb/soc/` | `dma_node_endpoint` | a pulp iDMA backend in place of the replayer. The SoC layer: a real AXI manager picks its own bursts, outstanding depth and alignment, so a run measures conformance to one |
| `sim/tb/` | — | `axi_vip_types_pkg.sv` and `link_perf_monitor.sv`, compiled into both |

`sim/tools/gen_tb_top.py` reads a config file (`sim/configs/<CONFIG>.yml`) and
emits one generated file per run class, never hand-edited:

| file | content |
|---|---|
| `$(BUILD_ROOT)/generated/<CONFIG>/topology_pkg.sv` | build-only per-config geometry, endpoint windows, NoC egress aperture, and the frozen typed SAM declarations/constant imported by the testbench and production NI RTL. Different configurations coexist by directory; no generated package or per-config golden RTL is committed. |
| `sim/tb/soc/tb_top_dma_<CONFIG>.sv` | `DMA=1` only: the SoC-layer top, whose per-node region compare has to come from the same geometry as the job files. |

The directed top is not generated. `sim/tb/tb_noc_mesh.sv` names the latency
profiles and instantiates `sim/tb/noc_tb_top.sv`, the shared body: self-clocked
(10 ns clock, 4-cycle reset), DPI create calls for every router/NMU/NSU context,
the fabric instance, one `user_node_endpoint` per endpoint, the watchdog and the
exit logic. `ref_model/top/noc_fabric.sv` holds N nodes (`ni_wrap` = NMU + NSU +
`dat_merge_wrap`, plus `router_wrap`) joined by directional (N/E/S/W)
inter-router links through a `genvar` generate loop. Boundary directions are
tied off; a tied-off direction driving a valid flit is a `$fatal`.

`router_wrap` carries three physical networks, each with its current generated flit width
(`ni_params_pkg`, fixed `NOC_ID_WIDTH = 3`: REQ 136 b, RSP 126 b, DAT 633 b):

| network | flow control | carries |
|---|---|---|
| REQ | `SimpleRouter`, ready/valid, 1-2 stages | Aw/W/Ar/DataAr requests |
| RSP | `SimpleRouter`, ready/valid, 1-2 stages | B/NarrowR responses |
| DAT | credit router, VC-capable | DataAw/DataW/DataR payload |

Link ports use a `tx_<net>_*`/`rx_<net>_*` pin contract (`<net>` = `req`/`rsp`/`dat`):
`tx` is this router's output to the peer, `rx` is the peer's output into this
router; adjacent nodes cross-wire `tx` to `rx`. DAT is the one shared
physical LOCAL port between NMU and NSU (NMU sources DataAw/DataW, NSU
sources DataR); `dat_merge_wrap` (`ref_model/top/dat_merge_wrap.sv`) arbitrates the
two onto `router_wrap`'s single DAT LOCAL rx/tx pair, sitting between
`nmu_wrap`/`nsu_wrap` and `router_wrap` inside `ni_wrap`.

Each node's `user_node_endpoint` (`sim/tb/test/user_node_endpoint.sv`, hand-written,
instantiated by the generator but not itself generated) presents two
`AXI_BUS_DV` interfaces and bridges them to the flat wire structs field by
field:

| interface | direction | drives / driven by |
|---|---|---|
| `master_dv` | feeds the NMU ingress | the directed file driver |
| `slave_dv` | driven by the NSU egress | the tile-memory slave |

```
directed driver --> master_dv --> NMU --> routers --> NSU --> slave_dv --> tile-memory slave
                     (scoreboard taps master_dv)
```

Every node carries the same endpoint layout. The model-facing `nmu_wrap`, `nsu_wrap`, and
`router_wrap` contain only DPI marshalling, registered sampling, checks, and the REQ/RSP held-valid
adaptation described below; production RTL under `rtl/` contains none of that verification logic.

### Hybrid block co-simulation

RTL block signoff does not wait for a complete RTL mesh. The verification composition selects the
NMU, NSU and Router implementations independently. A model selection instantiates the existing DPI
wrapper and consumes its `ctx` handle; an RTL selection instantiates the synthesizable block with the
same functional ports and has no DPI handle.

The NI hybrid tests use a zero-hop test link. RTL NMU connects to the reference NSU, and reference
NMU connects to RTL NSU. REQ and RSP use their ready/valid contracts directly. Where the current
model's symmetric DAT credit port differs from the target Router-to-NI ready/valid contract, a
verification-only adapter translates flow control without changing the flit, adding routing, or
modeling a Router pipeline. This verifies AXI-to-flit, flit-to-AXI, ordering, backpressure and
response behavior without requiring any Router or mesh.

Router block signoff uses identical flit, ready and credit stimulus against RTL and reference Router
instances and compares outputs only inside the approved conformance matrix. Documented intentional
cycle differences, including the current model's optimistic collective multi-output behavior, are
excluded explicitly rather than hidden by loose scoreboarding.

The acceptance order is unit DV, hybrid block co-sim, full-RTL `2x2 verify`, then the full-RTL
`4x4 verify` milestone. Every ready/valid hybrid boundary receives randomized stall injection.
The NMU-specific N0/N1/N2 tests, comparison exclusions, overtaking evidence, F0 adapter checks, and
non-vacuous pass criteria are defined in `docs/nmu-verification-plan.md`.
The NSU-specific S0/S1 tests, comparison exclusions, F0 adapter checks, and non-vacuous pass criteria
are defined in `docs/nsu-verification-plan.md`.
The Router-specific R0/R1/R2 tests, direct comparison points, LOCAL DAT normalizer, and approved
cycle-divergence exclusions are defined in `docs/router-verification-plan.md`.

### Model-facing REQ/RSP egress

The C++ models produce one-cycle REQ/RSP pop strobes. Each model-facing source captures its strobe
in the approved `common_cells` `spill_register` and returns the primitive's input capacity to the
model. The RTL-facing source therefore holds valid and flit until `valid && ready`; it never forces
ready high. Because the C++ Router ingress consumes pulses, `router_wrap` presents it with one
injection pulse per wire handshake. NMU RSP and NSU REQ ingress are always-ready in the current
model. DAT bypasses this adaptation and retains credit flow control.

`sim/tools/test_model_egress_hold.py` drives the previously lossy sequence directly: a fake C++ NMU
emits a REQ strobe, RTL-side ready falls before acceptance, and valid plus payload must remain
stable for three stalled cycles. It then sends 16 ordered flits through randomized ready stalls and
checks exactly-once delivery. Wrapper assertions check the same held-valid property for NMU REQ,
NSU RSP, and every Router REQ/RSP port; the co-sim retains randomized consumer stalls.

## VIP set

All four components are vendored under `sim/dv/` and run unmodified except
where noted.

| VIP | role | connects to |
|---|---|---|
| `axi_file_master` | directed driver, two-phase (drain writes, then issue reads) from per-node stimulus files | `master_dv` |
| `axi_sim_mem` | tile memory backing the node's address window, one per space | `tile_mem[t]` |
| `axi_delayer` | the tile memory's latency, separate from its storage; profile in `gen_tb_top.py` `_MEM_LATENCY` | between `tile_mst[t]` and `tile_mem[t]` |
| `axi_scoreboard` | in-endpoint, wired on `master_dv`; per-transaction write-vs-readback data-integrity check | `master_dv` |
| `axi_bw_monitor` | passive throughput and latency monitor | `master_dv` |

## Provenance

The AXI master/slave/memory model algorithms in `ref_model/c_model/include/axi/`
are ported to C++17 from cocotbext-axi (MIT license). The VIP set above is
vendored under `sim/dv/` from the pulp `axi` / `common_verification` /
`common_cells` packages and a FlooNoC test component (Solderpad 0.51);
per-package version, upstream commit, and the one flagged local modification
(`axi_bw_monitor.sv`, a two-line `$display` addition) are in
`sim/dv/README.md`. This is the only section in this document that names
external IP; the rest of this document describes the environment in the
DUT's own vocabulary.

The production RTL primitive dependency is separate from that vendored DV closure and from the
mounted FlooNoC reference's dependency revision. This project pins
[`common_cells` 2.0.0-beta.3](https://github.com/pulp-platform/common_cells/tree/63b7c50d43e462b59506f69d341ff1e40202866d)
at commit `63b7c50d43e462b59506f69d341ff1e40202866d` through `rtl/Bender.yml`. The selected primitive
interfaces are `cc_fifo`, `cc_cdc_fifo_gray`, `cc_stream_register`, `cc_spill_register`, and
[`cc_addr_decode`](https://github.com/pulp-platform/common_cells/blob/63b7c50d43e462b59506f69d341ff1e40202866d/src/cc_addr_decode.sv),
whose pinned source documents the overlap priority. Bender resolves
their transitive sources and include order from `rtl/Bender.yml`. Production builds fetch that
exact revision and do not copy or rewrite the files. The upstream license is
[Solderpad Hardware License v0.51](https://github.com/pulp-platform/common_cells/blob/63b7c50d43e462b59506f69d341ff1e40202866d/LICENSE).

For issue #43, the mounted production-approved FlooNoC reference was inspected at
`2fa02eb23c1babef9a8f714715ea7c78de98c364`. Its `Bender.lock` resolves `common_cells`
2.0.0-beta.3 at `63b7c50d43e462b59506f69d341ff1e40202866d`, and its
`floo_id_translation.sv` calls the `cc_addr_decode` interface. This project adopts the generated
typed-SAM and elaboration-time parameter-passing pattern, while `ni_sam` targets the same approved
project pin and API. No FlooNoC RTL is copied or instantiated
by production RTL.

For issue #44, the same production-approved FlooNoC revision was inspected for packed AXI/flit
typing, metadata lifetime, and RoB storage boundaries. Taxi was inspected at
`c71926499b47aa7e3dd963b994bd040319d596f5` only for ready/valid naming and source-organization
guidance. This project keeps its already-approved header-at-LSB flit layout, independent
valid/ready child ports, source-aware NSU mapping, and B-always-enabled RoB policy; it does not
instantiate or copy either reference's NI RTL. `ni_child_types_pkg.sv` and
`tb_ni_type_contract.sv` are project-authored from the approved project specs. Third-party license
files and source notices are unchanged.

Issues #11 and #12 inspected two additional source trees for NI verification planning. The
classifications below govern those plans and do not change the license of any source:

| ID | source and inspected revision | classification and permitted use | license obligation |
|---|---|---|---|
| P1 | FlooNoC at `cb7b2ba3fd4b7eac340a4117ffba05c2a9757699`; inspected `floo_rob.sv`, `floo_rob_wrapper.sv`, `tb_floo_rob.sv`, `floo_meta_buffer.sv`, and `floo_axi_chimney.sv` | production-approved external reference; architecture, behavior, and test intent may inform reviewed production work, but issues #11 and #12 copy or instantiate no RTL | inspected files carry `SPDX-License-Identifier: SHL-0.51`; preserve their copyright/SPDX notices and the repository's Solderpad Hardware License v0.51 terms in any separately approved reuse |
| P2 | Taxi at `c71926499b47aa7e3dd963b994bd040319d596f5`; inspected complete AXI register/FIFO and AXI-Stream register/FIFO families plus their tests | production-approved for complete AXI-interface units when their exact contract is required; AXI-Stream units are not used to assemble project AXI channels | inspected RTL carries `SPDX-License-Identifier: CERN-OHL-S-2.0`; any vendored derivative retains copyright/SPDX notices and records its upstream revision and modifications |

Issue #10's candidate inventory includes the upstream Router tops and wrappers, their directly
relevant transitive modules, the deprecated VC-router family, direct Router tests, synthesis
wrappers, and the packet/cut/join names that required an explicit scope decision. `adapt` means a
later, separately approved derivative may retain the useful core while changing the recorded
contract deltas. `rewrite` means
the production block is project-owned and the source is not copied. None is reused as-is. All
inspected P3 source files carry `SPDX-License-Identifier: SHL-0.51`; an adaptation must preserve
copyright/SPDX notices, carry a prominent modification notice, and comply with the repository's
Solderpad Hardware License v0.51. A rewrite copies no source or notice, so it introduces no
third-party source-license delta. This issue performs no adaptation or RTL implementation.

| ID | candidate at P1 revision | disposition | interface delta | behavior/dependency delta | license delta |
|---|---|---|---|---|---|
| P3-F01 | `hw/floo_route_xymask.sv` | adapt | replace upstream header/enums with generated coordinates and fixed project ports | retain pure fork/join mask equations; remove unsupported forwarding modes and add project legality guards | derivative obligations above |
| P3-F02 | `hw/floo_wormhole_arbiter.sv` | adapt | map generic request/grant handshake to project candidate identity | retain packet-boundary lock concept; implement exact project freeze, tail, and RR update rules with no extra dependency state | derivative obligations above |
| P3-F03 | `hw/floo_reduction_sync.sv` | adapt | replace upstream flit/route types with generated RSP fields and project ports | add exact ordering-tag/channel/BID qualification and target expected-set rules; keep loopback assumption out of the interface | derivative obligations above |
| P3-F04 | `hw/floo_reduction_arbiter.sv` | adapt | narrow generic reduction interface to CollectB RSP use | retain whole-survivor/first-SLVERR scan; remove other reductions and add worm-boundary priority control | derivative obligations above |
| P3-F05 | `hw/deprecated/vc_router_util/floo_vc_assignment.sv` | adapt | replace look-ahead and fixed-configuration inputs with per-flit `fixed_vc` and target port/VC types | retain preferred-map kernel; add mode eligibility, per-branch state, and target tail/lock rules | derivative obligations above; deprecated status is recorded, not removed |
| P3-F06 | `hw/deprecated/vc_router_util/floo_vc_selection.sv` | adapt | map candidate/credit arrays to target eligible masks | retain preferred-then-highest selection kernel; restrict overflow to nonfixed tail and selected traffic class | derivative obligations above; deprecated status is recorded, not removed |
| P3-F07 | `hw/floo_route_select.sv` | rewrite | generic routing/table/state interface conflicts with pure project child | unsupported algorithms and route-lock ownership exceed the approved dependency/behavior | no copied source; no source-license delta |
| P3-F08 | `hw/floo_fifo.sv` | rewrite | VC stream arrays do not match project FIFO adapters | frozen contract requires the approved synchronous FIFO wrapper, not another FIFO layer | no copied source; no source-license delta |
| P3-F09 | `hw/floo_vc_arbiter.sv` | rewrite | valid/ready VC interface differs from DAT credit candidates | arbitration modes and ready-dependent selection do not implement exact two-level target order | no copied source; no source-license delta |
| P3-F10 | `hw/floo_output_arbiter.sv` | rewrite | combined generic output/reduction interface conflicts with child ownership | target join qualification, strict priority, worm-boundary hold, and DAT VA differ | no copied source; no source-license delta |
| P3-F11 | `hw/floo_reduction_unit.sv` | rewrite/out of scope | arithmetic/offload ports have no target interface | non-CollectB operations and sequential reduction dependencies are unapproved | no copied source; no source-license delta |
| P3-F12 | `hw/floo_router.sv` | rewrite | top ports, packet records, and generic VC topology differ | target has asymmetric DAT flow control, fixed package ownership, and exact pipeline/collective behavior | no copied source; no source-license delta |
| P3-F13 | `hw/floo_nw_router.sv` | rewrite | wrapper records and network composition differ | target has exactly three independently owned networks and generated project types | no copied source; no source-license delta |
| P3-F14 | `hw/floo_axi_router.sv` | rewrite | wrapper exposes an incompatible aggregate interface | offload/configuration behavior and dependency closure exceed the frozen top | no copied source; no source-license delta |
| P3-F15 | `hw/deprecated/floo_vc_router.sv` | rewrite | heterogeneous VC/look-ahead/scalar-credit interface differs | pipeline, VC ownership, output protocol, and collective behavior conflict | no copied source; no source-license delta; deprecated notice untouched |
| P3-F16 | `hw/deprecated/floo_nw_vc_router.sv` | rewrite | deprecated wrapper records do not match production ports | composes the incompatible deprecated VC router | no copied source; no source-license delta; deprecated notice untouched |
| P3-F17 | `hw/deprecated/vc_router_util/floo_credit_counter.sv` | rewrite | scalar credit-ID interface differs from per-VC vectors and LOCAL ready/valid | lacks target ownership, LOCAL exception, and full conservation guards | no copied source; no source-license delta; deprecated notice untouched |
| P3-F18 | `hw/deprecated/vc_router_util/floo_input_fifo.sv` | rewrite | custom depth-specific FIFO interface differs | frozen contract requires the approved FIFO adapter and whole-flit per-VC storage | no copied source; no source-license delta; deprecated notice untouched |
| P3-F19 | `hw/deprecated/vc_router_util/floo_input_port.sv` | rewrite | split header/payload and scalar credit interface differs | multiple switch-pop paths and storage ownership conflict with one freed-slot credit event | no copied source; no source-license delta; deprecated notice untouched |
| P3-F20 | `hw/deprecated/vc_router_util/floo_look_ahead_routing.sv` | rewrite | target flits carry no look-ahead route interface | target recomputes approved XY direction from destination at each hop | no copied source; no source-license delta; deprecated notice untouched |
| P3-F21 | `hw/deprecated/vc_router_util/floo_mux.sv` | rewrite | helper shape/types are unnecessary | target can use direct typed selection without adding a dependency module | no copied source; no source-license delta; deprecated notice untouched |
| P3-F22 | `hw/deprecated/vc_router_util/floo_rr_arbiter.sv` | rewrite | request/update interface differs | pointer and update order conflict with VC-major/input-minor packet-boundary rules | no copied source; no source-license delta; deprecated notice untouched |
| P3-F23 | `hw/deprecated/vc_router_util/floo_sa_global.sv` | rewrite | look-ahead switch-allocation hierarchy differs | global selection and RR ownership do not match frozen output ownership | no copied source; no source-license delta; deprecated notice untouched |
| P3-F24 | `hw/deprecated/vc_router_util/floo_sa_local.sv` | rewrite | local switch-allocation interface differs | local arbitration hierarchy does not implement the target's exact combined ordering | no copied source; no source-license delta; deprecated notice untouched |
| P3-F25 | `hw/deprecated/vc_router_util/floo_vc_router_switch.sv` | rewrite | reconstructed header/payload interface differs from full-flit ports | target preserves every bit except an authorized `vc_id` rewrite | no copied source; no source-license delta; deprecated notice untouched |
| P3-F26 | `hw/deprecated/floo_nw_vc_chimney.sv` | rewrite/out of Router scope | NI boundary module, not a Router child | endpoint behavior and dependencies belong to NI verification | no copied source; no source-license delta; deprecated notice untouched |
| P3-F27 | `hw/synth/floo_synth_axi_router.sv` | rewrite/out of production scope | synthesis wrapper is not the frozen top interface | reference synthesis harness/configuration is not production behavior | no copied source; no source-license delta |
| P3-F28 | `hw/synth/floo_synth_nw_router.sv` | rewrite/out of production scope | synthesis wrapper is not the frozen top interface | reference synthesis harness/configuration is not production behavior | no copied source; no source-license delta |
| P3-F29 | `hw/floo_pkg.sv` | rewrite | upstream enums/records do not match generated project packages | includes unsupported routing, offload, and NI configuration types; generated sources remain authoritative | no copied source; no source-license delta |
| P3-F30 | `hw/floo_cut.sv` | rewrite/out of Router scope | generic multi-channel VC cut differs from frozen Router boundaries | introduces an unapproved register/VC-arbiter layer; approved shared primitives already own required register cuts | no copied source; no source-license delta |
| P3-F31 | `hw/floo_nw_join.sv` | rewrite/out of Router scope | joins two endpoint AXI buses rather than Router flit inputs | AXI width/ID conversion and endpoint dependencies are unrelated to CollectB | no copied source; no source-license delta |
| P3-T01 | `hw/tb/tb_floo_router.sv` | adapt test intent only | upstream queue/record agents are not reused | project-owned DV retains connectivity, backpressure, and ordered-result intent but supplies independent predictors/fault injection | no test source copied by issue #10; any future derivative has obligations above |
| P3-T02 | `hw/tb/tb_floo_vc_router.sv` | adapt test intent only | look-ahead/scalar-credit agents do not match target | project-owned DV retains VC/credit/worm stress intent and replaces the oracle, modes, assertions, and coverage | no test source copied by issue #10; any future derivative has obligations above |

For issue #10, the earlier P2 inspection covered arbitration and AXI-Stream register behavior.
The later production review covered the complete AXI register/FIFO family and established the
classification above. Production use remains unit-by-unit: a complete AXI unit may be vendored
when its interface and behavior match; AXI-Stream FIFO logic is not used as an AXI-channel CDC
substitute. All CERN-OHL-S-2.0 notices remain intact.

The project-owned C++ NMU and NSU models are conditional executable oracles rather than third-party
sources. Their target-aligned and excluded comparison areas are listed in
`docs/nmu-verification-plan.md` and `docs/nsu-verification-plan.md`.

Upstream references:

- IHI 0022H, AMBA AXI protocol specification (Arm Ltd.), cited by section
  throughout the block specs. The A5.3 rule set relied on for ordering:
  same-ID read data returns in AR-issue order, read data of different ARIDs
  may interleave, and AXI4 removed WID with write-data interleaving.
- FlooNoC RTL (production-approved reference; inspected read-only): `floo_rob.sv` (slot pool, high-water allocator,
  idle-ID and same-destination bypass, per-beat release), `floo_rob_wrapper.sv`
  (RoB type selection), `floo_simple_rob.sv` (ring-pointer allocator,
  documented alternative, not chosen), `floo_meta_buffer.sv` (meta buffer,
  downstream-ID collapse), `floo_axi_chimney.sv` (single-flit B / RoB-side R
  split, `MaxTxns` on the slave face), `floo_wormhole_arbiter.sv` (per-output
  wormhole lock), `floo_vc_arbiter.sv` (VC arbitration without the lock),
  `floo_vc_assignment.sv` (per-hop turn-model VC assignment, deprecated
  upstream, not ported), `floo_pkg.sv` (parameter names `BRoBSize`,
  `RRoBSize`, `MaxTxnsPerId`, `MaxTxns`), `axi_bw_monitor.sv` (DV bandwidth
  monitor, imported with one flagged modification).
- ID-space narrowing for a small-`NumIds` RoB: `axi_id_remap.sv`
  (pulp-platform axi v0.39.7), instantiated as `i_noc_id_remap` in
  `user_node_endpoint.sv`. Its input is the crossbar master-port id space,
  `AXI_ID_WIDTH` + `$clog2(XBAR_SLV_PORTS)` = 4 b, not
  `AXI_ID_WIDTH` itself, so it folds 16 tile ids onto the NI's fixed
  `NOC_ID_WIDTH` = 3, 8 ids.
- Traffic patterns and injection process: BookSim2 `src/traffic.cpp`
  (`NeighborTrafficPattern`, `TransposeTrafficPattern`,
  `UniformRandomTrafficPattern`, `HotSpotTrafficPattern`) and
  `BernoulliInjectionProcess`, ported destination-rule by destination-rule
  in `gen_test_patterns.py`.

## Traffic-pattern semantics

`sim/tools/gen_test_patterns.py` emits per-node stimulus
(`<out>/node<i>/{write,read}.txt`) for the directed driver. Destination
rules are ported from an established NoC simulator's traffic-pattern
classes (upstream reference in Provenance):

| pattern | destination rule | note |
|---|---|---|
| `neighbor` | `dst = ((x+1) mod X, (y+1) mod Y)` | diagonal shift per axis, wraps at the mesh edge; deterministic bijection |
| `transpose` | `dst = (y, x)` | requires a square, power-of-two mesh; diagonal nodes (x==y) self-target |
| `uniform_random` | independent uniform draw per transaction | self-traffic permitted by default (`--exclude-self` opts out); seeded |
| `hotspot` | one or more chosen nodes draw the traffic, weighted by `--hotspot-rates` | many-to-one congestion. `HOTSPOT_PERIPHERALS=1` names the target set off the router array instead: every tile draws from the whole declared peripheral set, weighted by `--hotspot-rates` exactly as the tile target set is. The peripheral keeps its own initiator traffic; the partner tile's extra lines are dropped, since every tile now addresses the peripheral out of the same slot band the partner drew from. Rejected on any other pattern, where it would suppress those lines and steer nothing |
| `multicast` | source nodes issue masked AW writes over a row, column, or 2x2-submesh member set (`MCAST_SHAPE`, one shape per run), then read back every member replica; other nodes carry unicast filler | AWUSER carries the address mask. `INJECTION_MODE=0` only, since one write answers to N readbacks. On a config topology each source also issues one narrow-class multicast into the members' config tiles, the config-space replication case. Ignores `--ids-per-initiator` |

The `multicast` schedule is what satisfies restriction R1 (`docs/router-spec.md`
Section 2.10), which the fabric does not enforce: trees of one shape are pairwise
disjoint across sources, and each source's own multicasts share one AXI id so the
NMU's R2 gate serializes them on the merged `B`. Mixing shapes in one run would
overlap trees at shared eject outputs. Any future collective stimulus carries the
same obligation: concurrently in-flight multicasts must have disjoint spanning
trees or be serialized on the merged `B`. An R1 violation surfaces as a wedge, not
as an error message.

Addresses are allocated so converging sources never collide: local offset =
`base + src_node * stride + seq * (n_nodes * stride)` inside the destination
tile's window, with payload address-in-data so the checker has a computable
golden value without a separate scoreboard model.

### Stimulus file format

Each transaction is a fixed-order block of lines. Read (AR) and write (AW) share
the address-phase fields; write adds `atop` and the W-beat lines.

`read.txt`, 11 lines per AR:

| line | field |
|---|---|
| 1 | AXI id |
| 2 | address (`0x...`) |
| 3 | len (ARLEN, beats minus 1) |
| 4 | size (log2 bytes per beat; the data bus is 512 b / 64 B: 5 = 32 B half-bus, 6 = 64 B full-bus) |
| 5 | burst (1 = INCR) |
| 6 to 10 | lock, cache, prot, qos, region |
| 11 | user |

`write.txt`, 12 lines per AW then one line per W beat:
- lines 1 to 10 as above, then line 11 `atop`, line 12 `user`.
- then `len + 1` beat lines, each `0x<data> 0x<strb> 0` (`w_data`, `w_strb`,
  `w_user`), with address-in-data payload (byte at address A holds `A & 0xFF`),
  full strobe, sized to the data bus. `w_last` is not in the file; the driver
  derives it from the burst length.

The default stimulus (`--size 5`, single beat) is a 32 B half-bus write: 12
field lines plus 1 beat line; the matching read is the 11 field lines at the
same address.

## Checkers and non-vacuous pass

- `axi_scoreboard.enable_all_checks()` + `.monitor()` arm read-data, B-resp,
  and R-resp checks against the write golden, sampled on `master_dv`. This
  requires the write-before-read precondition (see injection modes below).
- `tb_top` counts AW/AR handshakes per node (`txn_cnt_o`). `PASS` requires
  `txn_cnt_o > 0` on every node, so a run where a node completed zero
  transactions cannot report clean.
- `DIRECTED PASS` (the `make sim` console line) additionally requires the
  scoreboard to report zero mismatches: the run log must reach `PASS: all N
  nodes done, non-vacuous` and carry no scoreboard-mismatch or protocol-error
  string.
- Injection mode selects the run shape and which checker gates:

  | `INJECTION_MODE` | shape | checking |
  |---|---|---|
  | `0` (default) | two-phase: all writes drain, then reads | scoreboard armed, `DIRECTED PASS` gate |
  | `1` | continuous, reads and writes interleaved, paced per cycle by `INJECTION_RATE` | scoreboard disarmed (write-before-read does not hold); `axi_bw_monitor` gates instead, `result.csv` emitted |
  | `2` | continuous, writes paced per cycle by `INJECTION_RATE`, each read issues after its paired write's B response | scoreboard armed, `CHECKED PASS` gate |

  Mode 1 exists to measure throughput and latency under load, not to check
  data. Mode 2 is the data-integrity axis under continuous write load: the
  per-pair B interlock restores the scoreboard's write-before-read
  precondition, at the cost of a read stream that couples to response
  latency (so it does not measure offered injection rate).
- The `multicast` pattern adds a second scoreboard in `user_node_endpoint.sv`,
  keyed by replica address instead of by transaction. It captures a byte golden
  from each W beat of a masked write, replicates it to every member address, and
  compares the per-member readback:

  | check | failure |
  |---|---|
  | replica readback | `[mcast_sb] node<i>: replica readback mismatch addr=... exp=... act=...` |
  | non-vacuous | golden captured but zero replica bytes compared. A clean source node prints `[mcast_sb] node<i>: <n> replica byte compares against <m> golden bytes` |
  | single merged B | B handshake count must equal the node's AW count, and no `B` may remain asserted after the last write retires: `duplicate or lost B` / `extra B` |
  | merged BRESP | any non-OKAY merged `B` |

  The unicast filler in the same run stays under the standard scoreboard, so a
  clean multicast run also carries zero `Unexpected RData` from the AXI VIP
  monitor.
- `MCAST_FAULT=1` is the fault-injection proof for that scoreboard: it XORs
  `8'h01` into the captured replica golden, so the run must fail on the replica
  readback mismatch. A green `MCAST_FAULT=1` run means the compare is vacuous.
- Checkers are trusted only once fault injection has shown they fire on a
  deliberately planted violation; a checker that has never caught a planted
  mismatch verifies nothing.
- The constrained-random axis (a random driver plus a reorder-based checker)
  was retired. No sound data-integrity checker for random traffic
  exists yet; a future random axis is deferred (see Known limitations).

Per-task verification tiers (which pattern/topology combination a change must
pass before it counts as done) are not duplicated here: they are a standing,
frequently-updated section of `docs/backlog.md` ("Verification (acceptance)").

## Performance counters

As-built instrumentation is NoC-side counters plus the DV bandwidth monitor.
No AXI-side per-transaction profile or trace machinery is built, and the perf
dump carries no AXI section.

NoC counters (`PerfCollector`, dumped to `perf.json` via `+perf_out` at the
end of every run, in all injection modes):

| counter | source | semantics |
|---|---|---|
| `in_fifo_occ_max` / `out_fifo_occ_max` per router | `cmodel_perf_sample_tick` once per clock | max observed input/output FIFO occupancy |
| `flit_count` per link | `link_perf_monitor` (passive, per inter-router link) | REQ/RSP `valid && ready` transfers; DAT credit-qualified valid strobes |
| `stall_cyc` per link | same | REQ/RSP `valid && !ready` cycles; DAT idle cycles while at least one VC credit counter is zero |

`link_perf_monitor` also asserts the credit protocol: valid with zero credit
on that VC, or an out-of-range `vc_id`, is an error.

DV bandwidth monitor (`axi_bw_monitor`, one `[Read]` + `[Write]` line per node
in `run.log`):

| field | semantics |
|---|---|
| `Latency: m +- s, N: n` | mean and stddev of per-transaction round-trip in cycles (response cycle minus request cycle) over n completed transactions |
| `BW` (bits/cycle) | accepted throughput: `beats * data_width / total_cycles` |
| `Util` (%) | `beats * 100 / total_cycles`; fraction of the one-beat-per-cycle peak. `BW = (Util/100) * data_width`: same measurement, two units |

Reading the numbers: directed runs show 1 to 2 percent Util by design (a few
transactions per node against a fast slave; read PASS and latency, ignore Util;
throughput questions belong to the mode-1 rate sweep). Latency tracks hop
distance: on a 4x4 mesh the `neighbor` pattern shows three tiers, interior 2
hops, one-axis wrap 4, corner 6, because the mesh has no torus links and the
wrap routes back across the array. The `[HWM]` lines report the per-node NMU
sizing statistics: the R-RoB slot peak from `cmodel_nmu_read_slot_hwm`, plus the
order-list peak, the peak in-flight transaction count per direction and the
admission clause
split (AW and AR separately) from `cmodel_nmu_admission_stats`.

## Config file to generator to testbench

A configuration is one YAML file under `sim/configs/`, in FlooNoC's schema
(`floogen/model/*.py`): endpoints, a router array, the connections between
them, and the address ranges each endpoint owns.

```yaml
name: mesh_4x4
network_type: "axi"

routing:
  route_algo: "XY"
  use_id_table: true          # required; offset decode is deferred

endpoints:
  - name: "tile"
    array: [4, 4]             # member k is coordinate (x, y), k = (y << clog2(x_dim)) | x
    sbr_port_protocol: ["axi"]        # is_sbr() gates SAM participation
    addr_range:
      - { base: 0x0,       size: 0x2000000, stride: 0x100000000, space: memory, en_collective: true }
      - { base: 0x2000000, size: 0x1000,    stride: 0x100000000, space: config, en_collective: true }

routers:
  - { name: "router", array: [4, 4], degree: 5 }   # N/E/S/W links are implicit

connections:
  - src: "tile"
    dst: "router"
    src_range: [[0, 3], [0, 3]]
    dst_range: [[0, 3], [0, 3]]
    dst_dir: 4                # XYDirections.EJECT
```

`stride` and `space` are this project's two additions to `AddrRange`.
FlooNoC places array member k at `base + size * k`, stride equal to size; here
a node's two apertures sit at different offsets inside one 4 GiB block, so the
stride is declared and defaults to `size` when it is not. `space` selects the
AXI class the range decodes into: `config` maps to the narrow class, `memory`
(the default) to the data class, `peripheral` to a unicast-only region above the
tile array. Every shipped configuration gives each node one memory range and one
config range (`nmu::addr_trans::SamTable::validate`).

A peripheral endpoint declares `num` instead of `array` and takes its
coordinate from the router it hangs off; `dst_dir` names the port, never a
coordinate offset. Both readers refuse a peripheral whose direction names an
edge its coordinate is not on: on an edge router that port is terminal, while on
an interior router it carries a live inter-router link and the peripheral's
Y-to-X ejection turn closes a channel dependency cycle.

The selected configuration's `endpoints:` block is the sole SAM source. It is expanded by two
readers that must agree rule for rule: `sim/tools/address_map.py` `pack_config()` at generate time,
and `nmu/sam_yaml.hpp` `load_config_table()` at simulation time through `+sam_config`. The generator
emits the testbench topology view and the frozen typed NI rule table into the same build-only
per-config `topology_pkg.sv`. The C++ model keeps the runtime loader because it cannot consume an SV
package. No synthesizable block parses YAML or exposes a runtime SAM programming interface. Both
readers reject `routing.use_id_table: false` until offset decode is implemented. The generator
places each request at
`base(dst) + offset` and the NMU SAM translates the
address back to `dst_id`, so a divergence would leave the generated package and
the generated stimulus agreeing with each other and disagreeing with the runtime
translator. `sim/configs/sam_rules.golden` is where the two meet:
`tests/nmu/test_sam_config.cpp` holds the C++ expansion to it and
`sim/tools/test_sam_config_parity.py` holds the Python one, and equality is
transitive. This data golden is not generated RTL and is not per configuration. Package-specific
tests instead generate 2x2 and 4x4 output twice, parse or elaborate the public types/rules, and make
semantic assertions without committing an SV golden. Both suites are one gate — `make check` —
because regenerating a reference from one side alone would move that side's goalposts silently.

YAML authorship order is endpoint, range, then increasing X-fast member. Overlap is legal and the
earliest authored matching rule wins. The pinned `cc_addr_decode` selects the highest matching array
index, so package generation stores authored rule `i` at `SAM[SAM_NUM_RULES-1-i]`. Explicit
`en_collective: true` alone emits collective enable and nonzero X/Y selectors; false or absent emits
zero selector lengths. The invalid-range and exact 2x2/4x4 vectors are listed in `rtl/README.md` and
the NMU/NSU verification plans.

Member k of a range lands at `base + stride * k`, where k is the host router's
array index, X-fast: `k = (y << x_bits) | x` with `x_bits = clog2(x_dim)`. That
index is not the routing id, which uses a fixed shift: `dst_id = (y << X_WIDTH)
| x`. The two coincide only at 16 wide. Every shipped configuration gives each
node a `0x100000000` block holding a `0x2000000` memory aperture at offset 0 and
a `0x1000` config aperture at `0x2000000`. `TILE_TARGETS` is therefore 2
everywhere, and the endpoint carries one decode path, the two-window one. The
windows are per node and global. Unicast addresses do not rebase; a multicast
replica changes only the matched SAM rule's Config or Memory coordinate field at the destination
NSU, preserving the global map and the shared node-local offset. The two spaces may place their
fields at different address bits. A disagreement between the two shows up as an
address outside both windows, which DECERRs; the endpoint's `DECERR_FAULT_BIT`
fault injection and the RRESP fatal in `sim/tb/test/user_node_endpoint.sv` check
that path.

A uniform stride is what makes the node index a contiguous bit field an AWUSER
address mask can wildcard: memory bases at `k * stride`, config bases at
`k * stride + 0x2000000`.

`gen_tb_top.py` rejects a configuration whose mesh dimensions or VC count exceed
the flit field capacity (`X_WIDTH`/`Y_WIDTH`/`VC_ID_WIDTH` from the flit spec)
before emitting anything. `sim/verilator/Makefile` regenerates
`$(BUILD_ROOT)/generated/<CONFIG>/topology_pkg.sv` whenever the config file or generator changes.
The per-config path prevents a `CONFIG` switch from overwriting another package. Adding a geometry
needs only a new file under `sim/configs/`;
`make sim CONFIG=<name> PATTERN=<p>` picks it up with no other change. The VC
count and the NMU read RoB mode are not configuration: they are
`noc.DAT_NUM_VC` and `nmu.READ_ROB_ENABLED` in `specgen/source/constants.yaml`,
and changing either is an edit and a rebuild. `make clean-generated` removes the complete
`$(BUILD_ROOT)/generated/` subtree.

## Seed handling

`make sim-gen` and `make sim` each accept `SEED=<n>`. Left unset,
each draws its own random 30-bit seed (`RANDOM*32768+RANDOM`, under
Verilator's `+verilator+seed+` int32 ceiling) and prints it:

```
>>> gen CONFIG=mesh_4x4 PATTERN=neighbor SEED=538912734
>>> sim CONFIG=mesh_4x4 PATTERN=neighbor SEED=538912734
```

`gen` uses `SEED` for the stimulus generator (`--seed`, used by
`uniform_random`/`hotspot`); `sim` uses it for the simulator's own
`+verilator+seed+`. Passing the same value to both reproduces a run exactly:
generation and simulation are separate commands, so the value has to be
supplied to each rather than drawn once and shared.

## Known limitations

| limitation | detail |
|---|---|
| SAM failure mode | `translate()` miss and a config file without an `endpoints:` block fail via bare `assert`: fail-loud in a debug build, undefined under `NDEBUG`. Model policy only; a real interconnect returns DECERR on a decode miss, which the NI does not model. |
| Unswept sizing | `NMU_MAX_TXNS_PER_ID` = 32 (per-ID order-list depth) is the one depth that was swept: 32 down to 1, every point a non-vacuous PASS. At four ids the run peaked at 30 of the then-32-entry shared pool against a deepest per-ID list of 12, which is what identified the pool rather than the per-ID depth as the binding limit. No throughput figure was taken at any point of the sweep. `NMU_ROB_B_DEPTH`/`NMU_ROB_R_DEPTH` default to 128 (S2) and are expressible up to 256 (the full `ordering_tag` space) via `B_ROB_DEPTH`/`R_ROB_DEPTH`; a burst whose beats (len+1) exceed the RoB depth fails loud (`Rob::push_ar` assert) instead of wedging. Equally unswept at every setting. |
| RoB physical shape unmodelled | no SRAM/flip-flop distinction, no allocator timing (the model's linear scan stands in for a combinational leading-zero count), no area reporting. |
| Verification framework gaps | no covergroups, no wire-side SVA framework, no standing co-sim regression harness (fabric coverage relies on manual `make sim` runs), no slave-latency sweep axis. The retired constrained-random axis is covered under Checkers. |
| Meta buffer storage | the 8-bucket array is kept under both `max_unique_ids` settings; the FIFO-vs-ID-queue cost difference is not modelled. |
| AXI-side perf instrumentation absent | `perf.json` carries only the NoC section (dumped at the end of every run, all injection modes); no AXI-side per-transaction hooks exist, so nothing cross-checks `axi_bw_monitor` from the model side. |
| VCS flow | build-only; no directed run target, never executed on a real VCS install. |
| Deferred header fields | QoS, route parity, and flit ECC are unbuilt and have no header field at all: the 48 b header is fully assigned (`PADDING_FIELDS_COUNT` = 0) and carries no width-0 placeholder for them. |
| Conformity exclusions | exclusive access is unit-level only; SLVERR unexercised; single-clock CDC approximation (see Conformity scope). |
| Target NI ingress backpressure unmodelled | current model ties ready true or self-credits through the DAT merge, so LOCAL stall metrics remain 0 by construction. Target Router-to-NI DAT ejection instead uses ready/valid into separate DAT Write and DAT Read class FIFOs; NI-to-Router DAT injection remains per-VC credit-controlled by Router input FIFO capacity. The model therefore cannot verify target LOCAL stalls, asymmetric DAT flow control, or Router-only VC FIFO ownership. |
| SimpleRouter multi-read ruling (S3b) | grants up to one flit per OUTPUT per tick from the same input FIFO, matching the credit `Router`. Mainline `floo_router.sv` has one FIFO read port, a resource limit, not a protocol requirement. Kept as a deliberate c_model-optimistic divergence: multi-output fan-out from one input in a single tick that RTL would need more than one cycle for. |
| `ready_slack` calibration deferred (S3b) | `SimpleRouterConfig::ready_slack` default (2) is PROVISIONAL, never measured against a real wire loop. The compliant-sender high-water mark sits exactly at FIFO depth (zero spare). Needs a measured wire-loop experiment before the default is load-bearing. |
| vc{2,4,8} re-baseline (S3b) | the VA stage reassigns the downstream VC per hop for `fixed_vc=0` traffic. Co-sim/perf numbers on vc{2,4,8} topologies shift by construction versus pre-S3b. Matrix green after S3b means re-baselined, not bit-identical to the earlier numbers. |
