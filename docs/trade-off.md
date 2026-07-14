# Design trade-off record

Deliberate divergences from textbook or upstream convention, with rationale. Every mechanism
named here exists in the tree today; `docs/spec.md` is the as-built reference this record argues
against. Entries are grouped naming / response-ordering-and-VC / packaging; ids are stable and
non-sequential (carried over from the Round 1 audit ledger).

## Naming

### T-01 `VcArbiter` names a VA+arbitration hybrid

Textbook: VC allocation (VA) and switch (N:1) arbitration are distinct router pipeline stages
(on-chip-networks Ch 6). Ours: `nmu::VcArbiter` / `nsu::VcArbiter` each do both: select a VC per
flit (fixed VC id or round-robin), then credit-gated round-robin drain downstream.

Rationale: the class boundary follows the upstream RTL block it is a 1:1 port of (References in
`docs/spec.md`); splitting selection from drain would buy no test coverage the combined
class does not already have.

### T-02 `MetaBuffer` over TSHR

Textbook: Transaction Status Holding Registers (TSHR) at the NI (on-chip-networks Ch 2). Ours:
`nsu::MetaBuffer` keeps its upstream RTL counterpart's name (References in `docs/spec.md`) and a
field set (`{src_id, upstream_id, rob_req, rob_idx}`, restored via peek-plus-commit) wider than a
plain TSHR.

Rationale: parity traceability with the upstream port outweighs renaming a 1:1-ported struct to
the textbook term.

### T-03 `FEAT-ROUTER-VC_ARBITRATION` names switch allocation

Textbook: the mechanism (per-output flit-level round-robin across VCs, credit-gated) is switch
allocation (SA), Ch 6. Ours: the feature id is kept as-is; `docs/spec.md`'s router description
states the SA mapping explicitly.

Rationale: feature ids are stable identifiers consumed by specgen and `FEATURE_INVENTORY.md`; the
mismatch is documented rather than propagated through a rename.

### T-04 AXI role vocabulary: master/slave

Baseline: IHI 0022 issue H uses Manager/Subordinate; pre-H AXI4 and every class name already in
this tree use Master/Slave. Ours: master/slave, uniformly: `AxiMasterPort`/`AxiSlavePort`,
"Network Master Unit"/"Network Slave Unit", modports, and prose now agree (Round 2, gate D1).

Rationale: renaming to Manager/Subordinate would touch class names, specgen JSON, generated SV,
and every consumer for a vocabulary switch with no functional benefit. The prior mix (master
identifiers, manager prose) was the only wrong answer; Round 2 unified toward the identifiers
already load-bearing in code.

### T-05 `FEAT-ROUTER-WORMHOLE_ARBITRATION` names a SA hold

Textbook: in a fixed-VC-at-injection design (no VA stage), a per-output head-to-tail packet lock
is wormhole flow control's switch-allocation hold (Ch 5/6), not VC allocation. Ours: the feature
id is kept; `router::Router` holds one (input port, VC) pair per output from a packet's first
flit to its last.

Rationale: the name matches its upstream RTL counterpart (References in `docs/spec.md`);
`docs/spec.md`'s Virtual networks section states the SA-hold mapping, so the name is not read as
a VA claim.

## Response ordering and VC design

### T-10 Response ordering owned by the NMU

Baseline: reordering commonly sits at the receiving endpoint, or ordering is a per-VC guarantee
the fabric provides. Ours: `nmu::Rob` owns all same-ID response ordering; `nsu::MetaBuffer`
restores identity but performs no reordering. `rob_idx`/`rob_req` ride the wire as header fields
the fabric never reads.

Rationale: ordering toward the master is the master-side NI's job by construction: only the NMU
knows its own AR issue order, so only it can detect an out-of-order arrival. A second RoB on the
NSU would duplicate that state for no additional guarantee.

A related divergence: the upstream design gates the B-side RoB by mode; ours is unconditionally
on (a B slot is metadata only, so it is cheap), and only the R path carries `RobMode`. See
`docs/spec.md` Response ordering.

### T-11 RoBless mode: single-outstanding interlock

Baseline: an upstream RoB-less path can admit a second same-ID transaction while one is
outstanding whenever the destination has not changed, gated by a per-ID counter. Ours:
`RobMode::Disabled` refuses any push while `read_outstanding_[id]` is set, regardless of
destination.

Rationale: strictly more conservative, strictly less throughput, and simpler: one flag per ID
instead of a counter. The throughput cost against the counter-based scheme has never been
measured.

### T-12 Fixed VC id (deterministic VC assignment at injection)

Baseline: VC allocation happens per hop (VA stage), or by a turn-model rule. Ours: a
`rob_req = 0` AW/AR whose `(dst_id, id)` matches its last same-channel flit reuses that VC
(request side, `nmu::VcArbiter`); every B/R with the same `(dst_id, id)` hashes to
`vnet[(dst_id ^ id) % size]` (response side, `nsu::VcArbiter`, a pure function, zero state). A
blocked fixed VC refuses rather than rerouting.

Rationale: see `docs/spec.md` Virtual networks. Deterministic assignment at injection needs no
per-hop VA stage, and the response side needs no state at all.

### T-13 Same-destination bypass under multi-VC: risk identified, fix shipped

Baseline: recorded as an open risk in an earlier internal design note (2026-07-12): the
same-destination bypass assumes same-`(dst, id)` responses return in order, which holds under
deterministic direction-VC assignment but not under round-robin VC spread once a class pool
exceeds size 1. Ours: the fixed VC id (T-12) closes the gap by fixing a bypass streak to one VC
per network instead of spreading it round-robin.

Rationale: risk identified, then designed (a VC-safe `(dst, id)` binding, reviewed), then
shipped (Round 2). The earlier "measure before building" gate no longer applies: the mechanism
is in both VC arbiters today.

### T-14 RoB slot pool as deadlock guarantee

Baseline/ours: unchanged. "No free slot, no request": `rob_req = 1` traffic never enters the
network without a reserved slot.

Rationale: a response with nowhere to go stalls in the NoC and blocks every flit behind it; the
admission gate is the fabric's deadlock guarantee, not a removable coupling. See `docs/spec.md`
Response ordering > Invariants.

### T-15 SAM miss policy: assert, not DECERR

Baseline: an AXI interconnect returns DECERR on an address-decode miss. Ours:
`addr_trans::SamTable::translate()` fails via `assert` on a lookup miss; NDEBUG hardening is open
work.

Rationale: model policy, not silicon-faithful behavior: a config or stimulus bug should stop the
sim loudly rather than exercise a decode-miss path nothing in the testbench drives. See
`docs/spec.md` Known limitations.

### T-16 Deferred flit fields

Ours: `NOC_QOS_WIDTH`, `ROUTE_PAR_WIDTH`, `FLIT_ECC_WIDTH` are all 0, reserved constants with no
corresponding field in the flit, not a zero-filled payload. No ECC CSR counters exist in the tree;
the register file that would have held them was removed.

Rationale: QoS-driven VC mapping, route parity, and flit ECC are unbuilt, deferred to a future
round. See `docs/spec.md` Known limitations.

### T-17 `max_unique_ids`: storage-shape knob, not throughput knob

Ours: `nsu::remap_downstream_id(upstream_id, max_unique_ids)` chooses the downstream AXI ID the
slave sees. `1` collapses every request to the same ID (slave responds in issue order, the
metadata store degenerates to a FIFO); `AXI_ID_SPACE` (256) passes `upstream_id` through unchanged
(store must be ID-addressable). `MetaBuffer` keeps a 256-bucket array under both settings.

Rationale: sizes storage shape, not RoB depth or throughput. Response ordering no longer depends
on it: the response-path fixed VC id keys on `(dst_id ^ id)`, so same-ID streams from different
sources land on distinct keys regardless of the remap. See `docs/spec.md` NSU meta buffer.

### T-18 NSU `VcArbiter` `use_pools_` asymmetry (CLOSED)

Baseline: the NSU `VcArbiter` carried a `use_pools_` scalar-VC branch the NMU had already
collapsed into a size-1 pool (Round 1 finding L1-007). Resolved (Round 2, D6a): the NSU now
mirrors the NMU's single `num_vc_ == 1` fast path; no `use_pools_` field remains in either file.

### T-19 Unswept sizing parameters

Ours: `max_txns_per_id` defaults to 32 `[TBD]`, never depth-swept. `r_rob_depth` defaults to 32
(`NMU_ROB_R_DEPTH`) but is expressible up to 256, the full `rob_idx` address space, via
`R_ROB_DEPTH`; also unswept.

Rationale: see `docs/spec.md` Known limitations.

### T-20 Turn-model VC assignment unported

Baseline: a per-hop turn-model VC assignment rule (a deprecated upstream component; References in
`docs/spec.md`). Ours: not ported. The static `(dst, id)` fixed VC id (T-12) supersedes
the ordering guarantee a turn-model rule would otherwise serve.

Rationale: porting it would add a mechanism whose ordering job the fixed VC id already covers;
judged low priority.

### T-21 In-fabric perf counters kept

Convention: DV-side bus monitors only. Ours: `PerfCollector` keeps in-fabric NoC counters
(per-router FIFO occupancy, per-link flit count and credit-deficit stall cycles, dumped to
`perf.json`) alongside the DV `axi_bw_monitor`. AXI-side per-transaction profile/trace machinery
was designed but never wired, and was dropped rather than shipped half-built.

Rationale: no industry in-fabric NoC counter substitute exists to reuse in its place; the NoC
counters and the DV bandwidth monitor together are the as-built performance truth. See
`docs/spec.md` Performance counters.

### T-22 Constrained-random axis retired (2026-07-13)

Baseline: a constrained-random axis with a data-integrity checker runs alongside directed tests.
Ours: retired. Only directed `axi_file_master` + scoreboard traffic runs; no sound checker for
random traffic exists yet, and Verilator's constraint solver needs a `fork()`-based external
solver pipe Windows lacks.

Rationale: see `docs/spec.md` Known limitations and Verification environment.

### T-23 DV-IP hand-edit not isolated

Convention: vendored source stays verbatim, or a local patch is isolated as an overlay/wrapper.
Ours: `axi_bw_monitor.sv` carries one in-tree 2-line `$display` edit (latency-N emit, consumed by
`sim/tools/emit_result_csv.py`), not isolated as a patch. Resolved (D8): `sim/dv/README.md`'s
provenance table now carries a `modified` column flagging the exact file and edit.

Rationale: cheapest honest disclosure: reverting loses a csv column a consumer depends on;
wrapping pristine IP for a 2-line diff was judged not worth the indirection.

## Packaging

### T-30 Deliberate-absent packaging ceremony

Convention: public IP repos carry badges/citation files, a
CHANGELOG and version scheme, `CONTRIBUTING.md`, `.editorconfig`, and per-file SPDX headers. Ours:
none of the five exist. README's Contributing section replaces a standalone `CONTRIBUTING.md`;
`LICENSE` is a single proprietary notice with no third-party section.

Rationale: an internal single-team repo, read by engineers and reviewers, not a public release
soliciting drive-by contributors. The ceremony these files exist for (citation for reuse,
contributor onboarding, cross-editor formatting, per-file license provenance) has no reader
here. Per-file SPDX headers would restate what `LICENSE` already states once for the whole tree.
