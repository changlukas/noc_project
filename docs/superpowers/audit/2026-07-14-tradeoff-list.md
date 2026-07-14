# Trade-off list skeleton (Round 1 output; prose filled in Round 3 → docs/trade-off.md)

One section per deliberate divergence: textbook (or convention) baseline vs our design, with
rationale. Sources: ledger keep+trade-off rows + salvage-inventory rows targeting trade-off.md.

## Naming trade-offs (ledger keep+trade-off)

### T-01 `VcArbiter` (L1-004)
Textbook baseline: VC allocation (VA) and N→1 arbitration are distinct router stages (Ch 6).
Our design: one NI-side block does both — deterministic VC selection + credit-gated round-robin drain; name follows the upstream RTL counterpart.
Rationale: [Round 3]

### T-02 `MetaBuffer` (L1-005)
Textbook baseline: TSHR — Transaction Status Holding Registers at the NI (Ch 2).
Our design: `MetaBuffer` name kept as a 1:1 port of the upstream RTL meta buffer for parity traceability; also holds response-reassembly metadata beyond TSHR.
Rationale: [Round 3]

### T-03 `FEAT-ROUTER-VC_ARBITRATION` (L2-002)
Textbook baseline: the described mechanism (per-output flit-level round-robin across VCs, credit-gated) is switch allocation (SA), Ch 6.
Our design: feature-id kept for stability; spec text maps it to SA explicitly.
Rationale: [Round 3]

### T-04 AXI role vocabulary (L1-003/L2-001, direction per gate D1)
Baseline: IHI 0022 issue H Manager/Subordinate (specgen's cited edition) vs pre-H Master/Slave.
Our design: [per D1 decision]; load-bearing identifier scope [per D1].
Rationale: [Round 3]

### T-05 `FEAT-ROUTER-WORMHOLE_ARBITRATION` (L2-003)
Textbook baseline: in a fixed-VC wormhole design there is no VA stage; the per-(output,vc)
head→tail lock is wormhole flow control's switch-allocation hold (Ch 5/6).
Our design: fixed-VC at injection (static VC assignment), per-output wormhole lock in the router;
name matches the mechanism and its upstream RTL counterpart.
Rationale: [Round 3]

## Design trade-offs (from salvage inventory)

### T-10 Response ordering owned by the manager-side NI (NMU RoB; NSU has no RoB)
Textbook baseline: reordering handled at the receiving endpoint or via per-VC ordering guarantees.
Our design: same-id response ordering owned entirely by the NMU (rob_idx header field, per-ID order list); NSU-side RoB absent by design.
Rationale: [Round 3]

### T-11 RoBless (Disabled) mode: one-outstanding-per-ID interlock
Baseline: upstream per-ID counter admits multiple outstanding same-ID txns when safe.
Our design: strictly more conservative single-outstanding interlock; unconditional ordering, throughput cost never measured.
Rationale: [Round 3]

### T-12 Static VC assignment on the response path (mechanism formerly tagged RZ1)
Baseline: per-hop VC allocation (VA) or turn-model VC assignment.
Our design: deterministic pure-function (dst,id)→VC binding at injection; zero state; refuses rather than spills on no-credit.
Rationale: [Round 3]

### T-13 Clause-2 bypass unsafe under round-robin VC spread
Baseline: upstream deterministic direction-VC mapping makes the return-path bypass safe.
Our design: round-robin VC spread breaks the precondition at pool>1; VC-safe (dst,id) design DESIGNED, gated on Stage-0 same-dest streaming measurement.
Rationale: [Round 3]

### T-14 RoB slot pool as deadlock guarantee
Baseline/our design: "no free slot, no request" — admission control at the NI is the fabric's deadlock guarantee, not removable coupling.
Rationale: [Round 3]

### T-15 SAM miss policy: assert, not DECERR
Baseline: AXI interconnect returns DECERR on address miss.
Our design: model fails loud (assert; NDEBUG hardening pending per backlog) — model-only policy, not silicon-faithful.
Rationale: [Round 3]

### T-16 Deferred flit fields: noc_qos / route_par / flit_ecc zero-filled
Our design: fields carried in the wire format, semantics unbuilt (QoS-driven VC mapping, parity, ECC check logic); ECC CSR counters present without check logic.
Rationale: [Round 3]

### T-17 `max_unique_ids`: storage-shape knob, not throughput knob
What it is: out-of-order-return storage sizing (FIFO vs id-queue area); unrelated to RoB; not modelled in C++.
Rationale: [Round 3]

### T-18 NSU `VcArbiter` `use_pools_` asymmetry (pending D6a)
Our design: NMU collapsed scalar-VC into a size-1 pool; NSU still carries the branch.
Rationale: [Round 3 — or closed by Round-2 mirror if D6a approved]

### T-19 Unswept sizing parameters
`max_txns_per_id` = 32 [TBD]; `r_rob_depth` = 256 (8 KiB design point) expressible but unswept.
Rationale: [Round 3 — known-limitations cross-ref]

### T-20 Turn-model VC assignment unported
Baseline: per-hop turn-model VC assignment.
Our design: superseded by static (dst,id) VC binding; port judged low priority.
Rationale: [Round 3]

### T-21 In-fabric perf counters kept (PMU + link monitor)
Convention: DV-side bus monitors only.
Our design: in-fabric NoC counters kept — no industry in-fabric NoC counter substitute exists; AXI-side Profile/Trace machinery dropped (never wired), NoC-side counters + DV bw monitor are the as-built truth.
Rationale: [Round 3]

### T-22 Constrained-random axis retired (2026-07-13)
Our design: directed `axi_file_master` + scoreboard only; no sound data-integrity checker for random traffic yet.
Rationale: [Round 3 — known-limitations cross-ref]

### T-23 DV-IP hand-edit (`axi_bw_monitor.sv`) not isolated (pairs L4-004)
Convention: vendored source verbatim or manifest-flagged patch.
Our design: one 2-line `$display` edit in-tree; disposition per gate.
Rationale: [Round 3]

## Packaging trade-offs (L4-015, deliberate-absent for an internal repo)

### T-30 No badges/citation, no CHANGELOG/version scheme, no CONTRIBUTING, no .editorconfig
Convention: public IP repos carry all four (FlooNoC, pulp axi, OpenTitan).
Our design: internal single-team repo, manager audience — public-discoverability and drive-by-contributor ceremony deliberately omitted; SPDX headers deferred until the license copy is reconciled.
Rationale: [Round 3]
