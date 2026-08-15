# Tile-major address map Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Each node gets one contiguous power-of-two block, with memory and config laid out inside it. Today every memory region packs from 0 and every config region stacks above them.

**Architecture:** The node stride stops being the region size and becomes a declared number, `address_map.block_size`. Two packers implement the rule and must agree bit for bit: `sim/tools/address_map.py` `pack()` and `addr_trans.hpp` `SamTable::packed()`.

**Tech Stack:** C++17 + GoogleTest, Python 3 + pytest, Verilator co-sim on WSL.

**Spec:** `docs/superpowers/specs/2026-08-15-peripheral-addressing-design.md`, Decision 2.

## Global Constraints

- Behaviour-neutral round. Addresses move, nothing else. No header change, no peripheral work.
- The two packers are a twin. Changing one without the other is a broken tree, so they land in one commit.
- Memory and config must both stay collective targets. `declare_space_coords` returns `false` instead of asserting, so this is asserted directly.
- Commit format `type(scope): description`, English. Never `--no-verify`. `clang-format -i` on every C++ file touched.
- WSL, from `/mnt/e/05_NoC/noc_project`, `BUILD_ROOT=$HOME/noc_build`. One foreground `wsl.exe` call per unit of work, never poll a running one.
- Every co-sim call opens with `rm -f sim/filelist_*.f sim/tb/test/tb_top_*.sv sim/tb/soc/tb_top_dma_*.sv; rm -rf $BUILD_ROOT/verilator/obj_dir_*`.
- Before trusting any ctest number: `grep CMAKE_HOME_DIRECTORY $HOME/noc_build/cmodel/CMakeCache.txt` must read `/mnt/e/...`.

## The layout

    block_size = 0x100000000 per node, the stride
      +0x0         memory  0x2000000   (32 MB)
      +0x2000000   config  0x1000
      rest         free

    base = ((y << x_bits) | x) * block_size + offset[space]
    offset[memory] = 0
    offset[config] = align_up(slot[memory], slot[config])

## Files

| file | change |
|---|---|
| `sim/tools/address_map.py` | Python packer: `block_size`, offsets, new base formula |
| `nmu/addr_trans.hpp` | C++ packer: `packed()` takes `block_size`; `uniform()` passes the tile size |
| `nmu/sam_yaml.hpp` | read `address_map.block_size` |
| `sim/topologies/*.yaml` (6) | declare `block_size`, memory size to 32 MB |
| `sim/tools/test_gen_test_patterns_filemaster.py` | two assertions naming bases, plus the formula twin |
| `tests/nmu/test_sam_yaml.cpp` | new layout test and new eligibility test |
| `docs/noc-target-spec.md`, `gen_test_patterns.py` | two stale sentences |

Not touched: `SpaceCoords`, `collective_translate`, `route_mask.hpp`, NSU, routers, header. They follow the values.

---

### Task 1: The packing rule

Both packers and the topologies move in one commit. Split, the generator and the model disagree about where a node lives and nothing can be green.

**Files:**
- Modify: `sim/tools/address_map.py:19-21,43-47,50-126`
- Modify: `ref_model/c_model/include/nmu/addr_trans.hpp:56-84,86-100`
- Modify: `ref_model/c_model/include/nmu/sam_yaml.hpp:166-177`
- Modify: all six `sim/topologies/*.yaml`
- Modify: `sim/tools/test_gen_test_patterns_filemaster.py:282-287,655-663`
- Test: same Python file, and `ref_model/c_model/tests/nmu/test_sam_yaml.cpp`

**Interfaces:**
- Produces: `SamTable::packed(tiles, x_span, y_span, block_size)` — fourth argument `uint64_t`. `SamTable::uniform(x_dim, y_dim, tile_size)` unchanged signature, passes `tile_size` as the block size so existing fixtures keep their numbers. `address_map.pack(address_map, x_span, y_span)` unchanged signature, reads `block_size` from the dict it already gets.

- [ ] **Step 1: Failing Python test**

Add to `sim/tools/test_gen_test_patterns_filemaster.py`:

```python
def test_tile_major_packs_each_node_into_one_block():
    """A node's regions are contiguous inside its own block, and the stride is
    declared rather than taken from the largest region size."""
    topo = gen_tb_top.load_topology("mesh_2x2_vc1")
    _bases, entries = address_map.pack(topo["address_map"], 2, 2)
    got = {(e["space"], e["x"], e["y"]): e["base"] for e in entries}
    block = 0x100000000
    for idx, (x, y) in enumerate([(0, 0), (1, 0), (0, 1), (1, 1)]):
        assert got[("memory", x, y)] == idx * block
        assert got[("config", x, y)] == idx * block + 0x2000000
```

- [ ] **Step 2: Watch it fail**

`cd sim/tools && python -m pytest test_gen_test_patterns_filemaster.py::test_tile_major_packs_each_node_into_one_block -v`

Expected: FAIL. Config is at `0x400000000` today, not `0x2000000`.

- [ ] **Step 3: Failing C++ test**

Add to `ref_model/c_model/tests/nmu/test_sam_yaml.cpp`. If `topology_path` is spelled differently there, copy the neighbouring tests' spelling.

```cpp
TEST(SamYaml, TileMajorPacksEachNodeIntoOneBlock) {
    const SamTable t = load_sam_table(topology_path("mesh_2x2_vc1"));
    constexpr uint64_t kBlock = 0x100000000ull;
    for (unsigned idx = 0; idx < 4; ++idx) {
        const SamEntry* mem = t.lookup(idx * kBlock);
        ASSERT_NE(mem, nullptr);
        EXPECT_EQ(mem->base, idx * kBlock);
        EXPECT_EQ(mem->size, 0x2000000ull);
        EXPECT_EQ(mem->cls, axi::AxiClass::Data);
        const SamEntry* cfg = t.lookup(idx * kBlock + 0x2000000ull);
        ASSERT_NE(cfg, nullptr);
        EXPECT_EQ(cfg->base, idx * kBlock + 0x2000000ull);
        EXPECT_EQ(cfg->size, 0x1000ull);
        EXPECT_EQ(cfg->cls, axi::AxiClass::Narrow);
    }
}
```

- [ ] **Step 4: Watch it fail**

`wsl bash -lc "cd /mnt/e/05_NoC/noc_project && make build-cmodel && ctest --test-dir \$HOME/noc_build/cmodel -R SamYaml.TileMajorPacksEachNodeIntoOneBlock --output-on-failure"`

Expected: FAIL, the config lookup returns nullptr.

- [ ] **Step 5: Python packer**

Add above `_slot_size` in `sim/tools/address_map.py`:

```python
def _align_up(value, alignment):
    if alignment == 0:
        return value
    return (value + alignment - 1) // alignment * alignment


def _next_pow2(n):
    p = 1
    while p < n:
        p <<= 1
    return p
```

In `pack()`, replace the `space_base` block (`:82-85`):

```python
    # Spaces sit inside a node's block in a fixed order, memory first at 0,
    # each aligned to its own slot. block_size is the one declared number.
    offset = {"memory": 0}
    offset["config"] = _align_up(slot["memory"], slot["config"]) if slot["config"] else 0
    extent = offset["config"] + slot["config"]
    declared = (address_map or {}).get("block_size")
    block_size = int(declared) if declared is not None else _next_pow2(extent)
    if block_size & (block_size - 1):
        raise ValueError(f"address_map.block_size {block_size:#x} must be a power of two")
    if block_size < extent:
        raise ValueError(
            f"address_map.block_size {block_size:#x} is smaller than the spaces it must hold "
            f"({extent:#x})")
```

Replace the base line (`:102`):

```python
        base = (((y << x_bits) | x) * block_size) + offset[sp]
```

- [ ] **Step 6: Python docstring**

Replace `sim/tools/address_map.py:19-21`:

```
Packing rule: base = ((y << x_bits) | x) * block_size + offset[space], where
block_size is address_map.block_size (a power of two, the node stride) and
offset[space] lays the spaces out inside a node's block, memory first at 0.
slot[space] is the largest declared size in that space and now bounds the
aperture, not the stride. dst_id = (y << X_WIDTH) | x.
```

- [ ] **Step 7: C++ packer**

Replace `SamTable::packed` in `ref_model/c_model/include/nmu/addr_trans.hpp`:

```cpp
    static SamTable packed(const std::vector<PackedTile>& tiles, unsigned x_span, unsigned y_span,
                           uint64_t block_size) {
        (void)y_span;  // the caller still needs it for validate()
        const unsigned x_bits = clog2(x_span);
        uint64_t memory_slot = 0;
        uint64_t config_slot = 0;
        for (const auto& t : tiles) {
            uint64_t& slot = (t.cls == axi::AxiClass::Narrow) ? config_slot : memory_slot;
            slot = std::max(slot, t.size);
        }
        // Spaces sit inside a node's block, memory first at 0, each aligned to
        // its own slot. block_size is the stride and is declared.
        const uint64_t config_offset =
            config_slot == 0 ? 0 : ((memory_slot + config_slot - 1) / config_slot) * config_slot;
        assert((block_size & (block_size - 1)) == 0 && "SAM: block_size must be a power of two");
        assert(block_size >= config_offset + config_slot &&
               "SAM: block_size smaller than the spaces it must hold");
        std::vector<SamEntry> es;
        es.reserve(tiles.size());
        for (const auto& t : tiles) {
            const bool is_config = t.cls == axi::AxiClass::Narrow;
            const uint64_t base =
                (uint64_t{(t.y << x_bits) | t.x}) * block_size + (is_config ? config_offset : 0);
            es.push_back(
                {base, t.size, static_cast<uint8_t>((t.y << ni::width::X_WIDTH) | t.x), t.cls});
        }
        return SamTable(std::move(es));
    }
```

- [ ] **Step 8: Keep the fixtures' numbers**

In `SamTable::uniform`, the `packed` call becomes:

```cpp
        return packed(tiles, x_dim, y_dim, tile_size);
```

A memory-only fixture then has stride == size, exactly as before, so every test computing `addr / tile_size` still holds.

- [ ] **Step 9: Loader reads block_size**

Add above `load_sam_table` in `ref_model/c_model/include/nmu/sam_yaml.hpp`:

```cpp
// The stride a topology gets when it declares none: the next power of two at
// or above what the spaces occupy.
inline uint64_t default_block_size(const std::vector<PackedTile>& tiles) {
    uint64_t memory_slot = 0;
    uint64_t config_slot = 0;
    for (const auto& t : tiles) {
        uint64_t& slot = (t.cls == axi::AxiClass::Narrow) ? config_slot : memory_slot;
        slot = std::max(slot, t.size);
    }
    const uint64_t config_offset =
        config_slot == 0 ? 0 : ((memory_slot + config_slot - 1) / config_slot) * config_slot;
    uint64_t p = 1;
    while (p < config_offset + config_slot) p <<= 1;
    return p;
}
```

Replace the `packed` call (`:166`):

```cpp
    const uint64_t block_size =
        am["block_size"] ? am["block_size"].as<uint64_t>() : default_block_size(tiles);
    SamTable table = SamTable::packed(tiles, x_span, y_span, block_size);
```

- [ ] **Step 10: Topologies**

In each of the six `sim/topologies/*.yaml`: add `block_size: 0x100000000` under `address_map`, and change every memory-space `size` from `0x100000000` to `0x2000000`. Config sizes stay `0x1000`.

```yaml
address_map:
  block_size: 0x100000000
  tiles:
    - { x: 0, y: 0, size: 0x2000000 }
```

Correct each file's header comment where it states old bases. `mesh_2x2_vc1_periph.yaml` names `0x800000000` and bit positions `[33:32]` / `[13:12]`; replace with the tile-major rule, node index at bit 32 for both spaces.

- [ ] **Step 11: Two assertions naming bases**

`sim/tools/test_gen_test_patterns_filemaster.py:282-287` becomes:

```python
    # Each node's config aperture sits inside that node's own block, above its
    # memory aperture: idx * block_size + 0x2000000.
    assert txns0[-1]["addr"] == 0x2000000
    assert len(txns1) == n_beat_exact + 1
    assert txns1[-1]["addr"] == 0x100000000 + 0x2000000
```

- [ ] **Step 12: The formula twin**

`sim/tools/test_gen_test_patterns_filemaster.py:655-663` re-implements the old rule. `slot`, `x_bits`, `entries` and `topo` are already in scope. Replace the `space_base` and `expected` lines with:

```python
        block = int(topo["address_map"]["block_size"])
        offset = {"memory": 0,
                  "config": ((slot["memory"] + slot["config"] - 1) // slot["config"])
                            * slot["config"]}
        for e in entries:
            expected = (((e["y"] << x_bits) | e["x"]) * block) + offset[e["space"]]
```

- [ ] **Step 13: Both suites**

`cd sim/tools && python -m pytest -q` — expect PASS.

`wsl bash -lc "cd /mnt/e/05_NoC/noc_project && make build-cmodel && ctest --test-dir \$HOME/noc_build/cmodel --output-on-failure"` — expect PASS.

A failure naming a concrete address in a file not listed above is a real finding. Report it, do not edit the number: the design predicted only these sites move.

- [ ] **Step 14: Commit**

```bash
clang-format -i ref_model/c_model/include/nmu/addr_trans.hpp \
    ref_model/c_model/include/nmu/sam_yaml.hpp \
    ref_model/c_model/tests/nmu/test_sam_yaml.cpp
git add sim/tools/address_map.py sim/tools/test_gen_test_patterns_filemaster.py \
    sim/topologies ref_model/c_model/include/nmu ref_model/c_model/tests/nmu/test_sam_yaml.cpp
git commit -m "feat(sam): give the node stride its own number, and lay the spaces out inside it"
```

---

### Task 2: Pin the collective field offset

Non-null alone is not enough. It was non-null under the old layout too. The field offset is what separates a correct declaration from one that agreed by accident.

**Files:** Test: `ref_model/c_model/tests/nmu/test_sam_yaml.cpp`

**Interfaces:** Consumes `SamTable::collective_coords(axi::AxiClass)` and `SpaceCoords::x_range` / `y_range`, both unchanged this round.

- [ ] **Step 1: Write the test**

```cpp
TEST(SamYaml, BothTileSpacesStayCollectiveTargetsAtTheBlockStride) {
    // declare_space_coords RETURNS FALSE, it does not abort, so a space that
    // stops being a collective target shows up only as a multicast refused at
    // the source. 32 is log2(block_size).
    for (const char* name : {"mesh_2x2_vc1", "mesh_4x4_vc1", "mesh_4x4_vc4"}) {
        const SamTable t = load_sam_table(topology_path(name));
        const SpaceCoords* data = t.collective_coords(axi::AxiClass::Data);
        const SpaceCoords* narrow = t.collective_coords(axi::AxiClass::Narrow);
        ASSERT_NE(data, nullptr) << name << ": memory space is not a collective target";
        ASSERT_NE(narrow, nullptr) << name << ": config space is not a collective target";
        EXPECT_EQ(data->x_range.offset, 32u) << name;
        EXPECT_EQ(narrow->x_range.offset, 32u) << name;
        EXPECT_EQ(data->y_range.offset, narrow->y_range.offset) << name;
    }
}
```

- [ ] **Step 2: Run it**

`wsl bash -lc "cd /mnt/e/05_NoC/noc_project && make build-cmodel && ctest --test-dir \$HOME/noc_build/cmodel -R SamYaml.BothTileSpaces --output-on-failure"`

Expected: PASS on the Task 1 tree. It is a pin, not a driver.

- [ ] **Step 3: Fault-inject it**

Change `mesh_2x2_vc1.yaml`'s `block_size` to `0x200000000`, change nothing else, rerun. Expect FAIL on the offset (33, not 32). Restore the YAML, rerun, expect PASS. A checker never seen to fire is not evidence. Put both outcomes in the commit message.

- [ ] **Step 4: Commit**

```bash
clang-format -i ref_model/c_model/tests/nmu/test_sam_yaml.cpp
git add ref_model/c_model/tests/nmu/test_sam_yaml.cpp
git commit -m "test(sam): pin both tile spaces to a collective field at the block stride"
```

---

### Task 3: The docs that state the old rule

Two sentences say the coordinate field sits at `log2(region size)`. True while the stride was the region size, not now. Neither is code — both implementations were always stride-based (`sam_yaml.hpp:57`, `gen_test_patterns.py:690`), which is why nothing broke. A third file states the shipped map's old numbers outright.

**Files:** Modify `docs/noc-target-spec.md:365` and its repeat at `:521-522`; `sim/tools/gen_test_patterns.py`, `collective_addr_mask` docstring; `docs/verification-environment.md:306-307,332,342-343`.

- [ ] **Step 1: Target spec**

`docs/noc-target-spec.md:365` reads:

> The node index then occupies a contiguous address field at `log2(size)`, ...

Replace `log2(size)` with `log2(node_stride)` and add:

> `node_stride` is the power-of-two block a node's regions are laid out inside. Where a space is packed with its stride equal to its region size the two coincide.

Same correction at `:521-522`. In the same section, the fourth eligibility condition "mapped consecutively in coordinate order" becomes "mapped at a uniform power-of-two stride in coordinate order": a 32 MB region under a 4 GiB stride is not consecutive and is still eligible.

Confirm: `grep -n 'log2(size)' docs/noc-target-spec.md` returns nothing.

- [ ] **Step 2: Generator docstring**

In `collective_addr_mask`, the sentence naming `log2` of the region size and the positions `[33:32]` / `[13:12]` becomes:

> The node-index field sits at `log2(block_size)`, the same position for every space, because tile-major gives every space one stride. Deriving the mask from the bases rather than from a constant is what keeps this correct without the caller naming a bit position.

Keep the rest of the docstring.

- [ ] **Step 3: The verification environment doc**

`docs/verification-environment.md` states the shipped map's old numbers in three places. The Task 1 review found these; the plan had missed the file.

- `:306-307` shows a topology excerpt with `size: 0x100000000` memory tiles. Update to `size: 0x2000000` and add the `block_size: 0x100000000` line, matching what `sim/topologies/mesh_2x2_vc1.yaml` now says.
- `:332` states the map "is uniform at `0x100000000` per memory tile". The stride is `0x100000000`; the memory tile is `0x2000000`. Say both.
- `:342-343` gives config bases as `n_nodes * 0x100000000 + idx * 0x1000`. Under tile-major a node's config sits inside its own block: `idx * block_size + 0x2000000`.

Read the surrounding paragraphs before editing — the point each sentence is making has to survive the correction.

- [ ] **Step 4: Check nothing else states the old rule**

`grep -rn 'log2(size)\|log2 of that space' docs/ sim/tools/ ref_model/` — expect no hits.

`grep -rn '0x400000000\|0x100000000' docs/*.md` — every remaining hit must be a stride, not a memory tile size. Fix any that is not.

- [ ] **Step 5: Commit**

```bash
git add docs/noc-target-spec.md docs/verification-environment.md sim/tools/gen_test_patterns.py
git commit -m "docs(sam): the node index sits at the block stride, not the region size"
```

---

### Task 4: The round gate

The behaviour-neutral claim is about the co-sim, not ctest, and it covers both flavours because moving addresses changes what `gen_dma_jobs.py` computes.

**Files:** none modified.

- [ ] **Step 1: Confirm the build points at this tree**

`wsl bash -lc "grep CMAKE_HOME_DIRECTORY \$HOME/noc_build/cmodel/CMakeCache.txt"` — must read `/mnt/e/05_NoC/noc_project`. Anything else means the suite is testing a different tree; delete the cache, do not edit it.

- [ ] **Step 2: Unit suites**

`wsl bash -lc "cd /mnt/e/05_NoC/noc_project && make test 2>&1 | tail -5"` — expect all pass.

`cd sim/tools && python -m pytest -q` — expect PASS.

`python specgen/tools/codegen.py --check` from the repo root — expect clean. `specgen/` is untouched, so drift here is a real finding.

- [ ] **Step 3: Tier 2, small mesh**

`wsl bash -lc "cd /mnt/e/05_NoC/noc_project && rm -f sim/filelist_*.f sim/tb/test/tb_top_*.sv sim/tb/soc/tb_top_dma_*.sv && rm -rf \$HOME/noc_build/verilator/obj_dir_* && stdbuf -o0 make -C sim TB=mesh_2x2_vc1 PATTERN=neighbor 2>&1 | tail -3"`

Expected: `DIRECTED PASS: ... scoreboard clean, non-vacuous`

- [ ] **Step 4: Tier 2, scale and wrap**

Same command with `TB=mesh_4x4_vc1`. Expected: `DIRECTED PASS`.

- [ ] **Step 5: A collective run**

`wsl bash -lc "cd /mnt/e/05_NoC/noc_project && stdbuf -o0 make -C sim TB=mesh_4x4_vc1 PATTERN=multicast MCAST_SHAPE=row 2>&1 | tail -3"`

Expected: `DIRECTED PASS`. This is Task 2's assertion executed end to end — the field offset moved.

- [ ] **Step 6: The SoC flavour, both directions**

`wsl bash -lc "cd /mnt/e/05_NoC/noc_project && rm -f sim/filelist_*.f sim/tb/test/tb_top_*.sv sim/tb/soc/tb_top_dma_*.sv && rm -rf \$HOME/noc_build/verilator/obj_dir_* && stdbuf -o0 make -C sim TB=mesh_4x4_vc4 DMA=1 DMA_RW=write 2>&1 | tail -3"`

Then the same with `DMA_RW=read`. Expected: `DMA PASS: ... every job retired, every region intact`.

The DMA window check (`gen_dma_jobs.py:137-139`) now measures against 32 MB instead of 4 GiB. The shipped job set reaches about 1.6 MB, so it fits. An `overruns the ... memory tile` exit here is the check working and means the job geometry needs revisiting, not the check.

- [ ] **Step 7: Record**

Append the run tags and results to `docs/backlog.md` under "This round". If that is the only edit, commit as `docs(backlog): record the tile-major round-1 gate`.

---

## Self-Review

**Spec coverage.** Decision 2's packing rule is Task 1. "Round 1 work the blast radius makes explicit" splits into Task 1 (assertions naming bases) and Task 3 (the two sentences). The round-1 acceptance bar including collective eligibility is Tasks 2 and 4.

The C++ hardcoded addresses the spec names (`test_packetize.cpp:317-318`, `test_rob.cpp:1316,1333`) are deliberately **not** a task: they run against `SamTable::uniform` fixtures, not a topology YAML, and Task 1 Step 8 keeps `uniform`'s stride equal to its tile size. If Task 1 Step 13 shows them failing, that assumption is wrong and it is a finding to report.

**Placeholders.** None. Every code step carries its code.

**Type consistency.** `packed` gains `uint64_t block_size` in Step 7, called with it in Steps 8 and 9. `default_block_size(const std::vector<PackedTile>&)` is defined in Step 9 above its only caller. `_align_up` / `_next_pow2` are defined and used in Step 5. `collective_coords` and `SpaceCoords::x_range.offset` in Task 2 are existing names.
