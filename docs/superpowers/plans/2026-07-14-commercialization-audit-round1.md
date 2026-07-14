# Commercialization Round 1: Audit-to-Ledger Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce a user-approved findings ledger covering every textbook-terminology divergence, AI trace, and packaging gap in the repo, plus a trade-off list skeleton. No code changes.

**Architecture:** Preflight inventory first (so lanes audit against reality, not stale docs), then four audit lanes run as subagent fan-out with loop-until-dry, findings consolidate into one ledger file, Codex reviews the ledger, then the user gate.

**Tech Stack:** Agent tool (fan-out subagents), on-chip-networks skill files as authority, Codex plugin for review. Markdown deliverables only.

## Global Constraints

- Document-only round: subagents and orchestrator MUST NOT edit any file outside `docs/superpowers/audit/`.
- Every finding must cite an authority: glossary entry, chapter, AXI spec concept, or (lane 4) a named real-repo convention. Un-cited findings are dropped at consolidation.
- Authority precedence: AMBA AXI4 spec terms > textbook NoC terms > FlooNoC/peer conventions (packaging reference only, never cited in manager-facing docs).
- Ledger schema (spec): `id | lane | subsystem | ours | standard | location | severity | disposition | rationale`; disposition ∈ rename / keep+trade-off / slop; severity ∈ high / low.
- Design-level divergences are always `keep+trade-off` (no redesign this round).
- Loop-until-dry: a lane is done only after two consecutive passes surface zero new findings.
- No commits until the user gate passes (Task 8).
- Authority files live at `C:\Users\USER\.claude\skills\on-chip-networks\` (glossary.md, cheatsheet.md, patterns.md, chapters/ch01-09, noc-ordering/).

---

### Task 1: Preflight architecture inventory

**Files:**
- Create: `docs/superpowers/audit/2026-07-14-architecture-inventory.md`

**Interfaces:**
- Produces: the inventory doc — the source of truth every lane audits against; a "stale baseline claims" list that seeds lane-3 findings.

- [ ] **Step 1: Dispatch one Explore subagent** with this prompt:

```
Build an architecture inventory of the repo at E:\05_NoC\noc_project. Report:
1. Directory map (top 2 levels of src/, sim/, docs/, specgen/, tools/ — skip build/, Python/).
2. c_model: every sub-namespace under src/c_model/include/ and the classes each header defines (name only, one line each).
3. SV side: list modules in sim/ (tb, wrap, generated) with one-line role each.
4. Generators: what specgen/ and gen_tb_top.py produce.
5. Doc set: every .md under docs/ (excluding docs/superpowers/) with one-line topic.
Then diff this reality against the claims in CLAUDE.md ("Project Overview" section) and docs/architecture.md. List every stale or contradicting claim as: | doc | claim | reality |.
Return raw markdown, no prose commentary.
```

- [ ] **Step 2: Verify the stale-claims diff includes the known instance** (CLAUDE.md "no router class in c_model" vs `src/c_model/include/router/` six headers). If missing, the subagent skimmed — re-dispatch.

- [ ] **Step 3: Write the result** to `docs/superpowers/audit/2026-07-14-architecture-inventory.md` with a final section `## Stale baseline claims (seed findings for lane 3)`.

### Task 2: Lane 1 — c_model naming audit (pass 1 fan-out)

**Files:**
- Create: `docs/superpowers/audit/2026-07-14-ledger.md` (section `## Lane 1: c_model naming`)

**Interfaces:**
- Consumes: architecture inventory (subsystem list).
- Produces: lane-1 finding rows in the ledger schema (id assigned later at Task 6).

- [ ] **Step 1: Dispatch three parallel subagents** (single message, three Agent calls), splitting c_model by subsystem: (a) `nmu/` + `axi/`, (b) `nsu/`, (c) `router/` + `wrap/` + `tests/common/`. Each gets this prompt (with its path list substituted):

```
You are auditing C++ naming in a NoC behavior model against standard NoC terminology. Read ALL authority files before scanning:
- C:\Users\USER\.claude\skills\on-chip-networks\glossary.md (primary)
- C:\Users\USER\.claude\skills\on-chip-networks\cheatsheet.md
- C:\Users\USER\.claude\skills\on-chip-networks\noc-ordering\concepts.md (ordering/RoB terms)
Precedence: AMBA AXI4 (ARM IHI 0022) terms win for AXI interface-layer names; textbook terms win for NoC-layer names.

Scan every .hpp/.cpp under: <PATHS> (repo root E:\05_NoC\noc_project\src\c_model).
Report every class, function, member, constant, or comment term that (a) diverges from the standard term for the same concept, or (b) reads as AI-invented (bespoke codename like "RZ1", non-industry metaphor like "pinned" for VC binding).

Output ONLY markdown table rows, schema:
| lane | subsystem | ours | standard (with citation) | location (file:line) | severity | disposition | rationale |
- lane = 1. severity: high = misleading term or heavy AI trace; low = cosmetic.
- disposition: rename / keep+trade-off / slop. Design-level divergence (a mechanism, not just a name) = keep+trade-off.
- Every row MUST cite a glossary entry, chapter number, or AXI spec concept in the "standard" column. If you cannot cite, do not report.
- Do NOT report: names with no standard equivalent (project/domain-specific), industry-standard abbreviations, gtest boilerplate.
- If you find zero findings, return the single word DRY.
```

- [ ] **Step 2: Spot-check each returned table** — every row has a citation; rows about design mechanisms carry `keep+trade-off`. Drop rows that fail; note dropped count.

- [ ] **Step 3: Create the ledger file** `docs/superpowers/audit/2026-07-14-ledger.md` and append the surviving rows under `## Lane 1: c_model naming`, plus the two seed rows (`RZ1` → static VC assignment; `pinned` → binding) if the subagents missed them.

### Task 3: Lane 2 — SV / tb / generators / build / config audit (pass 1 fan-out)

**Files:**
- Modify: `docs/superpowers/audit/2026-07-14-ledger.md` (add section `## Lane 2: SV-tb / generators / build / config`)

**Interfaces:**
- Produces: lane-2 finding rows, same schema, lane = 2.

- [ ] **Step 1: Dispatch two parallel subagents**: (a) SV surface — `sim/verilator/`, `sim/vcs/`, generated tb sources, wrap `.sv`, `ni_flit_pkg.sv`, filelist.f (skip vendored `sim/dv/` except our hand-edited `axi_bw_monitor.sv`); (b) generator/build/config surface — `specgen/`, `gen_tb_top.py`, `tools/`, CMakeLists, Makefiles, `build_config.mk`, config + scenario YAML names. Same prompt as Task 2 Step 1 with these substitutions: lane = 2; add to the scan targets "module names, port names, parameter/macro names, make target names, YAML key names, script names"; add rule "vendored third-party code under sim/dv/ is out of scope except axi_bw_monitor.sv (hand-edited)".

- [ ] **Step 2: Spot-check citations** as in Task 2 Step 2.

- [ ] **Step 3: Append surviving rows** to the ledger under `## Lane 2`.

### Task 4: Lane 3 — docs salvage inventory (pass 1 fan-out)

Existing docs are deleted in Round 3 and replaced by a fresh `docs/spec.md` + `docs/trade-off.md`
+ rewritten `README.md`. This lane does NOT fix docs; it decides what content earns a place in the
fresh doc set. Historical process docs (`docs/superpowers/`, `docs/internal/`) die wholesale and
are out of scope for salvage.

**Files:**
- Create: `docs/superpowers/audit/2026-07-14-salvage-inventory.md`
- Modify: `docs/superpowers/audit/2026-07-14-ledger.md` (add section `## Lane 3: docs`, stale-claim rows only)

**Interfaces:**
- Consumes: stale-claims list from Task 1 (seed rows).
- Produces: salvage inventory (feeds Round 3 spec.md writing); lane-3 stale-claim rows.

- [ ] **Step 1: Dispatch two parallel subagents** over the live docs, split by half:
  (a) `README.md`, `docs/architecture.md`, `docs/verification-environment.md`, `docs/development.md`, `docs/cosim-log.md`;
  (b) `docs/nmu-rob-microarchitecture.md`, `docs/performance-probe.md`, `docs/pg037_axi_perf_mon.md`, `docs/2026-07-02-pulp-axi-vip-node-design.md`, `docs/slides/`, `docs/issue/`. Prompt:

```
The repo at E:\05_NoC\noc_project replaces its doc set next round: everything is deleted except a
fresh full design spec (docs/spec.md), a trade-off doc, and a rewritten README. You are building
the salvage inventory. Read each file in: <FILES>. For each doc, per section, judge:
- salvage: content a fresh design spec or README needs and that is NOT derivable from code/specgen
  json alone (design rationale, protocol decisions, verified measurement results, known limitations)
- dies: process narrative, round logs, stale claims, anything the tree itself already expresses
Cross-check claims against the tree before marking salvage; a stale claim is never salvage — report
it separately.
Output two markdown tables, no prose:
1. Salvage: | doc | section | target (spec.md section / trade-off.md / README) | content one-liner |
2. Stale claims: | doc | claim | reality (file:line evidence) |
```

- [ ] **Step 2: Write the merged salvage tables** to `docs/superpowers/audit/2026-07-14-salvage-inventory.md`.

- [ ] **Step 3: Append stale-claim rows** (theirs + Task 1 seeds) to the ledger under `## Lane 3` (schema: subsystem = docs, disposition = slop, severity = high, rationale = "doc lags implementation").

### Task 5: Lane 4 — packaging audit (pass 1)

**Files:**
- Modify: `docs/superpowers/audit/2026-07-14-ledger.md` (add section `## Lane 4: packaging`)

**Interfaces:**
- Produces: lane-4 finding rows (gap-style: `ours` = current state, `standard` = the convention with the repo that carries it), lane = 4.

- [ ] **Step 1: Dispatch one subagent** with web access:

```
Survey how professional open-source silicon IP repos present themselves, using 2-3 concrete examples (FlooNoC https://github.com/pulp-platform/FlooNoC and one or two peers, e.g. pulp-platform axi or OpenTitan). For each convention observed (README structure: badges/overview/architecture-figure/quickstart/citation; doc set: getting-started, architecture spec, changelog; repo hygiene: LICENSE, CONTRIBUTING, versioning), compare against E:\05_NoC\noc_project (README.md, LICENSE, docs/ layout).
Output ONLY markdown table rows:
| lane | subsystem | ours (current state) | standard (convention + which repo carries it) | location | severity | disposition | rationale |
lane = 4, subsystem = packaging. disposition: rename (adjust existing) / keep+trade-off (deliberately absent, e.g. no CONTRIBUTING for an internal repo) / slop (present but unprofessional).
Also verify the spec's minimum release content is covered by some row: readiness statement, known limitations, verification summary, quickstart.
If a convention is already met, do not report it.
```

- [ ] **Step 2: Spot-check** — every row names the repo carrying the convention. Append under `## Lane 4`.

### Task 6: Loop-until-dry passes (lanes 1-4)

**Files:**
- Modify: `docs/superpowers/audit/2026-07-14-ledger.md`

**Interfaces:**
- Consumes: all pass-1 rows.
- Produces: ledger complete (all lanes dry twice).

- [ ] **Step 1: For each lane, dispatch one fresh whole-lane subagent** (all four in parallel): same prompt as that lane's pass 1, plus this suffix:

```
A prior pass already found the items below. Report ONLY findings not in this list (a new term, a new location of a listed term counts only if a different subsystem). If nothing new, return the single word DRY.
<paste that lane's current "ours | location" pairs>
```

- [ ] **Step 2: Append any new surviving rows.** Track per-lane consecutive-dry count: DRY → count+1; new findings → count resets to 0.

- [ ] **Step 3: Repeat Step 1-2 until every lane reaches two consecutive DRY passes.** Log per-pass new-finding counts at the top of the ledger file under `## Audit log`.

### Task 7: Consolidation + trade-off skeleton

**Files:**
- Modify: `docs/superpowers/audit/2026-07-14-ledger.md`
- Create: `docs/superpowers/audit/2026-07-14-tradeoff-list.md`

**Interfaces:**
- Produces: final ledger (ids assigned) + trade-off skeleton, the two artifacts of the user gate.

- [ ] **Step 1: Dedupe and normalize inline** (orchestrator, no subagent): merge duplicate `ours` terms across lanes into one row with multi-location; normalize severity; verify every design-mechanism row is `keep+trade-off`.

- [ ] **Step 2: Assign stable ids** `L<lane>-<3-digit>` in file order (e.g. `L1-001`).

- [ ] **Step 3: Write the trade-off skeleton**: one section per `keep+trade-off` row —

```markdown
## <id> <our mechanism/term>
Textbook baseline: <standard, citation>
Our design: <one line>
Rationale: [filled in Round 3]
```

- [ ] **Step 4: Sanity totals** — row count per lane and per disposition at the top of the ledger.

### Task 8: Codex review + user gate

**Files:**
- Modify: `docs/superpowers/audit/2026-07-14-ledger.md` (fixes from review)

**Interfaces:**
- Produces: Codex-reviewed ledger; user disposition decisions; go/no-go for Round 2.

- [ ] **Step 1: Dispatch Codex** (codex:codex-rescue): review the ledger for (a) false positives — cited standard term wrong or citation does not support the claim; (b) coverage — subsystems or doc files with implausibly few findings; (c) disposition sanity — renames that would collide with AXI-spec or generated-code names. Findings list with severity.

- [ ] **Step 2: Fix accepted review points inline**; note rejected ones with reason in the ledger's `## Audit log`.

- [ ] **Step 3: Present to user**: totals table, all high-severity rows inline, the ledger + trade-off files for review. User approves/edits dispositions per row or in batch.

- [ ] **Step 4: After approval, commit** spec + plan + audit artifacts:

```bash
git add docs/superpowers/specs/2026-07-14-commercialization-textbook-alignment-design.md docs/superpowers/plans/2026-07-14-commercialization-audit-round1.md docs/superpowers/audit/
git commit -m "docs(audit): round-1 textbook-alignment ledger + trade-off skeleton"
```

- [ ] **Step 5: Decide Round 2/3 split** with the user (merge if approved rename count is small) and update `docs/backlog.md` NEXT ROUND section accordingly (part of the commit above or a follow-up).
