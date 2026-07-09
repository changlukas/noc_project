# NSU meta buffer FlooNoC alignment — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the NSU response path independent of system-wide-unique AXI IDs by porting FlooNoC's `floo_meta_buffer` semantics: a downstream ID remap driven by `max_unique_ids`, and a shared outstanding pool replacing the per-AXI-ID depth.

**Architecture:** `MetaBuffer` keeps its 256 buckets but keys them on the **downstream** ID and bounds them with one shared `max_outstanding` counter per direction. `Depacketize` stops allocating at the shared ingress loop and allocates at the per-channel `pop_aw` / `pop_ar` drain instead, so a full pool stalls only its own channel (mirrors the 2026-07-04 NMU independent-drain fix). `Packetize` restores the manager's original ID from `MetaEntry::upstream_id`.

**Tech Stack:** C++17 header-only c_model, GoogleTest (ctest), Verilator co-sim driven by `make sim`.

Spec: `docs/superpowers/specs/2026-07-09-nsu-meta-buffer-floonoc-alignment-design.md`

## Global Constraints

- Run all builds and simulations on WSL/Linux, never Windows. `make build` / `make test` / `make sim`.
- `local.mk` at repo root already sets `BUILD_ROOT := $(HOME)/noc_build`, `PYTHON3 := python3`, `VERILATOR := verilator`. Do not override them on the command line.
- Parameter values are fixed by the spec and approved by the user: `max_unique_ids = 1`, `max_outstanding = 32`. No `max_atomic_transactions`, no `IdMinWidth`. Do not change these values.
- No change to the flit format, `specgen` `AXI_ID_WIDTH`, the SV interface, or the DPI marshalling. The subordinate-facing ID width stays 8.
- Naming: `snake_case` for variables/methods/fields, `PascalCase` for types, full words, no abbreviations, no camelCase.
- After editing any `.hpp` / `.cpp`, run `clang-format -i <file>` (repo root `.clang-format`).
- Commit message format `type(scope): description`, English. Never `--no-verify`.
- Only two new unit tests are wanted (Task 3 and Task 4). The Verilator co-sim is the primary gate. Do not add unit tests beyond those, and do not add a deadlock unit test — a stall surfaces as a co-sim timeout.
- Do not push. Leave commits on the working branch for review.

---

## File Structure

| file | responsibility after this change |
|---|---|
| `src/c_model/include/wrap/wrap_defaults.hpp` | co-sim default depths (renamed from `poc_defaults.hpp`) |
| `src/c_model/config/port_params.yaml` | `nsu.meta_buffer.max_outstanding` + `.max_unique_ids` |
| `src/c_model/include/nsu/port_params.hpp` | loads the two new fields |
| `src/c_model/include/nsu/meta_buffer.hpp` | `MetaEntry`, shared-pool `MetaBuffer`, free function `remap_downstream_id` |
| `src/c_model/include/nsu/depacketize.hpp` | ingress decode into per-channel S1; remap + allocate at `pop_aw` / `pop_ar` |
| `src/c_model/include/nsu/packetize.hpp` | restore `upstream_id` into `bid` / `rid` |
| `src/c_model/include/nsu/nsu.hpp` | ctor plumbing |
| `src/c_model/include/wrap/nsu_wrap.hpp` | passes both parameters |

---

## Task 1: Rename `poc_defaults.hpp` to `wrap_defaults.hpp`

Pure rename. No value changes, no behaviour change. Done first so later tasks touch the final filename.

**Files:**
- Create: `src/c_model/include/wrap/wrap_defaults.hpp`
- Delete: `src/c_model/include/wrap/poc_defaults.hpp`
- Modify: `src/c_model/include/wrap/nsu_wrap.hpp`, `src/c_model/include/wrap/nmu_wrap.hpp`, `src/c_model/include/wrap/router_wrap.hpp`, `src/dpi/cmodel_dpi.cpp`, `src/c_model/tests/wrap/test_nmu_wrap.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `ni::cmodel::wrap::kAxiQueueDepth`, `kChannelModelDepth`, `kMetaBufferPerIdDepth`, `kArbiterFifoDepth`. (`kMetaBufferPerIdDepth` is renamed again in Task 2; keep the old semantics here.)

- [ ] **Step 1: Confirm the full set of references before touching anything**

Run: `grep -rn "poc_defaults\|kPoC" src/ docs/superpowers/plans/2026-07-09-nsu-meta-buffer-floonoc-alignment.md`

Expected: hits in exactly these files — `wrap/poc_defaults.hpp`, `wrap/nsu_wrap.hpp`, `wrap/nmu_wrap.hpp`, `wrap/router_wrap.hpp`, `src/dpi/cmodel_dpi.cpp`, `tests/wrap/test_nmu_wrap.cpp`. If any other file appears, stop and report it — the plan's file list is wrong.

- [ ] **Step 2: Create the renamed header**

`git mv src/c_model/include/wrap/poc_defaults.hpp src/c_model/include/wrap/wrap_defaults.hpp`

Then replace its contents with:

```cpp
// Default queue depths used by all wrap-layer adapters.
//
// Centralizes the magic numbers that previously appeared as bare literals
// across nmu_wrap.hpp and nsu_wrap.hpp. Names mirror the AdapterConfig /
// NmuConfig / NsuConfig field they populate.
#pragma once
#include <cstddef>

namespace ni::cmodel::wrap {

// AxiSlavePort / AxiMasterPort port_params.*_queue_depth and depkt_*_q_depth.
constexpr std::size_t kAxiQueueDepth = 16;

// ChannelModel req / rsp queue depths (port_params.channel_model_{req,rsp}_depth)
// and the standalone ChannelModel ctor depths.
constexpr std::size_t kChannelModelDepth = 64;

// MetaBuffer per-ID depth (port_params.meta_buffer_per_id_depth and
// NsuConfig::meta_buffer_per_id_depth).
constexpr std::size_t kMetaBufferPerIdDepth = 16;

// Wormhole + VC arbiter staging depth (wormhole_per_input_depth and
// vc_arbiter_pending_depth).
constexpr std::size_t kArbiterFifoDepth = 4;

}  // namespace ni::cmodel::wrap
```

- [ ] **Step 3: Update every consumer**

In `src/c_model/include/wrap/nsu_wrap.hpp`, `nmu_wrap.hpp`, `router_wrap.hpp`, `src/dpi/cmodel_dpi.cpp`, `src/c_model/tests/wrap/test_nmu_wrap.cpp`:

- `#include "wrap/poc_defaults.hpp"` becomes `#include "wrap/wrap_defaults.hpp"`
- `kPoCAxiQueueDepth` becomes `kAxiQueueDepth`
- `kPoCChannelModelDepth` becomes `kChannelModelDepth`
- `kPoCMetaBufferPerIdDepth` becomes `kMetaBufferPerIdDepth`
- `kPoCArbiterFifoDepth` becomes `kArbiterFifoDepth`

Also fix the prose in the trailing comments that say "PoC" where they mean "co-sim default" — for example `nsu_wrap.hpp:48` "construct NsuStandalone with a minimal PoC NsuConfig" becomes "construct NsuStandalone with the co-sim default NsuConfig", and the `kPoCChannelModelDepth (64) is reserved for the ChannelModel stub` comments become `kChannelModelDepth (64) is reserved ...`.

- [ ] **Step 4: Verify nothing references the old names**

Run: `grep -rn "poc_defaults\|kPoC" src/`
Expected: no output.

- [ ] **Step 5: Format and build**

```bash
clang-format -i src/c_model/include/wrap/wrap_defaults.hpp src/c_model/include/wrap/nsu_wrap.hpp src/c_model/include/wrap/nmu_wrap.hpp src/c_model/include/wrap/router_wrap.hpp src/dpi/cmodel_dpi.cpp src/c_model/tests/wrap/test_nmu_wrap.cpp
make test
```

Expected: ctest all pass. Record the pass count — later tasks compare against it.

- [ ] **Step 6: Commit**

```bash
git add -A src/
git commit -m "refactor(wrap): rename poc_defaults.hpp to wrap_defaults.hpp

These are the co-sim defaults, not proof-of-concept values. Rename only,
no value changes."
```

---

## Task 2: Parameter plumbing — `max_outstanding` and `max_unique_ids`

Rename `per_id_depth` to `max_outstanding` and add `max_unique_ids`, threading both down to `Depacketize`. `MetaBuffer` still behaves per-ID; Task 4 changes it. Done before Task 4 so every intermediate state compiles.

**Files:**
- Modify: `src/c_model/config/port_params.yaml`, `src/c_model/include/nsu/port_params.hpp`, `src/c_model/include/nsu/nsu.hpp`, `src/c_model/include/wrap/nsu_wrap.hpp`, `src/c_model/include/wrap/wrap_defaults.hpp`, `src/c_model/include/nsu/depacketize.hpp`

**Interfaces:**
- Consumes: `ni::cmodel::wrap::kMetaBufferPerIdDepth` (Task 1).
- Produces:
  - `nsu::PortParams::meta_buffer_max_outstanding` (`std::size_t`)
  - `nsu::PortParams::meta_buffer_max_unique_ids` (`std::size_t`)
  - `wrap::kMetaBufferMaxOutstanding = 32`, `wrap::kMetaBufferMaxUniqueIds = 1`
  - `Depacketize` ctor gains a trailing `std::size_t max_unique_ids` parameter.

- [ ] **Step 1: Update the YAML**

In `src/c_model/config/port_params.yaml`, replace the `nsu.meta_buffer` block:

```yaml
  meta_buffer:
    max_outstanding: 32
    max_unique_ids: 1
```

And update the units comment at the top of the file:

```yaml
#   meta_buffer.max_outstanding  — transactions (NSU shared outstanding pool, per direction)
#   meta_buffer.max_unique_ids   — count of distinct AXI IDs presented downstream (1 or 256)
```

(Delete the old `meta_buffer.*` units line.)

- [ ] **Step 2: Update `nsu/port_params.hpp`**

Replace the field and its loader line:

```cpp
    // NSU MetaBuffer shared outstanding pool size, per direction (write / read).
    std::size_t meta_buffer_max_outstanding;
    // Count of distinct AXI IDs the NSU presents downstream. 1 collapses every
    // request onto the all-ones ID; AXI_ID_SPACE passes the manager's ID through.
    std::size_t meta_buffer_max_unique_ids;
```

```cpp
    p.meta_buffer_max_outstanding = m["max_outstanding"].as<std::size_t>();
    p.meta_buffer_max_unique_ids = m["max_unique_ids"].as<std::size_t>();
```

Then, immediately before `return p;`, add the build guard:

```cpp
    if (p.meta_buffer_max_unique_ids != 1 && p.meta_buffer_max_unique_ids != 256) {
        throw std::runtime_error(
            "port_params.yaml: nsu.meta_buffer.max_unique_ids must be 1 or 256. Upstream and "
            "downstream AXI ID widths are both 8, so any value above 1 degenerates to the "
            "identity remap; an intermediate value would silently do nothing.");
    }
```

`256` is spelled literally rather than as `axi::AXI_ID_SPACE` because `port_params.hpp` does not include `axi/types.hpp` and should not start doing so for one constant. The `static_assert` at `axi/types.hpp:21-23` already pins `AXI_ID_SPACE == 256`, so the two cannot drift silently.

- [ ] **Step 3: Update `wrap/wrap_defaults.hpp`**

Replace the `kMetaBufferPerIdDepth` constant:

```cpp
// MetaBuffer shared outstanding pool size, per direction
// (port_params.meta_buffer_max_outstanding).
constexpr std::size_t kMetaBufferMaxOutstanding = 32;

// Distinct AXI IDs presented downstream (port_params.meta_buffer_max_unique_ids).
// 1 matches FlooNoC's ChimneyDefaultCfg. Set to 256 to pass the manager's ID
// through, which the VC throughput round requires.
constexpr std::size_t kMetaBufferMaxUniqueIds = 1;
```

- [ ] **Step 4: Update `wrap/nsu_wrap.hpp`**

Replace `cfg.port_params.meta_buffer_per_id_depth = kPoCMetaBufferPerIdDepth;` with:

```cpp
        static_assert(kMetaBufferMaxUniqueIds == 1 || kMetaBufferMaxUniqueIds == 256,
                      "max_unique_ids must be 1 or 256; see nsu/port_params.hpp");
        cfg.port_params.meta_buffer_max_outstanding = kMetaBufferMaxOutstanding;
        cfg.port_params.meta_buffer_max_unique_ids = kMetaBufferMaxUniqueIds;
```

The wrap path never loads the YAML, so it needs its own guard.

- [ ] **Step 5: Update `nsu/nsu.hpp` ctor**

`meta_buffer_(cfg_.port_params.meta_buffer_per_id_depth),` becomes:

```cpp
      meta_buffer_(cfg_.port_params.meta_buffer_max_outstanding),
```

and the `depacketize_` initializer gains the new argument:

```cpp
      depacketize_(upstream_req_, meta_buffer_, cfg_.port_params.depkt_aw_q_depth,
                   cfg_.port_params.depkt_w_q_depth, cfg_.port_params.depkt_ar_q_depth,
                   cfg_.port_params.meta_buffer_max_unique_ids),
```

- [ ] **Step 6: Update the `Depacketize` ctor**

In `src/c_model/include/nsu/depacketize.hpp`, extend the constructor and add the member:

```cpp
    Depacketize(router::NocReqIn& req_in, MetaBuffer& meta, std::size_t aw_q_depth,
                std::size_t w_q_depth, std::size_t ar_q_depth, std::size_t max_unique_ids)
        : req_in_(req_in),
          meta_(meta),
          aw_q_depth_(aw_q_depth),
          w_q_depth_(w_q_depth),
          ar_q_depth_(ar_q_depth),
          max_unique_ids_(max_unique_ids) {}
```

Add to the private section, next to `aw_q_depth_`:

```cpp
    std::size_t max_unique_ids_;
```

- [ ] **Step 7: Find and fix every other `Depacketize` construction site**

Run: `grep -rn "Depacketize(" src/`

Every direct construction (tests under `src/c_model/tests/nsu/`) needs the trailing argument. Pass `256` in existing tests so their behaviour is unchanged (identity remap) — Task 5 is what makes the argument matter, and Task 4's `MetaBuffer` is keyed on whatever ID is handed to it, so `256` preserves today's semantics exactly.

- [ ] **Step 8: Build and test**

```bash
clang-format -i src/c_model/include/nsu/port_params.hpp src/c_model/include/nsu/nsu.hpp src/c_model/include/nsu/depacketize.hpp src/c_model/include/wrap/wrap_defaults.hpp src/c_model/include/wrap/nsu_wrap.hpp
make test
```

Expected: same pass count as Task 1 Step 5. `max_unique_ids_` is stored and unused, which the compiler may warn about but must not error on.

- [ ] **Step 9: Commit**

```bash
git add -A src/
git commit -m "refactor(nsu): rename meta_buffer per_id_depth to max_outstanding, add max_unique_ids

Parameter plumbing only. MetaBuffer still bounds per AXI ID; the shared pool
lands in a later commit. max_unique_ids is threaded to Depacketize and not yet
read."
```

---

## Task 3: `remap_downstream_id` free function

A pure function. Its own test, per the spec. This is one of only two unit tests this plan adds.

**Files:**
- Modify: `src/c_model/include/nsu/meta_buffer.hpp`
- Test: `src/c_model/tests/nsu/test_meta_buffer.cpp`

**Interfaces:**
- Consumes: `ni::cmodel::axi::AXI_ID_SPACE` from `axi/types.hpp` (already included by `meta_buffer.hpp`).
- Produces: `uint8_t ni::cmodel::nsu::remap_downstream_id(uint8_t upstream_id, std::size_t max_unique_ids)`.

- [ ] **Step 1: Write the failing test**

Append to `src/c_model/tests/nsu/test_meta_buffer.cpp`:

```cpp
using ni::cmodel::nsu::remap_downstream_id;

TEST(RemapDownstreamId, CollapsesToAllOnesWhenSingleUniqueId) {
    SCENARIO("remap_downstream_id: max_unique_ids=1 maps every upstream id to 0xFF");
    EXPECT_EQ(remap_downstream_id(0x00, 1), 0xFF);
    EXPECT_EQ(remap_downstream_id(0x05, 1), 0xFF);
    EXPECT_EQ(remap_downstream_id(0xFF, 1), 0xFF);
}

TEST(RemapDownstreamId, IdentityWhenFullIdSpace) {
    SCENARIO("remap_downstream_id: max_unique_ids=AXI_ID_SPACE passes the id through");
    EXPECT_EQ(remap_downstream_id(0x00, ni::cmodel::axi::AXI_ID_SPACE), 0x00);
    EXPECT_EQ(remap_downstream_id(0x05, ni::cmodel::axi::AXI_ID_SPACE), 0x05);
    EXPECT_EQ(remap_downstream_id(0xFF, ni::cmodel::axi::AXI_ID_SPACE), 0xFF);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `make build && ctest --test-dir $HOME/noc_build/cmodel -R RemapDownstreamId`
Expected: compile error, `remap_downstream_id` is not a member of `ni::cmodel::nsu`.

- [ ] **Step 3: Implement it**

In `src/c_model/include/nsu/meta_buffer.hpp`, above `class MetaBuffer`:

```cpp
// Downstream AXI ID presented to the subordinate, from the manager's upstream ID.
// Ported from FlooNoC floo_meta_buffer.sv:89-91 (collapse to '1) and :138-139
// (offset by MaxAtomicTxns, which is 0 here because AtopSupport is off).
//
// The remap is a function of upstream_id ALONE. Feeding it src_id would let two
// sources' R bursts with the same restored rid be interleaved by the subordinate,
// which would contend nsu::VcArbiter::r_burst_vc_. See the design spec.
inline uint8_t remap_downstream_id(uint8_t upstream_id, std::size_t max_unique_ids) {
    return max_unique_ids == 1 ? static_cast<uint8_t>(axi::AXI_ID_SPACE - 1) : upstream_id;
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `make build && ctest --test-dir $HOME/noc_build/cmodel -R RemapDownstreamId`
Expected: 2 tests, both PASS.

- [ ] **Step 5: Commit**

```bash
clang-format -i src/c_model/include/nsu/meta_buffer.hpp src/c_model/tests/nsu/test_meta_buffer.cpp
git add -A src/
git commit -m "feat(nsu): add remap_downstream_id, ported from floo_meta_buffer

max_unique_ids=1 collapses every request onto the all-ones id; AXI_ID_SPACE
passes the manager id through. Function of upstream_id alone, by design."
```

---

## Task 4: `MetaBuffer` shared outstanding pool

Replace the per-ID depth cap with one shared counter per direction, report fullness instead of aborting, and carry `upstream_id`.

**Files:**
- Modify: `src/c_model/include/nsu/meta_buffer.hpp`, `src/c_model/include/nsu/depacketize.hpp`
- Test: `src/c_model/tests/nsu/test_meta_buffer.cpp`

**Interfaces:**
- Consumes: `remap_downstream_id` (Task 3), `PortParams::meta_buffer_max_outstanding` (Task 2).
- Produces:
  - `struct MetaEntry { uint8_t src_id; uint8_t upstream_id; uint8_t rob_req; uint8_t rob_idx; };`
  - `MetaBuffer::MetaBuffer(std::size_t max_outstanding)`
  - `bool MetaBuffer::write_full() const noexcept`
  - `bool MetaBuffer::read_full() const noexcept`
  - `allocate_write` / `allocate_read` take the **downstream** ID.

- [ ] **Step 1: Write the failing test**

Append to `src/c_model/tests/nsu/test_meta_buffer.cpp`:

```cpp
TEST(MetaBuffer, SharedPoolFullReportsInsteadOfAborting) {
    SCENARIO(
        "MetaBuffer: the write pool is shared across ids. 16 sources on one collapsed "
        "downstream id fill it to max_outstanding, write_full() reports, read pool is "
        "unaffected, and a commit frees one slot.");
    constexpr std::size_t kMaxOutstanding = 4;
    MetaBuffer mb(kMaxOutstanding);
    const uint8_t down = 0xFF;  // remap_downstream_id(any, 1)

    for (uint8_t src = 0; src < kMaxOutstanding; ++src) {
        EXPECT_FALSE(mb.write_full());
        mb.allocate_write(down, {src, /*upstream_id=*/0x05, 0, src});
    }
    EXPECT_TRUE(mb.write_full());

    // The read pool is a separate pool.
    EXPECT_FALSE(mb.read_full());

    // FIFO order survives: the first source out is the first source in.
    auto e = mb.peek_write(down);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->src_id, 0);
    EXPECT_EQ(e->upstream_id, 0x05);

    mb.commit_write(down);
    EXPECT_FALSE(mb.write_full());
    EXPECT_EQ(mb.peek_write(down)->src_id, 1);
}
```

Update the two pre-existing tests in this file for the new `MetaEntry` layout and ctor name. `MetaBuffer mb(/*per_id_depth=*/4)` becomes `MetaBuffer mb(/*max_outstanding=*/4)`, and every `{src_id, rob_req, rob_idx}` brace-init gains the `upstream_id` field in second position — for example `mb.allocate_write(0x05, {0x10, 1, 7})` becomes `mb.allocate_write(0x05, {0x10, 0x05, 1, 7})`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `make build && ctest --test-dir $HOME/noc_build/cmodel -R MetaBuffer`
Expected: compile error, `write_full` is not a member of `MetaBuffer`.

- [ ] **Step 3: Rewrite `MetaBuffer`**

Replace the whole `MetaEntry` struct and `MetaBuffer` class body in `src/c_model/include/nsu/meta_buffer.hpp`:

```cpp
struct MetaEntry {
    uint8_t src_id;       // requesting tile; becomes the response flit dst_id
    uint8_t upstream_id;  // manager's original AXI id; restored into bid / rid
    uint8_t rob_req;
    uint8_t rob_idx;
};

// Per-downstream-AXI-ID FIFO of {src_id, upstream_id, rob_req, rob_idx} entries,
// allocated at AW/AR egress toward the subordinate and looked up at B/R ingress
// via a peek+commit pattern.
//
// Capacity is a SHARED pool of max_outstanding entries per direction, not a
// per-ID depth. This mirrors FlooNoC's MaxTxns (floo_meta_buffer.sv:94,112,
// 148,173), whose doc calls it "the number of both incoming and outgoing
// transactions that can be handled by the network interface". A full pool
// reports through write_full() / read_full(); the caller backpressures.
//
// AXI4 ordering: per-ID transactions complete in issue order, so each bucket's
// front is the oldest outstanding for that downstream ID. Different IDs are
// independent.
//
// Atomic-ID tagging is OUT OF SCOPE (AtopSupport = 0).
class MetaBuffer {
  public:
    explicit MetaBuffer(std::size_t max_outstanding) : max_outstanding_(max_outstanding) {
        assert(max_outstanding > 0 && "MetaBuffer: max_outstanding must be positive");
    }

    // -- Write side (AW allocate + B consume) --
    bool write_full() const noexcept { return write_count_ >= max_outstanding_; }
    void allocate_write(uint8_t downstream_id, MetaEntry e) {
        assert(!write_full() && "MetaBuffer: allocate_write on a full pool; check write_full()");
        write_[downstream_id].push_back(e);
        ++write_count_;
    }
    std::optional<MetaEntry> peek_write(uint8_t bid) const noexcept {
        if (write_[bid].empty()) return std::nullopt;
        return write_[bid].front();
    }
    void commit_write(uint8_t bid) {
        assert(!write_[bid].empty() && "commit_write on empty queue");
        write_[bid].pop_front();
        --write_count_;
    }

    // -- Read side (AR allocate + R consume) --
    // Multi-beat R burst: peek every beat, commit only on rlast.
    bool read_full() const noexcept { return read_count_ >= max_outstanding_; }
    void allocate_read(uint8_t downstream_id, MetaEntry e) {
        assert(!read_full() && "MetaBuffer: allocate_read on a full pool; check read_full()");
        read_[downstream_id].push_back(e);
        ++read_count_;
    }
    std::optional<MetaEntry> peek_read(uint8_t rid) const noexcept {
        if (read_[rid].empty()) return std::nullopt;
        return read_[rid].front();
    }
    void commit_read(uint8_t rid) {
        assert(!read_[rid].empty() && "commit_read on empty queue");
        read_[rid].pop_front();
        --read_count_;
    }

  private:
    // 256 buckets sized by AXI_ID_SPACE; occupancy is bounded by the shared
    // counters, not by the bucket count. Static footprint is a modelling
    // artifact, not an RTL cost.
    std::array<std::deque<MetaEntry>, axi::AXI_ID_SPACE> write_;  // per downstream awid
    std::array<std::deque<MetaEntry>, axi::AXI_ID_SPACE> read_;   // per downstream arid
    std::size_t max_outstanding_;
    std::size_t write_count_ = 0;
    std::size_t read_count_ = 0;
};
```

Note the `std::abort()` and `<cstdlib>` usage is gone. Leave the `#include <cstdlib>` only if another symbol in the file still needs it — check and remove if not.

- [ ] **Step 4: Keep `Depacketize` compiling**

`depacketize.hpp` still allocates at ingress in this task. Its two brace-inits gain the `upstream_id` field. In the `AXI_CH_AW` case:

```cpp
                    auto aw = decode_aw(f);
                    meta_.allocate_write(aw.id,
                                         {
                                             static_cast<uint8_t>(f.get_header_field("src_id")),
                                             aw.id,
                                             static_cast<uint8_t>(f.get_header_field("rob_req")),
                                             static_cast<uint8_t>(f.get_header_field("rob_idx")),
                                         });
                    s1_aw_.accept(aw);
```

and symmetrically in `AXI_CH_AR` with `ar.id`. Behaviour is unchanged: the downstream ID is still the upstream ID.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `make test`
Expected: same pass count as Task 2, plus the new `MetaBuffer.SharedPoolFullReportsInsteadOfAborting`.

- [ ] **Step 6: Commit**

```bash
clang-format -i src/c_model/include/nsu/meta_buffer.hpp src/c_model/include/nsu/depacketize.hpp src/c_model/tests/nsu/test_meta_buffer.cpp
git add -A src/
git commit -m "feat(nsu): MetaBuffer shared outstanding pool, keyed on downstream id

Capacity moves from a per-AXI-ID depth to one shared max_outstanding pool per
direction, matching FlooNoC MaxTxns. A full pool now reports through
write_full()/read_full() instead of aborting. MetaEntry carries upstream_id so
a collapsed downstream id can be restored on the response."
```

---

## Task 5: Move remap and allocation to the per-channel drain

The load-bearing change. Allocation leaves the shared ingress loop so a full pool stalls only its own channel, mirroring the NMU independent-drain fix (`nmu.hpp:74-95`).

**Files:**
- Modify: `src/c_model/include/nsu/depacketize.hpp`
- Modify (fix-ups only): `src/c_model/tests/nsu/test_nsu_depacketize.cpp`, `src/c_model/tests/nsu/test_nsu.cpp`

**Interfaces:**
- Consumes: `remap_downstream_id` (Task 3), `MetaBuffer::write_full` / `read_full` / `allocate_*` (Task 4), `max_unique_ids_` (Task 2).
- Produces: `Depacketize::pop_aw()` / `pop_ar()` return `std::nullopt` when the matching pool is full. The returned beat's `id` is the **downstream** ID.

- [ ] **Step 1: Add the S1 element types**

In `src/c_model/include/nsu/depacketize.hpp`, above `class Depacketize`:

```cpp
// S1 payloads carry the beat together with the flit-header metadata the
// response path needs. Mirrors nmu::AdmittedAw (nmu.hpp), which the NMU
// independent-drain fix introduced for the same reason: the drain stage, not
// the shared ingress, is where a beat commits to a downstream resource.
struct PendingAw {
    axi::AwBeat beat;
    uint8_t src_id;
    uint8_t rob_req;
    uint8_t rob_idx;
};
struct PendingAr {
    axi::ArBeat beat;
    uint8_t src_id;
    uint8_t rob_req;
    uint8_t rob_idx;
};
```

Change the member declarations:

```cpp
    router::PipelineStage<PendingAw> s1_aw_;
    router::PipelineStage<axi::WBeat> s1_w_;
    router::PipelineStage<PendingAr> s1_ar_;
```

- [ ] **Step 2: Strip the MetaBuffer out of `tick()`**

Replace the `AXI_CH_AW` and `AXI_CH_AR` cases in `Depacketize::tick()`:

```cpp
            case ni::AXI_CH_AW:
                if (s1_aw_.full()) {
                    pending_ = f;
                    return;
                }
                s1_aw_.accept({decode_aw(f),
                               static_cast<uint8_t>(f.get_header_field("src_id")),
                               static_cast<uint8_t>(f.get_header_field("rob_req")),
                               static_cast<uint8_t>(f.get_header_field("rob_idx"))});
                break;
```

```cpp
            case ni::AXI_CH_AR:
                if (s1_ar_.full()) {
                    pending_ = f;
                    return;
                }
                s1_ar_.accept({decode_ar(f),
                               static_cast<uint8_t>(f.get_header_field("src_id")),
                               static_cast<uint8_t>(f.get_header_field("rob_req")),
                               static_cast<uint8_t>(f.get_header_field("rob_idx"))});
                break;
```

Rewrite the class comment block above `class Depacketize` (currently lines 17-28 and the block at 123-133) so it says where allocation happens now:

```cpp
// NSU-side request depacketizer. Stateful demux: tick() pulls flits from
// NocReqIn, decodes axi_ch, and parks each beat plus its header metadata in a
// per-channel S1 stage register. tick() touches no MetaBuffer state.
//
// pop_aw() / pop_ar() are the drain stage. Each remaps the manager's AXI id to
// the downstream id, allocates the MetaBuffer entry under that key, and hands
// the beat to AxiMasterPort. Each gates ONLY on its own pool: a full write pool
// stalls pop_aw and leaves pop_ar untouched. Allocating here rather than at
// ingress is what keeps a full pool from head-of-line blocking the other
// channels, mirroring the 2026-07-04 NMU request-path fix.
//
// Pending-flit stash semantics: if a pulled flit's S1 register is occupied, the
// flit is held in `pending_` and re-attempted next tick, blocking flits behind
// it. That is inherent to a serialized NoC link, not a modelling defect, and
// NocReqIn offers no peek to avoid it.
//
// W flits have no MetaBuffer side effect — W carries no AXI ID; W ordering is
// handled by a downstream W-meta FIFO.
```

- [ ] **Step 3: Move remap and allocation into `pop_aw` / `pop_ar`**

Replace the three pop functions:

```cpp
// pop_aw/pop_w/pop_ar: S2 consumer interface — take from the S1 register.
// Called <=1 time per channel per tick by AxiMasterPort::drain_*_from_depkt.
// Returns nullopt when the S1 register is empty, or when this channel's
// MetaBuffer pool is full (backpressure: the entry stays in S1).
inline std::optional<axi::AwBeat> Depacketize::pop_aw() {
    if (!s1_aw_.full()) return std::nullopt;
    if (meta_.write_full()) return std::nullopt;
    PendingAw p = s1_aw_.take();
    const uint8_t downstream_id = remap_downstream_id(p.beat.id, max_unique_ids_);
    meta_.allocate_write(downstream_id, {p.src_id, p.beat.id, p.rob_req, p.rob_idx});
    p.beat.id = downstream_id;
    return p.beat;
}
inline std::optional<axi::WBeat> Depacketize::pop_w() {
    if (!s1_w_.full()) return std::nullopt;
    return s1_w_.take();
}
inline std::optional<axi::ArBeat> Depacketize::pop_ar() {
    if (!s1_ar_.full()) return std::nullopt;
    if (meta_.read_full()) return std::nullopt;
    PendingAr p = s1_ar_.take();
    const uint8_t downstream_id = remap_downstream_id(p.beat.id, max_unique_ids_);
    meta_.allocate_read(downstream_id, {p.src_id, p.beat.id, p.rob_req, p.rob_idx});
    p.beat.id = downstream_id;
    return p.beat;
}
```

Note the ordering: `s1_*.take()` runs only after both guards pass, so a stalled entry is re-examined next tick and never allocated twice.

- [ ] **Step 4: Fix the existing tests**

Run: `make test 2>&1 | tail -40`

The failures will be compile errors where a test reaches into `s1_aw_` / `s1_ar_` expecting a bare beat, or asserts on a MetaBuffer entry right after `tick()`. Fix each by moving the assertion after the corresponding `pop_aw()` / `pop_ar()` call. **Do not add new test cases** — only adapt existing ones. If a test's intent is genuinely invalidated (it asserted allocation-at-ingress, which is the behaviour being removed), retarget it to assert allocation-at-pop rather than deleting it.

If a test constructs `Depacketize` directly, it already passes `256` from Task 2 Step 7, so `remap_downstream_id` is the identity and beat IDs are unchanged.

- [ ] **Step 5: Verify the full suite**

Run: `make test`
Expected: same pass count as Task 4.

- [ ] **Step 6: Commit**

```bash
clang-format -i src/c_model/include/nsu/depacketize.hpp src/c_model/tests/nsu/test_nsu_depacketize.cpp src/c_model/tests/nsu/test_nsu.cpp
git add -A src/
git commit -m "fix(nsu): allocate MetaBuffer at the per-channel drain, not the shared ingress

tick() now parks beat plus header metadata in per-channel S1 registers and
touches no MetaBuffer state. pop_aw/pop_ar remap the id, allocate, and gate on
their own pool only, so a full write pool cannot head-of-line block reads.
Mirrors the 2026-07-04 NMU request-path independent-drain fix."
```

---

## Task 6: Restore `upstream_id` on the response path

Without this the manager receives the collapsed `0xFF` as its `bid` / `rid`.

**Files:**
- Modify: `src/c_model/include/nsu/packetize.hpp`
- Modify (fix-ups only): `src/c_model/tests/nsu/test_nsu_packetize.cpp`

**Interfaces:**
- Consumes: `MetaEntry::upstream_id` (Task 4).
- Produces: response flits whose `B.bid` / `R.rid` payload fields carry the manager's original AXI ID.

- [ ] **Step 1: Restore the ID in both flit builders**

In `src/c_model/include/nsu/packetize.hpp`, `build_b_flit`, replace `f.set_payload_field("B", "bid", b.id);` with:

```cpp
    f.set_payload_field("B", "bid", m.upstream_id);
```

In `build_r_flit`, replace `f.set_payload_field("R", "rid", b.id);` with:

```cpp
    f.set_payload_field("R", "rid", m.upstream_id);
```

Note `b.id` remains the correct key for `peek_write` / `peek_read` / `commit_*` — that is the downstream ID the subordinate echoed back. Only the value written into the flit changes.

Add above `build_b_flit`:

```cpp
// The subordinate echoes the DOWNSTREAM id, which is the MetaBuffer key. The
// flit must carry the manager's original id, recovered from the buffered entry
// (FlooNoC floo_meta_buffer.sv:344-346, "Use original, buffered ID again for
// responses"). nsu::VcArbiter keys r_burst_vc_ on this restored rid.
```

- [ ] **Step 2: Fix the existing tests**

Run: `make test 2>&1 | tail -40`

Any test that fed a `MetaEntry` and asserted the flit's `bid` / `rid` equals the beat's ID must now supply a meaningful `upstream_id` and assert against it. **Do not add new test cases.**

- [ ] **Step 3: Verify the full suite**

Run: `make test`
Expected: same pass count as Task 5.

- [ ] **Step 4: Commit**

```bash
clang-format -i src/c_model/include/nsu/packetize.hpp src/c_model/tests/nsu/test_nsu_packetize.cpp
git add -A src/
git commit -m "fix(nsu): restore the manager AXI id into B/R response flits

The subordinate echoes the downstream id. Packetize now writes MetaEntry's
upstream_id into bid/rid, keeping the MetaBuffer lookup keyed on the echoed
downstream id."
```

---

## Task 7: Fault injection, then the co-sim gate

The co-sim is the real verification. Fault injection runs first: a checker that has never fired has not been verified.

**Files:**
- Temporarily modify then revert: `src/c_model/include/nsu/meta_buffer.hpp`
- Temporarily modify then revert: `src/c_model/include/wrap/wrap_defaults.hpp`
- Modify: `docs/backlog.md`

**Interfaces:**
- Consumes: everything above.
- Produces: nothing. This task gates the branch.

- [ ] **Step 1: Inject the fault**

The thing worth proving is that the co-sim actually catches a broken ID restore. Break exactly one line. In `src/c_model/include/nsu/packetize.hpp` `build_b_flit`, undo Task 6:

```cpp
    f.set_payload_field("B", "bid", b.id);  // FAULT INJECTION — revert before commit
```

`b.id` is the downstream ID. With `kMetaBufferMaxUniqueIds = 1` every write response now carries `bid = 0xFF` instead of the manager's own ID, so every master receives a B for an ID it never issued.

Change nothing else. A single-line fault proves a single checker.

- [ ] **Step 2: Run the hotspot co-sim and confirm it FAILS**

```bash
make sim TB=tb_mesh_4x4_vc1 PATTERN=hotspot SEED=12345
```

Expected: FAIL. The run log at `sim/verilator/output/directed_mesh_4x4_vc1_hotspot_s12345/run.log` must show either the `B_FRONT_CAN_ACCEPT` assertion from `axi_master.hpp` (a master drained a B with an empty per-ID deque) or a scoreboard `Unexpected RData`.

If it PASSES, the co-sim is not exercising the response ID path and the whole gate is worthless — **stop and report**. Do not proceed to Step 3.

- [ ] **Step 3: Revert the fault**

```bash
git checkout -- src/c_model/include/nsu/packetize.hpp
git diff --exit-code src/c_model/include/nsu/
```

Expected: no output from `git diff`, confirming a clean revert.

- [ ] **Step 4: Run the hotspot co-sim under `max_unique_ids = 1` (the default)**

```bash
make sim TB=tb_mesh_4x4_vc1 PATTERN=hotspot SEED=12345
```

Expected: `DIRECTED PASS: ... scoreboard clean, non-vacuous`. Sixteen distinct upstream IDs collapse onto one downstream ID at the hotspot NSU: this is the serialized path at its worst, and the case where a lost or mis-restored `upstream_id` corrupts the readback.

If the run times out rather than failing a check, that is the head-of-line hazard the design predicted. The fix is **not** a larger `max_outstanding` — re-read the request-path section of the spec and report before changing anything.

- [ ] **Step 5: Run the remaining directed patterns and `constrained_random`**

```bash
for p in neighbor transpose uniform_random; do make sim TB=tb_mesh_4x4_vc1 PATTERN=$p SEED=12345 || break; done
make sim TB=tb_mesh_4x4_vc1 PATTERN=constrained_random SEED=12345
```

Expected: each `DIRECTED PASS`, and `CR PASS` for the last.

- [ ] **Step 6: Confirm `max_unique_ids = 256` reproduces today's wire behaviour**

Edit `src/c_model/include/wrap/wrap_defaults.hpp` to `constexpr std::size_t kMetaBufferMaxUniqueIds = 256;`, then:

```bash
make sim TB=tb_mesh_4x4_vc1 PATTERN=hotspot SEED=12345
```

Expected: `DIRECTED PASS`. Then revert:

```bash
git checkout -- src/c_model/include/wrap/wrap_defaults.hpp
```

- [ ] **Step 7: Full ctest**

Run: `make test`
Expected: the Task 6 pass count, zero failures.

- [ ] **Step 8: Update the backlog**

In `docs/backlog.md`, under the "Next round: injection rate in `make sim`, VC comparison figures" section, replace the first "Open" bullet and add the invalidation notice:

- Strike `ID value per tile: 0 everywhere, or NODE_ID for log clarity.` and replace with: `RESOLVED — the AXI ID is a manager-local handle, not a topology field. The NSU no longer depends on system-wide-unique IDs (spec 2026-07-09-nsu-meta-buffer-floonoc-alignment-design.md). Stimulus IDs are free.`
- Add to "Facts that prevent a wasted session": `The recorded sim-saturation series (vc1=1248 ... vc8=1935 bits/cyc) is INVALID. It predates the NSU meta buffer change. Set kMetaBufferMaxUniqueIds = 256 in wrap/wrap_defaults.hpp before measuring, or the subordinate serializes every manager and flattens the sweep. State the setting in the figure caption.`

- [ ] **Step 9: Commit**

```bash
git add docs/backlog.md
git commit -m "docs(backlog): NSU meta buffer landed, saturation series invalidated

The recorded VC sweep predates the meta buffer change and must be re-run with
max_unique_ids = 256."
```

- [ ] **Step 10: Report, do not push**

Summarize: the ctest pass count before and after, the fault-injection FAIL followed by the clean PASS, the five co-sim axes, and the `max_unique_ids = 256` confirmation. Leave every commit on the working branch.

---

## Self-Review

**Spec coverage:**

| spec element | task |
|---|---|
| `remap_downstream_id`, collapse and identity | 3 |
| shared pool, `write_full` / `read_full`, no abort | 4 |
| `MetaEntry::upstream_id` | 4 |
| allocation at the per-channel drain, no cross-channel gate | 5 |
| `upstream_id` restored into `bid` / `rid` | 6 |
| `max_unique_ids` / `max_outstanding` parameters, values 1 and 32 | 2 |
| `poc_defaults.hpp` renamed | 1 |
| build assert `max_unique_ids == 1 || == AXI_ID_SPACE` | 2 (YAML loader guard + wrap `static_assert`) |
| fault injection first | 7 |
| hotspot co-sim gate, both `max_unique_ids` values | 7 |
| directed x4 + constrained_random regression | 7 |
| saturation series invalidated | 7 |
| no flit / specgen / SV / DPI change | all tasks; enforced by Global Constraints |
| subordinate ID width stays 8 | all tasks; no task touches it |

Two gaps were found and fixed inline: the build assert now lives in Task 2 (YAML loader guard plus a `static_assert` on the wrap path, which never loads the YAML), and Task 7's fault injection was reduced from two simultaneous edits to one line, so a single checker is proven rather than an ambiguous pair.

**Placeholder scan:** no TBD, no "handle edge cases", every code step carries the code.

**Type consistency:** `remap_downstream_id(uint8_t, std::size_t) -> uint8_t` is defined in Task 3 and used in Task 5 with that signature. `MetaEntry` field order `{src_id, upstream_id, rob_req, rob_idx}` is fixed in Task 4 and brace-initialized in that order in Tasks 4 and 5. `write_full` / `read_full` are declared in Task 4 and called in Task 5. `PendingAw` / `PendingAr` are introduced and consumed entirely within Task 5.
