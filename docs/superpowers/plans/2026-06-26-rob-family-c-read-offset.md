# ROB Family C Read Offset Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make NMU ROB Enabled-mode multi-beat reads land each R beat at its correct slot via a per-base arrival counter, then exercise the Enabled path end to end in co-sim.

**Architecture:** NSU stamps every R beat of a burst with `rob_idx = base`. NMU adds a per-base beat counter so beat i lands at `base+i`. A computed-slot guard replaces the old base-overwrite guard and still fails fast on malformed input. A dedicated Enabled co-sim TB validates the path (existing Disabled TBs untouched).

**Tech Stack:** C++17, GoogleTest (ctest), Verilator co-sim, Python codegen (`gen_tb_top.py`), CMake.

## Global Constraints

- Namespace `ni::cmodel::nmu` for ROB code. Tests under `ni::cmodel::testing`.
- snake_case for vars/methods, PascalCase for types. Full words, no abbreviation.
- C/C++ continuation lines: 4-space indent. Run `clang-format -i` on every edited `.hpp`/`.cpp` (`.clang-format` at repo root: Google base, IndentWidth 4, ContinuationIndentWidth 4, ColumnLimit 100).
- Build/test: `PYTHON3=python3` (mingw64), never `py -3`.
- `ROB_CAPACITY = 32` (`1 << ni::header::ROB_IDX_WIDTH`). `AXI_ID_SPACE = 256`.
- Every commit compiles, passes all existing tests, includes tests for new behavior. Commit message `type(scope): description`. No `--no-verify`. Do not push (working tree review first).
- Co-sim verify requires clean rebuild: `rm -rf build/verilator/obj_dir_<topo>`.

---

### Task 1: Per-base arrival counter + computed-slot guard (core fix)

**Files:**
- Modify: `c_model/include/nmu/rob.hpp` (add state ~line 165; `push_ar` Enabled branch 232-251; `pop_r_staged` 337-393)
- Test: `c_model/tests/nmu/test_rob.cpp` (rewrite `FiresOncePerIdDrain_Enabled` ~568; invert `ReadFillSameBaseRobIdxOverwriteGuarded` ~639)

**Interfaces:**
- Consumes: existing `Rob` Enabled-mode read path — `push_ar` reserves `n=len+1` consecutive slots `base..base+n-1` and records `read_order_by_id_[id].push_back({base, n})`; `pop_r_staged` pulls `(r, meta)` with `meta.rob_idx`.
- Produces: per-base counter behavior — beat i of a burst stamped with `rob_idx=base` lands at `read_entries_[base+i]`; counter reset when the range is popped from `read_order_by_id_`.

- [ ] **Step 1: Rewrite the happy-path test to feed real all-base stamping**

In `test_rob.cpp`, `FiresOncePerIdDrain_Enabled` (~line 568): change the four `push_r` calls from ideal `base+offset` to the real NSU all-base stamping.

```cpp
    // Real NSU stamps every beat of the burst with the SAME base rob_idx.
    push_r(0, false);
    push_r(0, false);
    push_r(0, false);
    push_r(0, true);
    depkt.tick();
    // Expect: per-base counter files them into slots 0..3; all 4 beats drain;
    // id=5 order list empties -> exactly one drain fire.
    int got = 0;
    for (int i = 0; i < 32 && got < 4; ++i) {
        if (rob.pop_r().has_value()) ++got;
    }
    ASSERT_EQ(got, 4);
    ASSERT_EQ(drained.size(), 1u);
    EXPECT_EQ(drained[0].first, false);
    EXPECT_EQ(drained[0].second, 5);
```

- [ ] **Step 2: Invert the guard death test to assert correct ordering**

Replace `ReadFillSameBaseRobIdxOverwriteGuarded` (~line 639) with an ordering-correctness test (rename to `ReadFillSameBaseRobIdxLandsInOrder`). Keep the `push_r(rob_idx, rlast, marker)` lambda; feed two all-base beats and assert the data markers come out in beat order.

```cpp
TEST(NmuRob, ReadFillSameBaseRobIdxLandsInOrder) {
    SCENARIO(
        "Enabled read ROB: two R beats stamped with the same base rob_idx (real NSU "
        "stamping) land at base+0 and base+1 via the per-base arrival counter, in order.");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    axi::ArBeat ar = make_ar(0x05, 0x100);
    ar.len = 1;  // 2 beats -> slots 0..1, base=0
    ASSERT_TRUE(rob.push_ar(ar));

    auto push_r = [&](uint8_t rob_idx, bool rlast, uint8_t marker) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_R);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("last", 1);
        f.set_header_field("rob_req", 1);
        f.set_header_field("rob_idx", rob_idx);
        f.set_payload_field("R", "rid", 0x05);
        f.set_payload_field("R", "rresp", 0);
        f.set_payload_field("R", "rlast", rlast ? 1u : 0u);
        std::array<uint8_t, 32> d{};
        d[0] = marker;
        f.set_payload_bytes("R", "rdata", d.data(), 256);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
    };
    push_r(/*rob_idx=*/0, /*rlast=*/false, 0xA0);
    push_r(/*rob_idx=*/0, /*rlast=*/true, 0xA1);
    depkt.tick();

    auto r0 = rob.pop_r();
    auto r1 = rob.pop_r();
    ASSERT_TRUE(r0.has_value());
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r0->data[0], 0xA0);  // base+0
    EXPECT_EQ(r1->data[0], 0xA1);  // base+1
}
```

- [ ] **Step 3: Run the two tests to verify they FAIL**

Run: `cmake --build build && ctest --test-dir build -R "RobDrainHook.FiresOncePerIdDrain_Enabled|NmuRob.ReadFillSameBaseRobIdxLandsInOrder" -V`
Expected: FAIL — the all-base second beat hits the old guard at `rob.hpp:360-368` and aborts (or misfiles), because the per-base counter does not exist yet.

- [ ] **Step 4: Add per-base counter state to rob.hpp**

After the `read_order_by_id_` declaration (~line 165), add:

```cpp
    // Family C: per-base (keyed by rob_idx base) arrival counter. NSU stamps every
    // R beat of a burst with rob_idx=base; this counter positions beat i at base+i.
    // Reset when the range is popped from read_order_by_id_ (ties counter lifecycle
    // to slot-reuse eligibility). read_range_len_[base] = burst length n, set in
    // push_ar, used to bound the counter (beat past burst length is malformed).
    std::array<uint8_t, ROB_CAPACITY> read_arrival_offset_{};
    std::array<uint8_t, ROB_CAPACITY> read_range_len_{};
```

- [ ] **Step 5: Record burst length in push_ar**

In `push_ar` Enabled branch, right after the `read_order_by_id_[b.id].push_back(...)` line (~249):

```cpp
        read_order_by_id_[b.id].push_back({static_cast<uint8_t>(base), static_cast<uint8_t>(n)});
        read_range_len_[base] = static_cast<uint8_t>(n);
```

- [ ] **Step 6: Rewrite pop_r_staged slot positioning + guard**

In `pop_r_staged` (~355-368), replace the direct `meta.rob_idx` indexing and the old base-overwrite guard:

```cpp
    uint8_t base = meta.rob_idx;
    uint8_t off = read_arrival_offset_[base];
    if (!(off < read_range_len_[base])) {
        assert(false && "nmu::Rob::pop_r_staged: R beat past burst length (Family C: "
                        "more beats than reserved for this base -- malformed burst)");
        std::abort();
    }
    std::size_t slot_idx = static_cast<std::size_t>(base) + off;
    if (!(slot_idx < ROB_CAPACITY)) {
        assert(false && "computed read slot out of range");
        std::abort();
    }
    auto& slot = read_entries_[slot_idx];
    if (!(slot.occupied && !slot.ready)) {
        assert(false && "computed read slot unallocated or already filled");
        std::abort();
    }
    slot.r_beat = r;
    slot.ready = true;
    ++read_arrival_offset_[base];
    uint8_t id = slot.axi_id;
```

Note: `slot.axi_id` is set at `push_ar` for every reserved slot, so `id` is valid for `base+off`.

- [ ] **Step 7: Reset the counter when the range is popped**

In the chain-flush loop (~372-388), after `read_order_by_id_[id].pop_front();`, add the reset keyed by the popped range base:

```cpp
        read_arrival_offset_[head.base] = 0;
        read_order_by_id_[id].pop_front();
```

(Place the reset immediately before or after `pop_front()` on `head` — `head.base` is the popped range's base.)

- [ ] **Step 8: clang-format and build**

Run: `clang-format -i c_model/include/nmu/rob.hpp c_model/tests/nmu/test_rob.cpp && cmake --build build`
Expected: builds clean.

- [ ] **Step 9: Run the two tests to verify they PASS**

Run: `ctest --test-dir build -R "RobDrainHook.FiresOncePerIdDrain_Enabled|NmuRob.ReadFillSameBaseRobIdxLandsInOrder" -V`
Expected: PASS.

- [ ] **Step 10: Run the full rob test suite**

Run: `ctest --test-dir build -R "Rob|NmuRob" -V`
Expected: all PASS (no regression in Disabled-mode or other Enabled tests).

- [ ] **Step 11: Commit**

```bash
git add c_model/include/nmu/rob.hpp c_model/tests/nmu/test_rob.cpp
git commit -m "fix(rob): per-base arrival counter for Enabled multi-beat read (Family C)"
```

---

### Task 2: Defensive boundary tests (lock the computed-slot guard)

**Files:**
- Test: `c_model/tests/nmu/test_rob.cpp` (append new tests)

**Interfaces:**
- Consumes: Task 1's guards in `pop_r_staged` — abort on `off >= read_range_len_[base]`, on out-of-range `slot_idx`, and on `!occupied || ready`; counter reset on range-pop.
- Produces: characterization tests that pin guard behavior against regression.

- [ ] **Step 1: Write death test — extra beat past burst length**

Append to `test_rob.cpp`. Reuse a local `push_r(rob_idx, rlast, marker)` lambda like Task 1 Step 2.

```cpp
TEST(NmuRobDeath, ReadExtraBeatPastBurstLengthAborts) {
    SCENARIO("Enabled read ROB: a 3rd R beat for a 2-beat burst (offset == len) aborts "
             "rather than writing into an adjacent burst's slot.");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    axi::ArBeat ar = make_ar(0x05, 0x100);
    ar.len = 1;  // 2 beats
    ASSERT_TRUE(rob.push_ar(ar));

    auto push_r = [&](bool rlast) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_R);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("last", 1);
        f.set_header_field("rob_req", 1);
        f.set_header_field("rob_idx", 0);
        f.set_payload_field("R", "rid", 0x05);
        f.set_payload_field("R", "rresp", 0);
        f.set_payload_field("R", "rlast", rlast ? 1u : 0u);
        std::array<uint8_t, 32> d{};
        f.set_payload_bytes("R", "rdata", d.data(), 256);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
    };
    push_r(false);  // slot 0
    push_r(false);  // slot 1 (no rlast yet)
    push_r(false);  // 3rd beat: offset==2==len -> abort
    EXPECT_DEATH({ depkt.tick(); rob.pop_r(); rob.pop_r(); rob.pop_r(); }, ".*");
}
```

- [ ] **Step 2: Write test — sequential reuse of same base starts at offset 0**

```cpp
TEST(NmuRob, ReadSameBaseReuseStartsAtZero) {
    SCENARIO("Enabled read ROB: after a burst fully commits and frees base 0, a new "
             "burst that reuses base 0 starts its arrival counter at 0.");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    auto push_r = [&](uint8_t id, bool rlast, uint8_t marker) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_R);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("last", 1);
        f.set_header_field("rob_req", 1);
        f.set_header_field("rob_idx", 0);
        f.set_payload_field("R", "rid", id);
        f.set_payload_field("R", "rresp", 0);
        f.set_payload_field("R", "rlast", rlast ? 1u : 0u);
        std::array<uint8_t, 32> d{};
        d[0] = marker;
        f.set_payload_bytes("R", "rdata", d.data(), 256);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
    };

    // Burst 1: id=5, len=1 -> base 0, 2 beats. Fill, drain, free.
    axi::ArBeat ar1 = make_ar(0x05, 0x100);
    ar1.len = 1;
    ASSERT_TRUE(rob.push_ar(ar1));
    push_r(0x05, false, 0xB0);
    push_r(0x05, true, 0xB1);
    depkt.tick();
    ASSERT_TRUE(rob.pop_r().has_value());
    ASSERT_TRUE(rob.pop_r().has_value());

    // Burst 2 reuses base 0 (slots freed). Counter must start at 0 again.
    axi::ArBeat ar2 = make_ar(0x06, 0x200);
    ar2.len = 1;
    ASSERT_TRUE(rob.push_ar(ar2));
    push_r(0x06, false, 0xC0);
    push_r(0x06, true, 0xC1);
    depkt.tick();
    auto r0 = rob.pop_r();
    auto r1 = rob.pop_r();
    ASSERT_TRUE(r0.has_value());
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r0->data[0], 0xC0);
    EXPECT_EQ(r1->data[0], 0xC1);
}
```

- [ ] **Step 3: Build, run new tests**

Run: `clang-format -i c_model/tests/nmu/test_rob.cpp && cmake --build build && ctest --test-dir build -R "NmuRobDeath.ReadExtraBeatPastBurstLengthAborts|NmuRob.ReadSameBaseReuseStartsAtZero" -V`
Expected: both PASS.

- [ ] **Step 4: Commit**

```bash
git add c_model/tests/nmu/test_rob.cpp
git commit -m "test(rob): defensive boundary tests for Family C read offset guard"
```

---

### Task 3: Same-id cross-dst interleave test (per-base isolation)

**Files:**
- Test: `c_model/tests/nmu/test_rob.cpp` (append)

**Interfaces:**
- Consumes: Task 1 per-base counter keyed by `rob_idx`.
- Produces: the regression that justifies per-base over per-id — two same-id bursts to different dst whose R beats interleave still fill their own ranges.

- [ ] **Step 1: Write the interleave test**

Two AR with the SAME id=5 but different addr (different dst), allocating distinct bases. Then interleave their R beats and assert each base fills correctly and both bursts drain in AR order.

```cpp
TEST(NmuRob, ReadSameIdDifferentDstInterleavedFilesPerBase) {
    SCENARIO("Enabled read ROB: two same-id bursts to different dst get distinct bases; "
             "interleaved R beats fill per base, not per id. Egress holds AR order.");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    // Burst A: id=5, len=1 -> base 0 (slots 0..1). Burst B: id=5, len=1 -> base 2 (slots 2..3).
    axi::ArBeat arA = make_ar(0x05, 0x100);
    arA.len = 1;
    axi::ArBeat arB = make_ar(0x05, 0x200);
    arB.len = 1;
    ASSERT_TRUE(rob.push_ar(arA));
    ASSERT_TRUE(rob.push_ar(arB));

    auto push_r = [&](uint8_t base, bool rlast, uint8_t marker) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_R);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("last", 1);
        f.set_header_field("rob_req", 1);
        f.set_header_field("rob_idx", base);
        f.set_payload_field("R", "rid", 0x05);
        f.set_payload_field("R", "rresp", 0);
        f.set_payload_field("R", "rlast", rlast ? 1u : 0u);
        std::array<uint8_t, 32> d{};
        d[0] = marker;
        f.set_payload_bytes("R", "rdata", d.data(), 256);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
    };

    // Interleave: A0, B0, A1(last), B1(last). Each carries its own base.
    push_r(/*base=*/0, false, 0xA0);
    push_r(/*base=*/2, false, 0xB0);
    push_r(/*base=*/0, true, 0xA1);
    push_r(/*base=*/2, true, 0xB1);
    depkt.tick();

    // Egress order = AR order: burst A (0xA0,0xA1) fully, then burst B (0xB0,0xB1).
    std::vector<uint8_t> got;
    for (int i = 0; i < 8 && got.size() < 4; ++i) {
        auto r = rob.pop_r();
        if (r.has_value()) got.push_back(r->data[0]);
    }
    ASSERT_EQ(got.size(), 4u);
    EXPECT_EQ(got[0], 0xA0);
    EXPECT_EQ(got[1], 0xA1);
    EXPECT_EQ(got[2], 0xB0);
    EXPECT_EQ(got[3], 0xB1);
}
```

- [ ] **Step 2: Build, run**

Run: `clang-format -i c_model/tests/nmu/test_rob.cpp && cmake --build build && ctest --test-dir build -R "NmuRob.ReadSameIdDifferentDstInterleavedFilesPerBase" -V`
Expected: PASS. If it fails on egress order, confirm `read_order_by_id_[5]` holds {base0, base2} in AR order and the chain-flush emits base0's range before base2's.

- [ ] **Step 3: Run full c_model suite to confirm no regression**

Run: `ctest --test-dir build`
Expected: all PASS (baseline was 550/550).

- [ ] **Step 4: Commit**

```bash
git add c_model/tests/nmu/test_rob.cpp
git commit -m "test(rob): same-id cross-dst interleave proves per-base isolation"
```

---

### Task 4: Co-sim Enabled wiring (DPI + wrap + codegen + topology)

**Files:**
- Modify: `c_model/include/wrap/nmu_wrap.hpp` (`init` ~46), `c_model/include/wrap/nsu_wrap.hpp` (`init`)
- Modify: `sim/c/cmodel_dpi.h` (~154, ~186), `sim/c/cmodel_dpi.cpp` (~783, ~903)
- Modify: `sim/tools/gen_tb_top.py` (DPI imports ~454-457; create call site ~480+), `sim/build_config.mk`
- Create: `sim/topologies/mesh_2x2_vc2_rob.yaml` (or chosen Enabled topology)

**Interfaces:**
- Consumes: Task 1 ROB Enabled read path.
- Produces: a co-sim build/TB where `read_rob_mode = write_rob_mode = Enabled`, selected per-topology, without changing the existing `cmodel_nmu_create` / `cmodel_nsu_create` signatures or Disabled TBs.

- [ ] **Step 1: Codex-confirm the wiring mechanism**

Before coding, run a read-only Codex review of the three candidate mechanisms and pick one:
  (a) new `cmodel_nmu_create_ex(name, src_id, num_vc, rob_mode)` + codegen emits `_ex` when topology flags `rob_mode: enabled`; old create kept as Disabled wrapper (recommended in spec);
  (b) build-time define `-DCMODEL_ROB_ENABLED` consumed in `NmuWrap::init`;
  (c) topology YAML flag threaded only through `NmuWrap::init` default arg.

Command:
```bash
codex exec -s read-only --skip-git-repo-check -C "$(pwd)" -o review.md - <<'PROMPT'
Review wiring options for enabling ROB Enabled mode in co-sim without changing the
existing cmodel_nmu_create/cmodel_nsu_create DPI signatures or affecting Disabled
regression TBs. Read sim/c/cmodel_dpi.cpp:783-920, sim/tools/gen_tb_top.py:440-520,
c_model/include/wrap/nmu_wrap.hpp:46-64. Recommend (a) _ex DPI, (b) build define, or
(c) wrap default-arg, with the least codegen churn and zero risk to the handle ABI.
PROMPT
```
Report the verdict, then implement the chosen option in the remaining steps. The steps
below assume (a); adjust if Codex recommends otherwise.

- [ ] **Step 2: Add rob_mode arg to NmuWrap::init and NsuWrap::init (default Disabled)**

In `nmu_wrap.hpp` `init`, add a trailing parameter and wire it:

```cpp
    void init(uint8_t src_id = 0, uint8_t num_vc = 1, std::size_t queue_depth = kPoCAxiQueueDepth,
              nmu::RobMode rob_mode = nmu::RobMode::Disabled) {
        ...
        cfg.read_rob_mode = rob_mode;
        cfg.write_rob_mode = rob_mode;
        ...
    }
```

Mirror in `nsu_wrap.hpp` if NSU config has an equivalent mode field; if NSU has no ROB mode, document that only NMU carries the mode and the NSU `_ex` simply matches the signature for symmetry.

- [ ] **Step 3: Add `_ex` DPI functions, keep old as Disabled wrappers**

In `cmodel_dpi.h` after line 154 and 186, declare:

```c
unsigned long long cmodel_nmu_create_ex(const char* name, int src_id, int num_vc, int rob_enabled);
unsigned long long cmodel_nsu_create_ex(const char* name, int src_id, int num_vc, int rob_enabled);
```

In `cmodel_dpi.cpp`, refactor `cmodel_nmu_create` to call a static helper with `rob_enabled=0`, and add `_ex` calling it with the passed flag. The helper passes `rob_enabled ? RobMode::Enabled : RobMode::Disabled` into `adapter->init(...)`. Repeat for NSU.

- [ ] **Step 4: Add `rob_mode` flag to topology YAML + build_config.mk**

Create `sim/topologies/mesh_2x2_vc2_rob.yaml` mirroring an existing 2x2 vc2 topology plus:

```yaml
  rob_mode: enabled
```

In `build_config.mk`, parse the flag (default disabled) and expose it to `gen_tb_top.py` (follow the existing `num_vc` extraction pattern at `build_config.mk:74`).

- [ ] **Step 5: gen_tb_top.py emits `_ex` create + import when rob_mode enabled**

In `gen_tb_top.py`, read `topo["topology"].get("rob_mode", "disabled")`. When `enabled`, emit the `_ex` DPI import lines (4-arg) and the per-node create call passing `rob_enabled=1`. When disabled, emit the existing 3-arg create unchanged. Keep the Disabled path byte-identical so existing TBs do not regenerate differently.

- [ ] **Step 6: Build the Enabled topology co-sim (clean)**

Run: `rm -rf build/verilator/obj_dir_mesh_2x2_vc2_rob && make sim TB=mesh_2x2_vc2_rob PATTERN=neighbor PYTHON3=python3`
Expected: builds and runs clean (a non-burst pattern first — smoke test the wiring before stressing the read path).

- [ ] **Step 7: Confirm existing Disabled TBs unchanged**

Run: `make sim TB=mesh_4x4_vc2 PATTERN=neighbor PYTHON3=python3` and diff the generated `tb_top` against the pre-change version.
Expected: identical generated SV for Disabled topologies (no `_ex`).

- [ ] **Step 8: Commit**

```bash
git add c_model/include/wrap/nmu_wrap.hpp c_model/include/wrap/nsu_wrap.hpp sim/c/cmodel_dpi.h sim/c/cmodel_dpi.cpp sim/tools/gen_tb_top.py sim/build_config.mk sim/topologies/mesh_2x2_vc2_rob.yaml
git commit -m "feat(sim): ROB Enabled co-sim path via _ex DPI + topology flag"
```

---

### Task 5: Co-sim Enabled end-to-end validation (multi-beat read)

**Files:**
- No source changes — validation only. May add a regression note under `sim/test_patterns/README.md` if a new pattern combo is pinned.

**Interfaces:**
- Consumes: Task 1-4. Enabled topology runs multi-beat reads through the real NSU stamping + NMU per-base counter.

- [ ] **Step 1: Run BUR-002 (8-beat read) on the Enabled topology**

Run:
```bash
rm -rf build/verilator/obj_dir_mesh_2x2_vc2_rob
make sim TB=mesh_2x2_vc2_rob PATTERN=hotspot HOTSPOT=0 \
    BASE=sim/test_patterns/AX4-BUR-002_incr_8beat/scenario.yaml PYTHON3=python3
```
Expected: scoreboard write->readback compare clean, 0 mismatch, no abort. (Before this fix the same run on an Enabled ROB would abort at the Family C guard; on Disabled it was the 64-mismatch / 0x40-offset Family B symptom — confirm neither occurs.)

- [ ] **Step 2: Run a cross-read interleave stress (hotspot, multi-node reads to same target)**

Run the hotspot pattern that makes multiple nodes read the same destination so R beats from distinct reads interleave in the fabric:
```bash
make sim TB=mesh_2x2_vc2_rob PATTERN=hotspot HOTSPOT=0 PYTHON3=python3
```
Expected: clean. This exercises per-base isolation in co-sim (the integration-level analog of Task 3).

- [ ] **Step 3: Full Disabled regression unchanged**

Run the prior-green Disabled multibeat set on at least one existing TB:
```bash
make sim TB=mesh_4x4_vc2 PATTERN=hotspot HOTSPOT=0 \
    BASE=sim/test_patterns/AX4-BUR-002_incr_8beat/scenario.yaml PYTHON3=python3
```
Expected: same result as before this branch (no new failure introduced by the wiring).

- [ ] **Step 4: Record results + commit (if a regression note was added)**

```bash
git add sim/test_patterns/README.md
git commit -m "docs(sim): pin ROB Enabled multi-beat read co-sim result"
```

If no file changed, skip the commit and report the run results in the task summary.

---

## Self-Review

**Spec coverage:**
- per-base counter mechanism -> Task 1 (state, push_ar len, pop_r_staged rewrite, range-pop reset).
- computed-slot guard replacing base-overwrite guard + `offset < len` -> Task 1 Step 6; locked by Task 2.
- reset on range-pop not rlast -> Task 1 Step 7.
- rewrite FiresOnce + invert guard test -> Task 1 Steps 1-2.
- defensive tests (extra beat, reuse-zero) -> Task 2; (early rlast / missing rlast) covered by `offset<len` + range-ready semantics, with the extra-beat death test as the concrete malformed-count guard.
- same-id cross-dst interleave -> Task 3.
- write path unchanged -> no task touches `pop_b_staged` (explicit).
- co-sim Enabled via `_ex` DPI, old signature kept, Disabled TBs untouched -> Task 4.
- end-to-end multi-beat read + interleave + Disabled regression -> Task 5.

**Placeholder scan:** Task 4 Step 1 defers the wiring-mechanism choice to a Codex gate by design (spec marked it code-level); steps 2-8 give concrete code for the recommended option (a). No "TBD" left in the fix itself.

**Type consistency:** `read_arrival_offset_` / `read_range_len_` (`std::array<uint8_t, ROB_CAPACITY>`) used consistently. `rob_mode` arg is `nmu::RobMode`; DPI `_ex` uses `int rob_enabled`. `RobMode::Enabled/Disabled` match `nmu.hpp:20`.
