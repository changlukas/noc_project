# Round 3: fresh doc set + packaging + release

Date: 2026-07-14
Status: approved design (user gate passed), pending Codex review + implementation plan
Parent: `2026-07-14-commercialization-textbook-alignment-design.md` (round structure, amendments)
Sources: `docs/superpowers/audit/2026-07-14-salvage-inventory.md` (content map),
`2026-07-14-tradeoff-list.md` (T-01..T-30 skeleton), `2026-07-14-ledger.md` (decisions),
current tree at `refactor/commercialization-round2` tip.

## Goal

Replace the accumulated doc set with four fresh documents whose every claim matches the current
tree, with the complete build→test→sim path documented and PROVEN by execution. Execute the
deletion scope and packaging ride-alongs. Output: manager-reviewable internal release.

## Hard requirements (user, 2026-07-14)

1. Doc content must exactly match current code state — no aspirational, retired, or unbuilt
   content (probe AXI/Trace and pulp-vip CR machinery are UNBUILT/retired: document as-built only).
2. The full build→sim operational path must be correct and complete.
3. Style: the current README's shape — short sections, code blocks, tables, terse prose. English.

## Doc set and content boundaries

### README.md (rewrite; sole carrier of the operational path)

| section | content |
|---|---|
| header | one-paragraph positioning: AXI4 NoC network-interface behavioural c_model + Verilator co-sim; IHI 0022 conformity intent |
| Status | readiness statement (internal release); no perishable numbers — point at `make test` |
| Architecture | ASCII datapath + where-code-lives table matching the real tree (`src/c_model`, `src/sv`, `src/dpi`, `sim/`, `specgen/`, `docs/`) |
| Prerequisites | verified toolchain (CMake floor, Verilator 5.048 WSL / 5.036 legacy-Win note, GCC, python3+PyYAML) + platform matrix: Linux/WSL = full flow; Windows = ctest only (known gtest `\|`-regex death-test limitation); VCS = build-only, never run on real install |
| Build | `make build`; offline via `DEPS_SRC` + `FETCHCONTENT_FULLY_DISCONNECTED`; `local.mk` overrides |
| Test | `make test` (ctest), `make specgen_pytest`, `codegen.py --check` drift gate |
| Simulate | `make sim TB=<topo> PATTERN=<pattern> [SEED=]`; available topologies + patterns; output location `sim/<simulator>/output/<run-tag>/run.log`; how to read DIRECTED PASS / monitor lines / HWM lines |
| Regenerate | specgen codegen commands per domain; tb_top/fabric auto-regen at build |
| Documentation | links to the three docs/ files + specgen guide |
| Contributing | current rules (make test clean, clang-format, commit format, no --no-verify) |
| License | aligned with root LICENSE (proprietary internal); no third-party section (moved to verification-environment.md per D5 revision) |

### docs/spec.md (new; single file)

Sections (salvage-inventory rows grouped): Overview + verification intent + conformity scope
(covered IHI sections, exclusions with reasons) · architecture layering (specgen→axi→ni→nmu/nsu→
router→wrap→sim dependency direction; NMU/NSU role asymmetry) · timing model (registered DPI
tick, 1-cycle/hop, adapters) · SAM address translation (range lookup, rebase, 4KB granularity)
· NMU RoB (Disabled/Enabled modes, idle-ID + same-destination bypass, high-water lzc allocator,
invariants, slot pool = deadlock guarantee, param mapping) · NSU (meta buffer, id remap rationale)
· virtual networks + fixed VC id + wormhole lock · DPI ABI (integer handle, wrap invariant)
· parameters (specgen single-source table) · traffic patterns (booksim provenance, destination
rules) · perf counters AS-BUILT only (NoC-side counters + DV bw monitor; dropped AXI
Profile/Trace explicitly out) · known limitations (from backlog: SAM NDEBUG asserts, unswept
sizing params, no CRV/coverage/SVA, no standing regression harness, VCS build-only, perf DPI
unwired in traffic mode) · references (floo_* file map, IHI clauses, booksim).

### docs/trade-off.md (new)

T-01..T-30 from the skeleton with Rationale filled. Updates: T-04 wording per D1 (master/slave);
T-12/T-13 vocabulary per Round-2 renames (fixed VC id, same-destination bypass); T-18 marked
CLOSED (Round-2 D6a collapsed the NSU scalar branch); T-22/T-19 cross-ref known-limitations.

### docs/verification-environment.md (rewrite)

Testbench architecture (generated tb_top + noc_fabric, user_node_endpoint, per-node layout) ·
VIP set (axi_file_master directed two-phase, axi_rand_slave MAPPED tile memory, axi_scoreboard
manager-face integrity, axi_bw_monitor) · DV IP provenance section (cocotbext-axi MIT ports in
src/c_model/include/axi/; vendored pulp Solderpad packages under sim/dv/ with pointer to
sim/dv/README.md details) · traffic pattern semantics · checkers + non-vacuous-pass principle
(txn_cnt gate + scoreboard) · topology YAML → generator → tb workflow · seed management.

## Correctness process (how requirement 1-2 are enforced)

- Writer subagents verify every claim against the tree before writing; docs cite signal/param/
  make-target NAMES, never file line numbers (they rot).
- Dedicated doc-vs-tree reviewer per document: re-verify each claim independently.
- **Doc-driven dry run**: a fresh subagent, allowed to read ONLY README.md, executes the
  documented path end-to-end on a clean native copy (build → test → sim to DIRECTED PASS).
  Any step that fails or needs undocumented knowledge = doc bug; fix the doc, not the code.
- Final Codex + fresh-Claude cross-review of the whole doc set.

## Deletion scope (executed after the fresh docs land)

Delete: `docs/architecture.md`, `docs/development.md`, `docs/cosim-log.md`,
`docs/nmu-rob-microarchitecture.md`, `docs/performance-probe.md`, `docs/pg037_axi_perf_mon.md`,
`docs/2026-07-02-pulp-axi-vip-node-design.md`, `docs/internal/`, `docs/issue/`, `docs/slides/`,
`docs/superpowers/` (including this spec and the Round-1 audit artifacts — salvage completed
first). Keep: `docs/image/` (spec ground truth), `docs/backlog.md` (switched to gitignored).

## Ride-alongs

- Delete `src/c_model/include/axi/ATTRIBUTION.md` (content not migrated; provenance paragraph
  lives in verification-environment.md per D5 revision).
- Delete the LICENSE third-party section (dead path; L4-002 revised).
- `sim/dv/README.md`: add modified-flag to the provenance table row for `axi_bw_monitor.sv` (D8).
- CLAUDE.md: fix stale claims (L3-001 list: router-class claim, xy_route, MasterWrap/SlaveWrap
  + cmodel_master/slave_create, "Config: YAML c_model/config", ChannelModel as co-sim datapath,
  `c_model/...` path prefixes, `docs/_archive` pointer) and align the doc pointers to the new set.
- .gitignore: add `docs/backlog.md`, `cross-review/`, `.superpowers/`; remove the stale
  `/docs/image/*.jpg` ignore line that contradicts the 6 tracked jpgs (L4-011).
- `specgen/README.md`: add a short boundary declaration (L4-014: what specgen owns, its regen
  entry points, its test gate) — one screen, matching the main README style.
- Release checklist content (known limitations, verification summary, platform matrix,
  quickstart) lands INSIDE README/spec.md sections above, not as a separate file.

## Style charter

Current README shape. English. Tables over prose. No semicolons/em-dash mannerisms, no filler.
Only what exists. No line-number citations. No external vendor/IP names outside the DV-IP
provenance section (legal exception per D5).

## Gates

- Per-doc: doc-vs-tree reviewer clean.
- README: doc-driven dry run passes end-to-end.
- Repo: ctest + specgen pytest + codegen --check still green after deletions (CMake/pytest must
  not reference deleted docs).
- Final: cross-review (Codex + Claude) on the whole doc set; user review.

## Out of scope

Code changes of any kind (rename rounds are closed); backlog open items not listed above;
push/merge decisions.
