# Peripheral addressing: destination id becomes (coordinate, port)

Status: design approved, not implemented.

## Problem

A peripheral has an NI but no router, so it hangs off one boundary port of an edge router whose
LOCAL port already carries a tile. Today it takes its own route coordinate OUTSIDE the tile
region, which makes the route-coordinate span wider than the router array
(`sim/topologies/mesh_2x2_vc1_periph.yaml`: `x_dim 2`, `x_span 3`, `tile_x_first 1`).

XY resolves X to completion first, so a packet bound for an x-face peripheral leaves the region
before Y has moved, on the SOURCE's row. It lands at whichever peripheral borders that row, whose
NSU rebases the address and answers normally, so nothing downstream can tell
(`nmu/addr_trans.hpp:460-463`). `check_dst_reachable` (`:470`) aborts on the cross-row pairing and
`docs/noc-target-spec.md:55` states the restriction as spec.

The requirement is now that every tile reaches every peripheral on all four faces.

Survey places the current scheme on the wrong side of the baseline. AMBA CHI assigns a NodeID per
component port on the interconnect, implementation-defined rather than coordinate-defined
(IHI 0050). gem5 Ruby/Garnet connects controllers to routers through Local ports, several per
router. FlooNoC attaches I/O components with an additional NI (arXiv:2409.17606 IV-B).
*On-Chip Networks* 2nd ed Ch 2 states the placement rule directly: give memory controllers their
own router port rather than sharing a tile's injection bandwidth. Treating an edge device as a
directional off-grid hop is what produces the row restriction.

## Decision 1: destination id is (coordinate, port)

A peripheral shares the coordinate of the router it hangs off. Two new flit header fields, 2 b
each, appended above `collective_mask` so no existing field's bit position moves.

| field | width | purpose |
|---|---|---|
| `dst_port_id` | 2 | which endpoint at the destination coordinate receives |
| `src_port_id` | 2 | which one issued, so the response reaches it |

| value | endpoint | router resolves as |
|---|---|---|
| `00` | LOCAL, the tile | eject LOCAL. All-zero default, so existing traffic is bit-identical |
| `01` | X face | `cfg.x == 0` gives WEST, `cfg.x == mesh_x_dim - 1` gives EAST |
| `10` | Y face | `cfg.y == 0` gives SOUTH, `cfg.y == mesh_y_dim - 1` gives NORTH |
| `11` | reserved | assert |

`HEADER_TOTAL_WIDTH` goes 44 to 48. `PADDING_FIELDS_COUNT` is 0, so there is no spare bit to
absorb this.

Two bits and not three: the direction follows from which edge the router sits on, so the field
states the axis only. Minimum mesh dimension is 2 (`sim/tools/gen_tb_top.py:98`), so one router
cannot be at both `x == 0` and `x == mesh_x_dim - 1`.

A field at each end and not one shared field: `nsu::Packetize::build_b_flit`
(`nsu/packetize.hpp:93-97`) sets a response's `dst_id` from the buffered request's `src_id`, so a
peripheral that issues a request needs its own face recoverable at response time. One field
meaning "the non-LOCAL end" mis-delivers as soon as the requester's face is also populated at the
responder's coordinate, which is what one peripheral per row is.

`route_compute` (`router/router.hpp:63-75`) is otherwise unchanged. Its final `return LOCAL`
becomes the port the field names, and a face illegal for this router's position asserts.

## Decision 2: the address map becomes tile-major

Today it is space-major: every memory region packed from 0, every config region stacked above
(`sim/tools/address_map.py:80-104`). A node's two regions sit a full memory space apart.

    block = 4 GiB per node, a customisation point, and the node stride
      +0x0_00000000  memory   32 MB   2^25, the collective target
      +0x0_02000000  config    4 KB
      +0x0_02001000  reserved, deliberately free for further memory regions

    a 4x4 tile array occupies 0x0 .. 0x10_00000000
    peripheral regions stack above it, size unconstrained

A peripheral region is not inside a node's block. An HBM channel wants more address space than a
block, and a peripheral is never a collective member, so none of the four collective-eligibility
conditions in `docs/noc-target-spec.md` 5.1 apply to it.

Both spaces keep the same coordinate field. `sam_yaml::declare_space_coords` derives the field
offset from the observed stride (`sam_yaml.hpp:57`, `stride = second->base - first->base`), and
under this layout memory and config both stride by 4 GiB, so both land at bit 32. `SpaceCoords`
stops needing one instance per class for the offset.

## Decision 3: what this deletes

| item | where |
|---|---|
| `x_span`, `y_span`, `tile_x_first`, `tile_x_last`, `tile_y_first`, `tile_y_last` | topology YAML, `gen_tb_top._route_span()` |
| the same four bounds in `RouterConfig` | `router/router_types.hpp:25-28` |
| the same four in `SimpleRouterConfig` and its users | `router/simple_router.hpp:100,323,380,465` |
| the same four across the wrap and DPI boundary | `wrap/router_wrap.hpp:66,75,92`, `dpi/cmodel_dpi.h:87`, `dpi/cmodel_dpi.cpp:185,198` |
| the same four in `SpaceCoords` | `ni/address_map.hpp` |
| `check_dst_reachable` and both call sites | `nmu/addr_trans.hpp:470`, `nmu/packetize.hpp:181,272` |
| `peripheral_reaches`, `peripheral_partner`'s legality filter, `peripheral_hotspot`'s single-target rule | `sim/tools/gen_test_patterns.py:878-930` |
| the corner-coordinate rejection | `sim/tools/gen_tb_top.py:210` |

The DPI signature change reaches the generated SystemVerilog, so the create calls in
`gen_tb_top.py` move with it.

Kept and re-bounded: the collective clip in `route_mask_fork` / `route_mask_join`
(`router/route_mask.hpp:113-121,175-184`) and its twin in `collective_translate`. The bound moves
from the tile region to `0 .. mesh_*_dim - 1`. It is still needed: a wildcard mask can name a
coordinate that does not exist when a mesh dimension is not a power of two.

## Decision 4: the port is part of the SAM key

A peripheral shares a tile's coordinate and carries the Data class, the same class as memory.
Three sites currently key on (class, coordinate) and would count a peripheral as a second tile.

| site | today | consequence if unfixed |
|---|---|---|
| `SamTable::validate` duplicate check | `addr_trans.hpp:154-157` | `duplicate mesh node` assert |
| `SamTable::validate` `memory_count == mesh_nodes` | `:161` | count mismatch assert |
| `SamTable::declare_space_coords` tile walk | `:200-215` | returns false, and **memory space silently stops being a collective target** |
| `sam_yaml::declare_space_coords` stride pair | `sam_yaml.hpp:44-57` | takes the first two entries of the class, so a peripheral listed before a memory tile yields a silently wrong field offset |

`SamEntry` and `Translated` gain `uint8_t port`. All four sites filter on `port == 0`. The third
is the dangerous one: it does not assert, it returns false, and the symptom is a multicast
refused at the source rather than a build failure.

## Decision 5: the eight things an implementer would otherwise invent

| # | item | decision |
|---|---|---|
| 1 | flit schema | two fields in `specgen/source/`, appended above `collective_mask`, so every existing offset is unchanged |
| 2 | topology YAML | a `peripherals:` block separate from `address_map.tiles`, entries `{ x, y, face: x\|y, size }` |
| 3 | duplicate face | the generator rejects two peripherals sharing (x, y, face), the shape `gen_tb_top.py:226` already uses for (router, direction) |
| 4 | peripheral windows | bases assigned in declaration order above the tile array, each aligned to its own size, not coordinate-derived |
| 5 | SAM returns the port | `SamEntry.port` and `Translated.port`, set by the loader from the `peripherals:` block |
| 6 | `src_port_id` config | `NmuConfig` and `NsuConfig` gain `port_id`, passed at create like `src_id`, through the DPI create calls |
| 7 | corner wiring | `_peripherals` already keys on (router, direction) and already emits all four; deleting the corner rejection is the whole change |
| 8 | perf monitor names | endpoint named `node<idx>.local` or `node<idx>.<face>` |

`MetaEntry` (`nsu/meta_buffer.hpp`) gains the request's `src_port`, so the response can fill
`dst_port`.

## Rounds

Each round ends green on its own acceptance bar.

| round | content | acceptance |
|---|---|---|
| 1 | address map to tile-major, memory 32 MB, config inside the block | behaviour identical. Full ctest, Tier 2 co-sim, DMA both directions, both testbench flavours |
| 2 | the two header fields and their plumbing end to end, every port `00` | behaviour identical again. Same bar as round 1 |
| 3 | peripherals move on-grid, deletions, four-face topology, patterns | every tile reaches every peripheral on all four faces |

Round 2 is behaviour-neutral because every shipped topology has only tiles, so every port field is
`00` and every route resolves as it does today. It exists so that a failure in round 3 is a
peripheral failure and not a header-width failure.

Round 3 has an internal ordering constraint: the port-based collective refusal goes in before the
tile-region clip bound comes out. Reversed, peripherals are legal collective members in between.

Round 1 verifies both testbench flavours because moving addresses changes what `gen_dma_jobs`
computes. Round 3 verifies `sim/tb/test/` only, since the DMA flavour cannot run a peripheral
topology (`docs/known-limitations.md`).

### Acceptance detail: collective eligibility

Both spaces must stay collective targets, and the failure mode is silent. Every round asserts it
directly rather than inferring it from a passing multicast run.

- round 1 adds a ctest asserting `SamTable::collective_coords(Narrow)` and `(Data)` are both
  non-null on every shipped topology
- round 3 re-runs that assertion with peripherals present, and fault-injects it by removing the
  `port == 0` filter to prove the assertion fails

### Round 1 work the blast radius makes explicit

Hardcoded addresses that move: `ref_model/c_model/tests/nmu/test_packetize.cpp:317-318`,
`tests/nmu/test_rob.cpp:1316,1333`, `sim/tools/test_gen_test_patterns_filemaster.py:282,285,660`.

Two stale sentences to correct, both describing the coordinate field as sitting at `log2(size)`
when the implementation derives it from the stride: `docs/noc-target-spec.md:365` with its
repeat at `:521-522`, and the `collective_addr_mask` docstring in
`sim/tools/gen_test_patterns.py`.

Everything else in the blast radius is value-driven and needs no code change: `SpaceCoords`
offsets, `collective_translate`'s mask-confinement check, `burst_footprint_ok` (a burst is at most
4 KB against a 32 MB region), the tile crossbar windows from `node_windows()`, `noc_egress_base`,
the `gen_dma_jobs` window check (largest offset about 1.6 MB), the pattern slot allocator
(`_DEFAULT_REGION_BYTES` is 0x1000), and the testbench collective AW restore path
(`sim/tb/test/user_node_endpoint.sv:328,577-582`).

## Rejected alternatives

**A prime, turn the packet early.** Keep the off-grid coordinate and route an x-face destination
Y first, X last. Deadlock-free only while peripherals occupy at most one x face. With both x faces
populated the admitted turn set contains all four turns of one cycle: `N->E` to an east-face
peripheral, `E->S` ordinary XY, `S->W` to a west-face peripheral, `W->N` ordinary XY. Around a 2x2
cell that closes a channel dependency cycle. Derived independently twice and agreed. It also makes
routing order a per-destination-class property, which `route_mask.hpp:10-12` assumes is global.

**An extra router column on the x faces.** Reaches every peripheral and needs no header change,
but keeps the span-versus-array split and costs a row of routers and links.

**Peripheral regions inside a node's block.** Caps a peripheral at the block size, which an HBM
channel exceeds.
