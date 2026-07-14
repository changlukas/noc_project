# NoC backlog

Forward-only: open work items and known gaps only. Completed work lives in git history, not here.
Read at session start; each round strikes what it closes and adds what it surfaces.

## NEXT ROUND (headline) -- commercialization Round 3: fresh doc set, release

Round 1 (audit) DONE 2026-07-14: user-approved ledger + gate decisions D1-D8 in
`docs/superpowers/audit/2026-07-14-ledger.md`; trade-off skeleton + salvage inventory alongside.
Spec: `docs/superpowers/specs/2026-07-14-commercialization-textbook-alignment-design.md`.

Round 2 (code) DONE 2026-07-14 on branch `refactor/commercialization-round2` (not pushed):
D1 master/slave prose, D2/D3 RZ1 removal + fixed-VC-id wording, D4 input register +
upstream/downstream modports, D6 NSU use_pools_ collapse + noc_types banner regen; ride-along
fix of pre-existing stale test path (test_feature_inventory.py, redded specgen pytest since
eca0c93). All gates green (ctest 431/431, specgen pytest 160/0, codegen --check, WSL directed
sim); Codex + fresh-Claude full-diff reviews applied.

- **Round 3 (docs)**: delete old doc set (docs/superpowers, docs/internal, top-level dying docs,
  issue/, slides/); write fresh README + docs/spec.md + docs/trade-off.md +
  docs/verification-environment.md (test env: co-sim architecture, testbench, scoreboard,
  patterns, DV IP provenance) from the salvage inventory (as-built only — probe AXI/Trace and
  pulp-vip CR machinery are UNBUILT/retired); delete src/c_model/include/axi/ATTRIBUTION.md +
  LICENSE third-party section (D5 revised); sim/dv README modified-flag (D8); fix CLAUDE.md stale
  claims; gitignore backlog.md; release checklist (known limitations, verification summary,
  platform matrix, quickstart).
  Queued additions from Round-2 reviews:
  - sweep code comments for `docs/superpowers/...` / `microarch §` refs before doc deletion
    (References blocks in nmu/vc_arbiter.hpp, nsu/vc_arbiter.hpp, router.hpp, wormhole_arbiter.hpp,
    nmu.hpp, nsu.hpp, nmu_wrap.hpp, tests/common/{scenario,test_logger}.hpp).
  - gen_inventory.py content strings still write `c_model/include/...` paths into
    src/c_model/FEATURE_INVENTORY.md (real tree is `src/c_model/include/...`); regen after fixing.

## Open -- defensive / correctness (small)

- **SAM `translate()` miss under `NDEBUG`.** A lookup miss asserts (fail-loud) in a debug build but
  null-derefs in a release/`NDEBUG` build (`nmu/addr_trans.hpp:49-52`). Make it a runtime throw so it
  fails cleanly in both.
- **SAM `+sam_config` error string.** A topology YAML lacking the `address_map` block hits a bare
  assert (`nmu/sam_yaml.hpp:15`); give a descriptive runtime error that survives `NDEBUG`.
- **`gen_tb_top.emit_tb_top(requested_name="")`** silently yields `rob_enabled=False` with no
  validation (`gen_tb_top.py:397-402`); reject an empty name loudly.
- **specgen pytest dirties the tree.** `specgen/tests/test_codegen.py` (cpp+sv twins) invokes
  codegen with `--out` pointing at the real `specgen/generated/` dirs, so every pytest gate run
  rewrites committed files (banner timestamps). Point those tests at a tmp dir.

## Open -- NMU RoB / NSU sizing & structure

- **`max_txns_per_id` default (32) is `[TBD]`** -- a per-AXI-id outstanding limit
  (`nmu/rob.hpp:249,310`); the value needs a depth sweep to be considered rather than a placeholder.
- **`r_rob_depth = 256`** (8 KiB, the paper's design point) is expressible (`R_ROB_DEPTH=256`,
  `sim/verilator/Makefile`) but unswept.
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
