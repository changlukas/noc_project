# Commercialization Round 2 (code execution) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Execute the Round-1 ledger's approved renames and the two pulled-in code debts, leaving a green tree with final identifiers (spec: `docs/superpowers/specs/2026-07-14-commercialization-textbook-alignment-design.md`, ledger: `docs/superpowers/audit/2026-07-14-ledger.md`).

**Architecture:** Eight tasks, one commit each: (1) NSU `use_pools_` collapse [D6a], (2) fixed-VC-id wording [D2/D3], (3) `landing_` → input register [D4a], (4) master/slave prose in c_model+dpi [D1], (5) master/slave prose in SV/tools + tb regen [D1], (6) mosi/miso → upstream/downstream [D4b], (7) noc_types banner regen [D6b], (8) full-diff Codex review + final gates. Batches 1-4 are C++-only (ctest gate); 5-7 touch specgen/SV (regen-diff + WSL sim gate).

**Tech Stack:** C++17 header-only c_model, GoogleTest via ctest, specgen Python codegen, Verilator co-sim on WSL.

## Global Constraints

- Branch: `refactor/commercialization-round2`, created from `docs/commercialization-audit-round1` HEAD (`211c475`).
- After every `.hpp`/`.cpp` edit run `clang-format -i <file>` (repo `.clang-format`).
- Commit messages: `type(scope): description`, English.
- NO push. Stop in working tree at the end.
- Do NOT touch `docs/` (Round 3), except the final backlog.md strike-through task.
- Identifiers that STAY (ledger keep+trade-off, do not rename): `AxiMasterPort`/`AxiSlavePort` class names, `FEAT-*-AXI_SLAVE/MASTER_PORT` ids, `master_axi_req`/`slave_axi_req` generated bus labels, `SLAVE_VC_BUFFER_DEPTH`, `axi4_intf` modports `["master","slave"]`, `MetaBuffer`, `VcArbiter`, `FEAT-ROUTER-VC_ARBITRATION`, `FEAT-ROUTER-WORMHOLE_ARBITRATION`.
- "sub-" words that are NOT AXI-role prose and must NOT be touched: sub-burst, sub-module, sub-channel, sub-assembly, sub-make, sub-packages, `re.sub`, `_ast.Sub`, `sub.len`/`sub.addr` descriptor vars.
- The bw_monitor runtime label format `node%0d.master` (user_node_endpoint.sv:305) is load-bearing (parsed by emit_result_csv.py); it already says master — do not change it.
- Gate commands (run from repo root unless stated):
  - **ctest**: on WSL: `make test BUILD_ROOT=$HOME/noc_build PYTHON3=python3` — expect 100% pass.
  - **specgen pytest**: `cd specgen && py -3 -m pytest tests/ -q` (Windows) — expect all pass.
  - **codegen drift**: `cd specgen && py -3 tools/codegen.py --check` — expect exit 0.
  - **WSL directed sim**: on WSL: `make sim TB=mesh_4x4_vc1 PATTERN=neighbor BUILD_ROOT=$HOME/noc_build PYTHON3=python3` — expect scoreboard pass, exit 0.

---

### Task 0: Branch setup

- [ ] **Step 1: Create the branch**

```bash
git checkout docs/commercialization-audit-round1
git checkout -b refactor/commercialization-round2
```

Expected: on `refactor/commercialization-round2`, clean tree, HEAD = current tip of
`docs/commercialization-audit-round1` (the branch carries the ledger, spec amendments, and this plan).

---

### Task 1: NSU `use_pools_` collapse [D6a, ledger L1-007]

**Files:**
- Modify: `src/c_model/include/nsu/vc_arbiter.hpp`
- Tests (existing, no new): `src/c_model/tests/nsu/test_nsu_vc_arbiter.cpp` (calls only, no edits expected)

**Interfaces:**
- Produces: `nsu::VcArbiter::read_write_split(downstream, num_vc, write_rsp_vc, read_rsp_vc, pending_depth)` and `read_write_split_pools(downstream, num_vc, write_rsp_vcs, read_rsp_vcs, pending_depth)` — same signatures as today; scalar factory now delegates to size-1 pools (mirror of `nmu::VcArbiter`, nmu/vc_arbiter.hpp:54-67).
- Consumes: nothing from other tasks.

Known behavior delta (accepted, mirrors the NMU collapse): in scalar mode, `push_flit` for B/R now also gates on `downstream_.credit_avail(vc)` at select time (previously scalar select returned the VC unconditionally and only pending-depth gated). `num_vc_ == 1` short-circuit is unchanged. Production co-sim is unaffected (both wraps always build the pools variant via `derive_vc_pools`); the real exposure surface is `src/c_model/tests/integration/test_request_response_loopback.cpp` (builds NSU in scalar mode with num_vc=2 and runs full-pipeline congestion under a cycle watchdog). If a test fails after this task, look there first, then STOP and report — do not adapt the test silently.

- [ ] **Step 1: Rewrite the factories + constructor**

Replace lines 43-60 (both factories) with:

```cpp
    static VcArbiter read_write_split(router::NocRspOut& downstream, std::size_t num_vc,
                                      uint8_t write_rsp_vc, uint8_t read_rsp_vc,
                                      std::size_t pending_depth = kDefaultPendingDepth) {
        return VcArbiter(downstream, num_vc, std::vector<uint8_t>{write_rsp_vc},
                         std::vector<uint8_t>{read_rsp_vc}, pending_depth);
    }

    static VcArbiter read_write_split_pools(router::NocRspOut& downstream, std::size_t num_vc,
                                            std::vector<uint8_t> write_rsp_vcs,
                                            std::vector<uint8_t> read_rsp_vcs,
                                            std::size_t pending_depth = kDefaultPendingDepth) {
        return VcArbiter(downstream, num_vc, std::move(write_rsp_vcs), std::move(read_rsp_vcs),
                         pending_depth);
    }
```

Replace the private constructor (lines 73-83) with:

```cpp
    VcArbiter(router::NocRspOut& downstream, std::size_t num_vc, std::vector<uint8_t> write_rsp_vcs,
              std::vector<uint8_t> read_rsp_vcs, std::size_t pending_depth)
        : downstream_(downstream),
          num_vc_(num_vc),
          write_rsp_vcs_(std::move(write_rsp_vcs)),
          read_rsp_vcs_(std::move(read_rsp_vcs)),
          pending_depth_(pending_depth) {
        assert(num_vc_ >= 1 && num_vc_ <= NUM_VC_MAX);
        for (uint8_t v : write_rsp_vcs_) assert(v < num_vc_ && "write_rsp_vcs element >= num_vc");
        for (uint8_t v : read_rsp_vcs_) assert(v < num_vc_ && "read_rsp_vcs element >= num_vc");
    }
```

- [ ] **Step 2: Delete the scalar members and flag**

In the private member block delete these three lines — note they are NOT adjacent
(`write_rsp_vc_`/`read_rsp_vc_` sit above `pending_`; `use_pools_` sits below the pool vectors);
delete each individually:

```cpp
    uint8_t write_rsp_vc_;
    uint8_t read_rsp_vc_;
```
```cpp
    bool use_pools_ = false;
```

- [ ] **Step 3: Collapse select_vc_for_axi_ch**

Delete the scalar branch (lines 106-111):

```cpp
    // ReadWriteSplit, scalar (no pools configured).
    if (!use_pools_) {
        if (axi_ch == ni::AXI_CH_B) return write_rsp_vc_;
        if (axi_ch == ni::AXI_CH_R) return read_rsp_vc_;
        return std::nullopt;
    }

    // ReadWriteSplit pools.
```

The pools body below it becomes the only path (comment line `// ReadWriteSplit pools.` deleted with the branch).

- [ ] **Step 4: Drop the use_pools_ guard in push_flit**

Line 152, change:

```cpp
    if (use_pools_ && num_vc_ > 1 && (axi_ch == ni::AXI_CH_B || axi_ch == ni::AXI_CH_R)) {
```

to:

```cpp
    if (num_vc_ > 1 && (axi_ch == ni::AXI_CH_B || axi_ch == ni::AXI_CH_R)) {
```

- [ ] **Step 5: Update the header doc-comment mode description**

Line 7-8, change:

```
// ReadWriteSplit (only mode): B -> write_rsp_vc; R -> read_rsp_vc.
// Pools mode:
```

to:

```
// ReadWriteSplit (only mode): per-class VC pool; the scalar factory wraps a
// single VC into a size-1 pool (mirror of nmu::VcArbiter).
```

(Leave the clause-2 wording lines untouched — Task 2 rewrites them.)

- [ ] **Step 6: Format, build, test**

```bash
clang-format -i src/c_model/include/nsu/vc_arbiter.hpp
```

Run ctest gate (WSL): `make test BUILD_ROOT=$HOME/noc_build PYTHON3=python3`
Expected: 100% tests passed.

- [ ] **Step 7: Commit**

```bash
git add src/c_model/include/nsu/vc_arbiter.hpp
git commit -m "refactor(nsu): collapse VcArbiter scalar mode into size-1 pool, mirror NMU"
```

---

### Task 2: fixed-VC-id wording, RZ1 removal [D2/D3, ledger L1-001/L1-002]

**Files:**
- Modify: `src/c_model/include/nmu/vc_arbiter.hpp`, `src/c_model/include/nsu/vc_arbiter.hpp`, `src/c_model/include/nsu/nsu.hpp:59-62`, `src/c_model/include/nsu/meta_buffer.hpp:25`, `src/c_model/tests/nmu/test_vc_arbiter.cpp`, `src/c_model/tests/nsu/test_nsu_vc_arbiter.cpp`

**Interfaces:** none exported; comment/test-name/local-var wording only.

Wording rules (apply everywhere in the listed files):
- mechanism title "Clause 2 VC pin" / "Clause 2 return-path static map (microarch §5a, RZ1)" → "Clause 2 fixed VC id" — deterministic VC allocation, no coined tag, no dying-doc §refs.
- verb "pin(s) ... to one VC" → "fix(es) ... to one VC"; "pinned stream/streak" → "fixed-VC stream/streak"; "pin miss" → "no fixed VC yet"; "unpinned / do not pin" → "never fixed / do not record a fixed VC".
- `RZ1` string: delete every occurrence.
- Dying-doc refs "(microarch §5a)" inside rewritten lines: drop the parenthetical. Do NOT sweep untouched References blocks (Round 3 backlog item, Task 8 records it).

- [ ] **Step 1: nmu/vc_arbiter.hpp**

Header block lines 13-18, replace:

```
// Clause 2 VC pin (microarch §5a): a rob_req=0 AW/AR flit whose (dst_id, id)
// matches the id's previous same-channel flit reuses that VC instead of
// round-robining -- pins a same-(dst,id) bypass streak to one VC so it
// cannot be reordered in-fabric. A pin miss (new id, or dst changed) falls
// back to round-robin and records the new (dst, VC) for next time. rob_req=1
// flits are RoB-owned and order-free, so they always round-robin, unpinned.
```

with:

```
// Clause 2 fixed VC id: a rob_req=0 AW/AR flit whose (dst_id, id) matches
// the id's previous same-channel flit reuses that VC instead of
// round-robining -- deterministic VC allocation that fixes a same-(dst,id)
// bypass streak to one VC so it cannot be reordered in-fabric. With no
// fixed VC yet (new id, or dst changed) it falls back to round-robin and
// records the new (dst, VC) for next time. rob_req=1 flits are RoB-owned
// and order-free, so they always round-robin, never fixed.
```

Member-block comment lines 111-112, replace:

```
    // Clause 2 VC pin (microarch §5a): last (dst_id, VC) a given AXI id took
    // on a rob_req=0 flit, per direction. nullopt dst = id never seen.
```

with:

```
    // Clause 2 fixed VC id: last (dst_id, VC) a given AXI id took on a
    // rob_req=0 flit, per direction. nullopt dst = id never seen.
```

select_vc_for_axi_ch comment lines 139-142, replace:

```
    // Clause 2 VC pin: rob_req=0 flit whose dst_id matches this id's last
    // same-channel dst_id reuses that VC. No fallback to round-robin on
    // block -- rerouting a pinned streak mid-flight is exactly the reorder
    // this pin exists to prevent.
```

with:

```
    // Clause 2 fixed VC id: rob_req=0 flit whose dst_id matches this id's
    // last same-channel dst_id reuses that VC. No fallback to round-robin on
    // block -- rerouting a fixed-VC streak mid-flight is exactly the reorder
    // the fixed VC exists to prevent.
```

push_flit comment lines 200-202, replace:

```
    // Clause 2 VC pin: record (dst_id, VC) for this id only after all accept
    // conditions pass (mirrors current_aw_vc_'s atomicity above). rob_req=1
    // flits are RoB-owned/order-free -- do not pin them.
```

with:

```
    // Clause 2 fixed VC id: record (dst_id, VC) for this id only after all
    // accept conditions pass (mirrors current_aw_vc_'s atomicity above).
    // rob_req=1 flits are RoB-owned/order-free -- do not record a fixed VC.
```

- [ ] **Step 2: nsu/vc_arbiter.hpp**

Clause-2 block (post-Task-1 line numbers shift; match by text), replace:

```
//   Clause 2 return-path static map (microarch §5a, RZ1): a rob_req=0 B, or
//   ANY R (regardless of rob_req), maps to
//   pool[(dst_id ^ id) % pool.size()] -- a deterministic pure function, zero
//   state. This pins a same-(dst,id) bypassed response stream to one VC (so
//   it cannot be reordered in-fabric) and gives R burst coherence for free:
//   every beat of a burst shares (dst_id, rid) and hashes identically. A
//   mapped VC that is full/no-credit refuses (`return false`) rather than
//   spilling to another pool VC -- spilling a pinned stream would reorder it.
//   rob_req=1 B is order-free at the NMU slot path and round-robins the
//   write pool, same as before clause 2.
```

with:

```
//   Clause 2 fixed VC id (return path): a rob_req=0 B, or ANY R (regardless
//   of rob_req), maps to pool[(dst_id ^ id) % pool.size()] -- deterministic
//   VC allocation, a pure function with zero state. This fixes a
//   same-(dst,id) bypassed response stream to one VC (so it cannot be
//   reordered in-fabric) and gives R burst coherence for free: every beat of
//   a burst shares (dst_id, rid) and hashes identically. A mapped VC that is
//   full/no-credit refuses (`return false`) rather than spilling to another
//   pool VC -- spilling a fixed-VC stream would reorder it. rob_req=1 B is
//   order-free at the NMU slot path and round-robins the write pool.
```

Local variable `bool pinned` and its three uses in `select_vc_for_axi_ch` → `bool fixed_vc`:

```cpp
    bool fixed_vc = false;
    if (axi_ch == ni::AXI_CH_B) {
        cand = &write_rsp_vcs_;
        rr = &write_rr_start_;
        fixed_vc = (rob_req == 0);  // rob_req=1 B is order-free; round-robins.
    } else if (axi_ch == ni::AXI_CH_R) {
        cand = &read_rsp_vcs_;
        rr = &read_rr_start_;
        fixed_vc = true;  // ALL R: the fixed map gives burst coherence.
    } else {
        return std::nullopt;
    }

    if (fixed_vc) {
        // Clause 2 fixed VC id (return path): deterministic pure function of
        // (dst_id, id), zero state. Full/no-credit -> refuse, never spill
        // (spilling a fixed-VC stream to another VC would reorder it).
```

(Note: the old line-124 comment "was r_burst_vc_'s role" — a reference to a deleted member — is dropped by the replacement above.)

- [ ] **Step 3: nsu/nsu.hpp config comment (stale burst-follow claim)**

Lines 59-62, replace:

```
    // ReadWriteSplit pool variant (response side): non-empty -> per-class pool.
    // B: id-agnostic round-robin over write_rsp_vcs.
    // R: first beat of each rid round-robins over read_rsp_vcs; later beats of
    //    that rid reuse the same VC until rlast (burst-follow, not per-id pin).
```

with (matches the as-built clause-2 behavior):

```
    // ReadWriteSplit pool variant (response side): non-empty -> per-class pool.
    // B: rob_req=0 -> fixed VC id pool[(dst_id ^ bid) % size]; rob_req=1 ->
    //    id-agnostic round-robin over write_rsp_vcs.
    // R: fixed VC id pool[(dst_id ^ rid) % size] for every beat; a burst's
    //    beats share (dst_id, rid) so the whole burst lands on one VC.
```

- [ ] **Step 4: nsu/meta_buffer.hpp stale member ref**

Lines 23-25, replace:

```
// The remap is a function of upstream_id ALONE. Feeding it src_id would let two
// sources' R bursts with the same restored rid be interleaved by the subordinate,
// which would contend nsu::VcArbiter::r_burst_vc_. See the design spec.
```

with (the old r_burst_vc_ contention rationale is obsolete — the (dst_id ^ id) keyed
fixed-VC map dissolved it; the surviving rationale is upstream parity):

```
// The remap is a function of upstream_id ALONE, matching the ported source
// above. Ordering no longer depends on this choice: the response-path fixed
// VC map keys on (dst_id ^ id), so same-id streams from different sources
// land on distinct keys instead of contending.
```

(Note "manager's"/"subordinate" words inside these lines belong to Task 4; the replacement
above already avoids them, which is fine — Task 4's verify grep is the safety net.)

- [ ] **Step 5: test files**

`src/c_model/tests/nmu/test_vc_arbiter.cpp` — rename tests and reword comments/SCENARIO strings by the wording rules. Known renames:

| old | new |
|---|---|
| `TEST(NmuVcArbiterRoundRobin, SameReadIdSameDestPinsVc)` | `TEST(NmuVcArbiterRoundRobin, SameReadIdSameDestFixedVcId)` |
| `TEST(NmuVcArbiter, WFollowsAW_ReusedPinnedVc)` | `TEST(NmuVcArbiter, WFollowsAW_ReusedFixedVc)` |

(`SameReadIdDifferentDestRoundRobins` at :258 keeps its name; only its comment at :256 "Pin miss: ..."
→ "No fixed VC yet: ..." and SCENARIO/EXPECT strings at :259,:266 "pin miss" → "no fixed VC yet".)

Comment/string rewrites in the same file: lines 225 ("would pin all"→"would fix all"), 242-246, 253, 256-266, 270, 284, 287-290, 299-321 — apply the wording rules; keep technical content identical.

`src/c_model/tests/nsu/test_nsu_vc_arbiter.cpp` — known renames:

| old | new |
|---|---|
| `TEST(NsuVcArbiterPools, SameBidSameDstBypassPinsOneVc)` | `TEST(NsuVcArbiterPools, SameBidSameDstBypassFixedVcId)` |
| `TEST(NsuVcArbiterPools, PinnedVcFullRefusesInsteadOfSpilling)` | `TEST(NsuVcArbiterPools, FixedVcFullRefusesInsteadOfSpilling)` |

Comment rewrites: line 83 drop the whole `microarch §5a (RZ1):` prefix (keep the formula description); line 199 drop the `(microarch §5a / meta_buffer.hpp:22-28)` parenthetical (keep the `W6` tag and the keying explanation); lines 169-175, 200, 221-226 per wording rules ("was r_burst_vc_'s role" phrasing at :84 and :200 may stay — it explains a design lineage in a test explanation; keep it factual: "replaces the retired r_burst_vc_ array's burst-coherence role").

- [ ] **Step 6: Verify no survivor**

Run the BROAD sweep and adjudicate the residue by hand (an empty-output expectation is not
achievable with any sane pattern here):

```bash
grep -rniE "rz1|\bpin(s|ned|ning)?\b|pinn" src/c_model/
```

Expected: `rz1` → zero hits. For pin words, the ONLY legitimate survivors are the
hardware-pin/struct senses — this allowlist, nothing else:

| file | sense |
|---|---|
| `router/req_in.hpp`, `req_out.hpp`, `rsp_in.hpp`, `rsp_out.hpp` | "RTL pin set" / pin bundle (Task 6 rewords bundle NAMES but the pin-bundle sense stays) |
| `wrap/router_wrap.hpp:17,27`, `wrap/router_wrap_io.hpp` | "LOCAL pin mapping" / pin groups |
| `tests/wrap/test_nmu_wrap.cpp:208` | "pins the YAML load" (idiom, non-VC) |
| `tests/router/test_router.cpp:18` | "pinned by RouterDatapath..." — reword this one too: "pinned by" → "verified by" (cheap, keeps the sweep clean) |
| `tests/nsu/test_nsu.cpp:55`, `tests/nmu/test_nmu.cpp:61` | "Pinpoints:" (unrelated word) |

Any OTHER pin hit = missed rewrite; fix it before proceeding.

- [ ] **Step 7: Format, test, commit**

```bash
clang-format -i src/c_model/include/nmu/vc_arbiter.hpp src/c_model/include/nsu/vc_arbiter.hpp \
  src/c_model/include/nsu/nsu.hpp src/c_model/include/nsu/meta_buffer.hpp \
  src/c_model/tests/nmu/test_vc_arbiter.cpp src/c_model/tests/nsu/test_nsu_vc_arbiter.cpp
```

ctest gate (WSL): `make test BUILD_ROOT=$HOME/noc_build PYTHON3=python3` — expect 100% pass (test COUNT unchanged; only names changed).

```bash
git add -u src/c_model
git commit -m "refactor(ni): rename VC pin wording to fixed VC id, drop RZ1 tag"
```

---

### Task 3: `landing_` → input register [D4a, ledger L1-006]

**Files:**
- Modify: `src/c_model/include/router/router.hpp`, `src/c_model/include/ni/pipeline_stage.hpp:8`, `src/c_model/include/router/router_adapters.hpp:41`, `src/c_model/include/wrap/router_wrap.hpp:108`, `src/c_model/tests/router/test_router.cpp`, `src/c_model/tests/router/test_router_adapters.cpp`, `src/c_model/tests/router/test_router_front_route.cpp:39`, `src/c_model/tests/router/test_wormhole_arbiter.cpp:159,165`

**Interfaces:**
- Produces: `Router::input_reg_` private member (renamed from `landing_`; no public API change).

Wording rules: "landing register" → "input register" (textbook input-buffered-router BW-stage term); member `landing_` → `input_reg_`; "landing VC" (test_wormhole_arbiter — means the VC the arbiter selected, not the register) → "selected VC".

- [ ] **Step 1: router.hpp**

- Line 167 comment: `// stage-1 input landing register, one flit/port/cycle` → `// stage-1 input register, one flit/port/cycle`
- Line 168: `std::array<std::optional<Flit>, ROUTER_PORT_COUNT> landing_{};` → `... input_reg_{};`
- All uses at :195, :199, :281-283: `landing_` → `input_reg_`
- Line 279 comment: `// Stage 1: landing register -> input VC FIFO.` → `// Stage 1: input register -> input VC FIFO.`

- [ ] **Step 2: satellite comments**

- `ni/pipeline_stage.hpp:8`: `Mirrors Router::landing_` → `Mirrors Router::input_reg_`
- `router/router_adapters.hpp:41`: `per-tick landing-register guard` → `per-tick input-register guard`
- `wrap/router_wrap.hpp:108`: `router landing register asserts` → `router input register asserts`

- [ ] **Step 3: tests**

- `test_router.cpp` comments :515, :604-608, :674-675, :822, :836: "landing register"→"input register", "WEST landing"→"WEST input register", "absorbs its landing"→"absorbs its input register", "landing consumed"→"input register consumed".
- `test_router_adapters.cpp`: `TEST(InjectAdapter, LandingGuardResetsOnTick)` → `TEST(InjectAdapter, InputRegGuardResetsOnTick)`; comments :54 ("landing free"→"input register free"), :68 ("landing->fifo"→"input register -> fifo").
- `test_router_front_route.cpp:39`: `// landing -> input FIFO` → `// input register -> input FIFO`
- `test_wormhole_arbiter.cpp:159,165`: "landing VC" → "selected VC".
- `nmu/rob.hpp:225`: "arrival places landing beats" → "arrival places incoming beats" (RoB usage of the same metaphor; reword so the sweep below comes back clean).

- [ ] **Step 4: Verify, format, test, commit**

```bash
grep -rni "landing" src/ sim/ specgen/
```
Expected: no output.

clang-format the touched files; ctest gate (WSL) — expect 100% pass.

```bash
git add -u src/c_model
git commit -m "refactor(router): rename landing register to input register"
```

(File list for this commit now includes `nmu/rob.hpp` from Step 3.)

---

### Task 4: master/slave prose unification, c_model + dpi [D1, ledger L1-003]

**Files (from the audit sweep — every hit is a comment; no identifier changes):**
- Modify: `src/c_model/include/nsu/axi_master_port.hpp` (:2,7,8,10,23,47,50), `src/c_model/include/nmu/axi_slave_port.hpp` (:2,5,8,15,47), `src/c_model/include/nsu/depacketize.hpp` (:23,160,163), `src/c_model/include/nsu/meta_buffer.hpp` (:14,19,24,31), `src/c_model/include/nsu/packetize.hpp` (:81,82), `src/c_model/include/nsu/port_params.hpp` (:27), `src/c_model/include/nmu/addr_trans.hpp` (:52), `src/c_model/include/wrap/nsu_wrap.hpp` (:6,21,25,26,122,195,197), `src/c_model/include/wrap/nsu_wrap_io.hpp` (:6,11,46,48,50,54,56,72,83,88,90,101), `src/c_model/tests/nmu/test_axi_slave_port.cpp` (:2), `src/c_model/tests/axi/test_wire_slave_port.cpp` (:9,:40 "subordinate not ready"), `src/dpi/cmodel_dpi.h` (:134,135)

**Interfaces:** none; comments only.

Rules: word "manager" → "master"; word "subordinate" → "slave"; adjust articles ("a subordinate"→"a slave", "an external manager"→"an external master"). Do NOT touch the `[NOT ROLE]` sub-* words (Global Constraints). Example, `nsu/axi_master_port.hpp:2`:

```
// NSU AxiMasterPort — thin transparent AXI4 manager transport, peer of
```
→
```
// NSU AxiMasterPort — thin transparent AXI4 master transport, peer of
```

- [ ] **Step 1: Apply the rename across the file list** (match each line from the sweep, rewrite in place)

- [ ] **Step 2: Verify no role-prose survivor in c_model/dpi**

```bash
grep -rniE "manager|subordinate" src/c_model src/dpi
```
Expected: no output.

- [ ] **Step 3: Format, test, commit**

clang-format touched `.hpp/.cpp`; ctest gate (WSL) — expect 100% pass.

```bash
git add -u src/c_model src/dpi
git commit -m "docs(cmodel): unify AXI role prose to master/slave"
```

---

### Task 5: master/slave prose, SV + tools + tb regen [D1, ledger L2-001 prose slice]

**Files:**
- Modify: `src/sv/nsu_wrap.sv` (:5,214 "subordinate" prose; :138,319 already "toward subordinate" → "toward slave"), `specgen/tools/elaborate/sv_signals.py` (:82 `master (manager) side` → `master side`), `sim/tools/gen_tb_top.py` (:139 "subordinate sees"→"slave sees"; :560-561 "manager"→"master"), `sim/tools/emit_result_csv.py` (:20 stale example `node0.manager` → `node0.master`, matching the real label at `sim/tb/user_node_endpoint.sv:305`)
- Regenerate: all 11 committed `sim/tb/tb_top_mesh_*.sv` (carry gen_tb_top's NSU-knobs comment)

**Interfaces:** none; comments only. sv_signals.py:82 is an emitter-source comment — emitted output does not change.

- [ ] **Step 1: Edit the four source files** per the line list above.

- [ ] **Step 2: Regenerate committed tb files**

From repo root in **Git Bash** (the loop is bash syntax; `py -3` is the canonical Windows interpreter), for each committed tb (topology name = filename minus `tb_top_` prefix and `.sv` suffix):

```bash
for t in mesh_1x1_vc1 mesh_2x2_nonuniform_vc1 mesh_2x4_vc1 mesh_4x4_vc1 mesh_4x4_vc1_rob \
         mesh_4x4_vc2 mesh_4x4_vc2_rob mesh_4x4_vc4 mesh_4x4_vc4_rob mesh_4x4_vc8 mesh_4x4_vc8_rob; do
  py -3 sim/tools/gen_tb_top.py --topology $t --out sim/tb/tb_top_$t.sv || exit 1
done
```

Then `git diff --stat sim/tb/` — expected: exactly the 11 tb files, comment-line deltas only (inspect one full diff: `git diff sim/tb/tb_top_mesh_4x4_vc1.sv`, expect only the NSU-knobs comment lines changed). gen_tb_top also re-emits `src/sv/noc_fabric_*.sv` each run; those carry no role prose and regenerate byte-identical — `git status src/sv` must show no change. If any non-comment delta appears anywhere, STOP and report.

Coverage note: the sim gate below compiles only `mesh_4x4_vc1`; the other 10 regenerated tbs are covered by generator determinism + this diff inspection, not by compilation. Accepted for a comment-only delta.

- [ ] **Step 3: Verify sweep-level completion**

```bash
grep -rniE "manager|subordinate" src/sv sim/tools sim/tb specgen/tools
```
Expected: no output.

- [ ] **Step 4: Gates + commit**

- specgen pytest: `cd specgen && py -3 -m pytest tests/ -q` — all pass.
- WSL directed sim: `make sim TB=mesh_4x4_vc1 PATTERN=neighbor BUILD_ROOT=$HOME/noc_build PYTHON3=python3` — pass.

```bash
git add src/sv sim/tools sim/tb specgen/tools
git commit -m "docs(sim): unify AXI role prose to master/slave in SV comments and generators"
```

---

### Task 6: NoC link mosi/miso → upstream/downstream [D4b, ledger L2-001 modport slice]

> SCOPE NOTE (user-visible decision, settled at plan review): the gate approved the modport
> rename assuming "source JSON only". The audit-verified blast radius is larger: the
> `NOC_INTF_MOSI/MISO` port-group names in the hand-curated `specgen/generated/json/ni_signals.json`
> (spec ground truth) and the generated `NocIntfMosiPins/NocIntfMisoPins` structs carry the same
> metaphor. This task executes the FULL sweep. Mapping: `mosi` → `upstream` (NMU end of the link,
> drives req), `miso` → `downstream` (NSU end, drives rsp).

**Files:**
- Modify (sources of truth): `specgen/source/interface_handshake.json` (:24-31 six `driven_by` values + `"modports": ["mosi", "miso"]`), `specgen/generated/json/ni_signals.json` (:450 `NOC_INTF_MOSI`→`NOC_INTF_UPSTREAM`, :963 `NOC_INTF_MISO`→`NOC_INTF_DOWNSTREAM`), `specgen/generated/json/ni_signals.schema.json` (:108 description example `NOC_INTF_MOSI`→`NOC_INTF_UPSTREAM`)
- Modify (specgen consumers): `specgen/ni_spec/constants.py` (:352-353 docstring), `specgen/tests/test_handshake_schema.py` (:254 `["mosi", "miso"]`→`["upstream", "downstream"]`), `specgen/tests/test_signals_schema.py` (:39,46,47), `specgen/tests/test_signals_resolver.py` (all `NOC_INTF_MOSI/MISO` at :37-41,58,59,90,104,112,125), `specgen/tests/test_constants_resolver.py` (:175), `specgen/tests/cpp_smoke/test_pins_compile.cpp` (:16-23 `NocIntfMosiPins`→`NocIntfUpstreamPins`, `NocIntfMisoPins`→`NocIntfDownstreamPins`, local vars `noc_mosi`/`noc_miso`→`noc_upstream`/`noc_downstream`)
- Regenerate: `specgen/generated/cpp/ni_signals.h` (struct names change), `specgen/generated/sv/ni_signals_pkg.sv` (banner only; body carries no mosi/miso), golden `specgen/tests/golden/ni_signals.h.golden` (copy from fresh regen)
- Modify (c_model comments): `src/c_model/include/router/req_in.hpp` (:8,9), `req_out.hpp` (:8), `rsp_in.hpp` (:8), `rsp_out.hpp` (:8) — `NocIntfMisoPins`→`NocIntfDownstreamPins`, `NocIntfMosiPins`→`NocIntfUpstreamPins`, `NOC_INTF_MISO`→`NOC_INTF_DOWNSTREAM`, `NOC_INTF_MOSI`→`NOC_INTF_UPSTREAM`

**Interfaces:**
- Produces: generated struct names `ni::pins::NocIntfUpstreamPins` / `NocIntfDownstreamPins`; port groups `NOC_INTF_UPSTREAM` (NMU) / `NOC_INTF_DOWNSTREAM` (NSU); `noc_intf` modports `["upstream", "downstream"]`.
- Consumes: nothing from other tasks.

- [ ] **Step 1: Edit the two source JSONs + schema example** per the mapping. In `interface_handshake.json`: `"driven_by": "mosi"` → `"driven_by": "upstream"`, `"driven_by": "miso"` → `"driven_by": "downstream"`, `"modports": ["mosi", "miso"]` → `"modports": ["upstream", "downstream"]`.

- [ ] **Step 2: Regenerate cpp + sv signals**

```bash
cd specgen
py -3 tools/codegen.py --target cpp --domain signals
py -3 tools/codegen.py --target sv  --domain signals
```

Expected: `generated/cpp/ni_signals.h` diff shows struct renames only; `generated/sv/ni_signals_pkg.sv` diff shows banner only.

- [ ] **Step 3: Refresh the golden**

From repo root (NOT from inside `specgen/` — Step 2 changed directory):

```bash
cd <repo-root>
cp specgen/generated/cpp/ni_signals.h specgen/tests/golden/ni_signals.h.golden
```

(`test_byte_identical_golden.py` strips the `Generated at:`/`Source SHA:` banner lines before compare, so a straight copy is correct.)

- [ ] **Step 4: Update specgen tests + constants.py docstring + cpp_smoke** per the file list.

- [ ] **Step 5: Update the four c_model router header comments** per the mapping.

- [ ] **Step 6: Verify sweep-level completion**

```bash
grep -rni "mosi\|miso" src/ sim/ specgen/ --include="*" | grep -vi "sim/dv"
```
Expected: no output.

- [ ] **Step 7: Gates + commit**

- `cd specgen && py -3 -m pytest tests/ -q` — all pass.
- `cd specgen && py -3 tools/codegen.py --check` — exit 0.
- ctest gate (WSL) — 100% pass (c_model consumes ni_signals.h? it does not use the renamed structs; comments only — still run for the codegen_check dependency).
- WSL directed sim — pass (ni_signals_pkg.sv regenerated; body unchanged).

```bash
git add specgen src/c_model/include/router
git commit -m "refactor(specgen): rename NoC link modports mosi/miso to upstream/downstream"
```

---

### Task 7: noc_types_pkg_vc* banner refresh [D6b, ledger L4-006]

**Files:**
- Regenerate: `specgen/generated/sv/noc_types_pkg_vc{1,2,4,8}.sv`

- [ ] **Step 1: Regenerate**

```bash
cd specgen
for n in 1 2 4 8; do py -3 tools/codegen.py --target sv --domain noc_types --num-vc $n || exit 1; done
```

- [ ] **Step 2: Verify banner-only diff**

`git diff specgen/generated/sv/noc_types_pkg_vc*.sv` — expected: only the `Generated at:` / `Source SHA:` provenance lines change. If any body line changes, STOP and report (ledger verified body-current via --check; a body delta means something else moved).

- [ ] **Step 3: Gate + commit**

`py -3 tools/codegen.py --check` — exit 0.

```bash
git add specgen/generated/sv
git commit -m "chore(specgen): refresh noc_types_pkg_vc* provenance banners"
```

---

### Task 8: Full-diff review + final gates + backlog strike

**Files:**
- Modify: `docs/backlog.md` (strike closed Round-2 items; add one Round-3 item)

- [ ] **Step 1: Run every gate once more on the final tree**

- WSL: `make test BUILD_ROOT=$HOME/noc_build PYTHON3=python3` — 100% pass.
- `cd specgen && py -3 -m pytest tests/ -q` — all pass.
- `cd specgen && py -3 tools/codegen.py --check` — exit 0.
- WSL: `make sim TB=mesh_4x4_vc1 PATTERN=neighbor BUILD_ROOT=$HOME/noc_build PYTHON3=python3` — pass.

- [ ] **Step 2: Codex full-diff review**

Review target: `git diff docs/commercialization-audit-round1..HEAD`. Prompt Codex for: missed rename occurrences, behavior drift in Task 1's collapse, generated-vs-source inconsistency. Fix findings or record justified rejections.

- [ ] **Step 3: backlog.md update**

- Strike (mark done or delete per backlog forward-only convention): Round-2 bullet items D1/D2-D3/D4/D6; the "NSU `VcArbiter` `use_pools_` asymmetry" row under "Open -- NMU RoB / NSU sizing & structure".
- Add under Round 3: "sweep code comments for `docs/superpowers/...` / `microarch §` references before doc deletion (References blocks in nmu/vc_arbiter.hpp, nsu/vc_arbiter.hpp, meta_buffer.hpp etc.)".

- [ ] **Step 4: Commit backlog, stop**

```bash
git add docs/backlog.md
git commit -m "docs(backlog): strike round-2 closed items, queue round-3 comment-ref sweep"
```

Do NOT push. Report final state (commit list, gate outputs) and wait for user review.
