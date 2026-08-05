# c_model spec-alignment campaign

Authority: `docs/noc-target-spec.md`. Where code disagrees, code changes.
Architecture reference (user-drawn block diagrams, check every stage's structure against them):

![NMU block diagram](docs/image/nmu.jpg)

![NSU block diagram](docs/image/nsu.jpg)
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

## Stage 3a: Third physical network, structural
Goal: DAT link IO + third router instance; simple-mode ready/valid router class for REQ/RSP
(1-2 stages); standard credit router retained for DAT; TX*/RX* pin contract; specgen
first-class per-network flit widths; wrapper/DPI naming; perf probes follow.
Port constraint (user ruling 2026-08-05, post-S2 bug retrospective): the REQ/RSP simple
router is a line-by-line translate of mainline `floo_router.sv`; network/channel mapping is a
translate of `floo_pkg::nw_chan_mapping`. No self-designed arbitration or routing logic in
S3a — where the RTL and our structure diverge, flag BLOCKED, do not improvise.
VIP swap (same retrospective — the S2 bug cluster lived in the self-written AXI VIP): bounded
spike replacing co-sim stimulus with vendored pulp axi_test (sim/dv/axi-0.39.7) on Verilator
5.048; if the spike passes, retire the self-written co-sim stimulus paths, AxiMasterT narrows
to ctest unit scope. Spike fails -> document why, stay put.
Carry-in from S2 reviews: delete specgen's dead "derived" width_param branches
(constants.py:227-244, :276-310) while touching per-network widths; check_strb_valid_bits is
vacuous at 64 lanes (kFullStrbMask == ~0ull) — delete the check+call site or mark explicitly.
Success Criteria: three networks carry traffic in co-sim, REQ/RSP ready/valid observed on wires.
Status: Not Started

## Stage 3b: Steering and VC collapse, semantic
Goal: axi_ch class-aware steering (narrow Aw/W/Ar on REQ; DataAw/DataW on DAT; DataAr on REQ;
DataR on DAT; B/NarrowR on RSP); VCs on DAT only; delete ni/virtual_network.hpp and read/write
VC split; rename VcArbiter to allocator naming; DAT standard router gains the VA stage
(deprecated vc_router port, direction-preference assignment) with fixed_vc=1 skipping VA,
pinned end to end; drop legacy write_rsp_vc/read_rsp_vc.
Standing ruling (keep): LOCAL->LOCAL is LEGAL by design — the self-transaction path, exercised
by passing co-sim; suppress self-traffic via the generator's `--exclude-self`, not the router.
Success Criteria: regression matrix green; write-pairing tests cover DataAw+DataW same-worm;
fixed_vc=1 stream verified to hold one vc_id end to end under contention.
Status: Not Started

## Stage 4: Collectives
Goal: NMU 48 b address mask to 8 b node mask translate + reject; AxLOCK-with-collective and
Ar-collective rejected at NMU packetize before any fanout or RoB allocation; router multicast
fork with credit discipline; CollectB in-network merge with error BRESP precedence; RoB accepts
one merged B; scoreboard keys writes by (dst_id, local_addr).
Blocking decisions, resolve before coding:
- [TBD] merged-B ordering_tag identity. The merge forwards one responder's flit, so the
  surviving header can carry that responder's ordering_tag, not the initiator's; the NMU RoB
  retires by tag, so an unconstrained merge can retire the wrong slot. Options: every replica
  inherits the initiator's tag at fanout, or the NI rebuilds the merged header from stored meta.
  Upstream RTL does not visibly resolve this either (surfaced 2026-07-21).
- [TBD] SLVERR vs DECERR precedence in merged BRESP. Spec says error-first only; RTL prioritizes
  SLVERR only; AXI4 B1.3.1 resolves the tie by first-arrival, which is arrival-dependent on a
  mesh — define a deterministic order.
- Write-ordering deadlock under overlapping destination sets (AXI4 A5.3.3, no W interleaving):
  two multicasts sharing >=2 destinations can establish opposite AW orders at two members and
  wedge (arXiv 2502.19215 §II-A Fig 2(e) for a crossbar; XY makes the inversion geometric).
  Synchronous replication closes the cycle only with the 2603.26438 stream_fork discipline
  (accept input only when all output ports ready). Confirm DataAw+W as one indivisible worm on
  DAT never opens the AW-queued-without-W window; if it does, design the fork discipline
  accordingly.
Success Criteria: multicast write matrix (row/col/submesh masks) green in co-sim; deliberate
illegal-mask and lock-multicast stimulus rejected at NMU; single-B invariant checked by
scoreboard.
Kickoff notes from S1 (final review): (1) today's AWUSER collective reject sits in
`Rob::push_aw` downstream of pool/RoB bookkeeping and is fatal (abort) — when S4 makes it a
legal-input path, the mask translate + reject must move upstream of `Rob::push_aw`, per this
stage's own "before any fanout or RoB allocation". (2) `cmodel_nmu_set_inputs` carries no
awuser argument — the DPI face needs it before any collective stimulus can be driven from
co-sim.
Status: Not Started

## Stage 5: Alignment tail
Goal: endpoint interface option (one shared vs two per-class AXI ports) or documented
unsupported; per-network perf metrics; block specs (nmu/nsu/router) re-synced to as-built;
regression re-baseline. GALS explicitly ignored (user decision 2026-08-04).
Tile endpoint integration (user direction 2026-08-05): each tile gains an AXI xbar behind the
NSU feeding two memories — config-space and data-space — implementing the spec's second-level
node-local-offset decode; xbar = off-the-shelf pulp axi_xbar RTL in the tb (Verilator compat
[UNVERIFIED], validate at integration); xbar range parameters derived from the SAME topology
YAML address_map as the SAM (single source, no second table). Couples with the endpoint-port
decision: two per-class ports would reduce the xbar to a demux or nothing. Address-map
semantics to settle with the xbar: config and memory tiles on the same node both rebase to
0-based node-local offsets, so they alias in a single-slave tb today (S2 gate worked around
it by disjoint probe offsets) — with the two-memory xbar the aliasing becomes intended
(separate targets); state it in the spec either way.
Success Criteria: FEATURE_INVENTORY.md and block specs match code; regression matrix
re-baselined.
Carry-in from S0 reviews: block-spec numeric drift vs constants.yaml (credit seed 4 vs
default 8, multiple table lines in nmu/nsu/router specs — Parameter Discipline applies);
inventory gaps (ni/wormhole_arbiter.hpp has no feature entry yet FEAT-NMU-VC_MAPPING lists
flit_tail; DEPACKETIZE uses_packet_fields omit ordering_req/ordering_tag they read).
Carry-in from S1: block-spec flit-format tables (nmu/nsu/router §2.2) still show the
pre-S1 layout — 56 b header, 408 b flit — vs as-built 44 b header, 48 b addr, 396 b flit,
axi_ch 4 b / 10-value enc, and the NMU_OUTSTANDING_DEPTH outstanding-pool params; re-sync
alongside the S0 numeric drift above. Also from S1: noc-performance-parameters.md formulas and
worked example still size concurrency from MAX_TXNS_PER_ID windows multiplying across IDs —
rework on the pool model (row + attribution already fixed in S1); regression/perf matrix
re-baseline expected since multi-ID patterns previously reached N_ids x 32 outstanding via the
bypass path and now cap at the 32-aggregate pools.
Status: Not Started

## Deferred (post-campaign)

Long-horizon items parked here; when the campaign ends and this file is deleted, roll what is
still open into that round's backlog "This round".

- ID compression width tiers: NSU max_unique_ids only 1 or 256; add selectable N per-id FIFOs.
- Per-tile compute rate: absent from spec and perf docs; every utilization figure and minimum
  viable tile size depend on it (docs/noc-workload-benchmark.md §9).
- Defensive smalls: SAM translate() miss must throw under NDEBUG (asserts today, null-deref in
  release); sam_yaml missing address_map needs a descriptive error; gen_tb_top rejects empty
  requested_name; specgen pytest must write to a temp dir (rewrites committed banners today);
  axi_bw_monitor.sv carries a 2-line local edit, upstream or wrap it; specgen
  examples/quickstart printf column padding misaligned since the S0 rename (cosmetic);
  gen_test_patterns.py validates neither AxLEN nor the AXI 4 KB rule (illegal BURST_LEN
  surfaces as the RoB oversized-burst abort, not a stimulus error); co-sim default beat is
  half-bus (--size 5 at 64 B bus) outside beat_exact; some test helpers still take uint32_t
  strb params (cannot express lanes >= 32).
- Verification methodology: AXI-side perf DPI hooks never driven (bw_monitor vs perf.json
  cross-check has never run); no coverage, no constrained-random, no wire-level SVA.
- Infrastructure notes: VCS flow builds but has never executed; WSL host instability
  (rsync to ~/noc_project, foreground one session at a time, echo-marker + retry).
- Deck: user regenerates three image-based diagrams (s6/s7 NMU/NSU block diagrams with old
  ADDR 64 b + NUM_OF_DAT_CHAN_VC, s8 NSU port symbol with old noc_* pin names).
- Release-package note: the shipped D:\noc_project copy lacks sim/verilator/perf_cli_summary.py
  and docs/image/pipeline_ref.jpg; send along if the recipient needs those flows.
- Trade-off record 2026-08-04 (for the ledger): deleted flat-LRU arbitration, NoC-layer QoS,
  reduction-operator-set, collective-scope items; turn-model VA folded into S3b.
