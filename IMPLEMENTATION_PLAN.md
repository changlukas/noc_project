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

Standing ruling: LOCAL->LOCAL is LEGAL by design — the self-transaction path, exercised by
passing co-sim; suppress self-traffic via the generator's `--exclude-self`, not the router.
SimpleRouter honors it via an explicit loopback-tie-off exemption (deliberate divergence from
floo_router's blanket NoLoopback, cited in simple_router.hpp).

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
Endpoint rulings (user, 2026-08-06), lint-verified before adoption:
- **One shared 512 b AXI port** at the endpoint. Two per-class ports rejected, so the four
  narrow-lane re-anchor sites stay as built.
- **Tile decode = taxi** (`E:\03_Learning\taxi`): `taxi_axi_crossbar_1s` with `M_COUNT=2`
  (one slave-side port fanning out to two targets — the right-sized piece, not the full
  N x M crossbar) plus two `taxi_axi_ram` as the config and data memories. Range parameters
  (`M_BASE_ADDR` / `M_ADDR_W`) come from the same topology YAML `address_map` as the SAM.
- **One interface convention per side, zero adapters.** taxi's `wr`/`rd` are modports of ONE
  `taxi_axi_if` instance, not two instances, so a single interface feeds both crossbar ports.
  The endpoint's slave face is already hand-wired per field from the flat DPI signals
  (`user_node_endpoint.sv:120-135` into `AXI_BUS_DV` today), so re-targeting those assigns at
  a `taxi_axi_if` costs nothing. The master face stays pulp `AXI_BUS_DV` because the VIP
  (`axi_file_master`, `axi_scoreboard`, S4's `mcast_preload_scoreboard`) lives there; the two
  faces never meet, the NoC is between them.
- **Licence**: taxi is CERN-OHL-S-2.0 (strongly reciprocal) while this repo is proprietary and
  the vendored pulp/common_cells are Solderpad. User ruled: ignore, internal use only.
- **`axi_rand_slave` is dropped** (option A). Accepted losses, recorded so nobody rediscovers
  them as bugs: (1) randomized slave backpressure and response delay disappear —
  `taxi_axi_ram` stalls only when genuinely busy, and taxi has no throttle component (their
  randomization is cocotb-side, which we do not use); this is the pressure that surfaced the
  WireSlavePort `push_aw` latch bug and the credit-depth bubble, so slave-path timing
  exploration narrows. (2) `taxi_axi_ram` initializes `mem` to 0, not X, so the multicast
  non-vacuity argument that leaned on unwritten replicas reading X now only bites when the
  golden byte is non-zero (T6 measured 6 such bytes in 768); the MCAST_FAULT red run and the
  per-node compare counter are unaffected. Option B (an LFSR-driven ready-gating shim on the
  crossbar's master ports, using taxi's own `taxi_lfsr`) is deferred to a future verification
  round, not built now.
- **Integration constraints found while checking**: `taxi_axi_ram`'s `mem[2**VALID_ADDR_W]` is
  a DENSE array, so `ADDR_W` must be sized to the tile region, never to the 48 b system
  address (pulp's associative array hid this). Verilator include paths are
  `src/axi/rtl` + `src/prim/rtl` (`taxi_arbiter`).
- **Tile-local address layering (user ruling 2026-08-06)** — the prerequisite that makes the
  xbar decode possible at all. Today `SamTable::translate` rebases to the matched ENTRY's base
  (`nmu/addr_trans.hpp:83`), and a node owns two entries (memory, config), so both spaces
  arrive at the tile starting at local 0 and the decoder has no bits to discriminate on.
  Re-ordering the system map to group a node's regions is NOT available: the memory-first /
  config-second layout is forced by the multicast masks (replica addresses must differ only in
  node-index bits, so the node index has to sit in a clean field per space — memory [22:20],
  config [14:12] on the 2x4 map). Ruling: keep the system map exactly as it is and add a fixed
  tile-local base at rebase time, `tile_local_addr = space_base(space) + (addr - entry_base)`,
  with `space_base` derived from the same YAML sizes (config 0x0, memory 0x100000 on today's
  map, i.e. each region rounded to a power of two and packed aligned). The NMU already reads
  `space` to pick the class, so this is one added term; the NSU still does no lookup; the xbar
  decodes on `M_BASE_ADDR`/`M_ADDR_W` generated from the same source. Rejected alternative:
  demuxing on the AXI class instead of the address — taxi's crossbar decodes by address only,
  and it would permanently bind config to narrow.
- **Sizing rule**: the xbar's `ADDR_W` is the tile-local span (21 b on today's map); each
  target's `M_ADDR_W` is its rounded region size (config 12, memory 20); each `taxi_axi_ram`'s
  own `ADDR_W` comes from the DV-tier `region_bytes` (what stimulus actually touches), NOT the
  mapped region size, so a 1 MB address window does not force a 1 MB dense array. Generator
  computes all of it from the topology YAML; accesses past a RAM's own size alias inside it,
  which is a stimulus-bounded assumption to state.
- **Lint spike PASSED** (Verilator 5.048, 16 modules, zero warnings): `taxi_axi_crossbar_1s`
  `M_COUNT=2` / `DATA_W=512` with unpacked interface arrays and two `taxi_axi_ram` behind it.
  The module's own "TODO fix parametrization once verilator issue 5890 is fixed" note does not
  bite at these parameters. The plan's earlier [UNVERIFIED] Verilator-compat flag is cleared.

Success Criteria: FEATURE_INVENTORY.md and block specs match code; regression matrix
re-baselined.
Carry-in from S0 reviews: block-spec numeric drift vs constants.yaml (credit seed 4 vs
default 8, multiple table lines in nmu/nsu/router specs — Parameter Discipline applies);
inventory gaps (ni/wormhole_arbiter.hpp has no feature entry yet FEAT-NMU-VC_MAPPING lists
flit_tail; DEPACKETIZE uses_packet_fields omit ordering_req/ordering_tag they read).
Carry-in from S3b (doc polish, fold into the block-spec re-sync): nmu-spec §2.4 fixed_vc
paragraph uses unqualified "AW" in two channel-class senses (line ~111 DataAw vs ~113
NarrowAw) — add the "narrow" qualifier; residual "VC arbiter" prose in nmu-spec G9 /
NOC_DAT_NUM_VC parameter row / nsu-spec §3.4; `cmodel_dpi.h:142` "vnet" wording. Also from
S3b: ready_slack calibration still deferred (needs a measured wire-loop experiment).
Carry-in from S4: (1) RULED per network (user, 2026-08-06). Read data is carried differently on
the two networks, and the block specs must say so rather than calling the whole fabric
"XY wormhole".

**DAT (DataR): virtual-channel flow control.** A read burst is ONE multi-flit packet. VC
allocation is packet-granular — the VC is allocated to the head flit at each hop and inherited
by body and tail (the standard VC-router pipeline, where body and tail skip RC and VA); switch
allocation stays flit-granular, so packets on different VCs interleave on the physical link.
Rejected alternative: keeping per-beat packets pinned by `fixed_vc`. That mechanism exists to
pin ordering-critical streaks (the S3b U1 hole); reusing it as the universal read-data carrier
conflates two purposes and permanently excludes all read data from per-hop VC reallocation.
Work implied: `nsu::Packetize::build_r_flit` marks only the final beat (burst length is already
in the MetaEntry) instead of today's unconditional `flit_tail = 1` at `:125`; NSU R switches to
`fixed_vc = 0` so VA runs per hop and the NI hash becomes only the first-hop pick; the DAT
router needs no new mechanism (`locked_output_vc` already implements body-follows-head, ported
and verified in S3b) but its continuation path must be exercised with R packets, not only
DataAw+W. Cost to accept: a long burst now holds its VC for the burst's duration, though other
VCs keep flowing.

**RSP (NarrowR, NarrowB, DataB): single-flit packets, unchanged.** Packet and flit coincide, so
no packet-granular link reservation is ever held; ordering within a burst follows from
deterministic routing over in-order channels. Rationale for not extending the DAT ruling here:
RSP is single-VC by spec (:40), so a multi-flit NarrowR would be wormhole-with-one-VC, the
textbook anti-pattern — a blocked packet holds the link while every packet behind it stalls,
and here those are the B flits of both classes, which have no ordering relationship to read
data. The standard remedy (add VCs) is unavailable by spec, so single-flit packets ARE the
head-of-line-blocking remedy on this network. Consequence: T4's join mid-worm hold stays
dormant and the held-join wait-for edge stays closed, as S4's final review found.

B stays single-flit on both networks. (2) router-spec §4
has no numbered SPEC entries for fork/join — §2.10 ends with ctest/co-sim pointers instead;
add them if the numbered contract list is meant to carry them. (3) router-spec §2.8 still says
two networks per node (carries the file's own Pre-S3a marker).
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
  (rsync to ~/noc_project, foreground one session at a time, echo-marker + retry); generated
  fabric drives one packed vector from a port connection (bit 0) plus always_comb (bits 1-4)
  — Verilator tolerates, VCS may reject (check before first VCS run); deleting only
  noc_fabric_<topo>.sv leaves the build failing on a missing file instead of regenerating.
- Deck: user regenerates three image-based diagrams (s6/s7 NMU/NSU block diagrams with old
  ADDR 64 b + NUM_OF_DAT_CHAN_VC, s8 NSU port symbol with old noc_* pin names).
- Release-package note: the shipped D:\noc_project copy lacks sim/verilator/perf_cli_summary.py
  and docs/image/pipeline_ref.jpg; send along if the recipient needs those flows.
- Trade-off record 2026-08-04 (for the ledger): deleted flat-LRU arbitration, NoC-layer QoS,
  reduction-operator-set, collective-scope items; turn-model VA folded into S3b.
- Stage 4 carry-out, model correctness: `remap_downstream_id` collapses ids to 8'hFF at
  `max_unique_ids == 1`, so a slave that violates B ordering can stamp a collective mask onto
  an unrelated B and inject a false CollectB into the join. Unicast AWs admitted through
  `Rob::push_aw` get no `burst_footprint_ok` check — only the direct `Packetize::push_aw` path
  asserts it (pre-existing asymmetry, now visible beside the collective translate, which does
  check every replica).
- Stage 4 carry-out, cost: 16x16 full-mesh mask enumeration is 256 x O(SAM entries) linear
  scans per `push_aw` attempt, repeated on every backpressure retry. Design K3 accepted <= 256
  lookups, not 256 x O(entries).
- Stage 4 carry-out, test shape: multi-hot to multi-hot fork completion across a link and
  `output_fifo_depth > 0` fork/join mode are untested; the held-join wait-for edge has
  probabilistic co-sim coverage only; no narrow-class red run.
- Stage 4 carry-out, co-sim minors: the merged-B checks run for all patterns and modes, not
  only multicast; the probe-window guard keys on `base_local` rather than the config tile size;
  `multicast` with `INJECTION_MODE != 0` is guarded only at the root Makefile.
- If REQ/RSP faces ever open >1 VC or AR/B steer to DAT, the ordering_req=0 same-(dst,id)
  bypass streak loses in-fabric ordering (AR pinning + B fixed-hash deleted in S3b).
  ChannelModel is vc-blind, so ctest cannot see it.
- Router VA divergence assert sits behind the credit gate. A zero-credit diverging fixed_vc=0
  worm idles silently until credit arrives (checker liveness gap, S3b T5).
