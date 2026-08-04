# c_model spec-alignment campaign

Authority: `docs/noc-target-spec.md`. Where code disagrees, code changes.
Inputs: gap analysis (26 deltas, 2026-08-04), gvsoc soft_hier floonoc inventory, FlooNoC v0.8.4
RTL study, Codex plan review, backlog trade-off round (2026-08-04).

Execution order is committed: S0 -> S1 -> S2 -> S3a -> S3b -> S4 -> S5. Each stage still gets
its own task breakdown + review at kickoff. Every stage lands commit-green: compiles, ctest
passes, co-sim smoke passes. Protocol: when a stage completes, delete its section from this
file; when the file is empty of stages, delete the file.

## Port-source map (copy, don't invent)

| Need | Source | Mode |
|---|---|---|
| Multicast per-hop output-port set | FlooNoC `hw/floo_route_xymask.sv` FwdMode=1 (mask = wildcard over dst_id, matches spec) | translate RTL to C++ |
| Fork under credit/VC backpressure | FlooNoC `hw/floo_router.sv:344-391` past_handshakes bookkeeping | translate RTL to C++ |
| CollectB merge | gvsoc `FlooNoc::handle_request_end` join protocol + FlooNoC `floo_reduction_sync` / `floo_reduction_arbiter` reverse-path structure, SLVERR precedence at `floo_reduction_arbiter.sv:101` | hybrid |
| NMU mask translate + reject, AW mask lifecycle (latched for W/B) | FlooNoC `hw/floo_axi_chimney.sv:534-546` + mask latch `:553` | translate RTL to C++ |
| gvsoc `collective_analyze` momentum table | structural reference only. Its mask convention is inverted (set = match src) and border ring is hardcoded; do not port those | cross-check |
| Standard-router VA stage + fixed_vc bypass | FlooNoC `hw/deprecated/vc_router_util/` (`floo_vc_assignment.sv` direction-preference policy, `floo_vc_selection.sv`); fixed_vc=1 skips VA, credit always enforced | translate RTL to C++ |
| Reduction ALU (gvsoc types 2-7) | out of scope, spec collective_op 2-3 reserved | none |
| NSU receive-side address handling | none needed, flit addr is node-local offset already | no-op |

gvsoc gotchas honored: no reverse path modeled (we build it), fork ignores backpressure (RTL
discipline instead), inverted mask convention (rejected), fp16 truncation (moot).

Provenance note (read before implementing S3): the DAT standard router's credit flow control,
VCs, and VA stage all come from FlooNoC `hw/deprecated/vc_router_util/`, NOT mainline — the
mainline router is ready/valid with NI-assigned VCs and no reallocation. Do not "correct" the
DAT router against mainline `floo_router.sv`; the deprecated VC-router suite is the reference.
Mainline is the reference for the REQ/RSP simple router and for the multicast/collective logic.

Design rule: everything parameterized, spec numbers are defaults, never hardcoded — RoB slot
depth (default 128 = 8 KB at 64 B beats), VC count (1-8), mesh dims, outstanding-per-ID, data
widths all stay free parameters exactly as FlooNoC keeps them; tests cover the parameter range,
not one value.

## Stage 0: Cheap wins
Goal: header renames `last`/`rob_req`/`rob_idx` to `flit_tail`/`ordering_req`/`ordering_tag`;
FEATURE_INVENTORY.md CDC honesty fix; dead-code sweep (`NSU_DEPKT_Q_DEPTH`,
`NOC_SLAVE_VC_BUFFER_DEPTH`, stale comments nmu.hpp:14-18 / cmodel_dpi.h:90 /
router_wrap.hpp:16).
Success Criteria: ctest green, codegen --check clean, grep zero stale names and dead params.
Status: Not Started

## Stage 1: Formats and params (specgen-first, data path stays 256 b)
Goal: addr 64 to 48 b; header 56 to 44 b with axi_ch 4 b ten encodings; fixed_vc field (field
only); collective_op/mask fields (enabled in format, runtime-rejected until S4); Aw/Ar payload
93 b; AwBeat user 8 to 58 b type + NMU strips [57:8]. Flit widths are interim here — final
REQ 137 / RSP 127 / DAT 629 need the narrow/data classes and land in S2.
Confirm with user before landing: mesh dim min 2 vs keep 1 as degenerate (constants min is 1
today, 1x1 was deliberately legalized); outstanding-per-ID narrowing 1-256 to spec's 1-32.
Success Criteria: specgen regenerated both languages, all call sites and tests updated, ctest +
co-sim green at 256 b.
Status: Not Started

## Stage 2: Dual data class 64/512
Goal: narrow 64 b + data 512 b classes; WSTRB past uint32_t; memory/master/slave/DPI
marshalling/VIP types; payload layouts NarrowW 81, DataW 585, B 18, NarrowR 83, DataR 531;
final flit widths REQ 137 / RSP 127 / DAT 629; RoB slot depth stays a free parameter, default
moves to 128 (= 8 KB at the 64 B data-class beat, spec: two 4 KB bursts). Interim: data-class flits ride the existing REQ/RSP links
(widened) until S3a splits the networks.
Success Criteria: ctest + co-sim green at both widths; DPI boundary verified beat-exact.
Status: Not Started

## Stage 3a: Third physical network, structural
Goal: DAT link IO + third router instance; simple-mode ready/valid router class for REQ/RSP
(1-2 stages); standard credit router retained for DAT; TX*/RX* pin contract; specgen
first-class per-network flit widths; wrapper/DPI naming; perf probes follow.
Success Criteria: three networks carry traffic in co-sim, REQ/RSP ready/valid observed on wires.
Status: Not Started

## Stage 3b: Steering and VC collapse, semantic
Goal: axi_ch class-aware steering (narrow Aw/W/Ar on REQ; DataAw/DataW on DAT; DataAr on REQ;
DataR on DAT; B/NarrowR on RSP); VCs on DAT only; delete ni/virtual_network.hpp and read/write
VC split; rename VcArbiter to allocator naming; DAT standard router gains the VA stage
(deprecated vc_router port, direction-preference assignment) with fixed_vc=1 skipping VA,
pinned end to end; drop legacy write_rsp_vc/read_rsp_vc.
Success Criteria: regression matrix green; write-pairing tests cover DataAw+DataW same-worm;
fixed_vc=1 stream verified to hold one vc_id end to end under contention.
Status: Not Started

## Stage 4: Collectives
Goal: NMU 48 b address mask to 8 b node mask translate + reject; AxLOCK-with-collective and
Ar-collective rejected at NMU packetize before any fanout or RoB allocation; router multicast
fork with credit discipline; CollectB in-network merge with error BRESP precedence; RoB accepts
one merged B; scoreboard keys writes by (dst_id, local_addr).
Blocking decisions, resolve before coding:
- [TBD] merged-B ordering_tag identity: replicas inherit initiator tag at fanout, or NI rebuilds
  header from stored meta (backlog "blocks multicast write" item)
- [TBD] SLVERR vs DECERR precedence in merged BRESP (spec says error-first only; RTL prioritizes
  SLVERR; AXI4 B1.3.1 tie-break is arrival-dependent)
- Confirm DataAw+W as one indivisible worm on DAT closes the overlapping-destination
  write-ordering deadlock window (backlog §C); if not, design the fork discipline accordingly
Success Criteria: multicast write matrix (row/col/submesh masks) green in co-sim; deliberate
illegal-mask and lock-multicast stimulus rejected at NMU; single-B invariant checked by
scoreboard.
Status: Not Started

## Stage 5: Alignment tail
Goal: endpoint interface option (one shared vs two per-class AXI ports) or documented
unsupported; per-network perf metrics; block specs (nmu/nsu/router) re-synced to as-built;
regression re-baseline. GALS explicitly ignored (user decision 2026-08-04).
Success Criteria: FEATURE_INVENTORY.md and block specs match code; regression matrix
re-baselined.
Status: Not Started

## Backlog absorption

Absorbed here: Next-up #1 vc_fixed incl. turn-model VA policy (S1 field + S3b), backlog §A all
rows (S1/S2/S3), read/write VC split removal + VcArbiter rename + legacy rsp_vc scalars (S3b),
dead-code sweep (S0), merged-B tag / tie-break / write-ordering deadlock decisions (S4).
Deleted in the 2026-08-04 trade-off: flat-LRU arbitration, NoC-layer QoS, reduction-operator
set, collective-scope justification.
Not absorbed, stays in backlog: ID compression width tiers, per-tile compute rate, defensive
small items, verification methodology, VCS/WSL infrastructure.
