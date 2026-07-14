# Docs salvage inventory (Round 1, lane 3)

Existing docs are deleted in Round 3; this inventory lists the content that moves into the fresh
doc set (`README.md`, `docs/spec.md`, `docs/trade-off.md`). Anything not listed dies with the
deletion. Stale claims live in the ledger (Lane 3), not here.

## Half A: README, architecture, verification-environment, development, cosim-log

| doc | section | target | content one-liner |
|---|---|---|---|
| architecture.md | §1 System context | spec.md Overview | verification intent: AXI4 IHI 0022H conformity at the NMU/NSU pin boundary before silicon; NoC fabric explicitly not under conformity |
| architecture.md | §2 NMU (SAM) | spec.md Address translation | SAM is a `{base,size,dst_id}` range lookup (not bit-slice), rebases to 0-based local addr; 4KB-granular tiles guarantee no burst splitting; wire format unchanged |
| architecture.md | §2 NMU (SAM miss) | trade-off.md | address miss asserts = model policy, not AXI-faithful; real interconnect returns DECERR (known deviation) |
| architecture.md | §2 NMU RoB | spec.md Response ordering | `rob_idx` is a header field not an AXI ID; per-ID order list; Disabled default = one outstanding per ID; Enabled bypasses allocation when ID idle |
| architecture.md | §2 NSU | spec.md NSU | NSU has no RoB by design (response ordering is the manager-side NI's responsibility) — NMU/NSU asymmetry rationale |
| architecture.md | §2 deferred fields | trade-off.md | `noc_qos`/`route_par`/`flit_ecc` zero-filled; ECC CSR counters present but check logic unimplemented (known limitations) |
| architecture.md | §2 meta buffer | spec.md NSU meta buffer | `{src_id,upstream_id,rob_req,rob_idx}` keyed by downstream ID; response homes via `src_id` never AXI ID; `remap_downstream_id` keyed on `upstream_id` alone (avoid rid-collision interleave) |
| architecture.md | §2 max_unique_ids | trade-off.md | what the knob is (out-of-order-return storage: FIFO vs id_queue area cost) vs is not (not a throughput knob — verified 2026-07-09); unrelated to RoB; not modelled in C++ |
| architecture.md | §3 handle ABI | spec.md DPI ABI | 64-bit integer handle encodes `HandleBlock*`; integer not SV `chandle` because VCS rejects chandle as a module port |
| architecture.md | §3.2 tick semantics | spec.md Timing model | beta tick = 1 rising edge; 1-cycle latency/hop; registered handshake; adapters absorb C++-immediate vs SV-delta timing |
| architecture.md | §4 wrap invariant | spec.md DPI wrap layer | adapter may only latch wires + call `tick()` once; business logic forbidden in adapter |
| architecture.md | §6 conformity scope | spec.md Conformity scope | covered IHI sections (HSH/BUR/BND/ORD/RSP+DECERR) and excluded-with-reason (no Exclusive Monitor, no SLVERR, single-clock CDC approximation) |
| verification-environment.md | Traffic patterns | spec.md Traffic patterns | destination rules (neighbor diagonal-wrap, transpose y/x, hotspot many-to-one) with booksim2 provenance |
| verification-environment.md | Injection rate | spec.md Perf methodology | mode 0 directed/scoreboard-armed vs mode 1 continuous/bw-gated; Bernoulli per-cycle gate; `MAX_UNIQUE_IDS`/`MAX_OUTSTANDING` are NSU (not injection) knobs |
| verification-environment.md | Test axis note | trade-off.md | constrained-random axis deferred: no sound data-integrity checker for random traffic yet (known gap) |
| development.md | §3 toolchain | README Prerequisites | verified toolchain combo (Verilator 5.036 Win / 5.048 WSL-z3, GCC 15.2, `--output-split 0` coroutine-split workaround) |
| development.md | §3 WSL/VCS | trade-off.md | WSL 5.048 needs self-build with z3 + PATH ordering; `SIM=vcs` dry-run validated only, first real VCS run pending (known limitation) |
| cosim-log.md | Per-node monitor | spec.md Perf metrics | field semantics: latency in cycles = resp−req cycle; `BW=(Util/100)*data_width`; Util = fraction of one-beat-per-cycle peak |
| cosim-log.md | Reading the numbers | spec.md Perf methodology | directed runs show ~1-2% Util by design (correctness not load); neighbor latency tiers on 4x4 mesh: interior 2 / one-axis-wrap 4 / corner 6 hops |

## Half B: nmu-rob-microarchitecture, performance-probe, pg037, pulp-vip-node, issue/, slides/

| doc | section | target | content one-liner |
|---|---|---|---|
| nmu-rob-microarchitecture.md | §2 gap vs NoRoB baseline | trade-off.md | Disabled mode's one-outstanding-per-ID interlock strictly more conservative than the per-ID-counter baseline; throughput cost never measured |
| nmu-rob-microarchitecture.md | §3 Enabled allocation/release | spec.md RoB | rationale for high-water `lzc` allocator (holes below mark unreusable) and per-beat read release; not obvious from the bitset code |
| nmu-rob-microarchitecture.md | §4 invariants | spec.md RoB | at-most-one-bypass-per-ID; total-order-list bound = NumIds+depth; asserted nowhere in code |
| nmu-rob-microarchitecture.md | §4 long-burst wedge fix | spec.md known-limitations | why bypass clause 1 makes bursts longer than pool depth admissible at all |
| nmu-rob-microarchitecture.md | §5 slot pool = deadlock guarantee | spec.md RoB | "no free slot, no request" is the fabric's deadlock guarantee, not removable coupling |
| nmu-rob-microarchitecture.md | §5 clause-2 unsafe here | trade-off.md | round-robin VC spread (vs deterministic direction-VC) is why clause 2 cannot port unchanged; hazard only at pool>1 |
| nmu-rob-microarchitecture.md | §5a clause-2 VC-safe design | trade-off.md | (dst,id) injection binding design, directed/random duality, measure-before-build gate for the R-RoB depth cut |
| nmu-rob-microarchitecture.md | §6 param mapping | spec.md param table | upstream RoB size params → our b/r_rob_depth/max_txns ctor params; rob_idx width independent of depth |
| nmu-rob-microarchitecture.md | §7 C++-not-designer decisions | trade-off.md | open RTL choices: independent B/R depths, high-water vs ring, per-ID deque vs shared store, SRAM/FF split |
| nmu-rob-microarchitecture.md | §8 not modelled | spec.md known-limitations | storage-class, allocator timing, per-direction mode unmodelled |
| nmu-rob-microarchitecture.md | §9 references | spec.md references | upstream RTL file map, AXI4 ARID-interleave rule, id-remap approach for small NumIds |
| performance-probe.md | §1 overview | spec.md probe | Profile vs Trace two-view model; best-case reference = in-run minimum, not a separate idle pass |
| performance-probe.md | §2 architecture | spec.md probe | design invariant: taps input-only, single collector writer, monitors never perturb the design |
| performance-probe.md | §4 metric definitions | spec.md probe metrics | latency start/stop points, idle-cycle interface-local semantics, service/flit/stall/occupancy meanings |
| performance-probe.md | §4 reading the profile | README / spec.md | symptom→cause interpretation table (max>>min = contention, high outstanding = queue pressure) |
| performance-probe.md | §6 latency composition method | spec.md probe | only round-trip total + memory service measured; NI/router from pipeline depth; wrap = residual |
| pg037_axi_perf_mon.md | whole | spec.md references | cite as external source for Profile/Trace metric definitions; do not carry the verbatim guide |
| issue/ARCHITECTURE.md | §1 layering | spec.md architecture | bottom-up dependency-direction mental model (specgen→axi→ni→nmu/nsu→router→wrap→sim) |
| issue/ARCHITECTURE.md | §3.4 NMU/NSU asymmetry | spec.md architecture | axi_slave↔master_port, rob↔meta_buffer, addr_trans/ni_tokens NMU-only are role differences, not defects |
| issue/ARCHITECTURE.md | §6.3 shared-core extraction | trade-off.md | diff-w rubric ranking port_params (extract) vs vc_arbiter/depacketize (borderline) vs packetize (reject) |
| 2026-07-02-pulp-axi-vip-node-design.md | Motivation | spec.md verification | why a second independent VIP: self-made C++ BFM cannot back AXI4 conformance |
| 2026-07-02-pulp-axi-vip-node-design.md | D1 | README / spec.md env | constrained-random must run on WSL/Linux — Verilator solver pipe needs `fork()`, unusable on Windows |
| 2026-07-02-pulp-axi-vip-node-design.md | D4 | spec.md verification | symmetric per-node endpoint layout; fabric/NI wraps/topology untouched |
| 2026-07-02-pulp-axi-vip-node-design.md | Scope exclusions | spec.md known-limitations | ATOP and user signals out of scope: `axi_req_t` struct has no awatop/awuser fields |
| 2026-07-02-pulp-axi-vip-node-design.md | Region contract | spec.md verification | address high-bits select dst node; one disjoint region per manager (permutation pairing) by construction |
| 2026-07-02-pulp-axi-vip-node-design.md | End-of-sim / watchdog | spec.md verification | watchdog formula BASE+K×beats with K_CYC_PER_BEAT=40 calibrated from vc1 15-30 cyc/beat |
| 2026-07-02-pulp-axi-vip-node-design.md | Seed management | spec.md verification | per-simulator seed plusargs + matrix.yaml seed field for reproducible regression |
| 2026-07-02-pulp-axi-vip-node-design.md | Monitors | trade-off.md | keep in-fabric PMU + link_perf_monitor: no industry in-fabric NoC counter substitute exists |
| slides/genamba-role1-port-SLIDES.md | Slide 3 Task E | spec.md ordering | same-ID R must return in AR-issue order (IHI 0022 A5.3); WID removed so W follows AW — the invariant survives, not the retired harness |
| slides/genamba-role1-port-SLIDES.md | Slide 4 checker liveness | spec.md verification | principle: a checker not fault-injection-verified is unverified |

## Pass 3 additions + corrections

CAUTION (pass 5) on the Half-B performance-probe rows above (§1 two-view model, §4 metric
definitions, §4 symptom→cause, §6 latency composition): the AXI-monitor/Trace machinery they
describe is UNBUILT — only NoC-side counters (router fifo occupancy, link flit/stall) and the DV
bw-monitor run.log lines ship. Salvage the concept as intended-design only; spec.md documents
as-built output exclusively (perf_collector.hpp:14-16 records the drop).

CAUTION on the Half-B pulp-vip rows above: the constrained-random axis (`axi_rand_master` +
`axi_reorder_compare`) was retired 2026-07-13; the doc is whole-doc stale. Salvage from it only
what survives the revert (Motivation, D1 fork() constraint, D4 layout, scope exclusions, watchdog
formula); the region-contract / seed-management / checker-pairing rows describe the retired
methodology — spec.md's verification section draws from verification-environment.md instead.

| doc | section | target | content one-liner |
|---|---|---|---|
| verification-environment.md | VIP + DUT boundary (post-2026-07-13 revert) | spec.md verification | authoritative live env: DUT = NMU + routers + NSU per node via `master_dv`/`slave_dv`; VIP set = `axi_file_master` (directed two-phase), `axi_rand_slave` MAPPED (tile memory), `axi_scoreboard` (manager-face data integrity), `axi_bw_monitor` |
| verification-environment.md / cosim-log.md | non-vacuous pass principle | spec.md verification | PASS gated on `txn_cnt_o > 0` per node so a zero-transaction vacuous pass cannot report clean; DIRECTED PASS additionally requires zero scoreboard mismatch |
| development.md | §4 specgen regen workflow (pass-4 addition) | README / spec.md build | `codegen.py --target {cpp,sv} --domain {packet,signals}` regen; `--check` drift gate wired as CMake `codegen_check` + `make specgen_pytest`; `gen_inventory.py` manual; JSON sources canonical (verified accurate against codegen.py:314-330) |
| development.md | §3 build portability mechanism (pass-7 addition) | README build / trade-off.md | `make build` identical on Win/Linux via auto-detect (cmake3-vs-cmake, stdc++fs auto-link for GCC<9, py -3-vs-python3); offline build via DEPS_SRC + FETCHCONTENT_FULLY_DISCONNECTED; overrides in gitignored local.mk — pairs with the lane-4 platform-matrix row (matrix = what, this = how) |

## Pass 2 addition: backlog.md (file kept, but open items feed the fresh docs)

| doc | section | target | content one-liner |
|---|---|---|---|
| backlog.md | Open defensive/correctness | spec.md known-limitations / trade-off.md | SAM `translate()` miss and `+sam_config` missing-`address_map` fail via bare assert that null-derefs under `NDEBUG` — model-only fail-loud, not silicon-faithful |
| backlog.md | Open RoB/NSU sizing | spec.md known-limitations | `max_txns_per_id`=32 and `r_rob_depth`=256 unswept placeholders; 256 expressible via `R_ROB_DEPTH` (sim/verilator/Makefile:226) but never depth-swept |
| backlog.md | Open RoB/NSU sizing | trade-off.md | NSU `VcArbiter` still carries the `use_pools_` scalar-VC branch the NMU collapsed into a size-1 pool — deliberate asymmetry pending mirror |
| backlog.md | Open DV-IP no-edit debt | trade-off.md | `axi_bw_monitor.sv` hand-edited inside the vendored DV tree, not isolated as patch/wrapper |
| backlog.md | Open verification methodology | spec.md known-limitations | four standing gaps: no co-sim regression harness; perf DPI unwired in traffic mode (perf.json=0 bytes); no covergroup/CRV/wire-side SVA; no subordinate-latency sweep axis |
| backlog.md | Open design rounds | trade-off.md | NoC-layer QoS unbuilt (`NOC_QOS_WIDTH=0` zero-fill); turn-model VC assignment unported, largely superseded by the shipped (dst,id) static VC binding |
| backlog.md | Open infra | spec.md known-limitations | VCS flow build-only, no directed run target, never run on a real VCS install |
