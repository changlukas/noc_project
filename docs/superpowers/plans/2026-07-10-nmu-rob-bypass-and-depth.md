# NMU RoB — FlooNoC alignment, sizeable pools, bypass clause 1 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make every shape `nmu::Rob` asserts a decision someone made, taking FlooNoC's where one exists; then port bypass clause 1, which is what lets a read burst longer than the pool complete at all.

**Architecture:** `rob_idx` widens to 8 bits so a pool can be sized up to 256 entries; the pool depth becomes a runtime parameter, defaulted to today's 32. The first-fit scan becomes FlooNoC's leading-zero-count high-water allocator, which is a stack. A reordered read burst releases beat by beat. Finally, a transaction whose AXI ID has nothing in flight allocates no slot at all, and its response is forwarded directly.

**Tech Stack:** C++17 header-only c_model, GoogleTest (ctest), Python specgen codegen, Verilator co-sim through a C DPI shim.

**Spec:** `docs/superpowers/specs/2026-07-10-nmu-rob-bypass-and-depth-design.md` — read D1 through D9 before starting. Decision letters are cited per task.
**Baseline:** `docs/nmu-rob-microarchitecture.md`.

## Global Constraints

- Build and run everything on WSL, never on Windows. `BUILD_ROOT := $(HOME)/noc_build` comes from the gitignored repo-root `local.mk`, which already exists. Do not create a build tree under `/mnt/e`.
- `make test` runs ctest and must be green at the end of every task. Baseline before Task 1 is 397/397.
- Run `clang-format -i` on every `.hpp`/`.cpp` you touch. Config at repo root: Google base, `IndentWidth 4`, `ContinuationIndentWidth 4`, `ColumnLimit 100`.
- Naming: `snake_case` for variables and methods, `PascalCase` for types, full words, no `camelCase`.
- Commit message format `type(scope): description`, English. Valid types: `feat` `fix` `docs` `style` `refactor` `test` `chore` `perf` `build` `revert`.
- Never `--no-verify`. Never disable a test to make a build pass.
- Do **not** push. Stop at the working tree; the user reviews before any push.
- If a task's instructions contradict what the code actually does, or the environment breaks, **stop and report BLOCKED**. Do not invent a workaround and commit it.
- `rob.hpp:33-36` claims the tick order is "drain B/R before forwarding AW/W/AR". That is true of standalone `AxiSlavePort::tick()` and false of `Nmu::tick()`, which runs the request side first (`nmu.hpp:291-294`). Do not build any argument on it. Fix the comment in Task 1.

## File Structure

| file | responsibility after this plan |
|---|---|
| `specgen/generated/json/ni_packet.json` | spec authority for the wire header. `ROB_IDX_WIDTH` lives here. |
| `src/c_model/include/nmu/rob.hpp` | all RoB state and policy: depths, allocator, bypass decision, per-ID order lists, per-beat release |
| `src/c_model/include/nmu/nmu.hpp` | `NmuConfig` carries the three parameters; the staged response pipeline forwards `rob_req` instead of hardcoding it |
| `src/c_model/include/wrap/wrap_defaults.hpp` | the three defaults, beside `kMetaBufferMaxOutstanding` |
| `src/c_model/include/wrap/nmu_wrap.hpp` | co-sim override |
| `src/dpi/cmodel_dpi.{h,cpp}` | `cmodel_nmu_create_ex` carries the three |
| `sim/tools/gen_tb_top.py` | plusargs → DPI create call |
| `sim/verilator/Makefile`, root `Makefile` | `B_ROB_DEPTH` `R_ROB_DEPTH` `MAX_TXNS_PER_ID` `BURST_LEN` |
| `src/c_model/tests/nmu/test_rob.cpp` | the whole behavioural contract |

`nmu/port_params.hpp` and `c_model/config/port_params.yaml` are deliberately **not** touched: `load_nmu_port_params` has only test callers and none constructs a `Rob`, so a YAML block there would have zero consumers.

## Task order and why

Each behaviour change is isolated in its own commit so a regression can be bisected to one decision.

| task | decision | behaviour changes? |
|---|---|---|
| 1 | D1, D1a, D1b, D2 | **no** — pool depth defaults to today's 32 |
| 2 | D7 allocator | **yes** — the pool becomes a stack |
| 3 | D8 per-beat release | **yes** — beats leave as they land |
| 4 | parameter plumbing | no |
| 5 | committed-entry `rob_req` | no |
| 6 | D4 `max_txns_per_id` | no at the default |
| 7 | D9 clause 1 + test migration | **yes** |
| 8 | burst co-sim | — |
| 9 | docs | — |

---

### Task 1: Widen `rob_idx` to 8 bits, make the pool depth a parameter

You cannot widen the index without deciding what the pool depth now is, so these land together. Depth
defaults to 32, so nothing observable changes. Spec: D1, D1a, D1b, D2.

**Files:**
- Modify: `specgen/generated/json/ni_packet.json`
- Regenerate: `specgen/generated/cpp/ni_flit_constants.h`, `specgen/generated/sv/ni_flit_pkg.sv`
- Modify: `src/c_model/include/nmu/rob.hpp`
- Modify: `src/c_model/tests/nmu/test_rob.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `Rob::ROB_IDX_SPACE` (`static constexpr std::size_t`, value 256). `Rob::ROB_CAPACITY` no longer exists. `Rob(NmuPacketizeSink&, ResponseDepacketizer&, RobMode mode_w, RobMode mode_r, addr_trans::SamTable sam, std::size_t b_rob_depth = 32, std::size_t r_rob_depth = 32)`. Accessors `b_rob_depth()`, `r_rob_depth()`, `free_write_slots()`, `free_read_slots()` — all `std::size_t`, `const noexcept`.

- [ ] **Step 1: Record the baseline**

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make test 2>&1 | grep -E "tests passed|tests failed"'
```
Expected: `100% tests passed, 0 tests failed out of 397`. If not, **stop and report BLOCKED** — the
baseline is wrong and every later "no regression" claim is meaningless.

- [ ] **Step 2: Widen the spec field**

`specgen/generated/json/ni_packet.json`, in `flit.field_widths`:

```json
      "ROB_IDX_WIDTH": 8,
```

`rsvd` is a derived padding field sized `HEADER_TOTAL_WIDTH` minus everything else
(`specgen/ni_spec/constants.py:225-248`), so it absorbs the three bits. Nothing else in the JSON changes.

- [ ] **Step 3: Regenerate and confirm the wire is unchanged**

```bash
cd specgen && python3 tools/codegen.py --target cpp --domain packet --out generated/cpp/ \
                && python3 tools/codegen.py --target sv --domain packet --out generated/sv/ && cd ..
grep -E "ROB_IDX_(WIDTH|LSB|MSB)|RSVD_(WIDTH|LSB|MSB)|constexpr int (FLIT_WIDTH|HEADER_WIDTH|PAYLOAD_WIDTH)" \
     specgen/generated/cpp/ni_flit_constants.h | head -12
```
Expected: `ROB_IDX_WIDTH = 8`, `ROB_IDX_LSB = 24`, `ROB_IDX_MSB = 31`, `RSVD_WIDTH = 24`,
`RSVD_LSB = 32`, and **`FLIT_WIDTH = 408`, `HEADER_WIDTH = 56`, `PAYLOAD_WIDTH = 352` unchanged**.

If `FLIT_WIDTH` moved, **stop and report BLOCKED**: spec D1 is wrong, and `cmodel_dpi.cpp:32`'s
`static_assert(::ni::FLIT_WIDTH == 408)` would block the build anyway.

- [ ] **Step 4: Run the specgen drift gate**

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make specgen_pytest 2>&1 | tail -5'
```
`specgen/tests/test_codegen.py:144` carries a comment mentioning `rob_idx=7` as the minimal-header
value. If a test asserts on it, update the assertion; if it is only a comment, update the comment.

- [ ] **Step 5: Rename, and widen the six overflow sites**

In `rob.hpp`, replace every `ROB_CAPACITY` with `ROB_IDX_SPACE` and update the declaration:

```cpp
    // === Enabled mode public constants (for testing + caller info) ===
    // Addressable range of the rob_idx header field, NOT the pool depth.
    // Pool depths are b_rob_depth_ / r_rob_depth_ and may be smaller.
    static constexpr std::size_t ROB_IDX_SPACE = 1u << ni::header::ROB_IDX_WIDTH;  // 256
    // AXI ID space alias — single source of truth lives in axi::AXI_ID_SPACE.
    static constexpr std::size_t AXI_ID_SPACE = axi::AXI_ID_SPACE;  // 256
```

A read range holds `n = len + 1` beats, up to 256. Spec D1b lists six `uint8_t` sites; three are
storage, three are loop machinery, and the loop machinery is the dangerous half — `uint8_t i` never
reaches 256, so it hangs rather than truncates.

```cpp
    struct BeatRange {
        uint8_t  base;
        uint16_t len_plus_1;   // up to 256
    };
```
```cpp
    std::array<uint16_t, ROB_IDX_SPACE> read_arrival_offset_{};
    std::array<uint16_t, ROB_IDX_SPACE> read_range_len_{};
```
and in `pop_r_staged`'s drain loop (`rob.hpp:356,363,364`):

```cpp
        for (std::size_t i = 0; i < head.len_plus_1; ++i) {
            if (!read_entries_[head.base + i].ready) { all_ready = false; break; }
        }
        if (!all_ready) break;
        for (std::size_t i = 0; i < head.len_plus_1; ++i) {
            const std::size_t idx = static_cast<std::size_t>(head.base) + i;
            committed_r_queue_.push_back(
                {read_entries_[idx].r_beat, static_cast<uint8_t>(idx), id});
            ++committed_r_pending_[idx];
        }
```

`rob_idx` itself stays `uint8_t` everywhere: an 8-bit index addresses `0..255` exactly.

Also fix the stale tick-order comment at `rob.hpp:33-36` while you are here (see Global Constraints).

- [ ] **Step 6: Add the depth parameters**

Depth is enforced by marking only `[0, depth)` free at construction. Allocation draws exclusively from
free bits and release touches only previously-allocated indices, so bits `>= depth` stay `0` forever.

```cpp
    Rob(NmuPacketizeSink& next_pkt, ResponseDepacketizer& next_depkt, RobMode mode_w,
        RobMode mode_r, addr_trans::SamTable sam, std::size_t b_rob_depth = 32,
        std::size_t r_rob_depth = 32)
        : next_pkt_(next_pkt),
          next_depkt_(next_depkt),
          mode_w_(mode_w),
          mode_r_(mode_r),
          sam_(std::move(sam)),
          b_rob_depth_(b_rob_depth),
          r_rob_depth_(r_rob_depth) {
        assert(b_rob_depth_ >= 1 && b_rob_depth_ <= ROB_IDX_SPACE &&
               "nmu::Rob: b_rob_depth outside [1, ROB_IDX_SPACE]");
        assert(r_rob_depth_ >= 1 && r_rob_depth_ <= ROB_IDX_SPACE &&
               "nmu::Rob: r_rob_depth outside [1, ROB_IDX_SPACE]");
        if (b_rob_depth_ < 1 || b_rob_depth_ > ROB_IDX_SPACE) std::abort();
        if (r_rob_depth_ < 1 || r_rob_depth_ > ROB_IDX_SPACE) std::abort();
        for (std::size_t i = 0; i < b_rob_depth_; ++i) free_write_entries_.set(i);
        for (std::size_t i = 0; i < r_rob_depth_; ++i) free_read_entries_.set(i);
    }
```

Delete the old `free_write_entries_.set(); free_read_entries_.set();` — they set all 256 bits.

Public, beside `write_occupancy()`:

```cpp
    std::size_t b_rob_depth() const noexcept { return b_rob_depth_; }
    std::size_t r_rob_depth() const noexcept { return r_rob_depth_; }
    // Free Enabled-mode slots. Slots >= depth are never free.
    std::size_t free_write_slots() const noexcept { return free_write_entries_.count(); }
    std::size_t free_read_slots() const noexcept { return free_read_entries_.count(); }
```

Private, beside `sam_`:

```cpp
    std::size_t b_rob_depth_;
    std::size_t r_rob_depth_;
```

In `push_ar`, the oversized-burst early-out now compares against the pool, not the index space:

```cpp
        if (n > r_rob_depth_) return false;
```

- [ ] **Step 7: Fix the one test that assumes a 32-wide bitset**

`Enabled_FindConsecutiveFree_FragmentedFailNoConsecutiveRun` (`test_rob.cpp:254-276`) does `free.set()`
then expects `find_consecutive_free(free, 33) == -1`. With a 256-bit bitset it now finds a run. Retarget
it to the index space (the function is deleted in Task 2, but the tree must be green here). Rename its
`std::bitset<Rob::ROB_CAPACITY>` to `std::bitset<Rob::ROB_IDX_SPACE>`, then:

```cpp
    // All free: n up to ROB_IDX_SPACE succeeds at base=0; one more fails.
    free.set();
    EXPECT_EQ(Rob::find_consecutive_free(free, 1), 0);
    EXPECT_EQ(Rob::find_consecutive_free(free, Rob::ROB_IDX_SPACE), 0);
    EXPECT_EQ(Rob::find_consecutive_free(free, Rob::ROB_IDX_SPACE + 1), -1);
```

- [ ] **Step 8: Write the new tests**

```cpp
TEST(NmuRob, Enabled_ConstructorMarksOnlyDepthSlotsFree) {
    SCENARIO("Rob Enabled: only [0, depth) is free at construction; slots above it never are");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled, legacy_sam(), 4, 8);
    EXPECT_EQ(rob.b_rob_depth(), 4u);
    EXPECT_EQ(rob.r_rob_depth(), 8u);
    EXPECT_EQ(rob.free_write_slots(), 4u);
    EXPECT_EQ(rob.free_read_slots(), 8u);
}

TEST(NmuRob, Enabled_DefaultDepthIsThirtyTwo) {
    SCENARIO("Rob Enabled: the default pool is 32 entries, not the 256-entry index space");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled, legacy_sam());
    EXPECT_EQ(rob.b_rob_depth(), 32u);
    EXPECT_EQ(rob.r_rob_depth(), 32u);
    EXPECT_EQ(Rob::ROB_IDX_SPACE, 256u);
}

TEST(NmuRob, Enabled_MaxBurst_LenPlus1DoesNotOverflow) {
    SCENARIO("Rob Enabled: a 256-beat AR into a 256-deep read pool allocates 256 slots");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled, legacy_sam(),
            Rob::ROB_IDX_SPACE, Rob::ROB_IDX_SPACE);

    axi::ArBeat ar = make_ar(0x05, 0x100);
    ar.len = 255;  // 256 beats: len_plus_1 does not fit in uint8_t
    ASSERT_TRUE(rob.push_ar(ar));
    EXPECT_EQ(rob.free_read_slots(), 0u) << "all 256 slots consumed";
    auto f = *ar_cap.pop();
    EXPECT_EQ(f.get_header_field("rob_idx"), 0u);
    EXPECT_EQ(f.get_header_field("rob_req"), 1u);
}

TEST(NmuRobDeath, Enabled_DepthZeroAborts) {
    SCENARIO("Rob: b_rob_depth = 0 is rejected at construction");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    EXPECT_DEATH(
        { Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled, legacy_sam(), 0, 32); }, ".*");
}

TEST(NmuRobDeath, Enabled_DepthAboveIdxSpaceAborts) {
    SCENARIO("Rob: r_rob_depth > ROB_IDX_SPACE is rejected at construction");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    EXPECT_DEATH({ Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled, legacy_sam(), 32,
                           Rob::ROB_IDX_SPACE + 1); },
                 ".*");
}
```

`Enabled_MaxBurst_LenPlus1DoesNotOverflow` is the one that fails on `uint8_t`. Run it against the
un-widened code once, watch it hang or abort, then apply Step 5.

The existing death suite is named `NmuRobDeath` (`test_rob.cpp:523`); match it, do not invent
`NmuRobDeathTest`.

- [ ] **Step 9: Build, format, test**

```bash
clang-format -i src/c_model/include/nmu/rob.hpp src/c_model/tests/nmu/test_rob.cpp
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make test 2>&1 | grep -E "tests passed|tests failed"'
```
Expected: all pass, count above 397.

- [ ] **Step 10: Confirm the co-sim still runs**

The header changed. Rebuild and run one directed pattern.

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make sim TB=tb_mesh_4x4_vc1_rob PATTERN=neighbor SEED=1 2>&1 | tail -3'
```
Expected: `DIRECTED PASS: ... scoreboard clean, non-vacuous`.

- [ ] **Step 11: Commit**

```bash
git add specgen/generated src/c_model/include/nmu/rob.hpp src/c_model/tests/nmu/test_rob.cpp
git commit -m "feat(nmu): rob_idx 5->8 bits; pool depth becomes a parameter, default 32"
```

---

### Task 2: The allocator becomes FlooNoC's `lzc` high-water mark

Behaviour changes: the pool becomes a stack. Space is recovered only from the top. Spec: D7.

**Files:**
- Modify: `src/c_model/include/nmu/rob.hpp`
- Modify: `src/c_model/tests/nmu/test_rob.cpp`

**Interfaces:**
- Consumes: `ROB_IDX_SPACE`, `b_rob_depth_`, `r_rob_depth_` (Task 1).
- Produces: private `alloc_write_` / `alloc_read_` (`std::bitset<ROB_IDX_SPACE>`, range **tops** only); private static `highest_set(const std::bitset<ROB_IDX_SPACE>&, std::size_t depth) -> int`; public `write_free_space()` / `read_free_space()` (`std::size_t`, `const noexcept`). `free_write_entries_`, `free_read_entries_`, `free_write_slots()`, `free_read_slots()` and `find_consecutive_free` are **deleted**.

- [ ] **Step 1: Write the failing tests**

Delete `Enabled_FindConsecutiveFree_FragmentedFailNoConsecutiveRun` (`test_rob.cpp:254-276`) — the
function it tests is going away.

Release must be driven through the response path, never by calling `commit_r_exit` directly:
`commit_r_exit` opens with `assert(committed_r_pending_[rob_idx] > 0)`, which only a real release sets.

`pop_r()` consumes **one** depacketized flit per call (`rob.hpp:320-323`) and, until Task 3, refuses to
commit anything until every beat of the head burst is ready (`rob.hpp:355-365`). Four injected beats
therefore need more than four `pop_r()` calls. Poll; do not assume one call per beat. This is the
single most likely way to lose a day on this task.

```cpp
TEST(NmuRob, Enabled_LzcAllocator_IsAStack) {
    SCENARIO("Rob Enabled: freeing a low range does not return its space while a higher range lives");
    ChannelModel noc(64, 64);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 64, 64);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled, legacy_sam(), 8, 8);

    axi::ArBeat a = make_ar(0x05, 0x100);
    a.len = 3;  // A occupies [0..3]; only index 3 is marked
    ASSERT_TRUE(rob.push_ar(a));
    axi::ArBeat b = make_ar(0x06, 0x200);
    b.len = 1;  // B occupies [4..5]; only index 5 is marked
    ASSERT_TRUE(rob.push_ar(b));
    EXPECT_EQ(rob.read_free_space(), 2u);

    auto push_r = [&](uint8_t base, bool rlast, uint8_t rid) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_R);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("last", 1);
        f.set_header_field("rob_req", 1);
        f.set_header_field("rob_idx", base);
        f.set_payload_field("R", "rid", rid);
        f.set_payload_field("R", "rresp", 0);
        f.set_payload_field("R", "rlast", rlast ? 1u : 0u);
        std::array<uint8_t, 32> d{};
        f.set_payload_bytes("R", "rdata", d.data(), 256);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
        depkt.tick();
    };

    // Drain A completely. pop_r() pulls ONE depacketized flit per call and, before
    // Task 3, only commits once every beat of the burst is ready — so four beats
    // take more than four calls. Poll. The loop is also correct after Task 3.
    push_r(0, false, 0x05);
    push_r(0, false, 0x05);
    push_r(0, false, 0x05);
    push_r(0, true, 0x05);
    std::size_t got = 0;
    for (int i = 0; i < 32 && got < 4; ++i) {
        if (rob.pop_r().has_value()) ++got;
    }
    ASSERT_EQ(got, 4u) << "all four beats of A must be released";

    EXPECT_EQ(rob.read_free_space(), 2u) << "high-water is still B's top at index 5";
    axi::ArBeat c = make_ar(0x07, 0x300);
    c.len = 3;  // four beats: four slots are notionally free but unreachable
    EXPECT_FALSE(rob.push_ar(c));
}

TEST(NmuRob, Enabled_LzcAllocator_NonTopReleaseDoesNotGrowFreeSpace) {
    SCENARIO("Rob Enabled: clearing a non-highest range top leaves free_space unchanged");
    ChannelModel noc(64, 64);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 64, 64);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled, legacy_sam(), 8, 8);

    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));  // slot 0
    ASSERT_TRUE(rob.push_aw(make_aw(0x06, 0x200)));  // slot 1
    EXPECT_EQ(rob.write_free_space(), 6u);

    auto push_b = [&](uint8_t rob_idx, uint8_t bid) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_B);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("last", 1);
        f.set_header_field("rob_req", 1);
        f.set_header_field("rob_idx", rob_idx);
        f.set_payload_field("B", "bid", bid);
        f.set_payload_field("B", "bresp", 0);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
        depkt.tick();
    };

    push_b(0, 0x05);
    ASSERT_TRUE(rob.pop_b().has_value());
    EXPECT_EQ(rob.write_free_space(), 6u) << "slot 0 is below the high-water mark";
    push_b(1, 0x06);
    ASSERT_TRUE(rob.pop_b().has_value());
    EXPECT_EQ(rob.write_free_space(), 8u) << "the mark cleared; everything is free again";
}

// Depth is a parameter now. The stack allocator must respect it at every legal value.
class RobDepthParam : public ::testing::TestWithParam<std::size_t> {};

TEST_P(RobDepthParam, Enabled_AllocationNeverExceedsDepth) {
    SCENARIO("Rob Enabled: free_space starts at depth and no base ever reaches it");
    const std::size_t depth = GetParam();
    ChannelModel noc(512, 512);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 512, 512);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled, legacy_sam(), depth, depth);

    EXPECT_EQ(rob.write_free_space(), depth);
    EXPECT_EQ(rob.read_free_space(), depth);

    // Distinct ids, so the per-id order list never gates. One slot each.
    for (std::size_t i = 0; i < depth; ++i) {
        ASSERT_TRUE(rob.push_aw(make_aw(static_cast<uint8_t>(i & 0xFF), 0x100))) << "AW " << i;
        auto f = *noc.req_in().pop_flit();
        EXPECT_LT(f.get_header_field("rob_idx"), depth) << "base " << i << " escaped the pool";
    }
    EXPECT_EQ(rob.write_free_space(), 0u);
    EXPECT_FALSE(rob.push_aw(make_aw(0xFF, 0x200)));
}

INSTANTIATE_TEST_SUITE_P(Depths, RobDepthParam,
                         ::testing::Values(1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u, 256u));
```

At `depth = 256` the loop walks ids `0x00..0xFF`, exactly the AXI ID space, so each id gets one
slot and the per-id order list never gates. At smaller depths only a prefix of the ids is used.
`Enabled_AllocationNeverExceedsDepth` is migrated in Task 7 like every other `Enabled_*` test:
after clause 1, each distinct id's first AW bypasses, so the loop must prime each id first, or
reuse one primed id and raise `max_txns_per_id`.

```cpp

TEST(NmuRob, Enabled_LzcAllocator_ReusesFromTheTop) {
    SCENARIO("Rob Enabled: once the bitmap is empty the next base is 0");
    ChannelModel noc(64, 64);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 64, 64);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled, legacy_sam(), 8, 8);

    axi::ArBeat a = make_ar(0x05, 0x100);
    a.len = 3;
    ASSERT_TRUE(rob.push_ar(a));
    ar_cap.pop();

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
        depkt.tick();
    };
    push_r(false);
    push_r(false);
    push_r(false);
    push_r(true);
    std::size_t got = 0;  // see the polling note in Enabled_LzcAllocator_IsAStack
    for (int i = 0; i < 32 && got < 4; ++i) {
        if (rob.pop_r().has_value()) ++got;
    }
    ASSERT_EQ(got, 4u);
    EXPECT_EQ(rob.read_free_space(), 8u);

    axi::ArBeat b = make_ar(0x06, 0x200);
    b.len = 1;
    ASSERT_TRUE(rob.push_ar(b));
    auto f = *ar_cap.pop();
    EXPECT_EQ(f.get_header_field("rob_idx"), 0u);
}
```

- [ ] **Step 2: Run to verify they fail**

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make build 2>&1 | tail -5'
```
Expected: compile error — `read_free_space` / `write_free_space` do not exist.

- [ ] **Step 3: Replace the bitmaps**

```cpp
    // FlooNoC's rob_alloc_q (floo_rob.sv:146): one bit per allocated RANGE, set at
    // the range's top index. Free space is the leading-zero count above it, so the
    // pool behaves as a stack: space returns only from the top.
    std::bitset<ROB_IDX_SPACE> alloc_write_;
    std::bitset<ROB_IDX_SPACE> alloc_read_;
```
Delete `free_write_entries_`, `free_read_entries_` and the constructor loops that seeded them. The depth
invariant no longer needs a seeded bitmap: allocation is bounded by `*_free_space()`, which is derived
from the depth.

- [ ] **Step 4: Implement the allocator**

```cpp
    // Highest set bit below `depth`, or -1 if none. std::bitset has no such query.
    // Scanning above `depth` would be wrong as well as wasteful: a stray bit there
    // makes `depth - 1 - msb` underflow, and std::size_t underflow is silent.
    // Allocation never sets one (base + n - 1 <= depth - 1), so the bound is also
    // the assertion.
    static int highest_set(const std::bitset<ROB_IDX_SPACE>& b, std::size_t depth) {
        for (std::size_t i = depth; i-- > 0;) {
            if (b.test(i)) return static_cast<int>(i);
        }
        return -1;
    }
```
```cpp
    // lzc over the allocation bitmap (floo_rob.sv:155-164). Free space is what lies
    // above the high-water mark; the next base is the index just past it.
    std::size_t write_free_space() const noexcept {
        const int msb = highest_set(alloc_write_, b_rob_depth_);
        return msb < 0 ? b_rob_depth_ : b_rob_depth_ - 1u - static_cast<std::size_t>(msb);
    }
    std::size_t read_free_space() const noexcept {
        const int msb = highest_set(alloc_read_, r_rob_depth_);
        return msb < 0 ? r_rob_depth_ : r_rob_depth_ - 1u - static_cast<std::size_t>(msb);
    }
```

`push_aw`, Enabled branch, `n = 1`:

```cpp
        if (write_free_space() < 1) return false;
        const std::size_t base = b_rob_depth_ - write_free_space();
        auto t = sam_.translate(b.addr);
        if (!next_pkt_.push_aw_with_meta(b, {t.dst_id, t.local_addr, /*rob_req=*/1,
                                             /*rob_idx=*/static_cast<uint8_t>(base)})) {
            return false;  // downstream backpressure: no state mutation
        }
        alloc_write_.set(base);  // a 1-slot range: base is its own top
        write_entries_[base] = WriteEntry{/*occupied=*/true, /*ready=*/false, b.id, {}};
        write_order_by_id_[b.id].push_back({static_cast<uint8_t>(base), 1});
        ++w_burst_credit_;
        return true;
```

`push_ar`, Enabled branch, `n = len + 1`:

```cpp
        const std::size_t n = static_cast<std::size_t>(b.len) + 1u;
        if (read_free_space() < n) return false;   // subsumes the old n > r_rob_depth_ check
        const std::size_t base = r_rob_depth_ - read_free_space();
        auto t = sam_.translate(b.addr);
        if (!next_pkt_.push_ar_with_meta(b, {t.dst_id, t.local_addr, /*rob_req=*/1,
                                             /*rob_idx=*/static_cast<uint8_t>(base)})) {
            return false;
        }
        for (std::size_t i = 0; i < n; ++i) {
            read_entries_[base + i] = ReadEntry{/*occupied=*/true, /*ready=*/false, b.id, {}};
        }
        alloc_read_.set(base + n - 1);   // only the range TOP is marked
        read_range_len_[base] = static_cast<uint16_t>(n);
        read_order_by_id_[b.id].push_back({static_cast<uint8_t>(base), static_cast<uint16_t>(n)});
        return true;
```

`base + n - 1 <= depth - 1` holds by construction: `base = depth - free_space` and `free_space >= n`.
Remove the old `n > r_rob_depth_` early-out, or it becomes dead code.

Release, in `commit_b_exit` / `commit_r_exit`, replaces `free_*_entries_.set(rob_idx)`:

```cpp
        alloc_write_.reset(rob_idx);   // no-op unless rob_idx is a range top
        write_entries_[rob_idx] = WriteEntry{};
```

- [ ] **Step 5: Delete `find_consecutive_free`**

Remove the declaration (`rob.hpp:87`), the definition (`rob.hpp:163-175`) and their comments. Confirm
nothing else refers to the old names:

```bash
grep -rn "find_consecutive_free\|free_write_entries_\|free_read_entries_\|free_write_slots\|free_read_slots" src/
```
Expected: no output.

- [ ] **Step 6: Migrate the two Task-1 tests that read free-slot counts**

`Enabled_ConstructorMarksOnlyDepthSlotsFree` and `Enabled_MaxBurst_LenPlus1DoesNotOverflow` use
`free_write_slots()` / `free_read_slots()`. Retarget both to `write_free_space()` / `read_free_space()`.
The expected values are identical at construction and after a single allocation from an empty pool.

- [ ] **Step 7: Build, format, test**

```bash
clang-format -i src/c_model/include/nmu/rob.hpp src/c_model/tests/nmu/test_rob.cpp
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make test 2>&1 | grep -E "tests passed|tests failed"'
```
Expected: all pass. `Enabled_PushAr_AllocatesConsecutiveSlotsForBurst` (`:228`) still expects bases `0`
then `4`, and the stack allocator produces exactly that. `Enabled_PushAw_PoolFull_ReturnFalseAtomic`
(`:326`) still fills 32 slots and refuses the 33rd.

If a test fails only because a slot below the high-water mark is no longer reusable, that **is** the
behaviour change. Say so in the commit body and adjust the test; do not restore first-fit.

- [ ] **Step 8: Commit**

```bash
git add src/c_model/include/nmu/rob.hpp src/c_model/tests/nmu/test_rob.cpp
git commit -m "refactor(nmu): port floo_rob lzc high-water allocator, replacing the first-fit scan"
```

---

### Task 3: A reordered read burst releases one beat at a time

Behaviour changes: a beat leaves as soon as it lands and every earlier beat of its burst has left.
Spec: D8.

**Files:**
- Modify: `src/c_model/include/nmu/rob.hpp`
- Modify: `src/c_model/tests/nmu/test_rob.cpp`

**Interfaces:**
- Consumes: the allocator from Task 2.
- Produces: private `std::array<uint16_t, ROB_IDX_SPACE> read_release_offset_{}`. No public signature changes.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(NmuRob, Enabled_PerBeatRelease_HeadBurstStreams) {
    SCENARIO("Rob Enabled: beat 0 of the head burst leaves before beats 1-3 have arrived");
    ChannelModel noc(64, 64);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 64, 64);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled, legacy_sam(), 8, 8);

    axi::ArBeat ar = make_ar(0x05, 0x100);
    ar.len = 3;  // 4 beats, base 0
    ASSERT_TRUE(rob.push_ar(ar));

    auto push_r = [&](bool rlast, uint8_t marker) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_R);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("last", 1);
        f.set_header_field("rob_req", 1);
        f.set_header_field("rob_idx", 0);  // the NSU stamps the burst base on every beat
        f.set_payload_field("R", "rid", 0x05);
        f.set_payload_field("R", "rresp", 0);
        f.set_payload_field("R", "rlast", rlast ? 1u : 0u);
        std::array<uint8_t, 32> d{};
        d[0] = marker;
        f.set_payload_bytes("R", "rdata", d.data(), 256);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
        depkt.tick();
    };

    // The range is [0..3]; only index 3 is marked, so free_space is 8 - 1 - 3 = 4.
    EXPECT_EQ(rob.read_free_space(), 4u);

    // After this task, one injected beat yields one pop_r(). Before it, this line
    // is exactly what fails: the burst gate holds beat 0 until beat 3 lands.
    push_r(false, 0xA0);  // only beat 0 has arrived
    ASSERT_TRUE(rob.pop_r().has_value()) << "beat 0 must not wait for beats 1-3";
    EXPECT_FALSE(rob.pop_r().has_value()) << "beat 1 has not arrived";
    EXPECT_EQ(rob.read_free_space(), 4u) << "beat 0 is not the range top; no marker cleared";

    push_r(false, 0xA1);
    ASSERT_TRUE(rob.pop_r().has_value());
    push_r(false, 0xA2);
    ASSERT_TRUE(rob.pop_r().has_value());
    EXPECT_EQ(rob.read_free_space(), 4u) << "still no marker cleared";

    push_r(true, 0xA3);
    ASSERT_TRUE(rob.pop_r().has_value());
    EXPECT_EQ(rob.read_free_space(), 8u) << "the range top cleared, so the pool is empty";
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make test 2>&1 | grep -E "FAILED"'
```
Expected: `Enabled_PerBeatRelease_HeadBurstStreams` fails at the first `ASSERT_TRUE(...has_value())` —
today the whole burst waits.

- [ ] **Step 3: Add the release offset**

```cpp
    // Beats of the head burst released so far, keyed by range base. FlooNoC keys the
    // same counter by ID (read_rob_idx_offset_q, floo_rob.sv:177-180); a range belongs
    // to exactly one ID, so keying by base carries the same information.
    std::array<uint16_t, ROB_IDX_SPACE> read_release_offset_{};
```

- [ ] **Step 4: Rewrite the read drain loop**

Replace the `all_ready` loop in `pop_r_staged` (`rob.hpp:353-370`):

```cpp
    while (!read_order_by_id_[id].empty()) {
        const BeatRange head = read_order_by_id_[id].front();
        uint16_t& release_off = read_release_offset_[head.base];
        while (release_off < head.len_plus_1 && read_entries_[head.base + release_off].ready) {
            const std::size_t idx = static_cast<std::size_t>(head.base) + release_off;
            committed_r_queue_.push_back(
                {read_entries_[idx].r_beat, static_cast<uint8_t>(idx), id});
            ++committed_r_pending_[idx];
            ++release_off;
        }
        if (release_off < head.len_plus_1) break;  // burst not drained yet
        read_arrival_offset_[head.base] = 0;
        release_off = 0;
        read_order_by_id_[id].pop_front();
    }
```

Beats leave in `base + 0, base + 1, ...` order, so AXI4's in-burst ordering holds. A later burst of the
same ID cannot overtake, because its range sits behind this one in the deque.

- [ ] **Step 5: Run the tests, and expect two possible migrations**

```bash
clang-format -i src/c_model/include/nmu/rob.hpp src/c_model/tests/nmu/test_rob.cpp
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make test 2>&1 | grep -E "FAILED|tests passed"'
```

`Enabled_PopR_MultiBeatBurstCommitInOrder` (`:419`) and `ReadFillSameBaseRobIdxLandsInOrder` (`:568`)
inject every beat before popping, so the *sequence* of beats they observe is unchanged and they should
stay green. If either fails because a beat now arrives earlier than it used to, the **sequence** is what
AXI4 requires: assert on the order of `rdata` markers, not on how many `pop_r()` calls return
`nullopt`. Adjust the test and say so in the commit body. Do **not** restore whole-burst release.

- [ ] **Step 6: Commit**

```bash
git add src/c_model/include/nmu/rob.hpp src/c_model/tests/nmu/test_rob.cpp
git commit -m "feat(nmu): release a reordered read burst beat by beat, as floo_rob does"
```

---

### Task 4: Thread the depths from the Makefile to the `Rob` constructor

No behaviour change at the defaults. This exists so the depth sweep can drive the parameters; a
parameter with no caller is dead config.

**Files:**
- Modify: `src/c_model/include/nmu/nmu.hpp`, `src/c_model/include/wrap/wrap_defaults.hpp`, `src/c_model/include/wrap/nmu_wrap.hpp`, `src/dpi/cmodel_dpi.h`, `src/dpi/cmodel_dpi.cpp`, `sim/tools/gen_tb_top.py`, `sim/verilator/Makefile`, `Makefile`

**Interfaces:**
- Consumes: the 7-argument `Rob` constructor from Task 1.
- Produces: `NmuConfig::b_rob_depth`, `NmuConfig::r_rob_depth` (`std::size_t`, default `32`; `nmu.hpp` must not include the wrap header, so the `kRob*Depth` constants are the wrap-layer default, not the struct default). `NmuWrap::init(uint8_t src_id, uint8_t num_vc, std::size_t queue_depth, nmu::RobMode rob_mode, const char* config_path, std::size_t b_rob_depth, std::size_t r_rob_depth)`. `cmodel_nmu_create_ex(const char* name, int src_id, int num_vc, int rob_enabled, int b_rob_depth, int r_rob_depth, const char* config_path)`.

- [ ] **Step 1: Add the defaults**

`src/c_model/include/wrap/wrap_defaults.hpp`, after `kMetaBufferMaxUniqueIds`:

```cpp
// NMU RoB pool depths, per direction. Both <= 1 << ROB_IDX_WIDTH = 256, the
// addressable range of the rob_idx header field. A B entry holds {id, resp};
// an R entry holds one beat of rdata, so the two are sized independently.
constexpr std::size_t kRobBDepth = 32;
constexpr std::size_t kRobRDepth = 32;
```

- [ ] **Step 2: Add the fields to `NmuConfig` and pass them on**

`src/c_model/include/nmu/nmu.hpp`, in `struct NmuConfig`, after `read_rob_mode`:

```cpp
    // RoB pool depths, per direction. Enabled mode only. See
    // docs/nmu-rob-microarchitecture.md section 6.
    std::size_t b_rob_depth = 32;
    std::size_t r_rob_depth = 32;
```

Thread them into the `Rob` construction (`nmu.hpp:283`):

```cpp
      rob_(req_s1_bridge_, depacketize_, cfg_.write_rob_mode, cfg_.read_rob_mode, cfg_.sam,
           cfg_.b_rob_depth, cfg_.r_rob_depth),
```

- [ ] **Step 3: Add the override to `NmuWrap::init`**

`src/c_model/include/wrap/nmu_wrap.hpp`. New parameters last, both defaulted:

```cpp
    void init(uint8_t src_id = 0, uint8_t num_vc = 1, std::size_t queue_depth = kAxiQueueDepth,
              nmu::RobMode rob_mode = nmu::RobMode::Disabled, const char* config_path = nullptr,
              std::size_t b_rob_depth = kRobBDepth, std::size_t r_rob_depth = kRobRDepth) {
```
and inside, next to `cfg.write_rob_mode = rob_mode;`:

```cpp
        cfg.b_rob_depth = b_rob_depth;
        cfg.r_rob_depth = r_rob_depth;
```

- [ ] **Step 4: Extend the DPI**

`src/dpi/cmodel_dpi.h`, replacing the `cmodel_nmu_create_ex` declaration:

```c
unsigned long long cmodel_nmu_create_ex(const char* name, int src_id, int num_vc, int rob_enabled,
                                        int b_rob_depth, int r_rob_depth, const char* config_path);
```

`src/dpi/cmodel_dpi.cpp`:

```cpp
static unsigned long long nmu_create_impl(const char* name, int src_id, int num_vc,
                                          ni::cmodel::nmu::RobMode rob_mode,
                                          const char* config_path, std::size_t b_rob_depth,
                                          std::size_t r_rob_depth) {
    if (g_session_state != SessionState::Initialized) {
        DPI_SET_ERR_IF_CLEAR(CMODEL_DPI_ERR_NOT_INITIALIZED, "cmodel_nmu_create: not initialized");
        return 0ull;
    }
    DPI_BOUNDARY_BEGIN_R(nmu_create_impl, 0ull) {
        auto adapter = std::make_unique<NmuWrap>();
        adapter->init(static_cast<uint8_t>(src_id), static_cast<uint8_t>(num_vc), kAxiQueueDepth,
                      rob_mode, config_path, b_rob_depth, r_rob_depth);
        auto* h = new HandleBlock{
            static_cast<uint32_t>(WrapType::Nmu), WrapType::Nmu, HandleState::Live,
            std::string(name),
            std::unique_ptr<void, void (*)(void*)>(
                adapter.release(), [](void* p) { delete static_cast<NmuWrap*>(p); })};
        g_handle_registry.insert(h);
        return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h));
    }
    DPI_BOUNDARY_END_R(nmu_create_impl);
}

extern "C" unsigned long long cmodel_nmu_create(const char* name, int src_id, int num_vc,
                                                const char* config_path) {
    return nmu_create_impl(name, src_id, num_vc, ni::cmodel::nmu::RobMode::Disabled, config_path,
                           kRobBDepth, kRobRDepth);
}

extern "C" unsigned long long cmodel_nmu_create_ex(const char* name, int src_id, int num_vc,
                                                   int rob_enabled, int b_rob_depth,
                                                   int r_rob_depth, const char* config_path) {
    return nmu_create_impl(
        name, src_id, num_vc,
        rob_enabled ? ni::cmodel::nmu::RobMode::Enabled : ni::cmodel::nmu::RobMode::Disabled,
        config_path, static_cast<std::size_t>(b_rob_depth),
        static_cast<std::size_t>(r_rob_depth));
}
```

- [ ] **Step 5: Extend the testbench generator**

`sim/tools/gen_tb_top.py`. The `rob_enabled` import block (around line 524):

```python
    if rob_enabled:
        w('    import "DPI-C" context function longint unsigned cmodel_nmu_create_ex(input string name,')
        w('                                                                 input int src_id, input int num_vc,')
        w('                                                                 input int rob_enabled,')
        w('                                                                 input int b_rob_depth,')
        w('                                                                 input int r_rob_depth,')
        w('                                                                 input string config_path);')
```

Declare the SV ints beside the existing `max_unique_ids` / `max_outstanding` block (around line 552):

```python
    if rob_enabled:
        w("    // NMU RoB pool depths, per direction. Both <= 256 (rob_idx is 8 bits).")
        w("    int unsigned b_rob_depth = 32;")
        w("    int unsigned r_rob_depth = 32;")
```
and read the plusargs inside the same `initial begin`:

```python
    if rob_enabled:
        w('        void\'($value$plusargs("b_rob_depth=%d", b_rob_depth));')
        w('        void\'($value$plusargs("r_rob_depth=%d", r_rob_depth));')
```

The create call (around line 568):

```python
            w(f'        nmu_ctx[{i}] = cmodel_nmu_create_ex("nmu_{i}", {c}, NUM_VC, 1, '
              f'b_rob_depth, r_rob_depth, sam_config_path);  '
              f'// src_id = node{i} coord {c}, ROB Enabled')
```

- [ ] **Step 6: Add the Makefile knobs**

`sim/verilator/Makefile`, beside `MAX_OUTSTANDING ?= 32`:

```make
# NMU RoB pool depths, per direction. Both <= 256 (rob_idx is 8 bits).
B_ROB_DEPTH     ?= 32
R_ROB_DEPTH     ?= 32
```
and in the `run-directed` recipe's plusarg list, next to `+max_outstanding`:

```make
	    "+b_rob_depth=$(B_ROB_DEPTH)" "+r_rob_depth=$(R_ROB_DEPTH)" \
```

Root `Makefile`, in `_INJ_ARGS`. Its current last line (`MAX_OUTSTANDING`, `Makefile:190`) has **no
trailing backslash** — add one before appending, or `make` fails immediately:

```make
    $(if $(MAX_OUTSTANDING),MAX_OUTSTANDING=$(MAX_OUTSTANDING)) \
    $(if $(B_ROB_DEPTH),B_ROB_DEPTH=$(B_ROB_DEPTH)) \
    $(if $(R_ROB_DEPTH),R_ROB_DEPTH=$(R_ROB_DEPTH))
```

- [ ] **Step 7: Verify the knob reaches the model**

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make test 2>&1 | grep -E "tests passed|tests failed"'
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make sim TB=tb_mesh_4x4_vc1_rob PATTERN=neighbor SEED=1 2>&1 | tail -3'
wsl -e bash -lc 'grep -c "b_rob_depth" /mnt/e/05_NoC/noc_project/sim/tb/tb_top_mesh_4x4_vc1_rob.sv'
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make sim TB=tb_mesh_4x4_vc1_rob PATTERN=neighbor SEED=1 R_ROB_DEPTH=4 B_ROB_DEPTH=4 2>&1 | tail -3'
```
Expected: ctest green; `DIRECTED PASS` twice; the grep prints a non-zero count. A depth of 4 must still
serve `--len 0` traffic. If the `R_ROB_DEPTH=4` run fails, **stop and report BLOCKED**.

- [ ] **Step 8: Commit**

```bash
git add src/c_model/include/nmu/nmu.hpp src/c_model/include/wrap/wrap_defaults.hpp \
        src/c_model/include/wrap/nmu_wrap.hpp src/dpi/cmodel_dpi.h src/dpi/cmodel_dpi.cpp \
        sim/tools/gen_tb_top.py sim/verilator/Makefile Makefile
git commit -m "feat(nmu): plumb b_rob_depth and r_rob_depth from Makefile to the Rob ctor"
```

---

### Task 5: `rob_req` on the committed entries and the staged pipeline

Pure refactor, no behaviour change: `rob_req` is `true` everywhere until Task 7. Doing it first means
Task 7 cannot silently corrupt slot 0. Three independent reviewers named this as the most likely way to
ship a silently wrong RoB.

`Nmu::advance_rsp_s2_b_` hardcodes `true` into `NmuRspBEntry::rob_enabled` (`nmu.hpp:387`, and `:394`
for R), and `Nmu::push_rsp_b_to_axi_` gates `commit_b_exit` on it (`:317,323`). `commit_b_exit` opens
with `assert(committed_b_pending_[rob_idx] > 0)`. A bypassed beat owns no slot and carries
`rob_idx = 0`; under `NDEBUG` it would decrement a counter it never incremented and then free a live
slot.

**Files:**
- Modify: `src/c_model/include/nmu/rob.hpp`, `src/c_model/include/nmu/nmu.hpp`

**Interfaces:**
- Produces: `Rob::CommittedBEntry{axi::BBeat beat; uint8_t rob_idx; uint8_t axi_id; bool rob_req;}` and the matching `CommittedREntry`. `NmuRspBEntry::rob_enabled` renamed to `rob_req`; same for `NmuRspREntry`.

- [ ] **Step 1: Add the field in `rob.hpp`**

```cpp
    struct CommittedBEntry {
        axi::BBeat beat;
        uint8_t rob_idx;
        uint8_t axi_id;
        bool rob_req;  // false => bypassed, owns no slot, must skip commit_b_exit
    };
    struct CommittedREntry {
        axi::RBeat beat;
        uint8_t rob_idx;
        uint8_t axi_id;
        bool rob_req;
    };
```
Every `committed_b_queue_.push_back({...})` and `committed_r_queue_.push_back({...})` gains a trailing
`true`. Guard the non-staged wrappers:

```cpp
inline std::optional<axi::BBeat> Rob::pop_b() {
    if (mode_w_ == RobMode::Enabled) {
        auto out = pop_b_staged();
        if (!out) return std::nullopt;
        if (out->rob_req) commit_b_exit(out->rob_idx, out->axi_id);
        return out->beat;
    }
    // Disabled branch unchanged.
```
and the same shape in `pop_r()`.

- [ ] **Step 2: Rename the staged-entry field in `nmu.hpp`**

```cpp
struct NmuRspBEntry {
    axi::BBeat beat;
    uint8_t rob_idx = 0;
    uint8_t axi_id = 0;
    bool rob_req = false;  // owns a RoB slot; false => bypassed
};
```
same for `NmuRspREntry`. Then:

```cpp
inline bool Nmu::push_rsp_b_to_axi_(const NmuRspBEntry& entry) {
    if (!axi_slave_port_.push_b_staged(entry.beat)) return false;
    if (entry.rob_req) rob_.commit_b_exit(entry.rob_idx, entry.axi_id);
    return true;
}
```
```cpp
inline void Nmu::advance_rsp_s2_b_() {
    if (s2_rsp_b_.full()) return;
    auto b = rob_.pop_b_staged();
    if (!b) return;
    s2_rsp_b_.accept({b->beat, b->rob_idx, b->axi_id, b->rob_req});
}
```
and the same for `push_rsp_r_to_axi_` / `advance_rsp_s2_r_`. Find any other reader:

```bash
grep -rn "rob_enabled" src/c_model/ | grep -v cmodel_dpi
```

- [ ] **Step 3: Build and test**

```bash
clang-format -i src/c_model/include/nmu/rob.hpp src/c_model/include/nmu/nmu.hpp
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make test 2>&1 | grep -E "tests passed|tests failed"'
```
Expected: all pass. Behaviour is unchanged — `rob_req` is `true` on every path.

- [ ] **Step 4: Commit**

```bash
git add src/c_model/include/nmu/rob.hpp src/c_model/include/nmu/nmu.hpp
git commit -m "refactor(nmu): committed entries carry rob_req; rename NmuRsp*Entry::rob_enabled"
```

---

### Task 6: `max_txns_per_id`

A named depth for the per-ID order list. Spec D3 and D4: not a safety requirement, but the per-ID FIFO
depth is a hardware parameter and today `std::deque` names none. The default is FlooNoC's 32
(`floo_rob.sv:12`), not `AXI_ID_SPACE`.

**Files:**
- Modify: `src/c_model/include/nmu/rob.hpp`, `nmu.hpp`, `wrap/wrap_defaults.hpp`, `wrap/nmu_wrap.hpp`, `src/dpi/cmodel_dpi.{h,cpp}`, `sim/tools/gen_tb_top.py`, `sim/verilator/Makefile`, `Makefile`
- Modify: `src/c_model/tests/nmu/test_rob.cpp`

**Interfaces:**
- Produces: `Rob` constructor gains a trailing `std::size_t max_txns_per_id = 32`, accessor `max_txns_per_id() const noexcept`. `NmuConfig::max_txns_per_id`. `wrap::kRobMaxTxnsPerId = 32`. `NmuWrap::init(..., std::size_t max_txns_per_id)`. `cmodel_nmu_create_ex(..., int max_txns_per_id, const char* config_path)`. Makefile knob `MAX_TXNS_PER_ID`, plusarg `+max_txns_per_id=`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(NmuRob, Enabled_MaxTxnsPerIdGate_RefusesWithFreeSlotsAvailable) {
    SCENARIO("Rob Enabled: the (max_txns_per_id+1)-th same-id AW is refused while slots remain");
    ChannelModel noc(256, 256);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 256, 256);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled, legacy_sam(), 32, 32, 3);

    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(rob.push_aw(make_aw(0x09, 0x100 + 0x40 * i))) << "same-id AW " << i;
    }
    EXPECT_FALSE(rob.push_aw(make_aw(0x09, 0x400))) << "per-id cap bites before the pool does";
    EXPECT_GT(rob.write_free_space(), 0u);
    EXPECT_TRUE(rob.push_aw(make_aw(0x0A, 0x500))) << "the gate is per-id, not global";
}

TEST(NmuRob, Enabled_MaxTxnsPerIdGate_AppliesToReadsIndependently) {
    SCENARIO("Rob Enabled: the per-id cap gates AR independently of AW");
    ChannelModel noc(256, 256);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 256, 256);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled, legacy_sam(), 32, 32, 2);

    ASSERT_TRUE(rob.push_ar(make_ar(0x0B, 0x100)));
    ASSERT_TRUE(rob.push_ar(make_ar(0x0B, 0x200)));
    EXPECT_FALSE(rob.push_ar(make_ar(0x0B, 0x300)));
    EXPECT_TRUE(rob.push_aw(make_aw(0x0B, 0x400))) << "the write list of the same id is independent";
}

TEST(NmuRob, Enabled_MaxTxnsPerIdDefaultIsThirtyTwo) {
    SCENARIO("Rob Enabled: the default per-id cap is FlooNoC's MaxRoTxnsPerId = 32");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled, legacy_sam());
    EXPECT_EQ(rob.max_txns_per_id(), 32u);
}

TEST(NmuRobDeath, Enabled_MaxTxnsPerIdZeroAborts) {
    SCENARIO("Rob: max_txns_per_id = 0 is rejected at construction");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    EXPECT_DEATH(
        { Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled, legacy_sam(), 32, 32, 0); }, ".*");
}
```

- [ ] **Step 2: Implement the gate**

Constructor gains `std::size_t max_txns_per_id = 32`, member `std::size_t max_txns_per_id_;`, accessor,
and:

```cpp
        assert(max_txns_per_id_ >= 1 && "nmu::Rob: max_txns_per_id must be positive");
        if (max_txns_per_id_ < 1) std::abort();
```
First statement of the Enabled branch of `push_aw`:

```cpp
        // ax_gnt_o: the per-id order list is FlooNoC's status FIFO (floo_rob.sv:414).
        if (write_order_by_id_[b.id].size() >= max_txns_per_id_) return false;
```
and of `push_ar`:

```cpp
        if (read_order_by_id_[b.id].size() >= max_txns_per_id_) return false;
```

- [ ] **Step 3: Check the pool-full test still measures the pool**

`Enabled_PushAw_PoolFull_ReturnFalseAtomic` (`test_rob.cpp:326`) fills 32 slots with 32 **distinct** IDs
(`i & 0xFF`), so a per-ID cap of 32 does not bite and it stays green. Confirm that. Task 7 rewrites it
for a different reason.

- [ ] **Step 4: Plumb it, exactly as Task 4 plumbed the depths**

`wrap_defaults.hpp`:

```cpp
// Per-AXI-ID order-list depth (FlooNoC MaxRoTxnsPerId, floo_rob.sv:12). [TBD] --
// 32 is FlooNoC's default over 8 ids; ours spans 256. The depth sweep decides.
constexpr std::size_t kRobMaxTxnsPerId = 32;
```

`NmuConfig`: `std::size_t max_txns_per_id = 32;`, passed as the `Rob` ctor's eighth argument.
`NmuWrap::init` gains a trailing `std::size_t max_txns_per_id = kRobMaxTxnsPerId` and sets it.
`cmodel_dpi.{h,cpp}`: `cmodel_nmu_create_ex(name, src_id, num_vc, rob_enabled, b_rob_depth, r_rob_depth, max_txns_per_id, config_path)`; `cmodel_nmu_create` passes `kRobMaxTxnsPerId`.
`gen_tb_top.py`: one more `input int max_txns_per_id,` in the import, one more
`int unsigned max_txns_per_id = 32;`, one more
`void'($value$plusargs("max_txns_per_id=%d", max_txns_per_id));`, and the create call becomes

```python
            w(f'        nmu_ctx[{i}] = cmodel_nmu_create_ex("nmu_{i}", {c}, NUM_VC, 1, '
              f'b_rob_depth, r_rob_depth, max_txns_per_id, sam_config_path);  '
              f'// src_id = node{i} coord {c}, ROB Enabled')
```

`sim/verilator/Makefile`: `MAX_TXNS_PER_ID ?= 32` and `"+max_txns_per_id=$(MAX_TXNS_PER_ID)"` in the
recipe. Root `Makefile`: append `$(if $(MAX_TXNS_PER_ID),MAX_TXNS_PER_ID=$(MAX_TXNS_PER_ID))` to
`_INJ_ARGS`, remembering the trailing backslash on the line above it.

- [ ] **Step 5: Verify the knob bites**

```bash
clang-format -i src/c_model/include/nmu/rob.hpp src/c_model/tests/nmu/test_rob.cpp
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make test 2>&1 | grep -E "tests passed|tests failed"'
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make sim TB=tb_mesh_4x4_vc1_rob PATTERN=neighbor SEED=1 MAX_TXNS_PER_ID=1 2>&1 | tail -3'
```
Expected: ctest green; the `MAX_TXNS_PER_ID=1` run is `DIRECTED PASS`. One outstanding transaction per
ID is slow, not wrong. If it hangs, **stop and report BLOCKED**.

- [ ] **Step 6: Commit**

```bash
git add src/c_model/include/nmu/rob.hpp src/c_model/include/nmu/nmu.hpp \
        src/c_model/include/wrap/wrap_defaults.hpp src/c_model/include/wrap/nmu_wrap.hpp \
        src/dpi/cmodel_dpi.h src/dpi/cmodel_dpi.cpp src/c_model/tests/nmu/test_rob.cpp \
        sim/tools/gen_tb_top.py sim/verilator/Makefile Makefile
git commit -m "feat(nmu): max_txns_per_id gates the per-AXI-ID order list, default 32"
```

---

### Task 7: Bypass clause 1, and the test migration it forces

A transaction whose ID has nothing in flight cannot have its response overtaken, so it needs no reorder
slot. Spec D9.

**This is the one commit where behaviour and the whole `Enabled_*` suite change together.** Clause 1
overturns the invariant those tests encode — that the first Enabled transaction owns slot 0 and stamps
`rob_req = 1`. They cannot be migrated in an earlier green commit: before the bypass, a primer
transaction consumes slot 0 and shifts every index; after it, the primer consumes nothing and every
index expectation is restored. Expect around sixteen test edits alongside the implementation. Do not
split them out; do not disable any of them.

**Files:**
- Modify: `src/c_model/include/nmu/rob.hpp`, `src/c_model/tests/nmu/test_rob.cpp`

**Interfaces:**
- Consumes: `CommittedBEntry::rob_req` (Task 5), `max_txns_per_id_` (Task 6), `read_free_space()` (Task 2), `read_release_offset_` (Task 3).
- Produces: `Rob::BeatRange{uint8_t base; uint16_t len_plus_1; bool rob_req;}` and private helpers `void drain_ready_write_heads_(uint8_t id)` / `void drain_ready_read_heads_(uint8_t id)`. No public signature changes.

- [ ] **Step 1: Add the test helpers**

In the anonymous namespace at the top of `test_rob.cpp`, beside `make_aw` / `make_ar`. GoogleTest allows
`ASSERT_*` in a `void` helper.

```cpp
// Clause 1 bypasses the first transaction of an idle id. A test that wants the
// transaction under test to take the RoB path must first put one transaction in
// flight for that id. The primer allocates no slot, so every rob_idx expectation
// in the migrated tests below is unchanged.
void prime_write_id(Rob& rob, ChannelModel& noc, uint8_t id) {
    ASSERT_TRUE(rob.push_aw(make_aw(id, 0x8000)));
    noc.req_in().pop_flit();  // discard the primer's AW flit
}

void prime_read_id(Rob& rob, ReqCapture& ar_cap, uint8_t id) {
    ASSERT_TRUE(rob.push_ar(make_ar(id, 0x8000)));
    ar_cap.pop();  // discard the primer's AR flit
}

// Overload for the backpressure tests, which wire all three Packetize outputs to
// noc.req_out() and construct no ar_cap (test_rob.cpp:303-306).
void prime_read_id(Rob& rob, ChannelModel& noc, uint8_t id) {
    ASSERT_TRUE(rob.push_ar(make_ar(id, 0x8000)));
    noc.req_in().pop_flit();  // discard the primer's AR flit
}

// Retire a primer by feeding its bypassed response and draining it. Call AFTER
// pushing the transaction under test, so that transaction becomes the list head.
void retire_write_primer(Rob& rob, ChannelModel& noc, Depacketize& depkt, uint8_t id) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_B);
    f.set_header_field("dst_id", kSrcId);
    f.set_header_field("last", 1);
    f.set_header_field("rob_req", 0);
    f.set_header_field("rob_idx", 0);
    f.set_payload_field("B", "bid", id);
    f.set_payload_field("B", "bresp", 0);
    ASSERT_TRUE(noc.rsp_out().push_flit(f));
    depkt.tick();
    ASSERT_TRUE(rob.pop_b().has_value());
}

void retire_read_primer(Rob& rob, ChannelModel& noc, Depacketize& depkt, uint8_t id) {
    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_R);
    f.set_header_field("dst_id", kSrcId);
    f.set_header_field("last", 1);
    f.set_header_field("rob_req", 0);
    f.set_header_field("rob_idx", 0);
    f.set_payload_field("R", "rid", id);
    f.set_payload_field("R", "rresp", 0);
    f.set_payload_field("R", "rlast", 1u);
    std::array<uint8_t, 32> d{};
    f.set_payload_bytes("R", "rdata", d.data(), 256);
    ASSERT_TRUE(noc.rsp_out().push_flit(f));
    depkt.tick();
    ASSERT_TRUE(rob.pop_r().has_value());
}
```

- [ ] **Step 2: Migrate the existing `Enabled_*` tests**

Mechanical, one insertion each, unless the row says otherwise. Insert immediately after the
`Rob rob(...)` construction line unless stated. `<id>` is the AXI ID that test already uses.

| test (current line) | edit |
|---|---|
| `Enabled_PushAw_AllocatesSlotAndStampsRobIdx` (`:213`) | insert `prime_write_id(rob, noc, 0x05);`. Assertions unchanged. |
| `Enabled_PushAr_AllocatesConsecutiveSlotsForBurst` (`:228`) | insert `prime_read_id(rob, ar_cap, 0x05);` and `prime_read_id(rob, ar_cap, 0x06);`. Assertions unchanged. |
| `Enabled_PushAr_OversizedBurst_ReturnFalse` (`:278-295`) | delete. Replaced in Step 3b. |
| `Enabled_PushAr_DownstreamBackpressure_AtomicRollback` (`:297`) | insert `prime_read_id(rob, noc, 0x05);` — the **ChannelModel overload**: this test wires all three `Packetize` outputs to `noc.req_out()` and constructs no `ar_cap` (`:303-306`). Without a primer the AR under test bypasses and there is no allocation to roll back, so the test would pass vacuously. |
| `Enabled_PushAw_PoolFull_ReturnFalseAtomic` (`:326`) | rewrite; see Step 3a. It fills the pool with 32 **distinct** IDs, every one of which now bypasses. |
| `Enabled_PushAw_DownstreamBackpressure_AtomicRollback` (`:342`) | insert `prime_write_id(rob, noc, <id>);`, same reason as `:297`. |
| `Enabled_PopB_InOrder_ImmediateCommit` (`:359`) | `prime_write_id` after construction; `retire_write_primer` after the `push_aw` under test. |
| `Enabled_PopB_OutOfOrder_HeldUntilHeadReady` (`:384`) | `prime_write_id` after construction; `retire_write_primer` after both `push_aw` calls. Slots stay 0 and 1. |
| `Enabled_PopR_MultiBeatBurstCommitInOrder` (`:419`) | `prime_read_id` after construction; `retire_read_primer` after both `push_ar` calls. Bases stay 0..3 and 4..5. |
| `Enabled_DifferentIdsInterleaveAtTransactionBoundary` (`:480`) | `prime_read_id` for `0x05` and `0x06` after construction; `retire_read_primer` for both after both `push_ar` calls. Slots stay 0 and 1. |
| `NmuRobDeath.Enabled_PopBWithUnallocatedRobIdx_Abort` (`:523`) | **no edit.** It pushes no AW at all (`:529-542`); it injects a `rob_req = 1` B for slot 7 into an empty pool. That takes the unchanged robbed arm, finds `write_entries_[7].occupied == false`, and aborts exactly as before. |
| `NmuRobDeath.Enabled_PopBWithDisabledFlit_Abort` (`:545-566`) | delete. `rob_req = 0` in Enabled mode is now the legal bypass encoding. Replaced in Step 3b. |
| `ReadFillSameBaseRobIdxLandsInOrder` (`:568`) | `prime_read_id` after construction; `retire_read_primer` after the `push_ar` calls. |
| `NmuRobDeath.ReadExtraBeatPastBurstLengthAborts` (`:614`) | `prime_read_id` after construction, so the AR under test allocates and `read_range_len_` is set. |
| `ReadSameBaseReuseStartsAtZero` (`:653`) | `prime_read_id` after construction; `retire_read_primer` after the first `push_ar`. |
| `ReadSameIdDifferentDstInterleavedFilesPerBase` (`:710`) | `prime_read_id` after construction; `retire_read_primer` after the `push_ar` calls. |
| `Enabled_LzcAllocator_*` (Task 2) | prime each ID whose range must be allocated: `prime_read_id` for `0x05`/`0x06`/`0x07`, `prime_write_id` for `0x05`/`0x06`. The primers allocate nothing, so the expected bases and free-space values are unchanged. |
| `Enabled_PerBeatRelease_HeadBurstStreams` (Task 3) | `prime_read_id(rob, ar_cap, 0x05);` after construction. |
| `RobDepthParam.Enabled_AllocationNeverExceedsDepth` (Task 2) | each distinct id's first AW now bypasses. Rewrite around one primed id: `prime_write_id(rob, noc, 0x40);`, then `depth` same-id AWs, with `max_txns_per_id` raised above `depth` in the ctor. |
| `Enabled_MaxTxnsPerIdGate_RefusesWithFreeSlotsAvailable` (Task 6) | the first same-ID AW now bypasses, so the cap of 3 admits a bypass plus two robbed. Keep the loop at 3 and the fourth `EXPECT_FALSE`: the cap counts list entries, not slots. Verify `write_free_space()` dropped by exactly 2. |
| `Enabled_MaxBurst_LenPlus1DoesNotOverflow` (Task 1) | `prime_read_id(rob, ar_cap, 0x05);` after construction, so the 256-beat AR takes the RoB path and really allocates 256 slots. |

- [ ] **Step 3a: Rewrite `Enabled_PushAw_PoolFull_ReturnFalseAtomic`**

```cpp
TEST(NmuRob, Enabled_PushAw_PoolFull_ReturnFalseAtomic) {
    SCENARIO("Rob Enabled: b_rob_depth AWs fill the write pool; the next push_aw returns false");
    ChannelModel noc(64, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});  // aw uses noc; w/ar use captures
    Depacketize depkt(noc.rsp_in(), 16, 16);
    // One id, primed so clause 1 does not exempt the transactions under test.
    // Distinct ids would each bypass and never fill the pool. The per-id cap must
    // exceed the pool depth for the pool to be the binding constraint (spec D4).
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled, legacy_sam(), 32, 32, 64);

    prime_write_id(rob, noc, 0x40);
    for (std::size_t i = 0; i < rob.b_rob_depth(); ++i) {
        ASSERT_TRUE(rob.push_aw(make_aw(0x40, 0x100))) << "AW " << i;
    }
    EXPECT_EQ(rob.write_free_space(), 0u);
    EXPECT_FALSE(rob.push_aw(make_aw(0x40, 0x200)));
}
```

- [ ] **Step 3b: Replace the deleted death test, and write the new bypass tests**

```cpp
TEST(NmuRobDeath, Enabled_PopBBypassFlitOnRobbedHead_Abort) {
    SCENARIO("Rob Enabled: a rob_req=0 B whose id's list head owns a slot is malformed, aborts");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled, legacy_sam());

    prime_write_id(rob, noc, 0x05);                  // bypassed head
    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));  // robbed, slot 0
    retire_write_primer(rob, noc, depkt, 0x05);      // head is now the robbed entry

    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_B);
    f.set_header_field("dst_id", kSrcId);
    f.set_header_field("last", 1);
    f.set_header_field("rob_req", 0);
    f.set_header_field("rob_idx", 0);
    f.set_payload_field("B", "bid", 0x05);
    f.set_payload_field("B", "bresp", 0);
    ASSERT_TRUE(noc.rsp_out().push_flit(f));
    depkt.tick();
    EXPECT_DEATH(rob.pop_b(), ".*");
}

TEST(NmuRob, Enabled_PushAr_OversizedBurst_AdmittedViaBypass) {
    SCENARIO("Rob Enabled: a 256-beat AR on an idle id is admitted with rob_req=0, no slots taken");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled, legacy_sam());  // depth 32

    const std::size_t free_before = rob.read_free_space();
    axi::ArBeat ar = make_ar(0x05, 0x100);
    ar.len = 255;  // 256 beats, eight times the 32-slot read pool
    EXPECT_TRUE(rob.push_ar(ar));
    auto f = *ar_cap.pop();
    EXPECT_EQ(f.get_header_field("axi_ch"), ni::AXI_CH_AR);
    EXPECT_EQ(f.get_header_field("rob_req"), 0u) << "bypassed AR carries rob_req=0";
    EXPECT_EQ(rob.read_free_space(), free_before) << "bypass allocates nothing";
}

TEST(NmuRob, Enabled_PushAr_OversizedBurst_SecondSameIdRefusedNotWedged) {
    SCENARIO("Rob Enabled: a second oversized AR on the same id is refused while the first flies");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled, legacy_sam());

    axi::ArBeat first = make_ar(0x05, 0x100);
    first.len = 255;
    ASSERT_TRUE(rob.push_ar(first));
    axi::ArBeat second = make_ar(0x05, 0x200);
    second.len = 255;
    EXPECT_FALSE(rob.push_ar(second)) << "list non-empty -> rob_req=1 -> 256 slots -> refuse";

    axi::ArBeat other = make_ar(0x06, 0x300);
    other.len = 255;
    EXPECT_TRUE(rob.push_ar(other)) << "the refusal is per-id, not a channel wedge";
}

TEST(NmuRob, Enabled_Clause1_FirstTxnPerIdAllocatesNoSlot) {
    SCENARIO("Rob Enabled: the first AW of an id takes no slot, stamps rob_req=0; the second does");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled, legacy_sam());

    const std::size_t free_before = rob.write_free_space();
    ASSERT_TRUE(rob.push_aw(make_aw(0x11, 0x100)));
    auto f0 = *noc.req_in().pop_flit();
    EXPECT_EQ(f0.get_header_field("rob_req"), 0u);
    EXPECT_EQ(rob.write_free_space(), free_before);

    ASSERT_TRUE(rob.push_aw(make_aw(0x11, 0x200)));
    auto f1 = *noc.req_in().pop_flit();
    EXPECT_EQ(f1.get_header_field("rob_req"), 1u);
    EXPECT_EQ(f1.get_header_field("rob_idx"), 0u) << "first allocated slot";
    EXPECT_EQ(rob.write_free_space(), free_before - 1);
}

TEST(NmuRob, Enabled_BypassedBeat_ReleasesNoSlot) {
    SCENARIO("Rob Enabled: a bypassed B forwards without touching the allocation bitmap");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled, legacy_sam());

    const std::size_t free_before = rob.write_free_space();
    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));  // bypassed: id 5 was idle
    noc.req_in().pop_flit();

    ni::cmodel::Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_B);
    f.set_header_field("dst_id", kSrcId);
    f.set_header_field("last", 1);
    f.set_header_field("rob_req", 0);
    f.set_header_field("rob_idx", 0);
    f.set_payload_field("B", "bid", 0x05);
    f.set_payload_field("B", "bresp", 0);
    ASSERT_TRUE(noc.rsp_out().push_flit(f));
    depkt.tick();

    auto b = rob.pop_b();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->id, 0x05u);
    EXPECT_EQ(rob.write_free_space(), free_before) << "no slot taken, none may be released";

    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x200)));  // the id is idle again
    auto f2 = *noc.req_in().pop_flit();
    EXPECT_EQ(f2.get_header_field("rob_req"), 0u);
}

TEST(NmuRob, Enabled_MixedList_OrderPreserved) {
    SCENARIO("Rob Enabled: id 5 holds [bypassed, robbed]; the robbed B arrives first and waits");
    ChannelModel noc(16, 16);
    ReqCapture w_cap, ar_cap;
    Packetize pkt(noc.req_out(), w_cap, ar_cap, kSrcId, {});
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled, legacy_sam());

    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));  // bypassed, rob_req=0
    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x200)));  // needs the RoB, slot 0

    auto push_b = [&](unsigned rob_req, unsigned rob_idx, unsigned bresp) {
        ni::cmodel::Flit f;
        f.set_header_field("axi_ch", ni::AXI_CH_B);
        f.set_header_field("dst_id", kSrcId);
        f.set_header_field("last", 1);
        f.set_header_field("rob_req", rob_req);
        f.set_header_field("rob_idx", rob_idx);
        f.set_payload_field("B", "bid", 0x05);
        f.set_payload_field("B", "bresp", bresp);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
        depkt.tick();
    };

    // Both responses carry bid=5, so bresp is the only way to tell them apart.
    // axi::Resp is an enum class: EXOKAY = 1, SLVERR = 2.
    push_b(/*rob_req=*/1, /*rob_idx=*/0, /*bresp=*/2);  // the LATER transaction returns first
    EXPECT_FALSE(rob.pop_b().has_value()) << "must wait behind the bypassed head";

    push_b(/*rob_req=*/0, /*rob_idx=*/0, /*bresp=*/1);  // the bypassed head returns
    auto first = rob.pop_b();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->resp, axi::Resp::EXOKAY) << "bypassed head released first";
    auto second = rob.pop_b();
    ASSERT_TRUE(second.has_value()) << "a missing re-drain strands this response forever";
    EXPECT_EQ(second->resp, axi::Resp::SLVERR) << "the robbed entry released after it";
    EXPECT_EQ(rob.write_free_space(), rob.b_rob_depth()) << "slot 0 returned to the pool";
}

TEST(NmuRob, Enabled_MaxTxnsPerId1_MatchesDisabled) {
    SCENARIO("Rob Enabled with max_txns_per_id=1 emits the same AR flits as Rob Disabled");
    ChannelModel noc_e(64, 64);
    ReqCapture w_e, ar_e;
    Packetize pkt_e(noc_e.req_out(), w_e, ar_e, kSrcId, {});
    Depacketize depkt_e(noc_e.rsp_in(), 64, 64);
    Rob rob_e(pkt_e, depkt_e, RobMode::Enabled, RobMode::Enabled, legacy_sam(), 32, 32, 1);

    ChannelModel noc_d(64, 64);
    ReqCapture w_d, ar_d;
    Packetize pkt_d(noc_d.req_out(), w_d, ar_d, kSrcId, {});
    Depacketize depkt_d(noc_d.rsp_in(), 64, 64);
    Rob rob_d(pkt_d, depkt_d, RobMode::Disabled, RobMode::Disabled, legacy_sam());

    axi::ArBeat a0 = make_ar(0x21, 0x100);
    axi::ArBeat a1 = make_ar(0x21, 0x200);  // same id: both must refuse
    axi::ArBeat a2 = make_ar(0x22, 0x300);  // different id: both must accept

    EXPECT_EQ(rob_e.push_ar(a0), rob_d.push_ar(a0));
    EXPECT_EQ(rob_e.push_ar(a1), rob_d.push_ar(a1));
    EXPECT_EQ(rob_e.push_ar(a2), rob_d.push_ar(a2));

    ASSERT_EQ(ar_e.size(), ar_d.size());
    while (ar_e.size() > 0) {
        auto fe = *ar_e.pop();
        auto fd = *ar_d.pop();
        EXPECT_EQ(fe.get_header_field("rob_req"), fd.get_header_field("rob_req"));
        EXPECT_EQ(fe.get_header_field("rob_idx"), fd.get_header_field("rob_idx"));
        EXPECT_EQ(fe.get_header_field("dst_id"), fd.get_header_field("dst_id"));
    }
    EXPECT_EQ(rob_e.read_free_space(), rob_e.r_rob_depth()) << "Enabled took no slot either";
}
```

- [ ] **Step 4: Run to verify the new tests fail**

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make test 2>&1 | grep -E "FAILED|tests failed"'
```
Expected: `Enabled_PushAr_OversizedBurst_AdmittedViaBypass` (returns false),
`Enabled_Clause1_FirstTxnPerIdAllocatesNoSlot` (`rob_req` is `1`),
`Enabled_MaxTxnsPerId1_MatchesDisabled` (`rob_req` differs), and `Enabled_MixedList_OrderPreserved` /
`Enabled_BypassedBeat_ReleasesNoSlot` abort on the `rob_req = 0` guard at `rob.hpp:284-287`.

The migrated tests will **not** be green at this step: a primer allocates a slot before the bypass
exists, so slot indices shift by one. That is expected, and it is why this task is one commit.

- [ ] **Step 5: Add `rob_req` to `BeatRange`**

```cpp
    // Per-id ordered range list. AW = {base, 1}; AR = {base, len+1}.
    // rob_req == false: bypassed, no slot reserved, base is meaningless.
    struct BeatRange {
        uint8_t base;
        uint16_t len_plus_1;
        bool rob_req;
    };
```

- [ ] **Step 6: Implement clause 1 in `push_aw`**

Nothing mutates before the downstream push succeeds.

```cpp
inline bool Rob::push_aw(const axi::AwBeat& b) {
    if (mode_w_ == RobMode::Enabled) {
        if (write_order_by_id_[b.id].size() >= max_txns_per_id_) return false;
        // Clause 1: nothing in flight for this id, so nothing can overtake this
        // response. No reorder storage needed. Ported from floo_rob.sv:422-425.
        const bool needs_rob = !write_order_by_id_[b.id].empty();
        auto t = sam_.translate(b.addr);
        std::size_t base = 0;
        if (needs_rob) {
            if (write_free_space() < 1) return false;
            base = b_rob_depth_ - write_free_space();
        }
        if (!next_pkt_.push_aw_with_meta(
                b, {t.dst_id, t.local_addr, static_cast<uint8_t>(needs_rob ? 1 : 0),
                    static_cast<uint8_t>(needs_rob ? base : 0)})) {
            return false;  // downstream backpressure: no state mutation
        }
        if (needs_rob) {
            alloc_write_.set(base);
            write_entries_[base] = WriteEntry{/*occupied=*/true, /*ready=*/false, b.id, {}};
        }
        write_order_by_id_[b.id].push_back({static_cast<uint8_t>(base), 1, needs_rob});
        ++w_burst_credit_;
        return true;
    }
    // Disabled branch unchanged.
```

- [ ] **Step 7: Implement clause 1 in `push_ar`**

The `read_free_space() < n` check moves **inside** `needs_rob`. A bypassed burst of any length is
admissible; that is the whole point of the task.

```cpp
inline bool Rob::push_ar(const axi::ArBeat& b) {
    if (mode_r_ == RobMode::Enabled) {
        if (read_order_by_id_[b.id].size() >= max_txns_per_id_) return false;
        const std::size_t n = static_cast<std::size_t>(b.len) + 1u;
        const bool needs_rob = !read_order_by_id_[b.id].empty();
        auto t = sam_.translate(b.addr);
        std::size_t base = 0;
        if (needs_rob) {
            if (read_free_space() < n) return false;
            base = r_rob_depth_ - read_free_space();
        }
        if (!next_pkt_.push_ar_with_meta(
                b, {t.dst_id, t.local_addr, static_cast<uint8_t>(needs_rob ? 1 : 0),
                    static_cast<uint8_t>(needs_rob ? base : 0)})) {
            return false;
        }
        if (needs_rob) {
            for (std::size_t i = 0; i < n; ++i) {
                read_entries_[base + i] = ReadEntry{/*occupied=*/true, /*ready=*/false, b.id, {}};
            }
            alloc_read_.set(base + n - 1);
            read_range_len_[base] = static_cast<uint16_t>(n);
        }
        read_order_by_id_[b.id].push_back(
            {static_cast<uint8_t>(base), static_cast<uint16_t>(n), needs_rob});
        return true;
    }
    // Disabled branch unchanged.
```

`read_range_len_` and `read_arrival_offset_` are only ever indexed by an allocated `base`, so a bypassed
AR leaves them untouched. That is correct: no beat of a bypassed burst is ever placed.

- [ ] **Step 8: Factor the head-drain loops into private helpers**

The direct-forward branch must run the same drain. Pull both loops out, adding the `!head.rob_req` guard.

```cpp
inline void Rob::drain_ready_write_heads_(uint8_t id) {
    while (!write_order_by_id_[id].empty()) {
        const BeatRange head = write_order_by_id_[id].front();
        if (!head.rob_req) break;  // waiting on a bypassed response
        if (!write_entries_[head.base].ready) break;
        committed_b_queue_.push_back({write_entries_[head.base].b_beat, head.base, id, true});
        ++committed_b_pending_[head.base];
        write_order_by_id_[id].pop_front();
    }
}

inline void Rob::drain_ready_read_heads_(uint8_t id) {
    while (!read_order_by_id_[id].empty()) {
        const BeatRange head = read_order_by_id_[id].front();
        if (!head.rob_req) break;
        uint16_t& release_off = read_release_offset_[head.base];
        while (release_off < head.len_plus_1 && read_entries_[head.base + release_off].ready) {
            const std::size_t idx = static_cast<std::size_t>(head.base) + release_off;
            committed_r_queue_.push_back(
                {read_entries_[idx].r_beat, static_cast<uint8_t>(idx), id, true});
            ++committed_r_pending_[idx];
            ++release_off;
        }
        if (release_off < head.len_plus_1) break;
        read_arrival_offset_[head.base] = 0;
        release_off = 0;
        read_order_by_id_[id].pop_front();
    }
}
```
Declare both private. The `rob_req == 1` arms of `pop_b_staged` / `pop_r_staged` call them instead of
inlining the loop.

- [ ] **Step 9: Direct-forward branch in `pop_b_staged`**

The `abort()` on `meta.rob_req != 1` (`rob.hpp:284-287`) is replaced. By the head invariant, a
`rob_req == 0` response is the head of its ID's list.

**The re-drain is not optional.** Popping the bypassed head can expose a robbed entry whose slot is
already `ready`. Nothing else would look at it: the next `pop_b_staged` finds `committed_b_queue_` empty
and waits on a flit that never comes. The response is lost and the slot leaks.

```cpp
    auto opt = next_depkt_.pop_b_with_meta();
    if (!opt) return std::nullopt;
    auto [b, meta] = *opt;
    if (meta.rob_req == 0) {
        const uint8_t id = b.id;
        if (write_order_by_id_[id].empty() || write_order_by_id_[id].front().rob_req) {
            assert(false && "bypassed B does not match the head of its id's order list");
            std::abort();
        }
        write_order_by_id_[id].pop_front();
        drain_ready_write_heads_(id);   // the entry behind it may already be ready
        return CommittedBEntry{b, 0, id, /*rob_req=*/false};
    }
    // ... rob_idx range check unchanged, mark the slot ready ...
    drain_ready_write_heads_(id);       // was the inlined while loop
    if (committed_b_queue_.empty()) return std::nullopt;
    // ... unchanged
```
Returning the bypassed beat directly while the drained entries sit in `committed_b_queue_` preserves
order: `pop_b_staged` reads that queue first on its next call.

- [ ] **Step 10: Direct-forward branch in `pop_r_staged`**

A bypassed R burst streams beat by beat. The list entry pops on `last`, and only then can a robbed entry
behind it become releasable.

```cpp
    auto opt = next_depkt_.pop_r_with_meta();
    if (!opt) return std::nullopt;
    auto [r, meta] = *opt;
    if (meta.rob_req == 0) {
        const uint8_t id = r.id;
        if (read_order_by_id_[id].empty() || read_order_by_id_[id].front().rob_req) {
            assert(false && "bypassed R does not match the head of its id's order list");
            std::abort();
        }
        if (r.last) {
            read_order_by_id_[id].pop_front();
            drain_ready_read_heads_(id);
        }
        return CommittedREntry{r, 0, id, /*rob_req=*/false};
    }
    // ... unchanged ...
    drain_ready_read_heads_(id);        // was the inlined while loop
```

- [ ] **Step 11: Run the tests**

```bash
clang-format -i src/c_model/include/nmu/rob.hpp src/c_model/tests/nmu/test_rob.cpp
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make test 2>&1 | grep -E "tests passed|tests failed"'
```
Expected: all pass, migrated and new alike.

If `Enabled_MixedList_OrderPreserved` fails with the second `pop_b()` returning `nullopt`, the re-drain
in Step 9 is missing.

- [ ] **Step 12: Fault-inject the bypass**

A check that cannot fail proves nothing. Temporarily force `const bool needs_rob = true;` in both
`push_aw` and `push_ar`, rebuild, confirm the bypass tests go red.

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make test 2>&1 | grep -E "FAILED"'
```
Expected red, and nothing else: `Enabled_PushAr_OversizedBurst_AdmittedViaBypass`,
`Enabled_PushAr_OversizedBurst_SecondSameIdRefusedNotWedged`,
`Enabled_Clause1_FirstTxnPerIdAllocatesNoSlot`, `Enabled_BypassedBeat_ReleasesNoSlot`,
`Enabled_MixedList_OrderPreserved`, `Enabled_MaxTxnsPerId1_MatchesDisabled`,
`Enabled_PopBBypassFlitOnRobbedHead_Abort`, and every migrated test whose primer now consumes a slot.
Anything outside that set is a real regression.

Revert the two lines, rebuild, confirm green. Do **not** commit the fault-injected state.

- [ ] **Step 13: Commit**

```bash
git add src/c_model/include/nmu/rob.hpp src/c_model/tests/nmu/test_rob.cpp
git commit -m "feat(nmu): bypass clause 1 - an idle id's transaction allocates no RoB slot"
```

---

### Task 8: Drive a burst through co-sim

The unit tests prove `push_ar` returns `true`. Only the co-sim proves the AR channel actually drains. A
fabric behaviour is verified on the wire, not in a C++ harness.

**Files:**
- Modify: `sim/verilator/Makefile`, `Makefile`, `docs/backlog.md`

**Interfaces:**
- Produces: Makefile knob `BURST_LEN` (default `0`), forwarded to `gen_test_patterns --len`.

- [ ] **Step 1: Add the knob**

`sim/verilator/Makefile`, beside `B_ROB_DEPTH`:

```make
# AXI burst length passed to gen_test_patterns (--len). 0 = single beat.
# At --size 5 (32 B/beat), 63 gives 64 beats = 2048 B, inside the 4 KB boundary.
BURST_LEN       ?= 0
```
In the `run-directed` recipe, replace the hardcoded `--len 0`:

```make
	    --transactions-per-node $(INJECTION_COUNT) --size 5 --len $(BURST_LEN) \
```
Root `Makefile`, in `_INJ_ARGS`: `$(if $(BURST_LEN),BURST_LEN=$(BURST_LEN))`.

`gen_test_patterns.py` sizes a slot as `(len + 1) << size` and grows the stride to the footprint
(`:154,300`). At `INJECTION_COUNT=4`, sixteen nodes, `BURST_LEN=63`, the allocator needs
`16 * 4 * 2048 = 0x20000` bytes and the recipe passes `--memory-size 0x40000`. It fits. **It does not
auto-grow**: raising `INJECTION_COUNT` with `BURST_LEN=63` needs a larger `--memory-size`, or the
generator raises `ValueError`.

- [ ] **Step 2: Reproduce the wedge on the pre-bypass build**

Task 7 is already committed, so the tree is clean and `git stash push <path>` would be a no-op that
silently leaves the fixed build in place. Check the pre-bypass `rob.hpp` out by SHA instead:

```bash
TASK6_SHA=$(git log --format=%H --grep="max_txns_per_id gates" -1)
git checkout "$TASK6_SHA" -- src/c_model/include/nmu/rob.hpp
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && timeout 900 make sim TB=tb_mesh_4x4_vc1_rob PATTERN=neighbor SEED=1 BURST_LEN=63 2>&1 | tail -6'
git checkout HEAD -- src/c_model/include/nmu/rob.hpp
git status --short
```
Expected: watchdog fatal, or the 900 s timeout fires. Record the exact failure line. Task 7 also changed
`test_rob.cpp`, but the co-sim does not compile it, so the single-file checkout is enough. Confirm
`git status` is clean before Step 3.

If this run **passes** on the pre-bypass build, the wedge does not reproduce on the wire. **Stop and
report BLOCKED** with the log: the spec's motivation would be wrong.

- [ ] **Step 3: Run the burst on the fixed build**

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && make sim TB=tb_mesh_4x4_vc1_rob PATTERN=neighbor SEED=1 BURST_LEN=63 2>&1 | tail -6'
```
Expected: `DIRECTED PASS: ... scoreboard clean, non-vacuous`.

If it fails with a scoreboard mismatch rather than a hang, that is a **different, pre-existing bug**
(`docs/backlog.md` records an unresolved co-sim burst issue). **Stop and report BLOCKED** with the
mismatch report. Do not fix it inline.

- [ ] **Step 4: Confirm no regression at `BURST_LEN=0`**

```bash
wsl -e bash -lc 'cd /mnt/e/05_NoC/noc_project && for p in neighbor transpose uniform_random hotspot; do
  make sim TB=tb_mesh_4x4_vc1_rob PATTERN=$p SEED=1 2>&1 | tail -1; done'
```
Expected: four `DIRECTED PASS` lines.

- [ ] **Step 5: Close the bug in the backlog**

Retitle the `### OPEN — RobMode::Enabled wedges the AR channel...` section to `### FIXED 2026-07-10 — …`
and append the verification: the exact `make sim` command, the pre-bypass failure mode, the post-bypass
pass line.

- [ ] **Step 6: Commit**

```bash
git add sim/verilator/Makefile Makefile docs/backlog.md
git commit -m "test(sim): BURST_LEN knob; a 64-beat read now drains the AR channel"
```

---

### Task 9: Reconcile the docs

The as-built document describes a `Rob` that no longer exists.

**Files:**
- Modify: `docs/nmu-rob-microarchitecture.md`, `docs/architecture.md`, `docs/backlog.md`

- [ ] **Step 1: Update the as-built document**

- Section 3: `ROB_CAPACITY` → `ROB_IDX_SPACE` = 256; the pools are `b_rob_depth_` / `r_rob_depth_`.
- Section 3, Release: the whole-burst gate is gone. Describe per-beat release and `read_release_offset_`.
- Section 4, invariants 1-4 all rest on "every admitted transaction consumes at least one slot", which is
  **no longer true**. Rewrite all four: at most one bypassed entry per ID, all others hold a slot, so a
  per-ID list holds at most `1 + depth` and the total at most `NumIds + depth`; the slot pool is no
  longer the outstanding limit.
- Section 4's "A read burst longer than the pool wedges the port" moves to past tense with a pointer to
  the fix.
- Section 5's paragraph beginning "A bypassed transaction still occupies an order-list entry" already
  states the post-bypass bound. Drop the future tense.
- Section 6 table: `BRoBSize` / `RRoBSize` / `MaxTxnsPerId` now map to real parameters.
- Section 7 rows 1, 2 (allocator), 3 (order table — still open), 6 (release granularity): close rows 1,
  2 and 6. Rows 3, 4 (SRAM/FF split) and 5 (`RobMode` per direction) remain open.
- Section 7a (the stale tick-order comment) closes if you fixed the comment in Task 1.
- Section 8 "Not modelled": drop the `Bypass` bullet, drop the `Bounded per-ID order lists` bullet, and
  drop the `Allocator timing` bullet's claim that the scan is free — there is no scan.

- [ ] **Step 2: Update `architecture.md`**

The NMU RoB paragraph gains one sentence: a transaction whose AXI ID has nothing in flight allocates no
slot and its response is forwarded directly, which is why a read burst longer than the pool is
admissible.

- [ ] **Step 3: Update `docs/backlog.md`**

In the RoB section's "Next" list, strike **items 2, 3 and 3b only**. Item 1 ("Fault-inject the probe")
is about the retired bypass-hit-rate probe, not this work, and stays open. Leave 4, 5, 6. Add:

- the `max_txns_per_id` default of 32 is still `[TBD]` and needs the depth sweep;
- `r_rob_depth = 256` (8 KiB, the paper's design point) is now expressible and unswept;
- **VC allocation** as a new design round: port `floo_vc_assignment.sv:70-88`, per-hop turn-model VC
  assignment with look-ahead routing, per-port VC counts `{2,4,2,4,4}`, `AllowVCOverflow = 0`. It would
  unlock bypass clause 2 and the `NoRoB` NI (25 kGE vs 281 kGE, paper §VI-C) at the cost of the
  `vc1/2/4/8` configuration axis. It lives in `hw/deprecated/`.

- [ ] **Step 4: Verify every file:line citation still resolves**

```bash
grep -oE '`(rob|nmu|nmu_wrap|axi_slave_port)\.hpp:[0-9]+' docs/nmu-rob-microarchitecture.md | sort -u
```
Spot-check each against the current file. A stale line number in an as-built document is worse than no
line number.

- [ ] **Step 5: Commit**

```bash
git add docs/nmu-rob-microarchitecture.md docs/architecture.md docs/backlog.md
git commit -m "docs(nmu): reconcile the RoB as-built spec with the FlooNoC-aligned implementation"
```

---

## Risks

| risk | signal | action |
|---|---|---|
| `FLIT_WIDTH` moves when `ROB_IDX_WIDTH` widens | Task 1 Step 3 shows `FLIT_WIDTH != 408` | BLOCKED. Spec D1 is wrong, and `cmodel_dpi.cpp:32` would block the build anyway. |
| The lzc stack allocator starves a workload that first-fit served | a co-sim run that passed at Task 1 fails at Task 2 | Expected in principle (spec D7). Record the pattern and depth; do **not** restore first-fit. Report and let the user decide. |
| Per-beat release breaks a read-order test | Task 3 Step 5 | The *sequence* of beats is what AXI4 requires. Assert on the sequence, not on pop timing. |
| The wedge does not reproduce on the wire | Task 8 Step 2 passes | BLOCKED. Re-examine `arready` in `nmu_wrap.hpp:188-190`. |
| A 64-beat burst hits a pre-existing co-sim burst bug | scoreboard mismatch, not a hang | BLOCKED. `docs/backlog.md` records an unresolved co-sim burst issue. Do not fix inline. |
| `gen_test_patterns` does not auto-grow `memory_size` | `ValueError: exceeds memory window` | Raise `--memory-size` in the same recipe if you raise `INJECTION_COUNT` with `BURST_LEN > 0`. |
| `Rob` grows to roughly 10 KiB (256 `ReadEntry`, each holding a 32-byte `RBeat`) | a unit test crashes oddly | `Rob` is constructed on the test stack. The Verilator link reserves a 64 MiB PE stack; ctest binaries do not. Heap-allocate the `Rob` in any test that misbehaves. |
| `pop_r()` is assumed to return one beat per call | Task 2's new R tests fail with `nullopt` | It pulls one flit per call and, before Task 3, commits only whole bursts (`rob.hpp:320-323,355-365`). Poll. |
| A death test is named `*DeathTest` | GoogleTest suite-name mismatch | The file's existing suite is `NmuRobDeath` (`test_rob.cpp:523`). Match it. |
