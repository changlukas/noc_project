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
- A peripheral is never a collective member and never a collective target.
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

## The four-copy landmine

`route_mask.hpp:108-112` states it in its own comment: the tile-region clip exists in **four**
places that must agree node by node, because the join is stateless and a one-node disagreement
hangs a collect.

| # | site |
|---|---|
| 1 | `router/route_mask.hpp` `route_mask_fork` (`:113-121`) |
| 2 | `router/route_mask.hpp` `route_mask_join` (`:175-184`) |
| 3 | `nmu/addr_trans.hpp` `collective_translate` |
| 4 | `sim/tools/gen_test_patterns.py` `collective_addr_mask` |

Task 4 re-bounds all four from the tile region to `0 .. mesh_*_dim - 1`. Changing three of them is
worse than changing none: three-of-four passes ctest and hangs a co-sim collect.

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

The class currently stands in for the space. `addr_trans.hpp:257` states the assumption in a
comment: `// Indexed by axi::AxiClass (Narrow = 0, Data = 1) -- one address space each.` A
peripheral is a third space carrying the Data class, so the correspondence ends and five sites stop
being correct. One of them fails silently and expensively: `declare_space_coords`' tile walk returns
false and **the memory space stops being a collective target**, with no error.

**Files:**
- Modify: `ref_model/c_model/include/axi/types.hpp:72` (add `Space` beside `AxiClass`)
- Modify: `ref_model/c_model/include/nmu/addr_trans.hpp:16-48` (`Translated`, `SamEntry`, `PackedTile`), `:154-188` (`validate`), `:202-266` (`declare_space_coords`, `collective_coords`), `:329-360` (`collective_translate`)
- Modify: `ref_model/c_model/include/nmu/sam_yaml.hpp:15-23` (`parse_tile_space`), `:36-69` (`declare_space_coords`), `:190-210` (the loader tail)
- Test: `ref_model/c_model/tests/nmu/test_sam_table.cpp`, `test_sam_yaml.cpp`, `test_addr_trans.cpp`

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
    uint64_t next = static_cast<uint64_t>(x_dim) * y_dim * block_size;
    for (const auto& p : peripherals) {
        next = (next + p.size - 1) & ~(p.size - 1);  // align up to its own size
        es.push_back({next, p.size, static_cast<uint8_t>((p.y << ni::width::X_WIDTH) | p.x),
                      axi::AxiClass::Data, p.port, axi::Space::Peripheral});
        next += p.size;
    }
```

The align-up expression assumes a power-of-two size; assert that (`(p.size & (p.size - 1)) == 0`)
rather than letting a non-power-of-two size compute a silently wrong base.

- [ ] **Step 4: Teach the Python packer the third space**

`sim/tools/address_map.py:30` — `SPACE_ORDER = ("config", "memory")` gains `"peripheral"`. Peripheral
regions are packed above the tile array in declaration order, matching the C++ loader exactly. The
two packers have twin formulas and have drifted before; make the peripheral arithmetic identical, not
merely equivalent.

- [ ] **Step 5: Run the tests**

```bash
ctest --test-dir $HOME/noc_build/cmodel --output-on-failure
python3 -m pytest sim/tools -q
```

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

Every caller reads `dst_port_id` off the same flit it already reads `dst_id` from. Find them with
`grep -rn "route_compute(" ref_model/` and change all of them — `router.hpp`, `simple_router.hpp`
and their test fixtures. A caller that has no flit (a pure routing unit test) passes 0 explicitly.

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
- Modify: `sim/topologies/*.yaml` — remove `x_span`, `y_span`, `tile_*` wherever they appear

**Interfaces:**
- Produces: `route_compute`, `route_mask_fork`, `route_mask_join` and `RouterConfig` with no
  tile-region concept; `cmodel_router_create(name, x, y, mesh_x_dim, mesh_y_dim, num_vc)` — four
  fewer arguments.

- [ ] **Step 1: Write the failing test**

The clip survives for a different reason than the one being deleted, and that reason needs a test of
its own — otherwise the next reader deletes it too:

```cpp
TEST(RouteMaskFork, ClipsAWildcardToTheMeshWhenADimensionIsNotAPowerOfTwo) {
    // A wildcard mask can name a coordinate that does not exist when a mesh
    // dimension is not a power of two: a 3-wide mesh has a 2-bit x field, so
    // the mask spans x = 0..3 and x = 3 has no router. The clip is now to the
    // mesh's own dimensions, which RouterConfig already carries -- not to a
    // configurable tile region.
    RouterConfig cfg{};
    cfg.x = 0;
    cfg.y = 0;
    cfg.mesh_x_dim = 3;
    cfg.mesh_y_dim = 1;
    // ... build a head flit whose collective_mask wildcards the whole x field,
    //     matching the fixture shape in the neighbouring fork tests ...
    const auto branches = route_mask_fork(f, cfg);
    // x = 3 must not appear in the branch set.
}
```

Read `ref_model/c_model/tests/router/test_router_fork.cpp` for the fixture shape before writing
this; the assertion above is exact, the setup is by reference.

- [ ] **Step 2: Re-bound all four copies**

In each of the four sites named in "The four-copy landmine", the clip changes from the tile region to
the mesh:

```cpp
    dst_min.x = std::max<uint8_t>(dst_min.x, 0);
    dst_min.y = std::max<uint8_t>(dst_min.y, 0);
    dst_max.x = std::min<uint8_t>(dst_max.x, cfg.mesh_x_dim - 1);
    dst_max.y = std::min<uint8_t>(dst_max.y, cfg.mesh_y_dim - 1);
```

The two `std::max` lines against 0 are no-ops on an unsigned type — write the clip as the two
`std::min` lines only, and update each site's comment to say the bound is the mesh, not the tile
region. Keep the empty-set assert; it still fires when a mask names nothing that exists.

Update the "Change one, change all four" comment in `route_mask.hpp` — the list of four is still
right, but its description of what is clipped is not.

- [ ] **Step 3: Delete check_dst_reachable**

Delete the function (`addr_trans.hpp:487`) and its two call sites (`packetize.hpp:181,272`). Also
delete `docs/noc-target-spec.md:55`, which states the row restriction as spec — the restriction no
longer exists, and a spec that states a constraint the implementation does not have is worse than
one that omits it.

- [ ] **Step 4: Delete the four bounds everywhere**

`RouterConfig` (`router_types.hpp:25-28`), `SimpleRouterConfig` and its users, `RouterWrap`, the DPI
signature and `SpaceCoords`. The DPI change reaches the generated SystemVerilog, so
`gen_tb_top.py`'s `import "DPI-C"` declaration and its `cmodel_router_create` call move with it —
argument for argument, in the same commit. A mismatch there fails at co-sim elaboration, not at
compile time.

`sam_yaml.hpp:163-210` stops reading the six span keys; `x_span` becomes `x_dim` throughout.
`gen_tb_top._route_span` is deleted and its callers use `x_dim` / `y_dim` directly.

Remove the keys from every `sim/topologies/*.yaml` that carries them.

- [ ] **Step 5: Run everything**

```bash
ctest --test-dir $HOME/noc_build/cmodel --output-on-failure
python3 -m pytest sim/tools -q
```

- [ ] **Step 6: Commit**

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

- [ ] **Step 1: Key the windows on the port**

`node_windows(entries, node_id)` becomes `node_windows(entries, node_id, port)`. It currently
matches on `dst_id` alone, so under scheme B a peripheral's window would be stamped into its
router's crossbar decode as that tile's. A router endpoint takes port 0's windows — memory and
config, exactly as today; a peripheral endpoint takes its own single window.

`TILE_BASE_ADDR` and `TILE_SIZE` are stamped per endpoint from that call, unchanged in shape.

- [ ] **Step 2: Read peripherals from the block, not from the span**

`_peripherals` currently derives peripherals by finding coordinates outside the tile region. It now
reads `address_map.peripherals` directly, and each entry keys on `(x, y, face)`. Delete the
corner-coordinate rejection (`:210-215`) — a peripheral on a corner router is now legal, because the
face names which port it hangs off and a corner router has two free faces.

Keep the duplicate rejection at `:226`, re-keyed from (router, direction) to (x, y, face).

- [ ] **Step 3: Gate the NSU's rebase on the port**

`nsu_wrap.hpp:80-90` populates `NsuConfig::space_coords` from a global per-space SAM lookup, so
every endpoint receives the same coordinates. A peripheral's NSU would be handed the memory space's
field and `rebase_` (`nsu/depacketize.hpp:181-184`) would rewrite those bits in an address that has
no coordinate field, corrupting it.

The gate is the `port_id` this NI already carries: an endpoint with `port_id != 0` leaves
`space_coords` undeclared, and `rebase_node_coords` returns the address unchanged
(`ni/address_map.hpp:69`) — correct, because a peripheral is never a collective member and its
address needs no rebasing.

- [ ] **Step 4: Name the endpoints**

The perf monitor names an endpoint `node<idx>.local` or `node<idx>.<face>`. Two peripherals on one
router now share a coordinate, so an index-only name is ambiguous.

- [ ] **Step 5: Run the generator tests**

```bash
python3 -m pytest sim/tools -q
```

- [ ] **Step 6: Commit**

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
- `grep -rn "tile_x_first\|x_span" ref_model/ sim/ docs/` returns nothing
