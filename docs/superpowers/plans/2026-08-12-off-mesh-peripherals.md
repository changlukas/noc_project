# Off-mesh Peripherals Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let an endpoint outside the mesh be addressed by tiles and originate traffic into them.

**Architecture:** A peripheral is an `ni_wrap` plus a `user_node_endpoint` on a boundary router port
that is tied off today, at a coordinate the topology file states. Its address entry lives in the
same space as the tiles at that coordinate's slot. Collectives are clipped to the tile region so a
wildcard mask can never name it.

**Tech Stack:** C++17 + GoogleTest (`ref_model/c_model`), SystemVerilog + Verilator
(`sim/`), Python 3 generators (`sim/tools/`), YAML topology files (`sim/topologies/`).

**Design:** `docs/superpowers/specs/2026-08-12-off-mesh-peripherals-design.md`. Read it before
Task 1. Everything below implements it and nothing below overrides it.

## Global Constraints

- Coordinates are **stated** in the topology file, never derived. No normalisation, no offset.
- The coordinate slot size is **stated** per space. Never `second->base - first->base`.
- `base(x, y) = space_base + ((y << x_bits) | x) * slot_size`, where `x_bits = clog2(route span x)`.
  Never an individual entry's `size`.
- Every entry satisfies `base == base(x, y)` and `size <= slot_size`.
- `tile_min` and `tile_max` are **inclusive** coordinates, matching `dst_min` / `dst_max`.
- `RouterConfig.tile_min` / `tile_max` are elaboration-time parameters, never control registers.
- Existing behaviour is unchanged for every topology with no peripheral. With `tile_min = 0` and
  `span = dim`, every new formula reduces to the current one. **Any task that moves an existing
  topology's addresses or `dst_id` has a bug.**
- `AXI_ID_WIDTH` and the coordinate widths are NOT touched by this plan. Per-topology widths are a
  separate plan; today's fixed `X_WIDTH = 4` holds every span this plan produces.
- Never `--no-verify`. Commit messages are `type(scope): description` in English.
- Build and test on WSL only, foreground, rsync into `~/noc_project` first,
  `BUILD_ROOT=$HOME/noc_build`, `PYTHON3=python3`, never `py -3`.

---

### Task 1: Stated slot size and coordinate-derived bases

**Files:**
- Modify: `sim/tools/address_map.py:55-96`
- Modify: `ref_model/c_model/include/ni/address_map.hpp:43-56` (`SpaceCoords`)
- Test: `sim/tools/test_gen_test_patterns_filemaster.py`
- Test: `ref_model/c_model/tests/nmu/test_sam_yaml.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `address_map.pack(address_map, x_dim, y_dim)` keeps its signature and return shape.
  `SpaceCoords` gains four fields, all `unsigned`: `x_first`, `x_last`, `y_first`, `y_last`,
  inclusive tile-region bounds, defaulting to `0`. Task 2 and Task 3 read them.

- [ ] **Step 1: Write the failing Python test**

Add to `sim/tools/test_gen_test_patterns_filemaster.py`:

```python
def test_pack_bases_are_coordinate_derived_with_a_border_column():
    from address_map import pack
    # tiles at x=1..2, peripherals at x=0, so the route span is 3 and a row
    # strides four slots.
    amap = {"tiles": [
        {"x": 0, "y": 0, "size": 0x100000}, {"x": 1, "y": 0, "size": 0x100000},
        {"x": 2, "y": 0, "size": 0x100000}, {"x": 0, "y": 1, "size": 0x100000},
        {"x": 1, "y": 1, "size": 0x100000}, {"x": 2, "y": 1, "size": 0x100000},
    ]}
    _, entries = pack(amap, 3, 2)
    got = {(e["x"], e["y"]): e["base"] for e in entries}
    assert got == {
        (0, 0): 0x000000, (1, 0): 0x100000, (2, 0): 0x200000,
        (0, 1): 0x400000, (1, 1): 0x500000, (2, 1): 0x600000,
    }


def test_pack_is_unchanged_for_a_plain_mesh():
    from address_map import pack
    amap = {"tiles": [
        {"x": 0, "y": 0, "size": 0x100000}, {"x": 1, "y": 0, "size": 0x100000},
        {"x": 0, "y": 1, "size": 0x100000}, {"x": 1, "y": 1, "size": 0x100000},
    ]}
    _, entries = pack(amap, 2, 2)
    assert [e["base"] for e in entries] == [0x000000, 0x100000, 0x200000, 0x300000]
```

- [ ] **Step 2: Run it and watch the border test fail**

Run: `wsl -e bash -lc 'cd ~/noc_project && python3 -m pytest sim/tools/test_gen_test_patterns_filemaster.py -k pack_bases -v'`
Expected: FAIL. List-order packing gives `(0,1)` a base of `0x300000`, not `0x400000`.

- [ ] **Step 3: Replace the accumulator with the coordinate formula**

In `sim/tools/address_map.py`, replace the `base = 0` accumulator and the
`base += size` tail of the loop. Compute the slot size and span per space first:

```python
def _clog2(n):
    b = 0
    while (1 << b) < n:
        b += 1
    return b


def _slot_size(tiles, space):
    sizes = {int(t["size"]) for t in tiles if t.get("space", "memory") == space}
    if not sizes:
        return 0
    return max(sizes)
```

Inside `pack`, before the loop:

```python
    x_bits = _clog2(x_dim)
    slot = {sp: _slot_size(tiles, sp) for sp in ("memory", "config")}
    # Each space starts above every slot the previous one could occupy, so the
    # coordinate field of one space never reaches into another's.
    space_base = {"memory": 0,
                  "config": (1 << x_bits) * y_dim * slot["memory"]}
```

and in the loop, replace the two `base` statements with:

```python
        sp = space
        base = space_base[sp] + (((y << x_bits) | x) * slot[sp])
        if size > slot[sp]:
            raise ValueError(
                f"address_map tile (x={x},y={y}) size {size:#x} exceeds the {sp} "
                f"slot size {slot[sp]:#x}")
```

Delete `base += size`.

- [ ] **Step 4: Run both tests**

Run: `wsl -e bash -lc 'cd ~/noc_project && python3 -m pytest sim/tools/test_gen_test_patterns_filemaster.py -v'`
Expected: PASS, both the border case and the unchanged plain mesh.

- [ ] **Step 5: Add the tile-region fields to `SpaceCoords`**

In `ref_model/c_model/include/ni/address_map.hpp`, inside `struct SpaceCoords`, after
`y_count`:

```cpp
    // Inclusive tile-region bounds inside the route span. Peripherals occupy
    // coordinates outside them. Default 0 with x_count/y_count as the span
    // reduces every rule below to a plain mesh.
    unsigned x_first = 0;
    unsigned x_last = 0;
    unsigned y_first = 0;
    unsigned y_last = 0;
```

- [ ] **Step 6: Run the full ctest and confirm nothing moved**

Run: `wsl -e bash -lc 'cd ~/noc_project && export BUILD_ROOT=$HOME/noc_build && export PYTHON3=python3 && make test 2>&1 | tail -5'`
Expected: the same pass count as before the task, 653/653 at time of writing.

- [ ] **Step 7: Commit**

```bash
git add sim/tools/address_map.py sim/tools/test_gen_test_patterns_filemaster.py \
        ref_model/c_model/include/ni/address_map.hpp
git commit -m "feat(sam): derive bases from the coordinate and a stated slot size"
```

---

### Task 2: Clip collectives to the tile region

**Files:**
- Modify: `ref_model/c_model/include/router/router_types.hpp:17-25` (`RouterConfig`)
- Modify: `ref_model/c_model/include/router/route_mask.hpp:88-165`
- Test: `ref_model/c_model/tests/router/test_route_mask.cpp`

**Interfaces:**
- Consumes: nothing from Task 1 at compile time. The same tile-region numbers reach here through
  `RouterConfig` rather than `SpaceCoords`.
- Produces: `RouterConfig` gains `tile_x_first`, `tile_x_last`, `tile_y_first`, `tile_y_last`, all
  `uint8_t`. Task 4 sets them from the topology.

- [ ] **Step 1: Write the failing test**

Add to `ref_model/c_model/tests/router/test_route_mask.cpp`:

```cpp
TEST(RouteMaskClip, RowWildcardStopsAtTheTileRegion) {
    // Tiles at x = 1..2, a peripheral coordinate at x = 0. A row wildcard names
    // 0..3 before clipping; only 1..2 exist as tiles.
    router::RouterConfig cfg{};
    cfg.x = 1; cfg.y = 0;
    cfg.mesh_x_dim = 3; cfg.mesh_y_dim = 2;      // route span
    cfg.tile_x_first = 1; cfg.tile_x_last = 2;
    cfg.tile_y_first = 0; cfg.tile_y_last = 1;

    const uint8_t anchor = static_cast<uint8_t>((0u << ni::width::X_WIDTH) | 1u);
    const uint8_t mask = static_cast<uint8_t>((0u << ni::width::X_WIDTH) | 3u);

    // At x = 1 the fork goes EAST toward x = 2 and ejects locally. It must NOT
    // go WEST toward the clipped-out coordinate 0.
    const auto route = router::route_mask_fork(anchor, mask, cfg);
    EXPECT_TRUE(router::port_in_mask(route, router::RouterPort::LOCAL));
    EXPECT_TRUE(router::port_in_mask(route, router::RouterPort::EAST));
    EXPECT_FALSE(router::port_in_mask(route, router::RouterPort::WEST));
}

TEST(RouteMaskClip, JoinExpectsNoInputFromTheClippedSide) {
    router::RouterConfig cfg{};
    cfg.x = 1; cfg.y = 0;
    cfg.mesh_x_dim = 3; cfg.mesh_y_dim = 2;
    cfg.tile_x_first = 1; cfg.tile_x_last = 2;
    cfg.tile_y_first = 0; cfg.tile_y_last = 1;

    const uint8_t collector = static_cast<uint8_t>((0u << ni::width::X_WIDTH) | 1u);
    const uint8_t src = collector;
    const uint8_t mask = static_cast<uint8_t>((0u << ni::width::X_WIDTH) | 3u);

    const auto route = router::route_mask_join(collector, src, mask, cfg);
    EXPECT_FALSE(router::port_in_mask(route, router::RouterPort::WEST));
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `wsl -e bash -lc 'cd ~/noc_project && export BUILD_ROOT=$HOME/noc_build && export PYTHON3=python3 && make build-cmodel && ctest --test-dir $HOME/noc_build/cmodel -R RouteMaskClip -V 2>&1 | tail -20'`
Expected: FAIL. Without the clip the fork sets WEST, because `dst_min.x` is 0.

- [ ] **Step 3: Add the bounds to `RouterConfig`**

In `ref_model/c_model/include/router/router_types.hpp`, after `mesh_y_dim`:

```cpp
    // Inclusive tile-region bounds inside the route span. mesh_*_dim is the
    // span and bounds the range check; these bound collectives. Defaults make
    // a plain mesh, where the two coincide.
    uint8_t tile_x_first = 0;
    uint8_t tile_x_last = static_cast<uint8_t>(NOC_MESH_X_DIM - 1);
    uint8_t tile_y_first = 0;
    uint8_t tile_y_last = static_cast<uint8_t>(NOC_MESH_Y_DIM - 1);
```

- [ ] **Step 4: Clamp in both directions**

In `ref_model/c_model/include/router/route_mask.hpp`, in `route_mask_fork` immediately after
`dst_min` and `dst_max` are formed, and in `route_mask_join` immediately after `src_min` and
`src_max` are formed, insert the same clamp. For the fork:

```cpp
    // Collectives name tiles only. The wildcard block may reach a border
    // coordinate or a coordinate that does not exist; both are clipped here, in
    // the same way on the fork and the join, so every router computes the same
    // member set (spec, "Collectives are clipped to the tile region").
    dst_min.x = std::max<uint8_t>(dst_min.x, cfg.tile_x_first);
    dst_min.y = std::max<uint8_t>(dst_min.y, cfg.tile_y_first);
    dst_max.x = std::min<uint8_t>(dst_max.x, cfg.tile_x_last);
    dst_max.y = std::min<uint8_t>(dst_max.y, cfg.tile_y_last);
    if (dst_min.x > dst_max.x || dst_min.y > dst_max.y) {
        assert(false && "route_mask_fork: collective member set is empty after clipping "
                        "to the tile region -- the source should have refused it");
        std::abort();
    }
```

and the identical four lines plus guard for `src_min` / `src_max` in `route_mask_join`.
Add `#include <algorithm>` at the top of the file if it is not already there.

- [ ] **Step 5: Run the new tests and the whole suite**

Run: `wsl -e bash -lc 'cd ~/noc_project && export BUILD_ROOT=$HOME/noc_build && export PYTHON3=python3 && make test 2>&1 | tail -5'`
Expected: the two new tests pass and the total rises by two with no failures.

- [ ] **Step 6: Commit**

```bash
git add ref_model/c_model/include/router/router_types.hpp \
        ref_model/c_model/include/router/route_mask.hpp \
        ref_model/c_model/tests/router/test_route_mask.cpp
git commit -m "feat(router): clip collective member sets to the tile region"
```

---

### Task 3: Source-side walk and guard

**Files:**
- Modify: `ref_model/c_model/include/nmu/addr_trans.hpp:185-210` (the coordinate walk)
- Modify: `ref_model/c_model/include/nmu/addr_trans.hpp:330-345` (the collective guard)
- Modify: `ref_model/c_model/include/nmu/sam_yaml.hpp:35-60` (`declare_space_coords`)
- Test: `ref_model/c_model/tests/nmu/test_addr_trans.cpp`

**Interfaces:**
- Consumes: `SpaceCoords::x_first/x_last/y_first/y_last` from Task 1.
- Produces: nothing later tasks call directly. Task 5's co-sim depends on the guard accepting a
  full-row mask.

- [ ] **Step 1: Write the failing test**

Add to `ref_model/c_model/tests/nmu/test_addr_trans.cpp`:

```cpp
TEST(CollectiveClip, FullTileRowIsAcceptedWhenTilesDoNotStartAtZero) {
    // Tiles at x = 1..2 with a peripheral at x = 0. The wildcard covering the
    // tile row also names 0 and 3; after clipping it names exactly 1 and 2, so
    // the source must accept it rather than refuse it as out of mesh.
    auto sam = make_sam_with_border_column();   // fixture below
    axi::AwBeat b{};
    b.addr = 0x100000;                          // anchor is the tile at (1, 0)
    b.user = axi::make_awuser_collective(axi::COLLECTIVE_OP_MULTICAST,
                                         /*addr_mask=*/0x300000);
    const uint8_t mask = nmu::addr_trans::collective_translate(sam, b);
    EXPECT_EQ(mask & ((1u << ni::width::X_WIDTH) - 1u), 0x3u);
}
```

Write `make_sam_with_border_column()` beside the test, building the six entries of the design's
worked example through the same loader path the topology files use.

- [ ] **Step 2: Run it and watch it fail**

Run: `wsl -e bash -lc 'cd ~/noc_project && export BUILD_ROOT=$HOME/noc_build && export PYTHON3=python3 && make build-cmodel && ctest --test-dir $HOME/noc_build/cmodel -R CollectiveClip -V 2>&1 | tail -20'`
Expected: FAIL with the abort message "collective destination set names a node outside the mesh",
because `(anchor_x | mask_x)` is 3 and `x_count` is 3.

- [ ] **Step 3: Move the walk to the tile region**

In `addr_trans.hpp`, in the validator that currently reads
`if (space_entries != x_count * y_count) return false;`, replace the count, the origin lookup and
the two loop bounds:

```cpp
        const unsigned tiles_x = c.x_last - c.x_first + 1;
        const unsigned tiles_y = c.y_last - c.y_first + 1;
        if (tile_entries != static_cast<std::size_t>(tiles_x) * tiles_y) return false;
        ...
        for (unsigned y = c.y_first; y <= c.y_last; ++y) {
            for (unsigned x = c.x_first; x <= c.x_last; ++x) {
```

where `tile_entries` counts entries of this class whose coordinate lies inside the tile region.
Replace the origin check `(origin->base & field) != 0` with a comparison against the tile-region
origin's coordinate bits, and take `origin` as the entry at `(c.x_first, c.y_first)` rather than
the first entry of the class. Leave the `e->dst_id == ((y << X_WIDTH) | x)` check untouched: it
holds for every walked tile.

- [ ] **Step 4: Change the collective guard**

Replace the out-of-mesh guard with the clipped-set test:

```cpp
    // Clip to the tile region exactly as route_mask.hpp does, so the source and
    // every router agree on the member set. A set that is empty after clipping
    // names no tile at all and is a stimulus error.
    const unsigned clip_min_x = std::max(anchor_x & ~mask_x, coords->x_first);
    const unsigned clip_max_x = std::min(anchor_x | mask_x, coords->x_last);
    const unsigned clip_min_y = std::max(anchor_y & ~mask_y, coords->y_first);
    const unsigned clip_max_y = std::min(anchor_y | mask_y, coords->y_last);
    if (clip_min_x > clip_max_x || clip_min_y > clip_max_y) {
        assert(false &&
               "nmu::addr_trans::collective_translate: collective destination set is empty "
               "after clipping to the tile region");
        std::abort();
    }
```

- [ ] **Step 5: Teach `declare_space_coords` the stated slot size and the tile region**

In `sam_yaml.hpp`, replace the stride derivation from the first two entries with the space's
declared slot size, and set the new fields:

```cpp
        const unsigned offset = clog2_exact(slot_size);   // stated, not derived
        SpaceCoords c;
        c.x_count = x_span;
        c.y_count = y_span;
        c.x_range = {offset, clog2(x_span)};
        c.y_range = {offset + clog2(x_span), clog2(y_span)};
        c.x_first = tile_x_first; c.x_last = tile_x_last;
        c.y_first = tile_y_first; c.y_last = tile_y_last;
```

- [ ] **Step 6: Run the full suite**

Run: `wsl -e bash -lc 'cd ~/noc_project && export BUILD_ROOT=$HOME/noc_build && export PYTHON3=python3 && make test 2>&1 | tail -5'`
Expected: the new test passes and every existing test still passes. A failure in an existing SAM
test means a plain mesh stopped reducing to the old behaviour, which is a bug in this task.

- [ ] **Step 7: Commit**

```bash
git add ref_model/c_model/include/nmu/addr_trans.hpp \
        ref_model/c_model/include/nmu/sam_yaml.hpp \
        ref_model/c_model/tests/nmu/test_addr_trans.cpp
git commit -m "feat(sam): walk the tile region and clip the collective guard"
```

---

### Task 4: Topology schema, fabric wiring and the peripheral NI

**Files:**
- Create: `sim/topologies/mesh_2x2_vc1_periph.yaml`
- Modify: `sim/tools/gen_tb_top.py` (topology load, fabric emitter, endpoint instantiation)
- Modify: `sim/tools/address_map.py` (accept the new topology keys)
- Test: co-sim elaboration and the existing gates

**Interfaces:**
- Consumes: `pack()` from Task 1, `RouterConfig`'s tile bounds from Task 2.
- Produces: a topology whose `tiles:` list contains entries outside the tile region, and a fabric
  that wires those boundary ports to an `ni_wrap` plus a `user_node_endpoint`.

- [ ] **Step 1: Write the topology**

Create `sim/topologies/mesh_2x2_vc1_periph.yaml`: a 2x2 tile mesh at `x = 1..2`, `y = 0..1`, with a
peripheral at `x = 0` on each row. Route span `x = 3`, `y = 2`. State the span, the tile region and
the slot size explicitly:

```yaml
topology:
  name: mesh_2x2_vc1_periph
  x_dim: 2          # physical router array
  y_dim: 2
  x_span: 3         # route-coordinate span, tiles plus the west border
  y_span: 2
  tile_x_first: 1
  tile_x_last: 2
  tile_y_first: 0
  tile_y_last: 1
  num_vc: 1

address_map:
  memory_slot_size: 0x100000
  config_slot_size: 0x1000
  tiles:
    - { x: 0, y: 0, size: 0x100000 }
    - { x: 1, y: 0, size: 0x100000 }
    - { x: 2, y: 0, size: 0x100000 }
    - { x: 0, y: 1, size: 0x100000 }
    - { x: 1, y: 1, size: 0x100000 }
    - { x: 2, y: 1, size: 0x100000 }
    - { x: 0, y: 0, size: 0x1000, space: config }
    - { x: 1, y: 0, size: 0x1000, space: config }
    - { x: 2, y: 0, size: 0x1000, space: config }
    - { x: 0, y: 1, size: 0x1000, space: config }
    - { x: 1, y: 1, size: 0x1000, space: config }
    - { x: 2, y: 1, size: 0x1000, space: config }
```

- [ ] **Step 2: Generate the tb and watch it fail**

Run: `wsl -e bash -lc 'cd ~/noc_project && python3 sim/tools/gen_tb_top.py --topology mesh_2x2_vc1_periph --out /tmp/tb.sv'`
Expected: FAIL. The generator does not know the new keys and ties off every boundary port.

- [ ] **Step 3: Teach the generator the three quantities**

In `gen_tb_top.py`, keep `X_DIM`/`Y_DIM` as the physical router array for neighbour wiring, add the
span for the router range check and the tile bounds for the collective clip, and pass all of them
to each `router_wrap`. An entry whose coordinate is outside the tile region names one boundary
router and one direction: `x < tile_x_first` is that router's `RP_WEST`, `x > tile_x_last` its
`RP_EAST`, and the same for `y` with `RP_SOUTH` and `RP_NORTH`.

- [ ] **Step 4: Pass the span to the routers, not the array**

Every `router_wrap` gets `mesh_x_dim` / `mesh_y_dim` set to the **span** (3 and 2 here), not to
`X_DIM` / `Y_DIM`. Those fields bound `route_compute`'s range check, and a peripheral at x = 0 is
inside the span but outside the array. Set the four `tile_*` bounds from the topology at the same
time. Neighbour wiring keeps using `X_DIM` / `Y_DIM`.

- [ ] **Step 5: Wire the populated boundary ports**

For each such entry, instantiate an `ni_wrap` and a `user_node_endpoint` exactly as a tile's are
instantiated, binding to `[router_index][RP_<dir>]` instead of `[i][RP_LOCAL]`, with the same
tx/rx crossing. Leave the tie-off and its `$fatal` on every boundary port with no entry.

**The peripheral's `ni_wrap` is constructed with the coordinate its topology entry states**, the
same way a tile's is with its own. That id is stamped into every request the peripheral emits and
is what its responses come back to. A peripheral given a tile's id, or an invented one, routes its
responses to the wrong node and the failure reads as a fabric bug rather than a wiring mistake, so
check this value before running anything.

- [ ] **Step 6: Build and run the new topology**

Run: `wsl -e bash -lc 'cd ~/noc_project && export BUILD_ROOT=$HOME/noc_build && export PYTHON3=python3 && rm -f sim/filelist_*.f sim/tb/tb_top_*.sv && rm -rf $BUILD_ROOT/verilator/obj_dir_* && make -C sim TB=mesh_2x2_vc1_periph PATTERN=neighbor'`
Expected: elaborates and reaches DIRECTED PASS. The peripheral is idle at this point; the run only
proves the fabric still works with a boundary port wired.

- [ ] **Step 7: Re-run the existing gates**

Run: `wsl -e bash -lc 'cd ~/noc_project && export BUILD_ROOT=$HOME/noc_build && export PYTHON3=python3 && make -C sim TB=mesh_2x2_vc1 PATTERN=neighbor && make -C sim TB=mesh_4x4_vc1 PATTERN=neighbor'`
Expected: both DIRECTED PASS, unchanged.

- [ ] **Step 8: Commit**

```bash
git add sim/topologies/mesh_2x2_vc1_periph.yaml sim/tools/gen_tb_top.py sim/tools/address_map.py
git commit -m "feat(sim): wire a peripheral NI to a populated boundary port"
```

---

### Task 5: Prove both directions and the checkers

**Files:**
- Modify: `sim/tools/gen_test_patterns.py` (a pattern that targets and originates from a peripheral)
- Test: co-sim on `mesh_2x2_vc1_periph`

**Interfaces:**
- Consumes: the topology and fabric from Task 4.
- Produces: nothing later tasks use.

- [ ] **Step 1: Make the peripheral a target**

Give the peripheral node a stimulus file like any other node, so a tile writes the peripheral's
window and reads it back. Run and confirm DIRECTED PASS.

Run: `wsl -e bash -lc 'cd ~/noc_project && export BUILD_ROOT=$HOME/noc_build && export PYTHON3=python3 && make -C sim TB=mesh_2x2_vc1_periph PATTERN=neighbor'`
Expected: DIRECTED PASS, scoreboard clean, non-vacuous.

- [ ] **Step 2: Make the peripheral originate**

The peripheral's own file master writes a tile's window and reads it back. **This is the round's
deliverable**: nothing outside the mesh originates traffic today, and a run that passes Step 1 but
not this one has not exercised the mechanism, because Step 1 never makes the peripheral stamp a
`src_id` or consult the SAM.

Run: the same command.
Expected: DIRECTED PASS with the peripheral's own transactions counted in the non-vacuity check.

- [ ] **Step 3: Prove the boundary `$fatal` still fires**

The tie-off `$fatal` became conditional in Task 4, so its silence is not yet evidence. Drive a flit
at an unpopulated boundary port and confirm it aborts.

Run: the topology's fault-injection plusarg, mirroring `+decerr_fault` in
`sim/tb/user_node_endpoint.sv`.
Expected: the run aborts naming the tied-off direction.

- [ ] **Step 4: Prove the clip on the wire**

Run `PATTERN=multicast` on `mesh_2x2_vc1_periph` with a mask covering the tile row. Confirm both
tiles receive the replica and the peripheral does not.

Run: `wsl -e bash -lc 'cd ~/noc_project && export BUILD_ROOT=$HOME/noc_build && export PYTHON3=python3 && make -C sim TB=mesh_2x2_vc1_periph PATTERN=multicast'`
Expected: DIRECTED PASS. Without the clip this mask is refused at the source, so a pass here is the
clip working end to end.

- [ ] **Step 5: Run the full gate set**

Run: the Tier 2 pair plus `mesh_2x2_vc1 multicast`, plus `make test` and `make pytest`.
Expected: all green, with the ctest count raised by the tests added in Tasks 1 to 3.

- [ ] **Step 6: Commit**

```bash
git add sim/tools/gen_test_patterns.py
git commit -m "test(sim): prove a peripheral is both addressable and an initiator"
```

---

## Deferred to its own plan

Per-topology coordinate widths: a generated `node_id_t`, per-topology `ni_flit_constants.h`,
per-topology `FlitMarshalT` instantiation, a c_model build per topology and ctest multiplied by
topology count. The peripheral mechanism does not need it: today's `X_WIDTH = 4` holds a span of
16 per axis, and this plan's largest span is 5.

Per-declaration address base, which a peripheral needs only when its window exceeds one coordinate
slot.
