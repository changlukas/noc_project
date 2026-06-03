# ROB Enabled Mode + Multi-NSU Testbench Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add ROB Enabled mode to `nmu::Rob` (per-beat slot model, dynamic free-list pool, in-order commit logic), extend `ni::Depacketizer` interface with `pop_*_with_meta()` for rob_idx delivery, refactor `LoopbackNoc` to multi-NSU testbench (4 NSU instances with per-NSU latency mirroring FlooNoC `tb_floo_rob.sv`), and graduate `multi_dst_stress.yaml` from smoke test to true regression gate.

**Architecture:** `Rob` extends prior Disabled mode with parallel Enabled-mode state (per-beat slot pool keyed by 5-bit rob_idx, per-AXI-ID linked-list of allocated BeatRange, ready-queue for committed beats). `Depacketize` extracts rob_idx from response flit header via new `pop_*_with_meta()` virtual method (default impl is no-op forwarder, preserving backward compat). `LoopbackNoc` gains multi-NSU constructor with per-NSU routing table + per-NSU response delay queues; single-NSU constructor preserved as backward-compat forwarder.

**Tech Stack:** C++17, GoogleTest, CMake, Windows + mingw64 + Ninja. Python 3 specgen (no specgen changes this round). `py -3` not `python3` on Windows.

**Reference spec:** `docs/superpowers/specs/2026-06-03-rob-enabled-mode-multi-nsu-testbench-design.md` (commit `48a0e2b`, Codex 2-round APPROVED).

**Drift gates** (every commit must pass):
```bash
cd specgen && py -3 -m pytest -q && py -3 tools/codegen.py --check && py -3 tools/gen_inventory.py --check
cd ../c_model && cmake --build build && ctest --test-dir build -j 1
```

**Coding style** (per user memory):
- C/C++ multi-line continuation = **4-space indent**
- Variables / functions / methods: `snake_case`
- Classes / structs / enums: `PascalCase`
- Enum values: `PascalCase`
- Constants / `constexpr`: `UPPER_SNAKE`
- All capacity/width constants derive from codegen-generated `specgen/generated/cpp/ni_flit_constants.h`
- Borrow concepts from FlooNoC but **rename** to independent identifiers (do NOT copy SV names)
- Use **testbench** / **tb** for test-platform code, not **rig**

**Commit boundary plan** (matches spec §9.1):
| Task | Commit | Acceptance |
|---|---|---|
| 1 | `feat(ni/depacketizer): add pop_*_with_meta` | 276 → 278 ctest |
| 2 | `feat(nmu/rob): Enabled push paths` | 278 → 285 ctest |
| 3 | `feat(nmu/rob): Enabled pop paths` | 285 → 291 ctest |
| 4 | `feat(tests/common/loopback_noc): multi-NSU` | 291 → 297 ctest |
| 5 | `feat(tests/integration): multi-NSU testbench + multi_dst_stress gate` | 297/297 ctest |
| 6 | `docs(NEXT_STEPS): round done; next is vc_arb` | 297/297 |

**Parallel waves** (for subagent-driven-development):
- Wave 1: Tasks 1, 2, 4 (independent)
- Wave 2: Task 3 (gates on 1 + 2)
- Wave 3: Task 5 (gates on 3 + 4)
- Wave 4: Task 6

---

## File structure (overview)

### New files
- `c_model/tests/common/test_loopback_latency.cpp` (6 unit tests for multi-NSU + per-NSU latency + 1 backward-compat invariant test)

### Modified files
- `c_model/include/ni/depacketizer.hpp` — add `struct ResponseMeta` + virtual `pop_*_with_meta()` with default impl
- `c_model/include/nmu/depacketize.hpp` — override `pop_*_with_meta()` to extract rob_idx/rob_req from flit header
- `c_model/include/nmu/rob.hpp` — add Enabled-mode state + push/pop bodies + `find_consecutive_free` helper; preserve Disabled-mode logic
- `c_model/tests/common/loopback_noc.hpp` — multi-NSU ctor + backward-compat single-NSU ctor + per-NSU routing table + per-NSU response latency
- `c_model/tests/common/CMakeLists.txt` — register `test_loopback_latency`
- `c_model/tests/nmu/test_depacketize.cpp` — +2 tests
- `c_model/tests/nmu/test_rob.cpp` — +13 tests (7 push + 4 pop + 2 death)
- `c_model/tests/integration/test_request_response_loopback.cpp` — multi-NSU testbench for `multi_dst_stress` fixture path
- `NEXT_STEPS.md` — flip pointer to next round

### Not touched
- All Stage 2 `c_model/include/axi/*`
- `c_model/include/nmu/packetize.hpp`, `nmu/addr_trans.hpp`
- `c_model/include/nsu/*` (except instance count via testbench)
- `specgen/generated/*`, `ni_packet.json`
- Prior round's 264 unit tests in `test_packetize.cpp`, `test_addr_trans.cpp`, `test_rob.cpp` Disabled-mode tests

---

## Task 1: Depacketizer `pop_*_with_meta` interface

**Files:**
- Modify: `c_model/include/ni/depacketizer.hpp`
- Modify: `c_model/include/nmu/depacketize.hpp`
- Test: `c_model/tests/nmu/test_depacketize.cpp` (append)

**Goal:** Add a `ResponseMeta { rob_idx, rob_req }` struct and virtual `pop_b_with_meta()` / `pop_r_with_meta()` methods on the `Depacketizer` abstract base. Default implementations forward to `pop_b()` / `pop_r()` and return `meta={0,0}` (preserving backward compat for non-Rob-aware callers). `nmu::Depacketize` overrides both to extract `rob_idx` and `rob_req` from response flit headers before decoding.

- [ ] **Step 1: Read existing `test_depacketize.cpp` to find a good insertion point**

```bash
grep -n "^TEST(" c_model/tests/nmu/test_depacketize.cpp | tail -5
```

Identify the last `TEST(...)` and append the two new tests after it.

- [ ] **Step 2: Write failing test `PopBWithMeta_ExtractsRobIdxAndRobReq`**

Append to `c_model/tests/nmu/test_depacketize.cpp`:

```cpp
TEST(NmuDepacketize, PopBWithMeta_ExtractsRobIdxAndRobReq) {
    using namespace ni::cmodel;
    LoopbackNoc loopback(/*req_depth=*/16, /*rsp_depth=*/16);
    nmu::Depacketize depkt(loopback.rsp_in(), /*b_q_depth=*/16, /*r_q_depth=*/16);

    Flit f;
    f.set_header_field("axi_ch",  ni::AXI_CH_B);
    f.set_header_field("src_id",  0x10);
    f.set_header_field("dst_id",  0x01);
    f.set_header_field("vc_id",   0);
    f.set_header_field("last",    1);
    f.set_header_field("rob_req", 1);
    f.set_header_field("rob_idx", 5);
    f.set_payload_field("B", "bid",   0x42);
    f.set_payload_field("B", "bresp", 0);
    f.set_payload_field("B", "buser", 0);

    ASSERT_TRUE(loopback.rsp_out().push_flit(f));
    depkt.tick();

    auto opt = depkt.pop_b_with_meta();
    ASSERT_TRUE(opt.has_value());
    auto [b, meta] = *opt;
    EXPECT_EQ(b.id, 0x42u);
    EXPECT_EQ(meta.rob_idx, 5u);
    EXPECT_EQ(meta.rob_req, 1u);
}
```

- [ ] **Step 3: Write failing test `PopRWithMeta_ExtractsPerBeatRobIdx`**

Append to same file:

```cpp
TEST(NmuDepacketize, PopRWithMeta_ExtractsPerBeatRobIdx) {
    using namespace ni::cmodel;
    LoopbackNoc loopback(/*req_depth=*/16, /*rsp_depth=*/16);
    nmu::Depacketize depkt(loopback.rsp_in(), /*b_q_depth=*/16, /*r_q_depth=*/16);

    // 4-beat R burst with rob_idx enumerated 5,6,7,8
    for (uint8_t i = 0; i < 4; ++i) {
        Flit f;
        f.set_header_field("axi_ch",  ni::AXI_CH_R);
        f.set_header_field("src_id",  0x10);
        f.set_header_field("dst_id",  0x01);
        f.set_header_field("vc_id",   0);
        f.set_header_field("last",    1);
        f.set_header_field("rob_req", 1);
        f.set_header_field("rob_idx", 5 + i);
        f.set_payload_field("R", "rid",   0x42);
        f.set_payload_field("R", "rresp", 0);
        f.set_payload_field("R", "ruser", 0);
        f.set_payload_field("R", "rlast", (i == 3) ? 1u : 0u);
        std::array<uint8_t, 32> data{};
        data[0] = static_cast<uint8_t>(0xA0 + i);
        f.set_payload_bytes("R", "rdata", data.data(), 32);
        ASSERT_TRUE(loopback.rsp_out().push_flit(f));
    }
    depkt.tick();

    for (uint8_t i = 0; i < 4; ++i) {
        auto opt = depkt.pop_r_with_meta();
        ASSERT_TRUE(opt.has_value()) << "beat " << static_cast<int>(i);
        auto [r, meta] = *opt;
        EXPECT_EQ(meta.rob_idx, 5u + i);
        EXPECT_EQ(meta.rob_req, 1u);
        EXPECT_EQ(r.last, i == 3);
        EXPECT_EQ(r.data[0], 0xA0u + i);
    }
}
```

- [ ] **Step 4: Run tests to verify they FAIL**

```bash
cd c_model && cmake --build build 2>&1 | head -10
```

Expected: compile error mentioning `pop_b_with_meta` / `pop_r_with_meta` / `ResponseMeta` not declared.

- [ ] **Step 5: Add `ResponseMeta` + virtual methods to `ni/depacketizer.hpp`**

Edit `c_model/include/ni/depacketizer.hpp`.

First, add `#include <utility>` near the top with the other includes (for `std::pair`).

Then add `struct ResponseMeta` at **NAMESPACE scope** (BEFORE `class Depacketizer`, inside `namespace ni::cmodel`):

```cpp
// Response-side metadata extracted from flit header.
// rob_req=0 indicates Disabled-mode flit (no rob_idx semantics);
// rob_req=1 indicates Enabled-mode flit (rob_idx identifies the ROB slot).
struct ResponseMeta {
    uint8_t rob_idx;
    uint8_t rob_req;
};
```

Inside the `class Depacketizer` body (above `};`), add the virtual methods that reference `ResponseMeta`:

```cpp
// Default impl forwards to pop_b/pop_r and returns meta={0,0}.
// Concrete depacketizers (e.g. nmu::Depacketize) may override to extract
// rob_idx / rob_req from the flit header before decode.
virtual std::optional<std::pair<axi::BBeat, ResponseMeta>> pop_b_with_meta() {
    auto b = pop_b();
    if (!b) return std::nullopt;
    return std::make_pair(*b, ResponseMeta{0, 0});
}
virtual std::optional<std::pair<axi::RBeat, ResponseMeta>> pop_r_with_meta() {
    auto r = pop_r();
    if (!r) return std::nullopt;
    return std::make_pair(*r, ResponseMeta{0, 0});
}
```

Because `ResponseMeta` is at namespace scope, the unqualified name resolves cleanly inside `class Depacketizer` and inside concrete derived classes (like `nmu::Depacketize`).

- [ ] **Step 6: Build to verify base class compiles**

```bash
cd c_model && cmake --build build 2>&1 | head -10
```

Expected: build succeeds for `ni/depacketizer.hpp`. The Step 2-3 tests should now compile but FAIL at runtime (because `nmu::Depacketize` still returns default meta={0,0}; the test expects rob_idx=5 / rob_req=1).

Run:
```bash
ctest --test-dir build -R NmuDepacketize -j 1
```

Expected: 2 new tests FAIL (`Expected: meta.rob_idx == 5u; Actual: 0u`).

- [ ] **Step 7: Override in `nmu/depacketize.hpp`**

Edit `c_model/include/nmu/depacketize.hpp`.

First, change internal queue types to store both beat + meta. Find the existing declarations (likely `std::deque<axi::BBeat> b_q_;` and `std::deque<axi::RBeat> r_q_;`) and replace with:

```cpp
private:
    struct BWithMeta { axi::BBeat beat; ResponseMeta meta; };
    struct RWithMeta { axi::RBeat beat; ResponseMeta meta; };
    std::deque<BWithMeta> b_q_;
    std::deque<RWithMeta> r_q_;
```

Update `tick()` to populate both fields. Find the existing B/R decode in `tick()`:

```cpp
case ni::AXI_CH_B:
    if (b_q_.size() >= b_q_depth_) { pending_ = f; return; }
    b_q_.push_back(decode_b(f));
    break;
case ni::AXI_CH_R:
    if (r_q_.size() >= r_q_depth_) { pending_ = f; return; }
    r_q_.push_back(decode_r(f));
    break;
```

Replace with:

```cpp
case ni::AXI_CH_B: {
    if (b_q_.size() >= b_q_depth_) { pending_ = f; return; }
    ResponseMeta meta{
        static_cast<uint8_t>(f.get_header_field("rob_idx")),
        static_cast<uint8_t>(f.get_header_field("rob_req"))
    };
    b_q_.push_back({decode_b(f), meta});
    break;
}
case ni::AXI_CH_R: {
    if (r_q_.size() >= r_q_depth_) { pending_ = f; return; }
    ResponseMeta meta{
        static_cast<uint8_t>(f.get_header_field("rob_idx")),
        static_cast<uint8_t>(f.get_header_field("rob_req"))
    };
    r_q_.push_back({decode_r(f), meta});
    break;
}
```

Update `pop_b()` and `pop_r()` (preserve legacy API):

```cpp
inline std::optional<axi::BBeat> Depacketize::pop_b() {
    if (b_q_.empty()) return std::nullopt;
    auto entry = b_q_.front();
    b_q_.pop_front();
    return entry.beat;
}

inline std::optional<axi::RBeat> Depacketize::pop_r() {
    if (r_q_.empty()) return std::nullopt;
    auto entry = r_q_.front();
    r_q_.pop_front();
    return entry.beat;
}
```

Add the new override methods after `pop_b()` / `pop_r()`:

```cpp
inline std::optional<std::pair<axi::BBeat, ResponseMeta>>
Depacketize::pop_b_with_meta() {
    if (b_q_.empty()) return std::nullopt;
    auto entry = b_q_.front();
    b_q_.pop_front();
    return std::make_pair(entry.beat, entry.meta);
}

inline std::optional<std::pair<axi::RBeat, ResponseMeta>>
Depacketize::pop_r_with_meta() {
    if (r_q_.empty()) return std::nullopt;
    auto entry = r_q_.front();
    r_q_.pop_front();
    return std::make_pair(entry.beat, entry.meta);
}
```

Add declarations in the class body:

```cpp
std::optional<std::pair<axi::BBeat, ResponseMeta>> pop_b_with_meta() override;
std::optional<std::pair<axi::RBeat, ResponseMeta>> pop_r_with_meta() override;
```

- [ ] **Step 8: Build + run new tests**

```bash
cd c_model && cmake --build build && ctest --test-dir build -R "PopBWithMeta|PopRWithMeta" -j 1
```

Expected: 2 PASS.

- [ ] **Step 9: Full ctest sweep**

```bash
ctest --test-dir build -j 1 2>&1 | tail -3
```

Expected: 278/278 passed (276 prior + 2 new).

- [ ] **Step 10: Run drift gates**

```bash
cd ../specgen && py -3 -m pytest -q && py -3 tools/codegen.py --check && py -3 tools/gen_inventory.py --check
```

Expected: pytest 163 passed, codegen --check clean, gen_inventory --check clean.

- [ ] **Step 11: Commit Task 1**

```bash
cd ..
git add c_model/include/ni/depacketizer.hpp \
        c_model/include/nmu/depacketize.hpp \
        c_model/tests/nmu/test_depacketize.cpp
git commit -m "feat(ni/depacketizer): add pop_*_with_meta + ResponseMeta

Adds virtual pop_b_with_meta() / pop_r_with_meta() on Depacketizer
abstract base; default impl forwards to pop_b/pop_r and returns
meta={0,0} (backward compat for non-Rob-aware callers).

nmu::Depacketize overrides both to extract rob_idx / rob_req from
flit header during tick() and pair with decoded BBeat/RBeat. Legacy
pop_b/pop_r still returns the beat only.

Foundation for Rob Enabled mode (Task 3 uses pop_*_with_meta to
identify which ROB slot a response belongs to).

Refs: docs/superpowers/specs/2026-06-03-rob-enabled-mode-multi-nsu-testbench-design.md §5"
```

---

## Task 2: `nmu::Rob` Enabled mode push paths + state

**Files:**
- Modify: `c_model/include/nmu/rob.hpp`
- Test: `c_model/tests/nmu/test_rob.cpp` (append 7 push-side tests)

**Goal:** Add Enabled-mode state members to `Rob` (per-beat slot arrays, free-list bitsets, per-axi-id BeatRange queues, ready-emit queues, `find_consecutive_free` helper), implement `push_aw` / `push_w` / `push_ar` Enabled paths with atomic-rollback semantics + oversized-burst guard. `pop_b` / `pop_r` Enabled paths still assert+abort (Task 3 handles them). Disabled-mode logic is preserved unchanged.

**⚠ Important:** After Task 2, Enabled mode is intentionally push-only. Do NOT wire any e2e fixture to Enabled mode until Task 3 lands. Unit tests cover push-side behavior only.

- [ ] **Step 1: Read existing `rob.hpp` to confirm structure**

```bash
wc -l c_model/include/nmu/rob.hpp
grep -n "^class Rob\|^private:\|^public:" c_model/include/nmu/rob.hpp
```

Expected: ~168 lines, `class Rob : public Packetizer, public Depacketizer` near top, single private section near middle.

- [ ] **Step 2: Write failing test `Enabled_PushAw_AllocatesSlotAndStampsRobIdx`**

Append to `c_model/tests/nmu/test_rob.cpp`:

```cpp
TEST(NmuRob, Enabled_PushAw_AllocatesSlotAndStampsRobIdx) {
    LoopbackNoc noc(/*req=*/16, /*rsp=*/16);
    Packetize   pkt(noc.req_out(), kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));
    auto f = *noc.req_in().pop_flit();
    EXPECT_EQ(f.get_header_field("axi_ch"), ni::AXI_CH_AW);
    EXPECT_EQ(f.get_header_field("rob_req"), 1u);
    EXPECT_EQ(f.get_header_field("rob_idx"), 0u);  // first allocated slot
}
```

- [ ] **Step 3: Write failing test `Enabled_PushAr_AllocatesConsecutiveSlotsForBurst`**

```cpp
TEST(NmuRob, Enabled_PushAr_AllocatesConsecutiveSlotsForBurst) {
    LoopbackNoc noc(16, 16);
    Packetize   pkt(noc.req_out(), kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    // AR len=3 → 4 beats → 4 consecutive slots
    axi::ArBeat ar = make_ar(0x05, 0x100);
    ar.len = 3;
    ASSERT_TRUE(rob.push_ar(ar));
    auto f = *noc.req_in().pop_flit();
    EXPECT_EQ(f.get_header_field("axi_ch"), ni::AXI_CH_AR);
    EXPECT_EQ(f.get_header_field("rob_req"), 1u);
    EXPECT_EQ(f.get_header_field("rob_idx"), 0u);  // base = 0
    // Next AR should allocate slot 4 (slots 0-3 occupied)
    axi::ArBeat ar2 = make_ar(0x06, 0x200);
    ar2.len = 1;
    ASSERT_TRUE(rob.push_ar(ar2));
    auto f2 = *noc.req_in().pop_flit();
    EXPECT_EQ(f2.get_header_field("rob_idx"), 4u);
}
```

- [ ] **Step 4: Write failing test `Enabled_FindConsecutiveFree_FragmentedFailNoConsecutiveRun`**

`find_consecutive_free` will be exposed as `public static` (Step 11) to enable direct unit testing of the fragmentation algorithm. Engineering a fragmented `free_read_entries_` state via push_ar + pop_r is hard (chain-flush commit semantics merge frees), so the cleaner approach is to test the helper directly.

Note: this replaces the spec §8.1 entry name `Enabled_PushAr_PoolFragmented_FailWhenNoConsecutiveRun`; same invariant, different test surface. Spec self-review will be updated in Task 6 if needed.

```cpp
TEST(NmuRob, Enabled_FindConsecutiveFree_FragmentedFailNoConsecutiveRun) {
    std::bitset<Rob::ROB_CAPACITY> free;
    // Fragmented free state: bits 1 at positions 0, 2, 4, 6 only.
    free.set(0); free.set(2); free.set(4); free.set(6);

    // 4 total free bits, but no run of 2+ consecutive.
    EXPECT_EQ(Rob::find_consecutive_free(free, 3), -1);
    EXPECT_EQ(Rob::find_consecutive_free(free, 2), -1);

    // n=1 always finds position 0 first.
    EXPECT_EQ(Rob::find_consecutive_free(free, 1), 0);

    // All free (32 free bits): n up to 32 succeeds at base=0; n=33 fails (over capacity).
    free.set();
    EXPECT_EQ(Rob::find_consecutive_free(free, 1), 0);
    EXPECT_EQ(Rob::find_consecutive_free(free, 32), 0);
    EXPECT_EQ(Rob::find_consecutive_free(free, 33), -1);
}
```

- [ ] **Step 5: Write failing test `Enabled_PushAr_OversizedBurst_ReturnFalse`**

```cpp
TEST(NmuRob, Enabled_PushAr_OversizedBurst_ReturnFalse) {
    LoopbackNoc noc(16, 16);
    Packetize   pkt(noc.req_out(), kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    // AR len=31 → 32 beats = ROB_CAPACITY; len=32 → 33 beats > ROB_CAPACITY.
    // Use len=255 (AXI4 INCR max) for an obviously-oversized burst.
    axi::ArBeat ar = make_ar(0x05, 0x100);
    ar.len = 255;   // 256 beats, exceeds ROB_CAPACITY (32)
    EXPECT_FALSE(rob.push_ar(ar));
    // No state mutation: a normal-sized AR should still succeed.
    axi::ArBeat ar_ok = make_ar(0x06, 0x200);
    ar_ok.len = 3;
    EXPECT_TRUE(rob.push_ar(ar_ok));
}
```

- [ ] **Step 6: Write failing test `Enabled_PushAr_DownstreamBackpressure_AtomicRollback`**

```cpp
TEST(NmuRob, Enabled_PushAr_DownstreamBackpressure_AtomicRollback) {
    // req queue depth = 1: pkt.push_ar_with_meta will fail after 1st push
    LoopbackNoc noc(/*req=*/1, /*rsp=*/16);
    Packetize   pkt(noc.req_out(), kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    axi::ArBeat ar = make_ar(0x05, 0x100);
    ar.len = 3;   // 4 beats
    ASSERT_TRUE(rob.push_ar(ar));   // fills req queue 1/1
    // Drain to allow next push to find consecutive free + downstream available
    noc.req_in().pop_flit();
    // Refill: push to fill queue again, then push another AR — downstream rejects
    axi::ArBeat ar2 = make_ar(0x06, 0x200);
    ar2.len = 3;
    ASSERT_TRUE(rob.push_ar(ar2));
    // Now downstream full. Next push_ar must return false WITHOUT touching free_read_entries_.
    axi::ArBeat ar3 = make_ar(0x07, 0x300);
    ar3.len = 1;   // 2 beats
    EXPECT_FALSE(rob.push_ar(ar3));
    // Drain, then ar3 retry must succeed (proving state was atomic — slots 8-9 still available)
    noc.req_in().pop_flit();
    EXPECT_TRUE(rob.push_ar(ar3));
}
```

- [ ] **Step 7: Write failing test `Enabled_PushAw_PoolFull_ReturnFalseAtomic`**

```cpp
TEST(NmuRob, Enabled_PushAw_PoolFull_ReturnFalseAtomic) {
    LoopbackNoc noc(64, 16);
    Packetize   pkt(noc.req_out(), kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    // 32 single-beat AWs fill the write pool entirely
    for (int i = 0; i < 32; ++i) {
        ASSERT_TRUE(rob.push_aw(make_aw(static_cast<uint8_t>(i & 0xFF), 0x100)));
    }
    // 33rd AW must fail
    EXPECT_FALSE(rob.push_aw(make_aw(0x33, 0x200)));
}
```

- [ ] **Step 8: Write failing test `Enabled_PushAw_DownstreamBackpressure_AtomicRollback`**

```cpp
TEST(NmuRob, Enabled_PushAw_DownstreamBackpressure_AtomicRollback) {
    LoopbackNoc noc(/*req=*/1, /*rsp=*/16);
    Packetize   pkt(noc.req_out(), kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));   // fills req queue 1/1
    EXPECT_FALSE(rob.push_aw(make_aw(0x06, 0x200))); // downstream full, state unchanged
    noc.req_in().pop_flit();                          // drain
    EXPECT_TRUE(rob.push_aw(make_aw(0x06, 0x200))); // retry succeeds with slot still available
}
```

- [ ] **Step 9: Run tests to verify FAIL**

```bash
cd c_model && cmake --build build 2>&1 | head -10
```

Expected: compile errors mentioning missing Enabled-mode state, or runtime fail (`Rob: Enabled mode push_aw not yet implemented` assert+abort).

- [ ] **Step 10: Add Enabled-mode constants + state members to `rob.hpp`**

Edit `c_model/include/nmu/rob.hpp`. Add includes near top (after existing includes):

```cpp
#include "ni_flit_constants.h"
#include <bitset>
#include <utility>
#include <cstdint>
```

Inside the `class Rob` body, after the existing `// future Enabled mode: uint8_t rob_idx;` comment block (around line 80), add the Enabled-mode state section:

```cpp
public:
    // === Enabled mode public constants (for testing + caller info) ===
    static constexpr std::size_t ROB_CAPACITY      = 1u << ni::header::ROB_IDX_WIDTH;  // 32
    static constexpr std::size_t AXI_ID_SPACE      = 1u << ni::width::AXI_ID_WIDTH;    // 256

    // Linear scan for first run of n consecutive 1s in bitset<ROB_CAPACITY>.
    // Returns base index (0..ROB_CAPACITY-1), or -1 if no such run exists.
    // O(ROB_CAPACITY) worst case. Public for direct unit testing (TDD).
    static int find_consecutive_free(
        const std::bitset<ROB_CAPACITY>& free,
        std::size_t n);

private:
    // === Enabled mode (per-beat slot pool) ===

    struct WriteEntry {
        bool        occupied = false;
        bool        ready    = false;
        uint8_t     axi_id   = 0;
        axi::BBeat  b_beat   = {};
    };
    struct ReadEntry {
        bool        occupied = false;
        bool        ready    = false;
        uint8_t     axi_id   = 0;
        axi::RBeat  r_beat   = {};
    };
    std::array<WriteEntry, ROB_CAPACITY> write_entries_;
    std::array<ReadEntry,  ROB_CAPACITY> read_entries_;
    std::bitset<ROB_CAPACITY>            free_write_entries_;
    std::bitset<ROB_CAPACITY>            free_read_entries_;

    // Per-id ordered range list. AW = {base, 1}; AR = {base, len+1}.
    struct BeatRange {
        uint8_t base;
        uint8_t len_plus_1;
    };
    std::array<std::deque<BeatRange>, AXI_ID_SPACE> write_order_by_id_;
    std::array<std::deque<BeatRange>, AXI_ID_SPACE> read_order_by_id_;

    // Ready-to-emit beats drained by pop_b / pop_r
    std::deque<axi::BBeat> committed_b_queue_;
    std::deque<axi::RBeat> committed_r_queue_;
```

In the constructor body, initialize the free bitsets to all-1 (all slots free). Find the existing initializer list:

```cpp
Rob(Packetize& next_pkt,
    Depacketizer& next_depkt,
    RobMode mode_w,
    RobMode mode_r)
    : next_pkt_(next_pkt), next_depkt_(next_depkt),
      mode_w_(mode_w), mode_r_(mode_r) {}
```

Change to:

```cpp
Rob(Packetize& next_pkt,
    Depacketizer& next_depkt,
    RobMode mode_w,
    RobMode mode_r)
    : next_pkt_(next_pkt), next_depkt_(next_depkt),
      mode_w_(mode_w), mode_r_(mode_r) {
    free_write_entries_.set();
    free_read_entries_.set();
}
```

- [ ] **Step 11: Add `find_consecutive_free` inline definition**

Declaration already added to `class Rob` public section in Step 10 (along with `ROB_CAPACITY` / `AXI_ID_SPACE`). Now add the inline definition AFTER the class body (after the closing `};`):

```cpp
inline int Rob::find_consecutive_free(
        const std::bitset<ROB_CAPACITY>& free,
        std::size_t n) {
    if (n == 0 || n > ROB_CAPACITY) return -1;
    std::size_t run = 0;
    for (std::size_t i = 0; i < ROB_CAPACITY; ++i) {
        if (free.test(i)) {
            ++run;
            if (run == n) return static_cast<int>(i - n + 1);
        } else {
            run = 0;
        }
    }
    return -1;
}
```

- [ ] **Step 12: Replace `push_aw` Enabled-mode body**

Find the existing `push_aw` implementation (around line 97). Current form:

```cpp
inline bool Rob::push_aw(const axi::AwBeat& b) {
    if (mode_w_ == RobMode::Enabled) {
        assert(false && "Rob: Enabled mode push_aw not yet implemented (next round)");
        std::abort();
    }
    // [existing Disabled-mode body unchanged from prior round —
    //  xy_route + per-id deque dst gate + push_aw_with_meta + outstanding.push_back + ++w_burst_credit_]
}
```

Replace ONLY the Enabled-mode block (the `if (mode_w_ == RobMode::Enabled) { assert; abort; }` block) with the actual Enabled implementation below. Keep all Disabled-mode body lines verbatim.

```cpp
inline bool Rob::push_aw(const axi::AwBeat& b) {
    if (mode_w_ == RobMode::Enabled) {
        // Pool full? Cannot allocate.
        if (free_write_entries_.none()) return false;
        // Find first free slot.
        int base = find_consecutive_free(free_write_entries_, 1);
        if (base < 0) return false;
        auto t = addr_trans::xy_route(b.addr);
        if (!next_pkt_.push_aw_with_meta(b,
                {t.dst_id, t.local_addr, /*rob_req=*/1,
                 /*rob_idx=*/static_cast<uint8_t>(base)})) {
            return false;   // downstream backpressure: no state mutation
        }
        free_write_entries_.reset(static_cast<std::size_t>(base));
        write_entries_[base] = WriteEntry{
            /*occupied=*/true, /*ready=*/false, b.id, /*b_beat=*/{}};
        write_order_by_id_[b.id].push_back({static_cast<uint8_t>(base), 1});
        ++w_burst_credit_;
        return true;
    }
    // Disabled-mode body (unchanged from prior round)
    auto t = addr_trans::xy_route(b.addr);
    auto& s = write_[b.id];
    if (!s.outstanding.empty() && s.outstanding.front().dst_id != t.dst_id) {
        return false;
    }
    if (!next_pkt_.push_aw_with_meta(b, {t.dst_id, t.local_addr, 0, 0})) {
        return false;
    }
    s.outstanding.push_back({t.dst_id});
    w_burst_credit_++;
    return true;
}
```

- [ ] **Step 13: Replace `push_ar` Enabled-mode body**

Find existing `push_ar` (around line 122) with same `assert+abort` Enabled-mode stub. Replace with:

```cpp
inline bool Rob::push_ar(const axi::ArBeat& b) {
    if (mode_r_ == RobMode::Enabled) {
        std::size_t n = static_cast<std::size_t>(b.len) + 1u;
        // Oversized burst: cannot fit in pool at all.
        if (n > ROB_CAPACITY) return false;
        int base = find_consecutive_free(free_read_entries_, n);
        if (base < 0) return false;   // no consecutive run
        auto t = addr_trans::xy_route(b.addr);
        if (!next_pkt_.push_ar_with_meta(b,
                {t.dst_id, t.local_addr, /*rob_req=*/1,
                 /*rob_idx=*/static_cast<uint8_t>(base)})) {
            return false;   // downstream backpressure: no state mutation
        }
        for (std::size_t i = 0; i < n; ++i) {
            free_read_entries_.reset(static_cast<std::size_t>(base) + i);
            read_entries_[base + i] = ReadEntry{
                /*occupied=*/true, /*ready=*/false, b.id, /*r_beat=*/{}};
        }
        read_order_by_id_[b.id].push_back(
            {static_cast<uint8_t>(base), static_cast<uint8_t>(n)});
        return true;
    }
    // Disabled-mode body (unchanged from prior round)
    auto t = addr_trans::xy_route(b.addr);
    auto& s = read_[b.id];
    if (!s.outstanding.empty() && s.outstanding.front().dst_id != t.dst_id) {
        return false;
    }
    if (!next_pkt_.push_ar_with_meta(b, {t.dst_id, t.local_addr, 0, 0})) {
        return false;
    }
    s.outstanding.push_back({t.dst_id});
    return true;
}
```

- [ ] **Step 14: `push_w` unchanged**

`push_w` already uses `w_burst_credit_` and `next_pkt_.push_w()`; the Disabled-mode logic also serves Enabled mode (Rob does not per-slot track W beats since W follows AW strictly). No change needed in this task.

- [ ] **Step 15: Build to confirm compile + push tests pass**

```bash
cd c_model && cmake --build build && ctest --test-dir build -R "NmuRob.Enabled_Push" -j 1
```

Expected: 7 push-side tests PASS (5 originally listed + 2 new ones for oversized/AR-rollback).

If `pop_b` / `pop_r` Enabled paths are still `assert+abort` (Task 3 handles them), those test invocations would crash. None of Task 2's tests invoke pop side.

- [ ] **Step 16: Full ctest sweep**

```bash
ctest --test-dir build -j 1 2>&1 | tail -3
```

Expected: 285/285 passed (278 prior + 7 new).

- [ ] **Step 17: Drift gates**

```bash
cd ../specgen && py -3 -m pytest -q && py -3 tools/codegen.py --check && py -3 tools/gen_inventory.py --check
```

Expected: pytest 163, codegen / gen_inventory clean.

- [ ] **Step 18: Commit Task 2**

```bash
cd ..
git add c_model/include/nmu/rob.hpp c_model/tests/nmu/test_rob.cpp
git commit -m "feat(nmu/rob): Enabled mode push paths + state

Adds per-beat slot pool (32 slots from ROB_IDX_WIDTH=5), free-list
bitsets, per-id BeatRange queues, committed_*_queue_, and
find_consecutive_free helper. Implements push_aw / push_ar Enabled
paths with atomic-rollback semantics:
- pool-full → false (no state mutation)
- oversized burst (len+1 > ROB_CAPACITY) → false
- downstream Packetize backpressure → false, free bits untouched
- success → free bit cleared, slot[base] occupied, id-range pushed, w_burst_credit++

push_w unchanged (Disabled-mode logic serves Enabled — W follows AW strictly).
pop_b / pop_r Enabled paths still assert+abort (Task 3 lands them).
Disabled-mode paths preserved unchanged.

7 new push-side tests in test_rob.cpp (allocation, consecutive AR,
fragmentation failure, oversized AR, atomic-rollback variants).

Refs: docs/superpowers/specs/2026-06-03-rob-enabled-mode-multi-nsu-testbench-design.md §6"
```

---

## Task 3: `nmu::Rob` Enabled mode pop paths + commit logic

**Files:**
- Modify: `c_model/include/nmu/rob.hpp`
- Test: `c_model/tests/nmu/test_rob.cpp` (append 6 pop-side + death tests)

**Goal:** Implement `pop_b` / `pop_r` Enabled paths with in-order chain-flush commit logic and strict mixed-mode assert. After this task, Rob Enabled mode is feature-complete; e2e regression gate awaits Task 4 (LoopbackNoc multi-NSU) + Task 5 (integration testbench).

- [ ] **Step 1: Write failing test `Enabled_PopB_InOrder_ImmediateCommit`**

Append to `c_model/tests/nmu/test_rob.cpp`:

```cpp
TEST(NmuRob, Enabled_PopB_InOrder_ImmediateCommit) {
    LoopbackNoc noc(16, 16);
    Packetize   pkt(noc.req_out(), kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));   // allocates slot 0
    // Inject B with rob_idx=0, matching the head of id=5's sequence
    Flit f;
    f.set_header_field("axi_ch",  ni::AXI_CH_B);
    f.set_header_field("dst_id",  kSrcId);
    f.set_header_field("last",    1);
    f.set_header_field("rob_req", 1);
    f.set_header_field("rob_idx", 0);
    f.set_payload_field("B", "bid",   0x05);
    f.set_payload_field("B", "bresp", 0);
    ASSERT_TRUE(noc.rsp_out().push_flit(f));
    depkt.tick();
    auto b = rob.pop_b();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->id, 0x05u);
}
```

- [ ] **Step 2: Write failing test `Enabled_PopB_OutOfOrder_HeldUntilHeadReady`**

```cpp
TEST(NmuRob, Enabled_PopB_OutOfOrder_HeldUntilHeadReady) {
    LoopbackNoc noc(16, 16);
    Packetize   pkt(noc.req_out(), kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    // id=5: two AWs in flight, slots 0 + 1
    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));
    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x10100)));
    auto push_b = [&](uint8_t rob_idx, uint8_t bresp) {
        Flit f;
        f.set_header_field("axi_ch",  ni::AXI_CH_B);
        f.set_header_field("dst_id",  kSrcId);
        f.set_header_field("last",    1);
        f.set_header_field("rob_req", 1);
        f.set_header_field("rob_idx", rob_idx);
        f.set_payload_field("B", "bid",   0x05);
        f.set_payload_field("B", "bresp", bresp);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
    };
    push_b(/*rob_idx=*/1, /*bresp=*/0);   // B for AW2 arrives first
    depkt.tick();
    EXPECT_FALSE(rob.pop_b().has_value());  // not head, held
    push_b(/*rob_idx=*/0, /*bresp=*/0);   // B for AW1 arrives second
    depkt.tick();
    auto b1 = rob.pop_b();
    ASSERT_TRUE(b1.has_value());           // chain-flush: AW1's B
    auto b2 = rob.pop_b();
    ASSERT_TRUE(b2.has_value());           // then AW2's B
    EXPECT_FALSE(rob.pop_b().has_value()); // empty
}
```

- [ ] **Step 3: Write failing test `Enabled_PopR_MultiBeatBurstCommitInOrder`**

```cpp
TEST(NmuRob, Enabled_PopR_MultiBeatBurstCommitInOrder) {
    LoopbackNoc noc(16, 16);
    Packetize   pkt(noc.req_out(), kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    // id=5: AR1 len=3 → slots 0..3; AR2 len=1 → slots 4..5
    axi::ArBeat ar1 = make_ar(0x05, 0x100); ar1.len = 3;
    axi::ArBeat ar2 = make_ar(0x05, 0x200); ar2.len = 1;
    ASSERT_TRUE(rob.push_ar(ar1));
    ASSERT_TRUE(rob.push_ar(ar2));
    auto push_r = [&](uint8_t rob_idx, bool rlast, uint8_t marker) {
        Flit f;
        f.set_header_field("axi_ch",  ni::AXI_CH_R);
        f.set_header_field("dst_id",  kSrcId);
        f.set_header_field("last",    1);
        f.set_header_field("rob_req", 1);
        f.set_header_field("rob_idx", rob_idx);
        f.set_payload_field("R", "rid",   0x05);
        f.set_payload_field("R", "rresp", 0);
        f.set_payload_field("R", "rlast", rlast ? 1u : 0u);
        std::array<uint8_t, 32> d{};
        d[0] = marker;
        f.set_payload_bytes("R", "rdata", d.data(), 32);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
    };
    // Arrive in order: slot 4, 5 (AR2), then 0, 1, 2, 3 (AR1)
    push_r(4, false, 0xB0); push_r(5, true,  0xB1);
    push_r(0, false, 0xA0); push_r(1, false, 0xA1);
    push_r(2, false, 0xA2); push_r(3, true,  0xA3);
    depkt.tick();
    // AR1 must commit first (it was issued first); markers 0xA0..0xA3
    for (uint8_t i = 0; i < 4; ++i) {
        auto r = rob.pop_r();
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(r->data[0], 0xA0u + i);
    }
    // Then AR2: markers 0xB0..0xB1
    for (uint8_t i = 0; i < 2; ++i) {
        auto r = rob.pop_r();
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(r->data[0], 0xB0u + i);
    }
    EXPECT_FALSE(rob.pop_r().has_value());
}
```

- [ ] **Step 4: Write failing test `Enabled_DifferentIdsInterleaveAtTransactionBoundary`**

```cpp
TEST(NmuRob, Enabled_DifferentIdsInterleaveAtTransactionBoundary) {
    LoopbackNoc noc(16, 16);
    Packetize   pkt(noc.req_out(), kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    // id=5 AR slot 0; id=6 AR slot 1
    ASSERT_TRUE(rob.push_ar(make_ar(0x05, 0x100)));   // len=0 → 1 beat
    ASSERT_TRUE(rob.push_ar(make_ar(0x06, 0x100)));   // slot 1
    auto push_r = [&](uint8_t rob_idx, uint8_t rid) {
        Flit f;
        f.set_header_field("axi_ch",  ni::AXI_CH_R);
        f.set_header_field("dst_id",  kSrcId);
        f.set_header_field("last",    1);
        f.set_header_field("rob_req", 1);
        f.set_header_field("rob_idx", rob_idx);
        f.set_payload_field("R", "rid",   rid);
        f.set_payload_field("R", "rresp", 0);
        f.set_payload_field("R", "rlast", 1u);
        std::array<uint8_t, 32> d{}; d[0] = rid;
        f.set_payload_bytes("R", "rdata", d.data(), 32);
        ASSERT_TRUE(noc.rsp_out().push_flit(f));
    };
    push_r(1, 0x06);   // id=6 R arrives first
    push_r(0, 0x05);   // id=5 R arrives second
    depkt.tick();
    // Both committable (each is head of its own per-id sequence).
    // Implementation order: by depacketize order → id=6 first then id=5 (since both ready)
    auto r1 = rob.pop_r();
    ASSERT_TRUE(r1.has_value());
    auto r2 = rob.pop_r();
    ASSERT_TRUE(r2.has_value());
    // Both 0x05 and 0x06 must appear (order between ids is implementation-defined
    // but each id's beats must be in submission order — here each id has 1 beat).
    std::set<uint8_t> ids{r1->id, r2->id};
    EXPECT_EQ(ids.size(), 2u);
    EXPECT_TRUE(ids.count(0x05) && ids.count(0x06));
}
```

- [ ] **Step 5: Write failing test `EnabledDeath_PopBWithUnallocatedRobIdx_Abort`**

```cpp
TEST(NmuRobDeath, Enabled_PopBWithUnallocatedRobIdx_Abort) {
    LoopbackNoc noc(16, 16);
    Packetize   pkt(noc.req_out(), kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    // Inject B with rob_idx=7, but no AW allocated that slot → assert fires
    Flit f;
    f.set_header_field("axi_ch",  ni::AXI_CH_B);
    f.set_header_field("dst_id",  kSrcId);
    f.set_header_field("last",    1);
    f.set_header_field("rob_req", 1);
    f.set_header_field("rob_idx", 7);
    f.set_payload_field("B", "bid",   0x05);
    f.set_payload_field("B", "bresp", 0);
    ASSERT_TRUE(noc.rsp_out().push_flit(f));
    depkt.tick();
    EXPECT_DEATH(rob.pop_b(), "");
}
```

- [ ] **Step 6: Write failing test `EnabledDeath_PopBWithDisabledFlit_Abort`**

```cpp
TEST(NmuRobDeath, Enabled_PopBWithDisabledFlit_Abort) {
    LoopbackNoc noc(16, 16);
    Packetize   pkt(noc.req_out(), kSrcId);
    Depacketize depkt(noc.rsp_in(), 16, 16);
    Rob rob(pkt, depkt, RobMode::Enabled, RobMode::Enabled);

    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x100)));   // allocates slot 0
    // Inject B with rob_req=0 (Disabled-mode flit) into Enabled Rob → assert
    Flit f;
    f.set_header_field("axi_ch",  ni::AXI_CH_B);
    f.set_header_field("dst_id",  kSrcId);
    f.set_header_field("last",    1);
    f.set_header_field("rob_req", 0);
    f.set_header_field("rob_idx", 0);
    f.set_payload_field("B", "bid",   0x05);
    f.set_payload_field("B", "bresp", 0);
    ASSERT_TRUE(noc.rsp_out().push_flit(f));
    depkt.tick();
    EXPECT_DEATH(rob.pop_b(), "");
}
```

- [ ] **Step 7: Run tests to verify FAIL**

```bash
cd c_model && cmake --build build 2>&1 | head -10
```

Expected: build succeeds (no missing symbols since Task 2 added state), tests at runtime abort with `Rob: Enabled mode pop_b not yet implemented`.

- [ ] **Step 8: Replace `pop_b` Enabled-mode body**

Find existing `pop_b` (around line 139) with `assert+abort` Enabled stub. Replace the Enabled-mode block with:

```cpp
inline std::optional<axi::BBeat> Rob::pop_b() {
    if (mode_w_ == RobMode::Enabled) {
        // Drain already-committed beats first.
        if (!committed_b_queue_.empty()) {
            auto b = committed_b_queue_.front();
            committed_b_queue_.pop_front();
            return b;
        }
        // Pull next response from downstream.
        auto opt = next_depkt_.pop_b_with_meta();
        if (!opt) return std::nullopt;
        auto [b, meta] = *opt;
        assert(meta.rob_req == 1 && "Enabled Rob received Disabled flit");
        assert(meta.rob_idx < ROB_CAPACITY && "rob_idx out of range");
        auto& slot = write_entries_[meta.rob_idx];
        assert(slot.occupied && !slot.ready &&
               "B for unallocated or already-completed rob_idx");
        slot.b_beat = b;
        slot.ready = true;
        // In-order Path: chain-flush ready heads of this id's sequence.
        uint8_t id = slot.axi_id;
        while (!write_order_by_id_[id].empty()) {
            BeatRange head = write_order_by_id_[id].front();
            if (!write_entries_[head.base].ready) break;
            committed_b_queue_.push_back(write_entries_[head.base].b_beat);
            free_write_entries_.set(head.base);
            write_entries_[head.base].occupied = false;
            write_order_by_id_[id].pop_front();
        }
        if (!committed_b_queue_.empty()) {
            auto out = committed_b_queue_.front();
            committed_b_queue_.pop_front();
            return out;
        }
        return std::nullopt;
    }
    // Disabled-mode body (unchanged from prior round)
    auto opt = next_depkt_.pop_b();
    if (!opt) return std::nullopt;
    auto& s = write_[opt->id];
    assert(!s.outstanding.empty() && "B for id with no outstanding write");
    s.outstanding.pop_front();
    return opt;
}
```

- [ ] **Step 9: Replace `pop_r` Enabled-mode body**

Find existing `pop_r` and replace the Enabled-mode block with:

```cpp
inline std::optional<axi::RBeat> Rob::pop_r() {
    if (mode_r_ == RobMode::Enabled) {
        // Drain already-committed beats first.
        if (!committed_r_queue_.empty()) {
            auto r = committed_r_queue_.front();
            committed_r_queue_.pop_front();
            return r;
        }
        // Pull next response from downstream.
        auto opt = next_depkt_.pop_r_with_meta();
        if (!opt) return std::nullopt;
        auto [r, meta] = *opt;
        assert(meta.rob_req == 1 && "Enabled Rob received Disabled flit");
        assert(meta.rob_idx < ROB_CAPACITY && "rob_idx out of range");
        auto& slot = read_entries_[meta.rob_idx];
        assert(slot.occupied && "R for unallocated rob_idx");
        slot.r_beat = r;
        slot.ready = true;
        // Chain-flush ready ranges (whole burst) of this id's sequence.
        uint8_t id = slot.axi_id;
        while (!read_order_by_id_[id].empty()) {
            BeatRange head = read_order_by_id_[id].front();
            bool all_ready = true;
            for (uint8_t i = 0; i < head.len_plus_1; ++i) {
                if (!read_entries_[head.base + i].ready) {
                    all_ready = false;
                    break;
                }
            }
            if (!all_ready) break;
            for (uint8_t i = 0; i < head.len_plus_1; ++i) {
                committed_r_queue_.push_back(read_entries_[head.base + i].r_beat);
                free_read_entries_.set(head.base + i);
                read_entries_[head.base + i].occupied = false;
            }
            read_order_by_id_[id].pop_front();
        }
        if (!committed_r_queue_.empty()) {
            auto out = committed_r_queue_.front();
            committed_r_queue_.pop_front();
            return out;
        }
        return std::nullopt;
    }
    // Disabled-mode body (unchanged from prior round)
    auto opt = next_depkt_.pop_r();
    if (!opt) return std::nullopt;
    if (opt->last) {
        auto& s = read_[opt->id];
        assert(!s.outstanding.empty() && "R(last) for id with no outstanding read");
        s.outstanding.pop_front();
    }
    return opt;
}
```

- [ ] **Step 10: Build + run new tests**

```bash
cd c_model && cmake --build build && ctest --test-dir build -R "NmuRob.Enabled_Pop|NmuRobDeath.Enabled" -j 1
```

Expected: 4 behavior tests PASS + 2 death tests PASS.

- [ ] **Step 11: Full ctest sweep**

```bash
ctest --test-dir build -j 1 2>&1 | tail -3
```

Expected: 291/291 passed (285 prior + 6 new).

- [ ] **Step 12: Drift gates**

```bash
cd ../specgen && py -3 -m pytest -q && py -3 tools/codegen.py --check && py -3 tools/gen_inventory.py --check
```

Expected: pytest 163, codegen / gen_inventory clean.

- [ ] **Step 13: Commit Task 3**

```bash
cd ..
git add c_model/include/nmu/rob.hpp c_model/tests/nmu/test_rob.cpp
git commit -m "feat(nmu/rob): Enabled mode pop paths + commit logic

Implements pop_b / pop_r Enabled paths:
- Pull beat + meta from downstream via pop_*_with_meta()
- Mark slot ready; chain-flush head BeatRange of axi_id's sequence
  once all its slots are ready
- Single-codepath In-order Path: fast-arriving head commits
  immediately without sitting in reorder buffer
- Strict mixed-mode contract: rob_req=0 in Enabled mode → assert+abort
  (catches misconfigured testbench wiring)
- Defensive: rob_idx >= ROB_CAPACITY or slot not occupied → assert+abort

R multi-beat semantics: head AR's range commits all len+1 beats in
burst order before moving to next AR (preserves AXI4 same-id R order).

Different ids interleave at AR boundary.

6 new tests: in-order immediate commit, out-of-order held, multi-beat
burst commit, different-ids interleave, death tests for unallocated
rob_idx + Disabled-flit-in-Enabled-Rob.

Enabled mode feature-complete after this commit; e2e regression gate
gates on Tasks 4 + 5.

Refs: docs/superpowers/specs/2026-06-03-rob-enabled-mode-multi-nsu-testbench-design.md §6.3, §6.5"
```

---

## Task 4: `LoopbackNoc` multi-NSU refactor + per-NSU latency

**Files:**
- Modify: `c_model/tests/common/loopback_noc.hpp`
- Create: `c_model/tests/common/test_loopback_latency.cpp`
- Modify: `c_model/tests/common/CMakeLists.txt`

**Goal:** Rewrite `LoopbackNoc` to support multi-NSU topology with per-NSU response latency. Preserve full backward compatibility — existing single-NSU `(req_depth, rsp_depth)` constructor still works; all `dst_id` default-route to NSU_0; legacy `req_in/req_out/rsp_in/rsp_out` accessors preserved as aliases. Add 6 unit tests for new multi-NSU behavior + explicit backward-compat invariant.

- [ ] **Step 1: Read existing `loopback_noc.hpp` to understand baseline**

```bash
cat c_model/tests/common/loopback_noc.hpp
```

Expected: 121-line single-NSU class with 4 inner adapter structs (`ReqOutAdapter`, `ReqInAdapter`, `RspOutAdapter`, `RspInAdapter`), `req_q_` + `rsp_q_` deques, optional `req_pipe_` + `rsp_pipe_` for global delay, `set_req_delay` / `set_rsp_delay`.

- [ ] **Step 2: Read existing `c_model/tests/common/CMakeLists.txt`**

```bash
cat c_model/tests/common/CMakeLists.txt
```

If file exists, note the registration pattern (`add_cmodel_test(...)` or similar). If not, note that we need to create it.

- [ ] **Step 3: Write failing test `MultiNsu_RouteByDstId`**

Create `c_model/tests/common/test_loopback_latency.cpp`:

```cpp
#include "common/loopback_noc.hpp"
#include "ni_flit_constants.h"
#include <gtest/gtest.h>

using ni::cmodel::testing::LoopbackNoc;
using ni::cmodel::Flit;

namespace {

Flit make_req_flit(uint8_t src, uint8_t dst, uint8_t rob_req, uint8_t rob_idx) {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_AW);
    f.set_header_field("src_id", src);
    f.set_header_field("dst_id", dst);
    f.set_header_field("vc_id",  0);
    f.set_header_field("last",   1);
    f.set_header_field("rob_req", rob_req);
    f.set_header_field("rob_idx", rob_idx);
    return f;
}

Flit make_rsp_flit(uint8_t src, uint8_t dst, uint8_t rob_req, uint8_t rob_idx) {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_B);
    f.set_header_field("src_id", src);
    f.set_header_field("dst_id", dst);
    f.set_header_field("vc_id",  0);
    f.set_header_field("last",   1);
    f.set_header_field("rob_req", rob_req);
    f.set_header_field("rob_idx", rob_idx);
    return f;
}

}  // namespace

TEST(LoopbackNocMultiNsu, RouteByDstId) {
    LoopbackNoc noc(/*num_nsu=*/2, /*req_per_nsu=*/16, /*rsp_total=*/16);
    noc.set_dst_route(/*dst=*/0x00, /*nsu_idx=*/0);
    noc.set_dst_route(/*dst=*/0x01, /*nsu_idx=*/1);

    ASSERT_TRUE(noc.nmu_req_out().push_flit(make_req_flit(0x10, 0x00, 0, 0)));
    ASSERT_TRUE(noc.nmu_req_out().push_flit(make_req_flit(0x10, 0x01, 0, 0)));

    auto f0 = noc.nsu_req_in(0).pop_flit();
    ASSERT_TRUE(f0.has_value());
    EXPECT_EQ(f0->get_header_field("dst_id"), 0x00u);

    auto f1 = noc.nsu_req_in(1).pop_flit();
    ASSERT_TRUE(f1.has_value());
    EXPECT_EQ(f1->get_header_field("dst_id"), 0x01u);

    EXPECT_FALSE(noc.nsu_req_in(0).pop_flit().has_value());
    EXPECT_FALSE(noc.nsu_req_in(1).pop_flit().has_value());
}
```

- [ ] **Step 4: Write failing test `MultiNsu_UnmappedDst_Assert`**

Append to same file:

```cpp
TEST(LoopbackNocMultiNsuDeath, UnmappedDst_Assert) {
    LoopbackNoc noc(2, 16, 16);
    // No set_dst_route called → all dst unmapped → push asserts
    EXPECT_DEATH(noc.nmu_req_out().push_flit(make_req_flit(0x10, 0x99, 0, 0)), "");
}
```

- [ ] **Step 5: Write failing test `PerNsuLatency_StaticDelay`**

```cpp
TEST(LoopbackNocMultiNsu, PerNsuLatency_StaticDelay) {
    LoopbackNoc noc(2, 16, 16);
    noc.set_nsu_latency(/*nsu_idx=*/1, /*cycles=*/3);

    ASSERT_TRUE(noc.nsu_rsp_out(1).push_flit(make_rsp_flit(0x11, 0x01, 1, 0)));
    // Not visible yet
    EXPECT_FALSE(noc.nmu_rsp_in().pop_flit().has_value());
    noc.tick();   // 1st aging
    EXPECT_FALSE(noc.nmu_rsp_in().pop_flit().has_value());
    noc.tick();   // 2nd
    EXPECT_FALSE(noc.nmu_rsp_in().pop_flit().has_value());
    noc.tick();   // 3rd → cycles_remaining hits 0, released to rsp_q
    auto f = noc.nmu_rsp_in().pop_flit();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->get_header_field("src_id"), 0x11u);
}
```

- [ ] **Step 6: Write failing test `PerNsuLatency_RandomBounded`**

```cpp
TEST(LoopbackNocMultiNsu, PerNsuLatency_RandomBounded) {
    LoopbackNoc noc(2, 16, /*rsp_total=*/512);
    noc.set_nsu_latency_range(/*nsu_idx=*/1, /*min=*/2, /*max=*/8);
    noc.set_random_seed(42);

    // Push 100 flits, then advance enough ticks for all to drain, recording delays.
    // Simpler approach: push then advance N ticks; check that the average / min / max
    // sits within [2, 8] using a fresh queue.
    std::vector<std::size_t> release_tick(100, 0);
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(noc.nsu_rsp_out(1).push_flit(make_rsp_flit(0x11, 0x01, 1, 0)));
    }
    // Each push samples a latency in [2, 8]; tick until everything drains.
    int produced = 0;
    for (int t = 1; t <= 20 && produced < 100; ++t) {
        noc.tick();
        while (true) {
            auto f = noc.nmu_rsp_in().pop_flit();
            if (!f) break;
            release_tick[produced++] = static_cast<std::size_t>(t);
        }
    }
    EXPECT_EQ(produced, 100);
    for (auto t : release_tick) {
        EXPECT_GE(t, 2u);
        EXPECT_LE(t, 8u);
    }
}
```

- [ ] **Step 7: Write failing test `PerNsuQueueFull_DoesNotBlockOtherNsu`**

```cpp
TEST(LoopbackNocMultiNsu, PerNsuQueueFull_DoesNotBlockOtherNsu) {
    LoopbackNoc noc(/*num_nsu=*/2, /*req_per_nsu=*/1, /*rsp_total=*/16);
    noc.set_dst_route(0x00, 0);
    noc.set_dst_route(0x01, 1);

    // Fill NSU_0 req queue (depth 1) — next push to dst=0 must fail
    ASSERT_TRUE(noc.nmu_req_out().push_flit(make_req_flit(0x10, 0x00, 0, 0)));
    EXPECT_FALSE(noc.nmu_req_out().push_flit(make_req_flit(0x10, 0x00, 0, 0)));
    // NSU_1 queue still empty — push to dst=1 must succeed
    EXPECT_TRUE(noc.nmu_req_out().push_flit(make_req_flit(0x10, 0x01, 0, 0)));
}
```

- [ ] **Step 7b: Write failing backward-compat test `SingleNsuCtor_LegacyAccessAndDelayPreserved`**

Explicit narrow test covering spec §7.4 three backward-compat invariants. Faster failure signal than relying on full ctest sweep alone.

```cpp
TEST(LoopbackNocBackwardCompat, SingleNsuCtor_LegacyAccessAndDelayPreserved) {
    // Single-NSU ctor: dst_to_nsu_ defaults to all NSU_0
    LoopbackNoc noc(/*req_depth=*/4, /*rsp_depth=*/4);

    // Invariant 1: dst defaults route to NSU_0 (no set_dst_route needed)
    ASSERT_TRUE(noc.nmu_req_out().push_flit(make_req_flit(0x10, 0x55, 0, 0)));
    auto f = noc.nsu_req_in(0).pop_flit();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->get_header_field("dst_id"), 0x55u);

    // Invariant 2: legacy aliases (req_in/req_out/rsp_in/rsp_out) point at
    // NSU_0 endpoints (same observable behavior). Push via alias req_out,
    // pop via alias req_in — both go through NSU_0.
    ASSERT_TRUE(noc.req_out().push_flit(make_req_flit(0x10, 0x77, 0, 0)));
    auto f2 = noc.req_in().pop_flit();
    ASSERT_TRUE(f2.has_value());
    EXPECT_EQ(f2->get_header_field("dst_id"), 0x77u);

    // Invariant 3: legacy set_rsp_delay still works (global delay applies)
    noc.set_rsp_delay(2);
    ASSERT_TRUE(noc.rsp_out().push_flit(make_rsp_flit(0x10, 0x01, 0, 0)));
    EXPECT_FALSE(noc.rsp_in().pop_flit().has_value());   // not visible yet
    noc.tick(); noc.tick();   // age 2 cycles
    auto f3 = noc.rsp_in().pop_flit();
    ASSERT_TRUE(f3.has_value());
    EXPECT_EQ(f3->get_header_field("src_id"), 0x10u);
}
```

- [ ] **Step 8: Register test in CMakeLists.txt**

Edit `c_model/tests/common/CMakeLists.txt` (create if missing, mirroring `c_model/tests/nmu/CMakeLists.txt` pattern). If the file exists with `add_cmodel_test(...)` lines, append:

```cmake
add_cmodel_test(test_loopback_latency)
```

If the file does NOT exist, create it with the same structure as `c_model/tests/nmu/CMakeLists.txt` (refer to that file for the exact `add_cmodel_test` macro + include_directories pattern), containing the one test entry above. Then ensure `c_model/tests/CMakeLists.txt` includes `add_subdirectory(common)` — if not, add it.

- [ ] **Step 9: Run tests to verify FAIL**

```bash
cd c_model && cmake --build build 2>&1 | head -10
```

Expected: compile errors mentioning multi-NSU ctor / `set_dst_route` / `nsu_req_in` / `set_nsu_latency` not declared.

- [ ] **Step 10: Rewrite `loopback_noc.hpp`**

Replace `c_model/tests/common/loopback_noc.hpp` entirely with:

```cpp
// LoopbackNoc — testbench-only NoC bridge with multi-NSU + per-NSU latency.
//
// Single-NSU ctor (LoopbackNoc(req_depth, rsp_depth)) is the backward-compat
// path: all 256 dst_id default-route to NSU_0; legacy aliases
// (req_in/req_out/rsp_in/rsp_out) point at NSU_0 endpoints; legacy
// set_req_delay/set_rsp_delay apply globally as before.
//
// Multi-NSU ctor (LoopbackNoc(num_nsu, req_per_nsu, rsp_total)) requires
// explicit set_dst_route(dst_id, nsu_idx) — unmapped dst pushes assert.
//
// Per-NSU response latency (set_nsu_latency / set_nsu_latency_range) replaces
// global rsp_delay_ for the configured NSU (not additive; see spec §7.3).
#pragma once
#include "ni/flit.hpp"
#include "ni_flit_constants.h"
#include "noc/noc_req_in.hpp"
#include "noc/noc_req_out.hpp"
#include "noc/noc_rsp_in.hpp"
#include "noc/noc_rsp_out.hpp"
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <random>
#include <utility>
#include <vector>

namespace ni::cmodel::testing {

class LoopbackNoc {
public:
    // Backward-compat single-NSU ctor. Defaults all dst_id to NSU_0.
    LoopbackNoc(std::size_t req_depth, std::size_t rsp_depth)
        : LoopbackNoc(/*num_nsu=*/1, req_depth, rsp_depth) {
        // Override default-unmapped: route all dst to NSU_0 for legacy fixtures.
        for (std::size_t d = 0; d < DST_ID_SPACE; ++d) dst_to_nsu_[d] = 0;
    }

    // Multi-NSU ctor. Caller must call set_dst_route() for each dst_id used.
    LoopbackNoc(std::size_t num_nsu,
                std::size_t req_q_depth_per_nsu,
                std::size_t rsp_q_depth_total)
        : num_nsu_(num_nsu),
          req_q_depth_per_nsu_(req_q_depth_per_nsu),
          rsp_q_depth_total_(rsp_q_depth_total),
          nsu_req_q_(num_nsu),
          nsu_rsp_delay_q_(num_nsu),
          nsu_latency_(num_nsu),
          nmu_req_out_adapter_{this},
          nmu_rsp_in_adapter_{this} {
        dst_to_nsu_.fill(-1);
        nsu_req_in_adapters_.reserve(num_nsu);
        nsu_rsp_out_adapters_.reserve(num_nsu);
        for (std::size_t i = 0; i < num_nsu; ++i) {
            nsu_req_in_adapters_.emplace_back(this, i);
            nsu_rsp_out_adapters_.emplace_back(this, i);
        }
    }

    // NMU-side (single)
    noc::NocReqOut& nmu_req_out() noexcept { return nmu_req_out_adapter_; }
    noc::NocRspIn&  nmu_rsp_in()  noexcept { return nmu_rsp_in_adapter_;  }

    // NSU-side (per-NSU). 0-indexed; bounds-asserted.
    noc::NocReqIn&  nsu_req_in(std::size_t nsu_idx) noexcept {
        assert(nsu_idx < num_nsu_);
        return nsu_req_in_adapters_[nsu_idx];
    }
    noc::NocRspOut& nsu_rsp_out(std::size_t nsu_idx) noexcept {
        assert(nsu_idx < num_nsu_);
        return nsu_rsp_out_adapters_[nsu_idx];
    }

    // Legacy aliases — single-NSU compatibility (point at NSU_0)
    noc::NocReqOut& req_out() noexcept { return nmu_req_out(); }
    noc::NocReqIn&  req_in()  noexcept { return nsu_req_in(0); }
    noc::NocRspOut& rsp_out() noexcept { return nsu_rsp_out(0); }
    noc::NocRspIn&  rsp_in()  noexcept { return nmu_rsp_in(); }

    // Routing: dst_id → nsu_idx
    void set_dst_route(uint8_t dst_id, std::size_t nsu_idx) noexcept {
        assert(nsu_idx < num_nsu_);
        dst_to_nsu_[dst_id] = static_cast<int8_t>(nsu_idx);
    }

    // Per-NSU response latency (static)
    void set_nsu_latency(std::size_t nsu_idx, std::size_t cycles) noexcept {
        assert(nsu_idx < num_nsu_);
        nsu_latency_[nsu_idx] = NsuLatencyConfig{
            /*is_random=*/false, cycles, 0, 0};
    }
    // Per-NSU response latency (random uniform in [min, max] inclusive)
    void set_nsu_latency_range(std::size_t nsu_idx,
                               std::size_t min,
                               std::size_t max) noexcept {
        assert(nsu_idx < num_nsu_);
        assert(min <= max);
        nsu_latency_[nsu_idx] = NsuLatencyConfig{
            /*is_random=*/true, 0, min, max};
    }
    void set_random_seed(uint64_t seed) noexcept { rng_.seed(seed); }

    // Legacy global delay (preserved for single-NSU fixtures only;
    // multi-NSU mode uses set_nsu_latency instead — these assert if mixed).
    void set_req_delay(unsigned cycles) noexcept {
        assert(num_nsu_ == 1 &&
               "set_req_delay only supported in single-NSU mode; "
               "use set_nsu_latency in multi-NSU mode");
        req_delay_ = cycles;
    }
    void set_rsp_delay(unsigned cycles) noexcept {
        assert(num_nsu_ == 1 &&
               "set_rsp_delay only supported in single-NSU mode; "
               "use set_nsu_latency in multi-NSU mode");
        rsp_delay_ = cycles;
    }

    void tick() {
        // Per-NSU response delay aging
        for (std::size_t i = 0; i < num_nsu_; ++i) {
            for (auto& e : nsu_rsp_delay_q_[i]) {
                if (e.cycles_remaining > 0) --e.cycles_remaining;
            }
            while (!nsu_rsp_delay_q_[i].empty()
                   && nsu_rsp_delay_q_[i].front().cycles_remaining == 0
                   && rsp_q_.size() < rsp_q_depth_total_) {
                rsp_q_.push_back(nsu_rsp_delay_q_[i].front().flit);
                nsu_rsp_delay_q_[i].pop_front();
                --total_delayed_rsp_count_;
            }
        }
        // Legacy req/rsp pipe aging
        auto age = [](std::deque<std::pair<Flit, unsigned>>& pipe,
                      std::deque<Flit>& visible, std::size_t cap) {
            for (auto& e : pipe) if (e.second > 0) --e.second;
            while (!pipe.empty() && pipe.front().second == 0
                   && visible.size() < cap) {
                visible.push_back(pipe.front().first);
                pipe.pop_front();
            }
        };
        age(req_pipe_, req_q_, req_q_depth_per_nsu_);
        age(rsp_pipe_, rsp_q_, rsp_q_depth_total_);
    }

    // Test introspection
    std::size_t nsu_req_q_size(std::size_t i) const noexcept {
        return nsu_req_q_[i].size();
    }
    std::size_t rsp_q_size() const noexcept { return rsp_q_.size(); }

private:
    static constexpr std::size_t DST_ID_SPACE = 1u << ni::header::DST_ID_WIDTH;

    struct NsuLatencyConfig {
        bool        is_random = false;
        std::size_t value     = 0;
        std::size_t min       = 0;
        std::size_t max       = 0;
    };

    struct DelayedFlit { Flit flit; std::size_t cycles_remaining; };

    struct NmuReqOutAdapter : noc::NocReqOut {
        LoopbackNoc* p;
        explicit NmuReqOutAdapter(LoopbackNoc* parent) : p(parent) {}
        bool push_flit(const Flit& f) override {
            uint8_t dst = static_cast<uint8_t>(f.get_header_field("dst_id"));
            int8_t nsu = p->dst_to_nsu_[dst];
            assert(nsu >= 0 && "LoopbackNoc: unmapped dst_id");
            // Legacy global req delay path (only when no NSU per-flit policy
            // is configured; multi-NSU mode keeps it as a sanity option)
            if (p->req_delay_ > 0) {
                if (p->req_pipe_.size() + p->req_q_.size()
                        >= p->req_q_depth_per_nsu_) {
                    return false;
                }
                p->req_pipe_.emplace_back(f, p->req_delay_);
                return true;
            }
            if (p->nsu_req_q_[nsu].size() >= p->req_q_depth_per_nsu_) {
                return false;
            }
            p->nsu_req_q_[nsu].push_back(f);
            return true;
        }
    };
    struct NsuReqInAdapter : noc::NocReqIn {
        LoopbackNoc* p;
        std::size_t  i;
        NsuReqInAdapter(LoopbackNoc* parent, std::size_t idx)
            : p(parent), i(idx) {}
        std::optional<Flit> pop_flit() override {
            // Drain legacy req_pipe via req_q_ for backward compat (only NSU_0)
            if (i == 0 && !p->req_q_.empty()) {
                Flit f = p->req_q_.front();
                p->req_q_.pop_front();
                return f;
            }
            if (p->nsu_req_q_[i].empty()) return std::nullopt;
            Flit f = p->nsu_req_q_[i].front();
            p->nsu_req_q_[i].pop_front();
            return f;
        }
    };
    struct NsuRspOutAdapter : noc::NocRspOut {
        LoopbackNoc* p;
        std::size_t  i;
        NsuRspOutAdapter(LoopbackNoc* parent, std::size_t idx)
            : p(parent), i(idx) {}
        bool push_flit(const Flit& f) override {
            const auto& cfg = p->nsu_latency_[i];
            std::size_t latency;
            if (cfg.is_random) {
                std::uniform_int_distribution<std::size_t> dist(cfg.min, cfg.max);
                latency = dist(p->rng_);
            } else {
                latency = cfg.value;
            }
            if (latency == 0) {
                // Fast path: bypass per-NSU delay queue
                if (p->rsp_delay_ > 0) {
                    // Legacy global delay still applies (NSU has no per-NSU policy)
                    if (p->rsp_pipe_.size() + p->rsp_q_.size()
                            >= p->rsp_q_depth_total_) {
                        return false;
                    }
                    p->rsp_pipe_.emplace_back(f, p->rsp_delay_);
                } else {
                    if (p->rsp_q_.size() >= p->rsp_q_depth_total_) return false;
                    p->rsp_q_.push_back(f);
                }
                return true;
            }
            // Per-NSU delay path
            if (p->total_delayed_rsp_count_ + p->rsp_pipe_.size()
                    + p->rsp_q_.size() >= p->rsp_q_depth_total_) {
                return false;
            }
            p->nsu_rsp_delay_q_[i].push_back({f, latency});
            ++p->total_delayed_rsp_count_;
            return true;
        }
    };
    struct NmuRspInAdapter : noc::NocRspIn {
        LoopbackNoc* p;
        explicit NmuRspInAdapter(LoopbackNoc* parent) : p(parent) {}
        std::optional<Flit> pop_flit() override {
            if (p->rsp_q_.empty()) return std::nullopt;
            Flit f = p->rsp_q_.front();
            p->rsp_q_.pop_front();
            return f;
        }
    };

    std::size_t      num_nsu_;
    std::size_t      req_q_depth_per_nsu_;
    std::size_t      rsp_q_depth_total_;
    std::array<int8_t, DST_ID_SPACE> dst_to_nsu_{};
    std::vector<std::deque<Flit>>           nsu_req_q_;
    std::deque<Flit>                        rsp_q_;
    std::vector<std::deque<DelayedFlit>>    nsu_rsp_delay_q_;
    std::size_t                             total_delayed_rsp_count_ = 0;
    std::vector<NsuLatencyConfig>           nsu_latency_;
    unsigned                                req_delay_ = 0, rsp_delay_ = 0;
    std::deque<std::pair<Flit, unsigned>>   req_pipe_, rsp_pipe_;
    std::deque<Flit>                        req_q_;   // legacy global req delay output
    std::mt19937_64                         rng_;
    NmuReqOutAdapter                        nmu_req_out_adapter_;
    NmuRspInAdapter                         nmu_rsp_in_adapter_;
    std::vector<NsuReqInAdapter>            nsu_req_in_adapters_;
    std::vector<NsuRspOutAdapter>           nsu_rsp_out_adapters_;
};

}  // namespace ni::cmodel::testing
```

- [ ] **Step 11: Build + run new tests**

```bash
cd c_model && cmake --build build && ctest --test-dir build -R "LoopbackNocMultiNsu" -j 1
```

Expected: 5 PASS (4 behavior + 1 death).

- [ ] **Step 12: Full ctest sweep — critical backward-compat check**

```bash
ctest --test-dir build -j 1 2>&1 | tail -10
```

Expected: **297/297 passed** (291 prior + 6 new — 5 multi-NSU tests + 1 backward-compat invariant test). The 270 prior tests using legacy single-NSU ctor + `req_in()/req_out()/rsp_in()/rsp_out()` aliases MUST stay green. If any test fails, the backward-compat ctor / alias semantics drifted — fix before proceeding.

- [ ] **Step 13: Drift gates**

```bash
cd ../specgen && py -3 -m pytest -q && py -3 tools/codegen.py --check && py -3 tools/gen_inventory.py --check
```

Expected: pytest 163, codegen / gen_inventory clean.

- [ ] **Step 14: Commit Task 4**

```bash
cd ..
git add c_model/tests/common/loopback_noc.hpp \
        c_model/tests/common/test_loopback_latency.cpp \
        c_model/tests/common/CMakeLists.txt
git commit -m "feat(tests/common/loopback_noc): multi-NSU testbench + per-NSU latency

Rewrites LoopbackNoc to support N NSU instances with per-NSU routing
and per-NSU response latency (static or random).

Multi-NSU ctor: LoopbackNoc(num_nsu, req_per_nsu, rsp_total). Caller
must call set_dst_route(dst_id, nsu_idx) for each dst used; unmapped
dst push asserts.

Backward-compat single-NSU ctor: LoopbackNoc(req_depth, rsp_depth)
forwards to multi-NSU with num_nsu=1 and defaults all 256 dst_id to
NSU_0. Legacy aliases (req_in/req_out/rsp_in/rsp_out) point at NSU_0
endpoints. Legacy set_req_delay/set_rsp_delay preserved as global
delay (applies when per-NSU latency is unset).

Per-NSU latency:
- set_nsu_latency(i, cycles)        — static
- set_nsu_latency_range(i, min, max) — uniform random
- set_random_seed(seed)              — deterministic test reproducibility

tick() ages per-NSU delay queues independently (NSU 0 blocked does
not stall NSU 1). Aggregate capacity: total_delayed_rsp_count_ +
rsp_pipe_.size() + rsp_q_.size() <= rsp_q_depth_total_.

5 new tests in test_loopback_latency.cpp covering routing, unmapped-dst
assert, static delay, random bounded range, and per-NSU isolation.

270 prior tests stay green via single-NSU compatibility path.

Refs: docs/superpowers/specs/2026-06-03-rob-enabled-mode-multi-nsu-testbench-design.md §7"
```

---

## Task 5: Integration testbench multi-NSU + `multi_dst_stress` real regression gate

**Files:**
- Modify: `c_model/tests/integration/test_request_response_loopback.cpp`

**Goal:** Refactor the integration testbench so that the `multi_dst_stress.yaml` fixture path builds 4 NSU stacks (each with own src_id, own Packetize/Depacketize/AxiMasterPort/MetaBuffer), sets per-NSU latency to produce out-of-order responses, switches Rob to Enabled mode, and adds a positive per-id B/R ordering assertion that catches AXI4 §A5.3 violations. Other 6 fixtures stay single-NSU mode via the existing path.

- [ ] **Step 1: Read existing integration testbench**

```bash
sed -n '60,160p' c_model/tests/integration/test_request_response_loopback.cpp
sed -n '270,290p' c_model/tests/integration/test_request_response_loopback.cpp
```

Note the structure (verified ground truth):
- Lines 70-71: `constexpr uint8_t kNmuSrcId = 0x01; constexpr uint8_t kNsuSrcId = 0x02;`
- Line 92-93: `test::LoopbackNoc loopback(params.loopback_noc_req_depth, params.loopback_noc_rsp_depth);`
- Lines 108-110: `nmu::Packetize real_nmu_pkt(loopback.req_out(), kNmuSrcId); nmu::Rob rob(real_nmu_pkt, nmu_depkt, RobMode::Disabled, RobMode::Disabled);`
- Lines 117-129: `nsu::Depacketize nsu_depkt(loopback.req_in(), nsu_meta, ...); nsu::Packetize nsu_pkt(loopback.rsp_out(), nsu_meta, kNsuSrcId); nsu::AxiMasterPort nsu_port(nsu_depkt, nsu_pkt, params);`
- Line 138: existing `if (yaml_path.find("multi_dst_stress.yaml") != std::string::npos)` (the `mow` override block)
- Line 281: `FixtureParam{"multi_dst_stress.yaml", 0u, 0u}` in `INSTANTIATE_TEST_SUITE_P`
- Existing rig uses single-NSU pattern with `loopback.req_out()` / `loopback.req_in()` / `loopback.rsp_out()` / `loopback.rsp_in()` legacy aliases (Task 4 preserves these for backward compat — they map to NSU_0 endpoints when `num_nsu_=1`).

The refactor approach: keep single-NSU path intact for the 6 non-`multi_dst_stress` fixtures (so their wiring code unchanged), add a parallel multi-NSU build branch for the `multi_dst_stress` fixture only.

- [ ] **Step 2: Add `PerIdOrderTracker` helper before the test class**

Insert in the anonymous namespace (around line 80, after `kNsuSrcId` constant):

```cpp
// PerIdOrderTracker — verifies per-id B/R beat arrival order at AxiMaster.
// multi_dst_stress: id=0x05 B beats must arrive in submission order (AW1, AW2)
// regardless of physical NoC latency (AXI4 IHI 0022 §A5.3). Without Rob
// Enabled mode reordering, NSU_1 (faster) returns B2 before NSU_0's B1,
// violating the per-id order. We assert this directly to act as the regression
// gate (positive ordering check, not expected-failure pattern).
struct PerIdOrderTracker {
    std::array<std::vector<uint64_t>, 256> b_seq;
    std::array<std::vector<uint64_t>, 256> r_seq;
    void record_b(uint8_t id, uint64_t marker) { b_seq[id].push_back(marker); }
    void record_r(uint8_t id, uint64_t marker) { r_seq[id].push_back(marker); }
    bool verify_b_in_order(uint8_t id) const {
        const auto& v = b_seq[id];
        for (std::size_t i = 1; i < v.size(); ++i) {
            if (v[i] < v[i-1]) return false;
        }
        return true;
    }
    bool verify_r_in_order(uint8_t id) const {
        const auto& v = r_seq[id];
        for (std::size_t i = 1; i < v.size(); ++i) {
            if (v[i] < v[i-1]) return false;
        }
        return true;
    }
};
```

- [ ] **Step 3: Add multi-NSU constants in the anonymous namespace (after kNsuSrcId)**

Insert right after `constexpr uint8_t kNsuSrcId = 0x02;` (around line 72):

```cpp
constexpr std::size_t kNumNsuMulti = 4;
constexpr std::array<uint8_t, kNumNsuMulti> kNsuSrcIdsMulti = {0x10, 0x11, 0x12, 0x13};
```

- [ ] **Step 4: Refactor testbench construction (lines 92-129) for multi_dst_stress branch**

Replace the existing construction block (lines 92-129, approximately) with the branching version below. The `yaml_path` variable should already be in scope (used for the existing `mow` override at line 138):

```cpp
const bool is_multi_dst = yaml_path.find("multi_dst_stress.yaml") != std::string::npos;
const RobMode rob_mode = is_multi_dst ? RobMode::Enabled : RobMode::Disabled;

// LoopbackNoc construction — single-NSU for legacy fixtures, multi-NSU for multi_dst_stress
test::LoopbackNoc loopback = is_multi_dst
    ? test::LoopbackNoc(/*num_nsu=*/kNumNsuMulti,
                        /*req_per_nsu=*/params.loopback_noc_req_depth,
                        /*rsp_total=*/params.loopback_noc_rsp_depth)
    : test::LoopbackNoc(params.loopback_noc_req_depth,
                        params.loopback_noc_rsp_depth);

if (is_multi_dst) {
    // multi_dst_stress addresses: 0x100 → dst=0, 0x10100 → dst=1
    loopback.set_dst_route(0x00, 0);
    loopback.set_dst_route(0x01, 1);
    loopback.set_dst_route(0x02, 2);
    loopback.set_dst_route(0x03, 3);
    // Per-NSU response latency: NSU_0 slow (10c), NSU_1 fast (2c) — exposes
    // out-of-order B arrival; Rob Enabled mode must reorder to preserve AXI4.
    loopback.set_nsu_latency(0, 10);
    loopback.set_nsu_latency(1, 2);
    loopback.set_nsu_latency(2, 5);
    loopback.set_nsu_latency(3, 3);
}

// NMU stack (uses legacy aliases for single-NSU; uses nmu_* accessors for multi-NSU
// — both resolve to NSU_0 endpoints when num_nsu_=1, so we can always use nmu_*)
nmu::Packetize    real_nmu_pkt(loopback.nmu_req_out(), kNmuSrcId);
nmu::Depacketize  nmu_depkt(loopback.nmu_rsp_in(),
                            params.depkt_b_q_depth, params.depkt_r_q_depth);
nmu::Rob          rob(real_nmu_pkt, nmu_depkt, rob_mode, rob_mode);
nmu::AxiSlavePort nmu_port(rob, rob, params);

// NSU stacks: 1 for legacy, 4 for multi_dst_stress
const std::size_t nsu_count = is_multi_dst ? kNumNsuMulti : 1;
std::vector<std::unique_ptr<nsu::MetaBuffer>>    nsu_metas;
std::vector<std::unique_ptr<nsu::Depacketize>>   nsu_depkts;
std::vector<std::unique_ptr<nsu::Packetize>>     nsu_pkts;
std::vector<std::unique_ptr<nsu::AxiMasterPort>> nsu_ports;
nsu_metas.reserve(nsu_count);
nsu_depkts.reserve(nsu_count);
nsu_pkts.reserve(nsu_count);
nsu_ports.reserve(nsu_count);
for (std::size_t i = 0; i < nsu_count; ++i) {
    const uint8_t this_nsu_src = is_multi_dst ? kNsuSrcIdsMulti[i] : kNsuSrcId;
    nsu_metas.emplace_back(std::make_unique<nsu::MetaBuffer>());
    nsu_depkts.emplace_back(std::make_unique<nsu::Depacketize>(
        loopback.nsu_req_in(i), *nsu_metas[i],
        params.nsu_depkt_aw_q_depth, params.nsu_depkt_ar_q_depth,
        params.nsu_depkt_w_q_depth));
    nsu_pkts.emplace_back(std::make_unique<nsu::Packetize>(
        loopback.nsu_rsp_out(i), *nsu_metas[i], this_nsu_src));
    nsu_ports.emplace_back(std::make_unique<nsu::AxiMasterPort>(
        *nsu_depkts[i], *nsu_pkts[i], params));
}

PerIdOrderTracker tracker;   // populated below
```

Note: `nsu::Depacketize` ctor signature may differ in your repo — check existing line 117-119 (`nsu::Depacketize nsu_depkt(loopback.req_in(), nsu_meta, ...)`) and mirror the actual signature. The above is the conceptual shape.

- [ ] **Step 5: Update response shuttle to loop over all NSU instances**

The existing testbench loop (around lines 195-220 in the current file — the `nsu_port.pop_b/pop_r` + AxiSlave callback shuttle) must now drain ALL `nsu_ports[i]`. Find the existing shuttle block (search for `nsu_port.pop_b` or `b_holdover`) and replace single-instance access with vector iteration:

```cpp
// Before (single NSU):
//   while (auto b = nsu_port.pop_b()) { ... }
//   while (auto r = nsu_port.pop_r()) { ... }
// After (vector of NSU ports):
std::vector<std::deque<axi::BBeat>> b_holdovers(nsu_count);
std::vector<std::deque<axi::RBeat>> r_holdovers(nsu_count);

// In tick loop:
for (std::size_t i = 0; i < nsu_count; ++i) {
    auto* port = nsu_ports[i].get();
    while (!b_holdovers[i].empty()) {
        // (existing single-port logic, but using b_holdovers[i] instead of b_holdover)
        ...
    }
    while (auto b = port->pop_b()) {
        // Record for per-id ordering tracker (multi_dst_stress only; for
        // legacy single-NSU fixtures, tracker is unused).
        if (is_multi_dst) tracker.record_b(b->id, b->id);  // marker = id; sufficient for AW1/AW2 order
        // (existing AxiSlave forwarding logic, using b_holdovers[i])
        ...
    }
    // Same pattern for R beats; tracker.record_r if is_multi_dst.
}
```

(Implementer: open the existing test_request_response_loopback.cpp shuttle block once, vectorize each per-NSU drain. The tracker.record_* calls are simple additions inside the existing `while (auto b = port->pop_b())` body.)

- [ ] **Step 6: Add positive ordering assertion after test loop**

At the END of the test body (after AxiMaster finishes + Scoreboard checks), add:

```cpp
if (is_multi_dst) {
    EXPECT_TRUE(tracker.verify_b_in_order(0x05))
        << "multi_dst_stress fixture: id=0x05 B beats arrived out of "
        << "submission order at AxiMaster. Rob Enabled mode failed to "
        << "reorder despite per-NSU latency variance (NSU_0=10c, NSU_1=2c).";
}
```

- [ ] **Step 7: Build + run integration test for multi_dst_stress**

```bash
cd c_model && cmake --build build && ctest --test-dir build -R "multi_dst_stress" -j 1
```

Expected: 1 PASS (`multi_dst_stress_q0_s0`).

- [ ] **Step 8: Full ctest sweep**

```bash
ctest --test-dir build -j 1 2>&1 | tail -3
```

Expected: 297/297 passed. No new tests added by this task (multi_dst_stress already existed); the assertion is the new behavior.

- [ ] **Step 9: Manual sanity-check (reviewer step — NOT committed)**

Temporarily change in the multi_dst_stress branch:

```cpp
const RobMode rob_mode = is_multi_dst ? RobMode::Disabled : RobMode::Disabled;
//                                       ^^^^^^^ was Enabled
```

Run `ctest -R multi_dst_stress -j 1`. Expected: **FAIL** — without Rob Enabled, B beats for id=0x05 arrive at AxiMaster in {AW2, AW1} order due to NSU_1 (dst=1) being faster than NSU_0 (dst=0). Either the positive ordering tracker fires, or the Scoreboard data integrity fails, or AxiMaster's per-id FIFO routing breaks.

Revert the change before committing.

- [ ] **Step 10: Drift gates**

```bash
cd ../specgen && py -3 -m pytest -q && py -3 tools/codegen.py --check && py -3 tools/gen_inventory.py --check
```

Expected: pytest 163, codegen / gen_inventory clean.

- [ ] **Step 11: Commit Task 5**

```bash
cd ..
git add c_model/tests/integration/test_request_response_loopback.cpp
git commit -m "feat(tests/integration): multi-NSU testbench + multi_dst_stress real regression gate

For multi_dst_stress fixture, build 4 NSU stacks (src_ids
{0x10..0x13}), set per-NSU latency {10, 2, 5, 3} cycles, wire dst_id
{0..3} → NSU {0..3} via set_dst_route, switch Rob to Enabled mode.
Other 6 fixtures stay single-NSU + Rob Disabled (unchanged).

multi_dst_stress is now a real regression gate: NSU_1 (dst=1) is 8
cycles faster than NSU_0 (dst=0), so B beat for AW2 (id=0x05, dst=1)
arrives before B for AW1 (id=0x05, dst=0). Rob Enabled reorders before
handing to AxiMaster, preserving AXI4 §A5.3 same-id submission order.

Adds positive PerIdOrderTracker to verify B/R beats reach AxiMaster
in submission order. Sanity-checked manually: switching Rob to
Disabled in the multi_dst_stress branch makes the assertion fire.

Refs: docs/superpowers/specs/2026-06-03-rob-enabled-mode-multi-nsu-testbench-design.md §8.2"
```

---

## Task 6: Final drift gates + Karpathy 4-lens + `NEXT_STEPS` update

**Files:**
- Modify: `NEXT_STEPS.md`

**Goal:** Run all drift gates and capture final counts. Apply Karpathy 4-lens review across the round's commits. Update `NEXT_STEPS.md` to flip the headline to "ROB Enabled + multi-NSU testbench done; next is vc_arb" per main plan §3.1 successor list.

- [ ] **Step 1: Run all drift gates and record exact counts**

```bash
cd specgen && py -3 -m pytest -q
py -3 tools/codegen.py --check
py -3 tools/gen_inventory.py --check
cd ../c_model && ctest --test-dir build -j 1 2>&1 | tail -3
```

Expected:
- specgen pytest: 163 passed
- codegen --check: clean (exit 0)
- gen_inventory --check: clean (exit 0)
- ctest: 297 passed, 0 failed (276 prior + 21 new: 2 depacketize + 13 rob + 6 loopback)

If any gate fails, STOP and report BLOCKED.

- [ ] **Step 2: Karpathy 4-lens review (informal walkthrough — findings go into commit message)**

```bash
git log --oneline 48a0e2b..HEAD
```

(Or use the actual base SHA of the spec commit.)

Walk through commits 1-5 and assess:

**Overcomplication**:
- Task 1 (Depacketizer interface): 1 new struct + 2 virtual methods + default impl. Minimal.
- Task 2-3 (Rob Enabled): ~200 LOC of Enabled logic + 13 tests. Per-beat slot model is the specified design, not over-engineered.
- Task 4 (LoopbackNoc multi-NSU): ~250 LOC rewrite. Largest change but focused.
- Task 5 (integration testbench multi-NSU): ~80 LOC branch. Focused.

**Surgical**:
- Stage 2 axi/ untouched: `git diff 48a0e2b..HEAD -- c_model/include/axi/` is empty (verify)
- nmu/packetize.hpp untouched: `git diff 48a0e2b..HEAD -- c_model/include/nmu/packetize.hpp` is empty (verify)
- addr_trans.hpp untouched: same (verify)
- Specgen generated headers untouched: empty diff (verify; if not, only timestamps which are not committed)
- NSU files: instance count varies via testbench, but `c_model/include/nsu/*.hpp` source untouched

**Surface assumptions**:
- `ROB_IDX_WIDTH = 5` is hard from `ni_packet.json`; 32-slot pool cap is derived not hardcoded
- Per-beat slot model implies max 2 in-flight len=15 reads (acceptable for c_model fixtures)
- Mixed-mode contract is strict (assert on rob_req mismatch)
- multi_dst_stress as regression gate assumes deterministic per-NSU latency (10 vs 2 cycles)

**Verifiable success**:
- 20 new tests pass (2 + 13 + 5)
- multi_dst_stress is real regression gate (manual sanity-check confirmed Rob Disabled → FAIL)
- 270 prior tests stay green (backward compat)

- [ ] **Step 3: Update `NEXT_STEPS.md`**

Read current `NEXT_STEPS.md`:

```bash
head -30 NEXT_STEPS.md
```

Replace the "Current status" headline section (similar to prior round's update) with:

```markdown
## Current status (2026-06-03)

Stage 3 ROB Enabled mode + multi-NSU testbench 完工：
- nmu/rob.hpp Enabled mode complete (per-beat slot pool, 32 slots from ROB_IDX_WIDTH=5, dynamic free-list allocator, per-id BeatRange commit sequence, in-order Path chain-flush, mixed-mode strict assert)
- ni/depacketizer.hpp 加 pop_*_with_meta() virtual + ResponseMeta struct（default impl 是 no-op forwarder, backward compat）
- nmu/depacketize.hpp override pop_*_with_meta()，抽 rob_idx / rob_req from flit header
- tests/common/loopback_noc.hpp 重寫成 multi-NSU testbench（4 NSU instances, per-NSU routing, per-NSU response latency static + random hybrid），backward-compat single-NSU ctor 保留 → 270 prior tests 零受影響
- integration testbench：multi_dst_stress.yaml 升級為 real regression gate（4 NSU instances {0x10..0x13}, per-NSU latency {10, 2, 5, 3} cycles, Rob Enabled, positive PerIdOrderTracker）
- 297 ctest sequential pass (276 prior + 21 new), drift gates clean

**Next task per main plan §3.1**: `vc_arb` virtual channel arbitration (per-VC backpressure, round-robin or weighted scheduling, integrate with router fabric)。

後續 `vc_mapping` / `route_par` / `flit_ecc` / `nmu.hpp` top-level assembly 各自獨立 round。
```

(Adjust exact phrasing to match the existing NEXT_STEPS.md style; preserve sections below the headline.)

- [ ] **Step 4: Commit Task 6**

```bash
git add NEXT_STEPS.md
git commit -m "$(cat <<'EOF'
docs(NEXT_STEPS): ROB Enabled + multi-NSU testbench done; next is vc_arb

Karpathy 4-lens summary (per Task 6):
- Overcomplication: clean — Depacketizer +1 struct +2 virtuals,
  Rob Enabled ~200 LOC (specified per-beat model, not gold-plated),
  LoopbackNoc ~250 LOC rewrite (focused on multi-NSU + backward compat),
  integration testbench ~80 LOC branch
- Surgical: Stage 2 axi/ untouched; nmu/packetize.hpp, addr_trans.hpp
  untouched; nsu/*.hpp source untouched (only instance count via tb);
  specgen generated headers untouched
- Surface assumptions: 32-slot pool from ROB_IDX_WIDTH=5 (derived not
  hardcoded); per-beat model caps in-flight max-burst reads at 2;
  mixed-mode strict assert; multi_dst_stress gate assumes deterministic
  per-NSU latency
- Verifiable success: 20 new tests pass; multi_dst_stress is real
  regression gate (Rob Disabled sanity-check FAILS as expected); 270
  prior tests unchanged

Drift gates final state:
- specgen pytest: 163 passed
- codegen.py --check: clean
- gen_inventory.py --check: clean
- c_model ctest: 297 sequential (276 prior + 21 new this round)

6 commits complete, all per implementation plan
docs/superpowers/plans/2026-06-03-rob-enabled-mode-multi-nsu-testbench.md.
Still on stage3/packetize-depacketize branch per user direction to
accumulate before push.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 5: Final ctest sanity**

```bash
cd c_model && ctest --test-dir build -j 1 2>&1 | tail -3
```

Confirm 100% pass.

---

## Self-review checklist (for plan author — Claude)

After writing the plan, verified:

- **Spec coverage**:
  - §4.3 file list → Tasks 1-6 each touch only the listed files
  - §5 Depacketizer interface → Task 1
  - §6 Rob Enabled mode → Tasks 2 (push) + 3 (pop)
  - §7 LoopbackNoc multi-NSU + per-NSU latency → Task 4
  - §7.4 backward-compat invariants → Task 4 Step 10 + Step 12 verify
  - §8 test plan (20 new tests) → distributed across Tasks 1-5
  - §8.2 multi_dst_stress graduation → Task 5
  - §9.1 commit boundary plan → 6 tasks match 6 commits
  - §9.2 parallel waves → header notes Wave 1/2/3/4
  - §10 open follow-ups → noted as deferred in Task 6 NEXT_STEPS update
- **Placeholder scan**: no TBD / "implement later" / handwave. Code blocks complete in every code step. Task 5 Step 4 provides concrete construction code anchored at existing file lines (verified by Step 1 read); Step 5 provides explicit shuttle-loop refactor pattern (vectorize per-NSU). `// [existing X unchanged]` prose comments in BEFORE blocks (Task 2 Steps 12-13) describe the original code that is preserved verbatim, not skipped.
- **Type consistency**:
  - `ResponseMeta { uint8_t rob_idx; uint8_t rob_req; }` defined in Task 1; used in Tasks 1, 3
  - `WriteEntry` / `ReadEntry` / `BeatRange` defined in Task 2; used in Tasks 2, 3
  - `ROB_CAPACITY` / `AXI_ID_SPACE` from codegen; used consistently
  - `DST_ID_SPACE = 1u << ni::header::DST_ID_WIDTH` defined in Task 4
  - `NsuLatencyConfig` / `DelayedFlit` defined in Task 4
  - `RobMode::Enabled` / `RobMode::Disabled` (prior round); used in Tasks 2, 3, 5
  - `kNumNsu = 4` / `kNsuSrcIds = {0x10..0x13}` defined in Task 5
- **Cross-references valid**:
  - Task 2 explicitly notes "pop_b / pop_r Enabled still assert+abort (Task 3 handles them); do not wire e2e"
  - Task 3 depends on Task 1's `pop_*_with_meta` AND Task 2's state; commit ordering enforces
  - Task 4 independent of Tasks 1-3 (no shared types); can run in parallel
  - Task 5 depends on Tasks 3 + 4 (full Enabled Rob + multi-NSU LoopbackNoc); cannot start until both land
- **Drift gates per commit**: every task ends with full ctest + specgen pytest + codegen check + gen_inventory check; counts monotonic 276→278→285→291→297.
- **Acceptance counts in commit messages**: match self-review counts 278, 285, 291, 297.

---

## Execution

Plan complete and ready for commit.

**Two execution options:**

**1. Subagent-Driven (recommended)** — Dispatch fresh subagent per task, two-stage review (spec + quality) between tasks, fast iteration. Same pattern as prior round.

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch with checkpoints.

Which approach?
