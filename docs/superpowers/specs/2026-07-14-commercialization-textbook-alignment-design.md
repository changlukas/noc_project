# Commercialization round: textbook alignment + de-AI + release

Date: 2026-07-14
Status: approved design, pending implementation plan

## Goal

Finish the project as a reviewable professional IP for an internal (manager-facing) release.
Document-only round: no design changes. Every design divergence from the textbook is recorded as a
trade-off entry, not reworked.

## Authorities

| Source | Role |
|--------|------|
| AMBA AXI4 spec (ARM IHI 0022, public protocol spec) | AXI interface-layer terminology |
| `~/.claude/skills/on-chip-networks/glossary.md` + `chapters/ch01-09` | NoC-layer terminology and design baseline (Enright Jerger / Krishna / Peh, On-Chip Networks 2nd Ed) |
| `~/.claude/skills/on-chip-networks/noc-ordering/` | ordering / RoB terminology |
| Real IP repos (FlooNoC and peers) | packaging baseline (README, doc set, layout presentation) |

Precedence on conflict: AXI spec terms win at the AXI interface layer; textbook terms win at the
NoC layer; FlooNoC and peer repos serve as packaging/implementation reference only and are not
cited in manager-facing docs.

## Deliverable

Clean repo + a fresh, from-scratch doc set. Existing docs are deleted, not rewritten (rewriting
137 accumulated specs/plans has a lower quality ceiling than writing three real docs).

Final doc set:

| Doc | Content |
|-----|---------|
| `README.md` | rewritten, every claim aligned with the current tree |
| `docs/spec.md` | full design spec, written fresh (sources: salvage inventory, code, specgen json) |
| `docs/trade-off.md` | textbook design vs ours, with rationale |
| `docs/verification-environment.md` | test environment: co-sim architecture, testbench, scoreboard, traffic patterns, DV IP provenance (cocotbext-axi ports, sim/dv pulp packages) |
| `docs/backlog.md` | kept as working doc, switched to git-ignored (local only) |

Deletion scope (executed in Round 3): `docs/superpowers/`, `docs/internal/`, and top-level docs
whose salvageable content moved into spec.md. Kept: `docs/image/` (spec ground truth),
`specgen/generated/json/` (generator input source). This round's own specs/plans/audit artifacts
are deleted at release too.

Release = zero AI traces (code + doc), packaging aligned with industry IP-repo convention,
docs ready for a manager-level review. Internal only; no tag/publish requirement.

## Method: one findings ledger, three dispositions

Every audit finding lands in a single ledger:

| Field | Content |
|-------|---------|
| id | stable finding id (lane prefix + number) |
| lane | audit lane 1-4 |
| subsystem | nmu / nsu / router / wrap / sv-tb / specgen / docs / packaging / ... |
| ours | our term / current state |
| standard | authority term or convention, with citation (glossary entry, chapter, or spec section) |
| location | file:line or doc section |
| severity | high (misleading term / heavy AI trace) / low (cosmetic) |
| disposition | rename / keep+trade-off / slop |
| rationale | why |

Dispositions:

- **rename** -- align identifier or wording to the standard term (this is also the de-AI action)
- **keep+trade-off** -- deliberate divergence; rationale goes into the trade-off doc.
  All design-level divergences (NMU-side RoB, fixed-VC wormhole, static VC assignment, ...) are
  fixed to this disposition (document-only round).
- **slop** -- AI-flavored filler with no content; delete or rewrite

Known seeds: `RZ1` -> static VC assignment; `pinned` -> binding.

## Rounds

### Round 1 -- audit to approved ledger (this round)

Preflight: build a current architecture inventory (what actually exists in the tree) before any
lane runs; baseline docs that lag the implementation become findings themselves. Known instance:
CLAUDE.md still claims "no router class in c_model" while `src/c_model/include/router/` ships six
headers.

Four subagent lanes, loop-until-dry (stop only after two consecutive passes surface nothing new).
Every finding must cite an authority (glossary entry, chapter, or spec section); "feels
non-standard" is not admissible.

| Lane | Surface |
|------|---------|
| 1 | c_model class / function / variable naming vs authorities |
| 2 | SV / testbench / generators (specgen, gen_tb_top), build (CMake / Make / filelist), config + scenario YAML, scripts and tools naming |
| 3 | docs: salvage inventory -- which content earns a place in the fresh spec.md, which dies with the deletion (no per-doc de-AI rewrite; deleted docs need no fixing) |
| 4 | packaging: README, doc set, layout presentation vs real IP repos |

Ledger passes Codex review (false positives, missed surfaces) before the user gate.
No code changes this round. Output: approved ledger + trade-off list skeleton.

Small code debts already on the backlog (e.g. NSU `use_pools_` asymmetry) enter the ledger when
they read as unprofessional traces; the user decides at the gate whether they are in scope.

### Round 2 -- code execution

Renames batched per subsystem. Gate per touched surface, per batch, before commit:

| Surface | Gate |
|---------|------|
| C++ (c_model) | compile + ctest green |
| specgen / generators | regenerate + diff check |
| SV / tb / wrap | WSL `make sim` directed run |

Approved small code debts ride along. Full-diff Codex review at the end.
Output: green tree, identifiers final.

### Round 3 -- fresh doc set + packaging + release

Write `docs/spec.md` from scratch (salvage inventory + code + specgen json as sources), write out
`docs/trade-off.md` and `docs/verification-environment.md`, rewrite `README.md`, execute the
deletion scope, gitignore `docs/backlog.md`. Attribution simplification (amendment below) executes
here: delete `src/c_model/include/axi/ATTRIBUTION.md`, delete the LICENSE third-party section,
add the sim/dv README modified-flag (D8).
Minimum release content: readiness statement, known limitations, verification summary, quickstart.
Further checklist items enter only if the lane-4 packaging audit shows real IP repos carry them.
Final cross-review (Claude + Codex). Output: manager-reviewable version.

Rounds 2/3 merge into one if the approved rename count is small; decided after Round 1.

Ordering constraint: code before docs -- docs cite code identifiers, so renames must be final
before the doc rewrite.

## Amendments (2026-07-14, post-gate brainstorm)

- `pinned` replacement picked: **fixed VC id** (`bool fixed_vc`, verb "fixes ... to one VC",
  test names `*FixedVc*`). Consistent with the design's existing fixed-VC vocabulary (L2-003:
  vc_id stamped at injection, no per-hop VA stage).
- Attribution minimalized (supersedes D5): no formal attribution files, no per-file license tags.
  `src/c_model/include/axi/ATTRIBUTION.md` deleted, content not migrated. LICENSE third-party
  section deleted. DV IP provenance = one short paragraph in `docs/verification-environment.md`.
  Informal "Ported from floo_*.sv" source comments stay (traceability). L4-003/L4-005 cancelled.
- Final doc set is 4 docs (verification-environment.md added).
- Round split: all attribution/LICENSE/sim-dv-README doc actions move to Round 3; Round 2 is
  pure code renames (D1, D2/D3, D4, D6).

## Out of scope

- Any design change
- Other open backlog items (SAM throw, VCS flow, coverage/CRV, ...) unless the ledger classifies
  one as an AI trace and the user approves it at the gate
