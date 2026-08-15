# Round 3: peripherals move on-grid

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every tile reaches every peripheral on all four mesh faces.

**Architecture:** A peripheral stops having its own route coordinate and shares the coordinate of
the router it hangs off; `dst_port_id` (added in round 2, currently always 0) says which endpoint at
that coordinate receives. The SAM stops using the AXI class as a stand-in for the address space,
because a peripheral is a third space carrying the Data class. The six route-span YAML keys, the
four tile-region bounds they feed, and `check_dst_reachable` all go away.

**Tech Stack:** C++17, CMake/GoogleTest, specgen codegen, Python generators, Verilator co-sim.

**Spec:** `docs/superpowers/specs/2026-08-15-peripheral-addressing-design.md` — Decisions 1, 3, 4, 5
and the round 3 row of the Rounds table.

**Branch:** `feat/peripherals-on-grid`, off whatever `feat/port-id-header-fields` lands as.

## Global Constraints

- **Task order is load-bearing.** The space-keyed SAM and the loader's eligibility policy (Tasks 1
  and 2) land BEFORE the tile-region clip bound comes out (Task 4). Reversed, peripherals are legal
  collective members in the window between.
- A peripheral is never a collective member, never a collective target, and **never a collective
  issuer**. The issuer half is the one with no structural backstop — see "The guard that disappears".
- `port_id` encoding, unchanged from round 2: `00` LOCAL (the tile), `01` x face, `10` y face,
  `11` reserved. Which face `01`/`10` mean follows from the router's position: `cfg.x == 0` gives
  WEST, `cfg.x == mesh_x_dim - 1` gives EAST; `cfg.y == 0` gives SOUTH, `cfg.y == mesh_y_dim - 1`
  gives NORTH.
- C++17, no designated initializers. New members on `SamEntry`, `Translated`, `PackedTile`,
  `MetaEntry`, `AwHeaderMeta`, `AdmittedAw`, `AdmittedAr` go LAST — every one of these is
  initialised positionally somewhere.
- snake_case for variables and methods, PascalCase for types, no camelCase. Full words, no
  abbreviations. 4-space indent, 100 columns, repo-root `.clang-format` on every touched `.hpp`/`.cpp`.
- Commit message format `type(scope): description`, English.
- Build and test on WSL, from `/mnt/e/05_NoC/noc_project`, with `BUILD_ROOT=$HOME/noc_build`.
  Use `python3`, never `py -3`. Prefix `wsl` calls carrying POSIX paths with `MSYS_NO_PATHCONV=1`.
- Every co-sim invocation opens with the pre-clean:
  `rm -f sim/filelist_*.f sim/tb/test/tb_top_*.sv sim/tb/soc/tb_top_dma_*.sv; rm -rf $BUILD_ROOT/verilator/obj_dir_*`
- One synchronous foreground `wsl` call per unit of work. Never poll a running WSL job with a second
  `wsl.exe` call. A single call past ~15 minutes with no output is wedged, not slow — report it.
- New asserts need a fault-injection proof that they fire.
- Round 3 verifies `sim/tb/test/` only. The DMA flavour cannot run a peripheral topology
  (`docs/known-limitations.md`).

## The clip is deleted, not re-bounded

`route_mask.hpp:108-112` names four copies of the tile-region clip that must agree node by node,
because the join is stateless and a one-node disagreement hangs a collect:

| # | site |
|---|---|
| 1 | `router/route_mask.hpp` `route_mask_fork` (`:113-121`) |
| 2 | `router/route_mask.hpp` `route_mask_join` (`:175-184`) |
| 3 | `nmu/addr_trans.hpp` `collective_translate` (`:416-419`) |
| 4 | `sim/tools/gen_test_patterns.py` `collective_addr_mask` |

The spec (Decision 3) proposed re-bounding all four from the tile region to the mesh, justified by
exactly one case: "a wildcard mask can still name a coordinate that does not exist when a mesh
dimension is not a power of two."

**Mesh dimensions are power-of-two only, so that case cannot arise and all four copies go.** The
argument is short. `declare_space_coords` requires `x_range.len == clog2(x_count)`
(`addr_trans.hpp:206`), and `collective_translate` already refuses a mask with any bit outside the
coordinate field. A wildcard therefore expands over exactly `0 .. 2^len - 1` per axis, and with a
power-of-two dimension that is exactly `0 .. dim - 1` — every coordinate it names exists. The clip
has nothing to clip. `detail::in_mesh` still catches a malformed `dst_id`; that is a separate check
and it stays.

Deleting beats re-bounding twice over: four structurally different sites stop having to agree, and
the "Change one, change all four" coupling that made this the round's biggest hazard disappears with
them.

**The convention has to become a check.** Nothing in the tree enforces power-of-two today — the only
validation is `x_dim >= 2` (`sam_yaml.hpp:159`, `gen_tb_top.py:112`), and every shipped topology
happens to be 2 or 4. Task 4 adds the assert, because a deletion resting on an unenforced convention
is precisely the failure mode "The guard that disappears" is about.

## The guard that disappears

`addr_trans.hpp:373-382` refuses a collective whose ISSUER sits outside the tile region:

```cpp
    if (src_x < coords->x_first || src_x > coords->x_last || src_y < coords->y_first ||
        src_y > coords->y_last) {
        assert(false && "... a collective issued from outside the tile region ...");
```

It works today only because a peripheral has an off-grid coordinate. Round 3 puts the peripheral on
its router's coordinate, so `src_x` / `src_y` land inside the region and the check passes — and
Task 4 deletes the `SpaceCoords` bounds it reads. The guard does not fail; it evaporates, with no
compile error and no failing test, and a peripheral-issued collective then forks along a row where
no router expects the replicas and the CollectB never completes.

The replacement is the port, not the coordinate: an issuer with `src_port_id != 0` is a peripheral.
Task 4 Step 1 threads it in, BEFORE the same task deletes the bounds. That ordering is the whole
point — a deletion and its replacement land in one commit or the guard is gone in between.

## Why scheme B does not deadlock, and what that rests on

The spec rejected alternative A' (turn the packet early) because with both x faces populated the
admitted turn set closes a channel dependency cycle. Scheme B is not obviously safer, and the reason
it is safe is worth writing down, because a later change can take it away.

Ejecting to a face admits turns XY forbids. A flit that has already resolved X is travelling in Y;
ejecting it out a WEST or EAST port is an N→W, S→W, N→E or S→E turn, and XY admits none of those.
Pair them with the ordinary XY turns and a cycle closes: N→E (face eject), E→S (XY), S→W (face
eject), W→N (XY).

It is not a cycle, because **a boundary port on an edge router is terminal**. There is no western
neighbour at `x == 0`, so the WEST port feeds a peripheral NI that consumes the flit — it is not an
input to any other router's routing decision, and a channel dependency cycle needs one. Ejecting to
a face is LOCAL with a different pin, not a turn.

That argument holds only while peripherals sit on edge routers. Task 2's face-legality assert
(`face: x` needs `x == 0` or `x == x_dim - 1`) is what guarantees it. **That assert is the
deadlock-freedom precondition, not input tidiness.** If a later round wants a peripheral on an
interior router, the port it hangs off is a live inter-router link, the eject becomes a real turn,
and the cycle above closes for real. Say so in the assert's message.

## Rulings carried in from round 2

**A heterogeneous-port multicast fork is not implemented, it is ruled illegal.** The router copies a
fork replica verbatim (`router.hpp:522-529`, `simple_router.hpp:719-726`), so one `dst_port_id`
reaches every node in the fork set. Decision 4 keeps peripherals out of collective eligibility, so
the set can only ever contain tiles, and tiles are all port 0. Task 1 makes that structural rather
than incidental; nothing needs rewriting. If a later round wants peripheral collectives, the fork
path needs per-branch port derivation — that is a new design, not a bug fix.

**The collective-join `dst_port_id` needs no work.** An earlier note claimed the joined B carries an
arbitrary port. It does not: `simple_router.hpp:536-554` forwards a whole flit, so the surviving
replica's `dst_port_id` is the requester's. Only the joined B's `src_port_id` is arbitrary, and
nothing anywhere reads a response's `src_port_id`.

---

### Task 1: The SAM keys on space, not class

The class currently stands in for the space. `addr_trans.hpp:274` states the assumption in a
comment: `// Indexed by axi::AxiClass (Narrow = 0, Data = 1) -- one address space each.` A
peripheral is a third space carrying the Data class, so the correspondence ends and five sites stop
being correct. One of them fails silently and expensively: `declare_space_coords`' tile walk returns
false and **the memory space stops being a collective target**, with no error.

**Files:**
- Modify: `ref_model/c_model/include/axi/types.hpp:72` (add `Space` beside `AxiClass`)
- Modify: `ref_model/c_model/include/nmu/addr_trans.hpp:16-48` (`Translated`, `SamEntry`, `PackedTile`), `:154-188` (`validate`), `:202-266` (`declare_space_coords`, `collective_coords`), `:329-360` (`collective_translate`)
- Modify: `ref_model/c_model/include/nmu/sam_yaml.hpp:15-23` (`parse_tile_space`), `:36-69` (`declare_space_coords`), `:75-80` (`space_present`), `:96-119` (`check_decode_mode`), `:190-218` (the loader tail and the `region_stated` loop)
- Modify: `ref_model/c_model/include/wrap/nsu_wrap.hpp:88-92` (its `collective_coords` loop over `AxiClass`)
- Test: `ref_model/c_model/tests/nmu/test_sam_table.cpp`, `test_sam_yaml.cpp`, `test_addr_trans.cpp`

**The signature change reaches four call sites this task's own narrative does not name.**
`declare_space_coords` and `collective_coords` change type, so every caller changes with them:
`space_present`, `check_decode_mode`, the `region_stated` loop in the loader tail, and
`nsu_wrap.hpp`'s loop that fills `NsuConfig::space_coords`. All four iterate
`{AxiClass::Narrow, AxiClass::Data}` and become `{Space::Config, Space::Memory}` — the peripheral
space is deliberately not in that set. These are compile-visible, so they will not escape, but the
"five class-keyed sites" framing above undercounts and you should expect nine edits, not five.

**One of the four is more than a type change.** `check_decode_mode` (`sam_yaml.hpp:96-119`) asserts
that EVERY present space has non-null `collective_coords` when a topology declares `decode: offset`.
The peripheral space is deliberately null, so a peripheral topology that also declares
`decode: offset` would abort on a legal configuration. Restricting the loop to
`{Space::Config, Space::Memory}` fixes it, but do it deliberately and say why in the comment: the
check is about the spaces that carry a node stride, and a peripheral region has none. No shipped
topology declares `decode` today, so this is latent rather than failing — which is exactly why it
needs the comment.

**Interfaces:**
- Produces: `axi::Space { Config = 0, Memory = 1, Peripheral = 2 }`; `axi::class_of(Space)`;
  `SamEntry::space`, `Translated::space`, `PackedTile::space` (all `axi::Space`, default
  `Space::Memory`, appended LAST); `SamTable::declare_space_coords(axi::Space, const SpaceCoords&)`;
  `SamTable::collective_coords(axi::Space) const`.

- [ ] **Step 1: Write the failing tests**

Add to `ref_model/c_model/tests/nmu/test_sam_yaml.cpp`:

```cpp
TEST(SamYaml, PeripheralSpaceIsNotACollectiveTarget) {
    // The loader declares only the tile spaces. A peripheral space's bases are
    // assigned in declaration order at arbitrary sizes, so there is no uniform
    // power-of-two stride to read a coordinate field from -- the declaration is
    // not attempted, rather than attempted and failed.
    auto sam = load_sam_table(std::string(TOPOLOGY_DIR) + "/mesh_2x2_vc1_periph.yaml");
    EXPECT_NE(sam.collective_coords(axi::Space::Memory), nullptr);
    EXPECT_NE(sam.collective_coords(axi::Space::Config), nullptr);
    EXPECT_EQ(sam.collective_coords(axi::Space::Peripheral), nullptr)
        << "a peripheral must never be a collective target";
}

TEST(SamYaml, MemorySpaceStaysACollectiveTargetAlongsideAPeripheral) {
    // The regression this task exists to prevent: keyed on class, a peripheral
    // carrying the Data class joins the memory space's tile walk, the walk's
    // count check fails, and the memory space silently loses eligibility.
    auto sam = load_sam_table(std::string(TOPOLOGY_DIR) + "/mesh_2x2_vc1_periph.yaml");
    const auto* memory = sam.collective_coords(axi::Space::Memory);
    ASSERT_NE(memory, nullptr);
    EXPECT_EQ(memory->x_range.offset, 32u);  // log2(block_size, 4 GiB)
}
```

Both need the round-3 `mesh_2x2_vc1_periph.yaml`, which Task 2 writes. **Until Task 2 lands they
fail on the current YAML.** That is expected; Step 2 records which way they fail so Task 2's
reviewer can tell a fixed test from a still-broken one.

Add to `ref_model/c_model/tests/nmu/test_sam_table.cpp`:

```cpp
TEST(SamTable, PeripheralEntryDoesNotJoinTheMemorySpacesTileWalk) {
    // Four memory tiles in a 2x2, plus one peripheral entry that also carries
    // the Data class. Keyed on class the walk counts five and rejects; keyed on
    // space it counts four and declares.
    std::vector<SamEntry> es;
    for (unsigned y = 0; y < 2; ++y) {
        for (unsigned x = 0; x < 2; ++x) {
            const uint64_t base = ((y << 1) | x) * 0x100000000ull;
            es.push_back({base, 0x2000000, static_cast<uint8_t>((y << 4) | x),
                          axi::AxiClass::Data, /*port=*/0, axi::Space::Memory});
        }
    }
    es.push_back({0x400000000ull, 0x1000, /*dst_id=*/0x00, axi::AxiClass::Data,
                  /*port=*/1, axi::Space::Peripheral});
    SamTable sam{std::move(es)};
    SpaceCoords c{};
    c.x_range = {32, 1};
    c.y_range = {33, 1};
    c.x_count = 2;
    c.y_count = 2;
    c.x_first = 0;
    c.x_last = 1;
    c.y_first = 0;
    c.y_last = 1;
    EXPECT_TRUE(sam.declare_space_coords(axi::Space::Memory, c));
    EXPECT_NE(sam.collective_coords(axi::Space::Memory), nullptr);
    EXPECT_EQ(sam.collective_coords(axi::Space::Peripheral), nullptr);
}
```

Read the file's existing `SpaceCoords` fixtures before writing this — match how they spell the
struct, and confirm the `SamEntry` member order against `addr_trans.hpp` after Step 3.

- [ ] **Step 2: Run them and watch them fail**

```bash
ctest --test-dir $HOME/noc_build/cmodel -R "SamTable|SamYaml" --output-on-failure
```

Expect compile errors: no `axi::Space`, no `SamEntry::space`, and `declare_space_coords` /
`collective_coords` taking a class. Record the exact failure text in your report.

- [ ] **Step 3: Add the enum and the class map**

In `ref_model/c_model/include/axi/types.hpp`, immediately after `enum class AxiClass` (`:72`), in
the same comment register as its neighbours:

```cpp
// SAM address space. Distinct from AxiClass: the two agreed one-to-one while
// there were two spaces and two classes, but a peripheral region is a third
// space carrying the Data class. The SAM keys on this; the flit still carries
// only the class, through axi_ch.
enum class Space : uint8_t { Config = 0, Memory = 1, Peripheral = 2 };

inline constexpr AxiClass class_of(Space space) {
    return space == Space::Config ? AxiClass::Narrow : AxiClass::Data;
}
```

- [ ] **Step 4: Give the SAM structs a space**

`Translated`, `SamEntry` and `PackedTile` each gain, appended LAST:

```cpp
    axi::Space space = axi::Space::Memory;
```

`SamTable::translate` (`addr_trans.hpp:135`) copies `e->space` into the `Translated` it returns,
beside the `e->port` it already copies.

`SamTable::packed` (`:99`) sets each entry's space from its `PackedTile`'s.

- [ ] **Step 5: Re-key the table's five class-driven sites**

`coords_` and `eligible_` become 3-wide, indexed by `Space`. `declare_space_coords` and
`collective_coords` take `axi::Space` instead of `axi::AxiClass`; inside the walk, the filter
`if (e.cls != cls) continue;` (`:221`) becomes `if (e.space != space) continue;` and the reachability
check `if (e == nullptr || e->cls != cls) return false;` (`:250`) becomes `e->space != space`.

`validate` (`:154-188`) stops bucketing on class. Coverage becomes a per-space property: the memory
space must cover the mesh exactly once, config must cover it or be absent, and any other space
covers nothing in particular and is skipped by both the duplicate check and the count check.

`collective_translate` (`:358`) calls `sam.collective_coords(entry->space)`.

- [ ] **Step 6: Make the loader state the policy instead of inferring it**

`sam_yaml::parse_tile_space` (`:15-23`) returns `axi::Space` rather than `axi::AxiClass`; each entry
takes its class from `axi::class_of(space)`. The YAML strings stay `config` / `memory`, plus
`peripheral` for Task 2.

`sam_yaml::declare_space_coords` (`:36-69`) currently attempts a declaration for every class and
ignores the result, which makes "not a collective target" indistinguishable from "the declaration
happened to fail". Declare only `Space::Config` and `Space::Memory`, and assert that
`collective_coords(Space::Peripheral)` is null afterwards, so the property is stated:

```cpp
    assert(table.collective_coords(axi::Space::Peripheral) == nullptr &&
           "sam_yaml: the peripheral space must not be collective-eligible");
```

- [ ] **Step 7: Run the tests**

```bash
ctest --test-dir $HOME/noc_build/cmodel --output-on-failure
```

The two `SamYaml` cases from Step 1 still fail until Task 2 supplies the round-3 YAML — everything
else must be green, at the same count as before plus the one new `SamTable` case.

- [ ] **Step 8: Commit**

```bash
clang-format -i ref_model/c_model/include/axi/types.hpp ref_model/c_model/include/nmu/*.hpp ref_model/c_model/tests/nmu/test_sam_table.cpp
git add ref_model/c_model
git commit -m "refactor(sam): key the address map on space, not on the AXI class"
```

---

### Task 2: The peripherals YAML block

**Files:**
- Modify: `ref_model/c_model/include/nmu/sam_yaml.hpp:120-215` (the loader)
- Modify: `sim/tools/address_map.py` (`SPACE_ORDER`, the packer)
- Rewrite: `sim/topologies/mesh_2x2_vc1_periph.yaml`
- Test: `ref_model/c_model/tests/nmu/test_sam_yaml.cpp`, `sim/tools/test_gen_test_patterns_filemaster.py`

**Interfaces:**
- Consumes: `axi::Space`, `SamEntry::space`, `SamEntry::port` (Task 1 and round 2).
- Produces: `struct PeripheralRegion { unsigned x; unsigned y; uint8_t port; uint64_t size; };` in
  `nmu/addr_trans.hpp` beside `PackedTile`; `SamTable::packed(tiles, x_dim, y_dim, block_size,
  const std::vector<PeripheralRegion>& peripherals = {})`; the `peripherals:` YAML block, entries
  `{ x, y, face: x|y, size }`.

**`SamTable` is built by `packed()`, not appended to.** The table's constructor takes its whole
entry vector and `SamTable::packed` (`addr_trans.hpp:64-99`) is what derives tile bases from
coordinates. Peripheral regions are NOT coordinate-derived — they are declaration-ordered above the
tile array — but they still have to arrive through the same construction, or the loader ends up
holding a half-built table. Hence the extra `packed()` parameter rather than a mutator.

- [ ] **Step 1: Rewrite the topology**

`sim/topologies/mesh_2x2_vc1_periph.yaml` loses `x_span`, `y_span` and the four `tile_*` keys, its
tile map shrinks from six coordinates to four, and it gains a `peripherals:` block. Peripherals sit
on the router's own coordinate:

```yaml
topology:
  name: mesh_2x2_vc1_periph
  x_dim: 2
  y_dim: 2
  num_vc: 1

address_map:
  block_size: 0x100000000
  tiles:
    - { x: 0, y: 0, size: 0x2000000 }
    - { x: 1, y: 0, size: 0x2000000 }
    - { x: 0, y: 1, size: 0x2000000 }
    - { x: 1, y: 1, size: 0x2000000 }
    - { x: 0, y: 0, size: 0x1000, space: config }
    - { x: 1, y: 0, size: 0x1000, space: config }
    - { x: 0, y: 1, size: 0x1000, space: config }
    - { x: 1, y: 1, size: 0x1000, space: config }
  # A peripheral shares its router's coordinate and is told apart by the port it
  # hangs off. Its region sits above the tile array, sized independently -- a
  # peripheral is never a collective member, so none of the four
  # collective-eligibility conditions in docs/noc-target-spec.md 5.1 apply to it.
  peripherals:
    - { x: 0, y: 0, face: x, size: 0x1000 }
    - { x: 0, y: 1, face: x, size: 0x1000 }
```

Rewrite the file's header comment to describe this layout. The current one explains the off-grid
span at length and every sentence of it is now false.

- [ ] **Step 2: Run the Task 1 tests and watch them pass**

```bash
ctest --test-dir $HOME/noc_build/cmodel -R "SamYaml" --output-on-failure
```

`PeripheralSpaceIsNotACollectiveTarget` and `MemorySpaceStaysACollectiveTargetAlongsideAPeripheral`
were written in Task 1 and have been failing since. They must pass once the loader reads the block —
they are this task's acceptance, not decoration.

- [ ] **Step 3: Read the block in the loader**

In `sam_yaml.hpp`, after the `tiles` loop (`:190-194`) and before the `SamTable::packed` call
(`:197`), read the block into a `std::vector<PeripheralRegion>` and pass it through:

```cpp
    std::vector<PeripheralRegion> peripherals;
    for (const auto& p : am["peripherals"]) {
        const unsigned x = p["x"].as<unsigned>();
        const unsigned y = p["y"].as<unsigned>();
        const std::string face = p["face"].as<std::string>();
        assert((face == "x" || face == "y") && "address_map peripheral: face must be 'x' or 'y'");
        const bool on_x_edge = (x == 0 || x == x_dim - 1);
        const bool on_y_edge = (y == 0 || y == y_dim - 1);
        assert((face == "x" ? on_x_edge : on_y_edge) &&
               "address_map peripheral: face names an edge this coordinate is not on");
        for (const auto& q : peripherals) {
            assert(!(q.x == x && q.y == y && q.port == (face == "x" ? 1 : 2)) &&
                   "address_map: two peripherals share the same (x, y, face)");
        }
        peripherals.push_back({x, y, static_cast<uint8_t>(face == "x" ? 1 : 2),
                               p["size"].as<uint64_t>()});
    }
    SamTable table = SamTable::packed(tiles, x_dim, y_dim, block_size, peripherals);
```

In `SamTable::packed`, after the tile loop has assigned every tile base, place the peripheral
regions in DECLARATION ORDER above the tile array, each aligned to its own size. They are not
coordinate-derived, because a peripheral region has no uniform stride and is not collective-eligible:

```cpp
    // packed() derives a tile base as ((y << x_bits) | x) * block_size with
    // x_bits = clog2(x_dim) (addr_trans.hpp:78,96-97). Mesh dimensions are
    // powers of two -- Task 4 Step 2 asserts it -- so 1 << x_bits == x_dim, the
    // top tile index is exactly x_dim * y_dim - 1, and this is the address just
    // above the whole array.
    uint64_t next = static_cast<uint64_t>(x_dim) * y_dim * block_size;
    for (const auto& p : peripherals) {
        next = (next + p.size - 1) & ~(p.size - 1);  // align up to its own size
        es.push_back({next, p.size, static_cast<uint8_t>((p.y << ni::width::X_WIDTH) | p.x),
                      axi::AxiClass::Data, p.port, axi::Space::Peripheral});
        next += p.size;
    }
```

The align-up expression assumes a non-zero power-of-two size. Assert BOTH — `p.size != 0` and
`(p.size & (p.size - 1)) == 0` — because `0 & (0 - 1)` is 0 and passes a power-of-two test on its
own, then aligns to base 0 and overlaps the whole tile array.

- [ ] **Step 4: Teach the Python packer the third space**

`sim/tools/address_map.py:30` — `SPACE_ORDER = ("config", "memory")` gains `"peripheral"`.

**`pack()` must emit peripheral entries, not just order them.** `pack(address_map, ...)` reads
`address_map["tiles"]` and nothing else (`address_map.py:91`). Widening `SPACE_ORDER` alone changes
how `node_windows` sorts what it is given; it does not put peripherals into `entries`. Give `pack()`
the same declaration-ordered placement the C++ loader does, above the tile array, aligned to each
region's own size.

**This is not cosmetic — `noc_egress_base` collides without it.** `noc_egress_base(entries)`
(`address_map.py:181-201`) is "the first power of two at or above the top of the map", and its
docstring promises the aperture "cannot collide with a real region however the map grows". That
promise holds only if `entries` contains every region. With peripherals missing, `top` is the top of
the TILE array, the aperture lands below the peripheral regions, and `NOC_EGRESS_BASE` — stamped
into the SV at `gen_tb_top.py:1113` and used by `user_node_endpoint.sv` to offset collective writes
toward the NI — points inside a peripheral's window. A collective write would be decoded as a
peripheral access instead of reaching the NI.

The two packers have twin formulas and have drifted before. Make the peripheral arithmetic
identical, not merely equivalent, and assert in the C++ loader that its own top-of-map matches what
`noc_egress_base` would compute, so a future drift fails loudly rather than aliasing.

- [ ] **Step 5: Run the C++ tests only**

```bash
ctest --test-dir $HOME/noc_build/cmodel --output-on-failure
```

**Do NOT run `pytest sim/tools` here, and do not try to make it green.** This task rewrites the
topology and adds `"peripheral"` to `SPACE_ORDER`, but the readers do not learn about either until
Task 5: `gen_tb_top._peripherals` still derives peripherals from off-region tiles, and
`node_windows` still keys on the coordinate alone, so a router endpoint would now pick up its
peripheral's window as a third target. `pytest sim/tools` is Task 5's gate, and it is red in between
by construction. Say so in your report rather than papering over it.

- [ ] **Step 6: Commit**

```bash
clang-format -i ref_model/c_model/include/nmu/sam_yaml.hpp
git add ref_model/c_model sim/tools/address_map.py sim/topologies/
git commit -m "feat(sam): read a peripherals block and place its regions above the tile array"
```

---

### Task 3: route_compute resolves the port

**Files:**
- Modify: `ref_model/c_model/include/router/router.hpp:65-76` (`route_compute`) and its call sites
- Modify: `ref_model/c_model/include/router/simple_router.hpp` (its own `route_compute` call sites)
- Test: `ref_model/c_model/tests/router/test_router_front_route.cpp`, `test_router.cpp`

**Interfaces:**
- Consumes: `dst_port_id` on the flit header (round 2), `RouterConfig::mesh_x_dim` / `mesh_y_dim`.
- Produces: `route_compute(uint8_t dst_id, uint8_t dst_port_id, const RouterConfig& cfg)`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(RouteCompute, PortZeroEjectsLocalAtTheDestinationCoordinate) {
    RouterConfig cfg{};
    cfg.x = 1;
    cfg.y = 1;
    cfg.mesh_x_dim = 2;
    cfg.mesh_y_dim = 2;
    EXPECT_EQ(route_compute(/*dst_id=*/0x11, /*dst_port_id=*/0, cfg), RouterPort::LOCAL);
}

TEST(RouteCompute, XFaceResolvesByTheRoutersOwnEdge) {
    RouterConfig cfg{};
    cfg.mesh_x_dim = 2;
    cfg.mesh_y_dim = 2;
    cfg.x = 0;
    cfg.y = 0;
    EXPECT_EQ(route_compute(0x00, /*dst_port_id=*/1, cfg), RouterPort::WEST);
    cfg.x = 1;
    EXPECT_EQ(route_compute(0x01, /*dst_port_id=*/1, cfg), RouterPort::EAST);
}

TEST(RouteCompute, YFaceResolvesByTheRoutersOwnEdge) {
    RouterConfig cfg{};
    cfg.mesh_x_dim = 2;
    cfg.mesh_y_dim = 2;
    cfg.x = 0;
    cfg.y = 0;
    EXPECT_EQ(route_compute(0x00, /*dst_port_id=*/2, cfg), RouterPort::SOUTH);
    cfg.y = 1;
    EXPECT_EQ(route_compute(0x10, /*dst_port_id=*/2, cfg), RouterPort::NORTH);
}

TEST(RouteComputeDeath, AnInteriorRouterHasNoFace) {
    RouterConfig cfg{};
    cfg.x = 1;
    cfg.y = 1;
    cfg.mesh_x_dim = 4;
    cfg.mesh_y_dim = 4;
    EXPECT_DEATH(route_compute(0x11, /*dst_port_id=*/1, cfg), "no x face");
}

TEST(RouteComputeDeath, TheReservedPortEncodingAborts) {
    RouterConfig cfg{};
    cfg.x = 0;
    cfg.y = 0;
    cfg.mesh_x_dim = 2;
    cfg.mesh_y_dim = 2;
    EXPECT_DEATH(route_compute(0x00, /*dst_port_id=*/3, cfg), "reserved");
}
```

- [ ] **Step 2: Run them and watch them fail**

```bash
ctest --test-dir $HOME/noc_build/cmodel -R "RouteCompute" --output-on-failure
```

Expect a compile error: `route_compute` takes two arguments.

- [ ] **Step 3: Resolve the port**

Only the tail changes. The X-then-Y walk above it is untouched — a peripheral is reached by routing
to its router first, exactly like a tile, and the port only decides the ejection.

```cpp
    // Arrived at the destination coordinate. Which endpoint here receives is
    // the header's, not the coordinate's: port 0 is the tile on LOCAL, and a
    // non-zero port names a boundary-port peripheral. The face follows from
    // which edge this router sits on, so the field states the axis only --
    // minimum mesh dimension is 2, so one router cannot be at both x == 0 and
    // x == mesh_x_dim - 1.
    switch (dst_port_id) {
        case 0:
            return RouterPort::LOCAL;
        case 1:
            if (cfg.x == 0) return RouterPort::WEST;
            if (cfg.x == cfg.mesh_x_dim - 1) return RouterPort::EAST;
            assert(false && "route_compute: dst_port_id names an x face, this router has no x face");
            std::abort();
        case 2:
            if (cfg.y == 0) return RouterPort::SOUTH;
            if (cfg.y == cfg.mesh_y_dim - 1) return RouterPort::NORTH;
            assert(false && "route_compute: dst_port_id names a y face, this router has no y face");
            std::abort();
        default:
            assert(false && "route_compute: dst_port_id 3 is the reserved encoding");
            std::abort();
    }
```

- [ ] **Step 4: Feed it at every call site**

Most callers read `dst_port_id` off the same flit they already read `dst_id` from. Find them with
`grep -rn "route_compute(" ref_model/`.

**Two callers have no flit and must not be given a literal 0.** `Router::next_hop_route(out, dst)`
(`router.hpp:216`) and `Router::preferred_out_vc(out, dst)` (`:238`) take a bare `dst` — the flit is
at `:257`. They feed the VC lookahead, and `router.hpp:213-215` states that lookahead is
bit-identical to the RTL's stored `hdr.lookahead`. Leave them at 0 and a peripheral-bound flit's
next hop resolves LOCAL instead of WEST/EAST, the preferred VC comes out wrong, and the co-sim
compare fails — not a performance wobble, a mismatch.

Both gain a `dst_port` parameter beside `dst`, threaded from the same flit at `:257`:

```cpp
    RouterPort next_hop_route(std::size_t out, uint8_t dst, uint8_t dst_port) const {
        ...
        return route_compute(dst, dst_port, n);
    }

    uint8_t preferred_out_vc(std::size_t out, uint8_t dst, uint8_t dst_port) const {
        const auto o = static_cast<RouterPort>(out);
        if (o == RouterPort::LOCAL) return 0;  // floo_vc_assignment.sv:86
        return preferred_vc(o, next_hop_route(out, dst, dst_port), cfg_.num_vc);
    }
```

A pure routing unit test that genuinely has no destination port passes 0 explicitly, with a comment
saying it means the tile.

- [ ] **Step 5: Run the tests, then fault-inject**

```bash
ctest --test-dir $HOME/noc_build/cmodel --output-on-failure
```

Then prove the two new aborts fire for the right reason: temporarily make `case 1` return
`RouterPort::LOCAL` unconditionally and confirm `AnInteriorRouterHasNoFace` stops dying while the
positive face cases fail. Revert, rebuild green, report the injection output.

- [ ] **Step 6: Commit**

```bash
clang-format -i ref_model/c_model/include/router/*.hpp ref_model/c_model/tests/router/*.cpp
git add ref_model/c_model
git commit -m "feat(router): resolve the ejection port from dst_port_id"
```

---

### Task 4: The deletions and the re-bounded clip

Read "The four-copy landmine" above before starting. This task's risk is entirely in doing three of
the four.

**Files:**
- Modify: `ref_model/c_model/include/router/route_mask.hpp:113-121,175-184`
- Modify: `ref_model/c_model/include/nmu/addr_trans.hpp` (`collective_translate`'s clip; delete `check_dst_reachable` at `:487`)
- Modify: `ref_model/c_model/include/nmu/packetize.hpp:181,272` (the two `check_dst_reachable` call sites)
- Modify: `sim/tools/gen_test_patterns.py` (`collective_addr_mask`'s clip)
- Modify: `ref_model/c_model/include/router/router_types.hpp:25-28`, `router/simple_router.hpp:100,323,380,465`, `wrap/router_wrap.hpp:66,75,92`, `ref_model/dpi/cmodel_dpi.h:87`, `ref_model/dpi/cmodel_dpi.cpp:185,198`, `ref_model/c_model/include/ni/address_map.hpp` (`SpaceCoords`)
- Modify: `ref_model/c_model/include/nmu/sam_yaml.hpp:163-210` (the six span keys)
- Modify: `sim/tools/gen_tb_top.py:68` (`_route_span`) and `:1236-1242,1310` (the DPI create signature and calls)
- Modify: `sim/tools/gen_dma_jobs.py:139` — the OTHER `_route_span` caller. Round 3 verifies
  `sim/tb/test/` only, which is about which co-sim runs, not about leaving a dead import behind:
  deleting `_route_span` breaks this file, and `test_gen_dma_jobs.py` will say so.
- Modify: `ref_model/c_model/include/nmu/rob.hpp:372` and its constructor (the issuer gate, Step 1)
- Modify: `sim/topologies/*.yaml` — remove `x_span`, `y_span`, `tile_*` wherever they appear
- Modify: the docs that describe the deleted concepts — `docs/noc-target-spec.md:53-56,376-389`
  (off-grid peripheral semantics and the row restriction) and `docs/router-spec.md:99-100`
  (both port fields called "Transparent to the router", which Task 3 falsifies), `:440-441`
  (`mesh_*_dim` documented as the span, plus the tile-region bounds), `:523` (the 10-argument
  `cmodel_router_create`)

**Interfaces:**
- Produces: `route_compute`, `route_mask_fork`, `route_mask_join` and `RouterConfig` with no
  tile-region concept; `cmodel_router_create(name, x, y, mesh_x_dim, mesh_y_dim, num_vc)` — four
  fewer arguments.

- [ ] **Step 1: Replace the issuer guard BEFORE deleting what it reads**

Read "The guard that disappears" above first. `collective_translate` gains the issuer's port and
gates on it instead of on the coordinate:

```cpp
inline uint8_t collective_translate(const SamTable& sam, const axi::AwBeat& b, uint8_t src_id,
                                    uint8_t src_port) {
```

and the coordinate test at `:373-382` becomes:

```cpp
    // A peripheral hangs off a boundary port, not off a router's LOCAL port, so
    // no router sits where its fork would have to spread or its join collect.
    // Round 2's port field is what says so -- the coordinate no longer can,
    // because a peripheral now shares its router's.
    if (src_port != 0) {
        assert(false &&
               "nmu::addr_trans::collective_translate: a collective issued from a peripheral -- "
               "the fork spreads along the issuer's row and the join collects in its column, and "
               "a boundary port sits outside both");
        std::abort();
    }
```

The single call site is `rob.hpp:372`. `Rob` gains a `uint8_t port_id_` from `NmuConfig::port_id`
(round 2 added it) as a TRAILING defaulted constructor parameter, matching how round 2 threaded
`port_id` into `nmu::Packetize` and `nmu::Depacketize`, so no existing `Rob` fixture changes.

Write the death test first:

```cpp
TEST(CollectiveTranslateDeath, APeripheralCannotIssueACollective) {
    // Round 2 gave every endpoint a port. A peripheral's is non-zero, and that
    // is now the only thing that distinguishes it -- it shares its router's
    // coordinate, so the old outside-the-tile-region test would pass.
    auto sam = load_sam_table(std::string(TOPOLOGY_DIR) + "/mesh_2x2_vc1_periph.yaml");
    axi::AwBeat b{};
    b.addr = 0x0;
    b.user = axi::make_awuser_collective(axi::COLLECTIVE_OP_MULTICAST, /*mask=*/0x100000000ull);
    EXPECT_DEATH(collective_translate(sam, b, /*src_id=*/0x00, /*src_port=*/1), "peripheral");
}
```

Check the file's existing collective fixtures for how they build a collective AWUSER before writing
this — the helper name above is illustrative, the assertion is exact.

- [ ] **Step 2: Make the power-of-two rule a check, with a test**

The clip deletion in Step 3 rests on mesh dimensions being powers of two. Nothing enforces that
today, so enforce it first — in the YAML loader, beside the existing `x_dim >= 2` assert
(`sam_yaml.hpp:159`), which is the config trust boundary:

```cpp
    assert((x_dim & (x_dim - 1)) == 0 && (y_dim & (y_dim - 1)) == 0 &&
           "topology: mesh dimensions must be powers of two -- the collective coordinate field is "
           "clog2(dim) bits wide, so a non-power-of-two dimension leaves a wildcard address naming "
           "a coordinate with no router");
```

and its death test:

```cpp
TEST(SamYamlDeath, ANonPowerOfTwoMeshDimensionIsRejected) {
    // 3x3 gives a 2-bit x field spanning four coordinates while only three
    // exist, so a collective wildcard would name a node with no router.
    // Refused at load, while the topology is still a document and not a mesh.
    EXPECT_DEATH(load_sam_table(kThreeByThreeTopology), "powers of two");
}
```

Read how the neighbouring `SamYamlDeath` cases stand up a rejected topology before writing this —
some write a temp file, some point at a fixture. The assertion is exact; the fixture is by
reference.

- [ ] **Step 3: Delete all four clips**

Read "The clip is deleted, not re-bounded" above. In each of the four sites the clamp lines go
entirely, and so does the empty-set assert that follows them — with nothing clipped, the set cannot
become empty by clipping.

Delete the "Change one, change all four" cross-reference comment in `route_mask.hpp:104-112` as
well. It describes a coupling that no longer exists, and leaving it sends the next reader hunting
for three siblings that are not there.

`detail::in_mesh(dst, cfg)` stays. It catches a malformed `dst_id`, which is a different claim from
clipping a wildcard, and Task 3's `route_compute` relies on it.

Update the "Change one, change all four" comment in `route_mask.hpp` — the list of four is still
right, but its description of what is clipped is not.

- [ ] **Step 4: Delete check_dst_reachable**

Delete the function (`addr_trans.hpp:487`) and its two call sites (`packetize.hpp:191,286`).

Then fix the docs that state the restriction as spec — a spec asserting a constraint the
implementation does not have is worse than one that omits it. `docs/noc-target-spec.md:53-56` is a
sentence spanning four lines: delete the whole sentence, not line `:55` alone, or `:56` is left a
dangling fragment. `:376-389` describes off-grid peripheral addressing and needs rewriting to the
(coordinate, port) scheme. In `docs/router-spec.md`, `:99-100` says both port fields are
"Transparent to the router" — `dst_port_id` no longer is, since Task 3 makes it select the ejection
port; `:440-441` documents `mesh_*_dim` as the route span and lists the tile-region bounds; `:523`
carries the 10-argument `cmodel_router_create`.

- [ ] **Step 5: Delete the four bounds everywhere**

`RouterConfig` (`router_types.hpp:25-28`), `SimpleRouterConfig` and its users, `RouterWrap`, the DPI
signature and `SpaceCoords`. The DPI change reaches the generated SystemVerilog, so
`gen_tb_top.py`'s `import "DPI-C"` declaration and its `cmodel_router_create` call move with it —
argument for argument, in the same commit. A mismatch there fails at co-sim elaboration, not at
compile time.

`sam_yaml.hpp:163-210` stops reading the six span keys; `x_span` becomes `x_dim` throughout.
`gen_tb_top._route_span` is deleted and its callers use `x_dim` / `y_dim` directly.

Remove the keys from every `sim/topologies/*.yaml` that carries them.

- [ ] **Step 6: Run everything**

```bash
ctest --test-dir $HOME/noc_build/cmodel --output-on-failure
```

Then fault-inject the new issuer gate: change `src_port != 0` to `false` and confirm
`CollectiveTranslateDeath.APeripheralCannotIssueACollective` stops dying. Revert, rebuild green,
report the output. `pytest sim/tools` stays red until Task 5, as Task 2 recorded.

- [ ] **Step 7: Commit**

```bash
clang-format -i ref_model/c_model/include/router/*.hpp ref_model/c_model/include/nmu/*.hpp ref_model/c_model/include/ni/address_map.hpp
git add -A
git commit -m "refactor(router): drop the tile-region bounds and clip collectives to the mesh"
```

---

### Task 5: The generator puts peripherals on the grid

**Files:**
- Modify: `sim/tools/gen_tb_top.py:184-234` (`_peripherals`), `:236-244` (`_endpoints`), `:308-330` (per-endpoint windows), `:704-730` (the peripheral link wiring), the perf-monitor names
- Modify: `sim/tools/address_map.py:157-178` (`node_windows`)
- Modify: `ref_model/c_model/include/wrap/nsu_wrap.hpp:80-90` (the rebase gate)
- Test: `sim/tools/test_gen_test_patterns_filemaster.py`

**Interfaces:**
- Consumes: the `peripherals:` block (Task 2), `port_id` through the DPI (round 2).
- Produces: `node_windows(entries, dst_id, port)`; peripheral endpoints created with a non-zero
  `port_id`; perf-monitor endpoint names `node<idx>.local` / `node<idx>.<face>`.

- [ ] **Step 1: Key the windows on the port, and pad the rows**

`node_windows(entries, node_id)` becomes `node_windows(entries, node_id, port)`. It currently
matches on `dst_id` alone, so under scheme B a peripheral's window would be stamped into its
router's crossbar decode as that tile's. A router endpoint takes port 0's windows — config and
memory, exactly as today; a peripheral endpoint takes its own single window.

**The emitted array is rectangular and a 1-window endpoint does not fit it.** `gen_tb_top.py:1007`
takes `n_targets = len(per_node[0])` — one global count, from endpoint 0 — and `:1106-1109` emits
`TILE_BASE_ADDR` / `TILE_SIZE` as `logic [n_ep-1:0][TILE_TARGETS-1:0][ADDR_WIDTH-1:0]`. A peripheral
row of length 1 beside tile rows of length 2 is ragged and will not elaborate. This is the one place
the plan's "unchanged in shape" would have been false: the shape IS the problem.

Pad instead of reshaping. `TILE_TARGETS` becomes `max(len(w) for w in per_node.values())`, and every
short row is padded to that length with `{"space": None, "base": 0, "size": 0}`. A zero-size window
matches nothing in the pulp `axi_xbar` rule, which is start/end — so the padding is inert decode,
not a live target, and the SV genvar loop and `user_node_endpoint` need no change at all.

Say in a comment beside the padding why a zero size is safe, so the next reader does not "fix" it
into a one-byte window.

- [ ] **Step 2: Loosen the window-order cross-check**

`gen_tb_top.py:331-339` raises `SystemExit` unless an endpoint's window order is exactly
`["config", "memory"]`. A peripheral's is `["peripheral"]`, so the check rejects the topology before
anything else runs. It is a real cross-check on `address_map.SPACE_ORDER` and worth keeping — widen
it rather than deleting it:

```python
        if port == 0:
            expected = ["config", "memory"]
        else:
            expected = ["peripheral"]
        if order != expected:
            raise SystemExit(
                f"gen_tb_top: endpoint {idx} (port {port}) window order {order} must be "
                f"{expected} -- user_node_endpoint puts the config memory on target 0 and the "
                f"data memory on the last target (see address_map.SPACE_ORDER)")
```

Compare the order BEFORE padding, or every row reads as ragged-then-padded and the check says
nothing.

- [ ] **Step 3: Read peripherals from the block, not from the span**

`_peripherals` currently derives peripherals by finding coordinates outside the tile region. It now
reads `address_map.peripherals` directly, and each entry keys on `(x, y, face)`. Delete the
corner-coordinate rejection (`:213-216`) — a peripheral on a corner router is now legal, because the
face names which port it hangs off and a corner router has two free faces.

Keep the duplicate rejection at `:227`, re-keyed from (router, direction) to (x, y, face).

- [ ] **Step 4: Gate the NSU's rebase on the port**

`nsu_wrap.hpp:80-90` populates `NsuConfig::space_coords` from a global per-space SAM lookup, so
every endpoint receives the same coordinates. A peripheral's NSU would be handed the memory space's
field and `rebase_` (`nsu/depacketize.hpp:181-184`) would rewrite those bits in an address that has
no coordinate field, corrupting it.

The gate is the `port_id` this NI already carries: an endpoint with `port_id != 0` leaves
`space_coords` undeclared, and `rebase_node_coords` returns the address unchanged
(`ni/address_map.hpp:69`) — correct, because a peripheral is never a collective member and its
address needs no rebasing.

- [ ] **Step 5: Name the endpoints**

The perf monitor names an endpoint `node<idx>.local` or `node<idx>.<face>`. Two peripherals on one
router now share a coordinate, so an index-only name is ambiguous.

- [ ] **Step 6: Run the generator tests**

```bash
python3 -m pytest sim/tools -q
```

This is where `pytest sim/tools` goes green again. Task 2 left it red on purpose — it rewrote the
topology and `SPACE_ORDER` before the readers here learned about either. If it is still red after
this task, that is a finding, not a leftover.

- [ ] **Step 7: Commit**

```bash
clang-format -i ref_model/c_model/include/wrap/nsu_wrap.hpp
git add ref_model/c_model sim/tools
git commit -m "feat(tb): place peripherals on their router's coordinate and key windows on the port"
```

---

### Task 6: Four-face topology and patterns

**Files:**
- Create: `sim/topologies/mesh_4x4_vc1_periph4.yaml`
- Modify: `sim/tools/gen_test_patterns.py:876-930` (`peripheral_reaches`, `peripheral_partner`, `peripheral_hotspot`), `:1025-1041` (the tile-offset arithmetic)
- Test: `sim/tools/test_gen_test_patterns_filemaster.py`

**Interfaces:**
- Consumes: everything above.
- Produces: a topology with a peripheral on each of the four faces; patterns with no legality filter.

- [ ] **Step 1: Write the four-face topology**

`sim/topologies/mesh_4x4_vc1_periph4.yaml`. This is the topology the round's acceptance bar names,
so every face is populated and no two peripherals share a router. Copy `mesh_4x4_vc1.yaml`'s tile
block verbatim — sixteen memory tiles and sixteen config tiles — and add:

```yaml
  # One peripheral per face. A face peripheral shares its router's coordinate
  # and is told apart by the port it hangs off: west/east are port 1, south/north
  # port 2, and which of the pair a router means follows from its own edge.
  peripherals:
    - { x: 0, y: 1, face: x, size: 0x1000 }   # west
    - { x: 3, y: 2, face: x, size: 0x1000 }   # east
    - { x: 1, y: 0, face: y, size: 0x1000 }   # south
    - { x: 2, y: 3, face: y, size: 0x1000 }   # north
```

The four routers are distinct, so no router carries two peripherals — that case is legal under this
design but is not what this topology is for, and mixing it in would blur which property a failure
came from.

- [ ] **Step 2: Delete the legality filters**

`peripheral_reaches` (`:876`) encodes the row restriction Task 4 deleted: a peripheral was reachable
only from its own row. Every tile now reaches every peripheral, so the function goes, and with it
`peripheral_partner`'s legality filter and `peripheral_hotspot`'s single-target rule — both exist
only to respect it.

`:1025-1041`'s `tile_x_first + x` offset arithmetic goes too: a tile's coordinate is now its own.

- [ ] **Step 3: Run the generator tests**

```bash
python3 -m pytest sim/tools -q
```

Update the fixtures that pin the old peripheral addresses. They are generator-output fixtures, not
behavioural assertions — but read each one before changing it, and say in your report which you
changed and why.

- [ ] **Step 4: Commit**

```bash
git add sim/tools sim/topologies
git commit -m "feat(patterns): every tile reaches every peripheral on all four faces"
```

---

### Task 7: The co-sim gate

**Files:** none. This task runs the simulator and reports.

- [ ] **Step 1: Pre-clean and run the regression set**

```bash
rm -f sim/filelist_*.f sim/tb/test/tb_top_*.sv sim/tb/soc/tb_top_dma_*.sv
rm -rf $BUILD_ROOT/verilator/obj_dir_*
make -C sim TB=mesh_2x2_vc1 PATTERN=neighbor
make -C sim TB=mesh_4x4_vc1 PATTERN=neighbor
make -C sim TB=mesh_4x4_vc4 PATTERN=transpose
make -C sim TB=mesh_4x4_vc1 PATTERN=multicast MCAST_SHAPE=row
make -C sim TB=mesh_4x4_vc1 PATTERN=multicast MCAST_SHAPE=col
```

Note `MCAST_SHAPE`, not `MULTICAST_SHAPE` — the wrong name is silently ignored and re-runs the
default shape under the other shape's tag.

- [ ] **Step 2: Run the peripheral topologies**

```bash
make -C sim TB=mesh_2x2_vc1_periph PATTERN=neighbor
make -C sim TB=mesh_4x4_vc1_periph4 PATTERN=neighbor
make -C sim TB=mesh_4x4_vc1_periph4 PATTERN=peripheral_partner
```

The four-face run is the round's acceptance bar. A failure here is the finding this whole campaign
was built to surface — root-cause it and report, do not narrow the run to make it pass.

- [ ] **Step 3: Assert collective eligibility directly**

The failure mode is silent, so it is asserted rather than inferred from a passing multicast run:

```bash
ctest --test-dir $HOME/noc_build/cmodel -R "SamYaml" --output-on-failure
```

`PeripheralSpaceIsNotACollectiveTarget` must pass on the four-face topology too — add the row if it
is table-driven.

Then fault-inject the loader policy: declare the peripheral space collective-eligible in
`sam_yaml::declare_space_coords` and confirm the negative assertion fails. Revert, rebuild green,
report the output.

- [ ] **Step 4: Record the results**

Write a pass/fail line per run into the task report.

---

## Round acceptance

- full `ctest` green
- `python3 -m pytest specgen/tests sim/tools -q` green
- `codegen.py --check` exits 0
- every co-sim run in Task 7 green, including the four-face topology
- `SamYaml.PeripheralSpaceIsNotACollectiveTarget` passes on every shipped peripheral topology, and
  fails when the loader is made to declare the peripheral space eligible
- `grep -rn "tile_x_first\|x_span" ref_model/ sim/` returns nothing. `docs/` is deliberately out of
  scope: this plan and the spec both quote the old keys as the subject of the change, the same way
  round 2's plan kept its pre-round flit widths
