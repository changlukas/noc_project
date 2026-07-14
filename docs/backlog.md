# NoC backlog

Forward-only: open work items and known gaps only. Completed work lives in git history, not here.
Read at session start; each round strikes what it closes and adds what it surfaces.

## NEXT ROUND (headline) -- commercialization Rounds 2+3: execute ledger, fresh doc set, release

Round 1 (audit) DONE 2026-07-14: user-approved ledger + gate decisions D1-D8 in
`docs/superpowers/audit/2026-07-14-ledger.md`; trade-off skeleton + salvage inventory alongside.
Spec: `docs/superpowers/specs/2026-07-14-commercialization-textbook-alignment-design.md`.

- **Round 2 (code)**: prose master/slave unification (D1), RZ1 tag removal + `pinned` rename to
  deterministic-VC-allocation wording (D2/D3), `landing_`→input register + `mosi/miso`→
  upstream/downstream (D4), FlooNoC ATTRIBUTION + per-file tags (D5), NSU `use_pools_` branch
  deletion + noc_types_pkg banner regen (D6), sim/dv README modified-flag (D8), LICENSE dead-path
  fix (L4-002), root third-party notice fold-in (L4-003). Gates: ctest / regen diff / WSL make sim
  directed per surface.
- **Round 3 (docs)**: delete old doc set (docs/superpowers, docs/internal, top-level dying docs,
  issue/, slides/); write fresh README + docs/spec.md + docs/trade-off.md from the salvage
  inventory (as-built only — probe AXI/Trace and pulp-vip CR machinery are UNBUILT/retired);
  fix CLAUDE.md stale claims; gitignore backlog.md; release checklist (known limitations,
  verification summary, platform matrix, quickstart).

## Open -- defensive / correctness (small)

- **SAM `translate()` miss under `NDEBUG`.** A lookup miss asserts (fail-loud) in a debug build but
  null-derefs in a release/`NDEBUG` build (`nmu/addr_trans.hpp:49-52`). Make it a runtime throw so it
  fails cleanly in both.
- **SAM `+sam_config` error string.** A topology YAML lacking the `address_map` block hits a bare
  assert (`nmu/sam_yaml.hpp:15`); give a descriptive runtime error that survives `NDEBUG`.
- **`gen_tb_top.emit_tb_top(requested_name="")`** silently yields `rob_enabled=False` with no
  validation (`gen_tb_top.py:397-402`); reject an empty name loudly.

## Open -- NMU RoB / NSU sizing & structure

- **`max_txns_per_id` default (32) is `[TBD]`** -- a per-AXI-id outstanding limit
  (`nmu/rob.hpp:249,310`); the value needs a depth sweep to be considered rather than a placeholder.
- **`r_rob_depth = 256`** (8 KiB, the paper's design point) is expressible (`R_ROB_DEPTH=256`,
  `sim/verilator/Makefile`) but unswept.
- **NSU `VcArbiter` `use_pools_` asymmetry.** The NMU collapsed the scalar-VC case into a size-1 pool;
  the NSU still carries the `use_pools_` branch (`nsu/vc_arbiter.hpp:58,97,107,152`). Mirror it.
- **DV-IP no-edit debt: `axi_bw_monitor.sv`.** Hand-edited in the vendored floonoc-test tree
  (commit `b7b5db3` + a follow-up), not isolated as a patch/wrapper. Either upstream the change or wrap
  pristine IP. ([[feedback_trust_upstream_defaults]])

## Open -- verification methodology (large)

- **No standing co-sim regression harness.** Binding + fabric coverage is verified only by manual
  `make sim` runs; the old `sim/regress/` runner was retired. Build a runner (or a curated subset) if a
  standing automated gate is wanted.
- **perf DPI not wired for traffic/injection mode** (`perf.json` = 0 bytes there), so the bw_monitor
  vs perf.json cross-check is unrun. Wire it, or validate bw_monitor against a hand-computed
  single-stream case.
- **Coverage / CRV / wire-side SVA.** No covergroup, constrained-random, or wire-level assertion
  framework exists in the tree. Capability gap.
- **Slave-latency testbench axis.** No read/write-latency sweep axis on the subordinate model.

## Open -- design rounds (large)

- **NoC-layer QoS.** `NOC_QOS_WIDTH = 0` (`ni_flit_pkg.sv`), zero-fill. QoS-driven VC mapping unbuilt.
- **VC-allocation turn-model port.** `floo_vc_assignment.sv` per-hop turn-model VC assignment unported.
  Largely superseded by the shipped RZ1 (dst,id) binding; low priority.

## Open -- infra

- **VCS flow is build-only and dry-run.** After the run-tb-top removal, `sim/vcs/` has only a build
  target (its sole run target was the random smoke). A VCS directed run mirroring verilator
  `run-directed` is needed if the VCS/workstation path is used, and it has never run on a real VCS
  install.
