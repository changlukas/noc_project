# Off-mesh peripherals

## Why

`docs/noc-target-spec.md` was written without a way to reach the mesh from outside it. Every node
is a tile and nothing originates traffic from beyond the grid, so there is no host interface, no
memory controller, and no answer to what coordinate something outside the grid would even have.
`hotspot_boundary` was dropped from the pattern set for the same reason: nothing at an edge was
worth targeting.

This round builds the mechanism only. What gets attached, how many, and on which edges is a later
decision, and nothing here should constrain it.

## What was surveyed, and what of it we do not need

FlooNoC, from source.

**An off-grid endpoint is not given an off-grid coordinate.** It hangs off the boundary router port
that a plain mesh ties off, and the router array is not enlarged: `degree` stays 5
(`floogen/examples/axi_mesh_xy.yml`). That part we take.

**Its coordinate is derived, then normalised.** floogen infers a coordinate from the connection
graph and then shifts the whole space so nothing is negative:

```python
# floogen/model/network.py
node_xy_id = graph.nodes[neighbor]["id"] + XYDirections.to_coords(edge["dst_dir"])   # west -> -1
min_x = min(ni.id.x for ni in ni_nodes); xy_id_offset = Coord(x=min_x, y=min_y)
# gen_sam():  dest -= xy_id_offset
```

**That part we do not need, and copying it was the first draft's mistake.** floogen needs it because
it derives coordinates from a graph. Our topology files state them:

```yaml
tiles:
  - { x: 0, y: 0, size: 0x100000 }
```

A peripheral west of the mesh is written as `x: 0` with the tiles at `x: 1..n`. There is no
derivation, so there is nothing to normalise: no direction vector, no negative intermediate, no
offset applied at SAM generation, and no asymmetry where some topologies shift and others do not.
The author writes the final coordinate.

**There is no "peripheral" concept.** `hbm` is declared exactly like `cluster`, same fields, same
code path. What differs is that each endpoint declaration carries its own independent address base,
and an array index is a linear offset inside that declaration's region
(`routing.py`, `AddrRange.set_arr`).

## Three quantities that are currently one

| quantity | what it is | who uses it |
|---|---|---|
| physical router array | how many routers exist, per axis | the fabric generator, for neighbour wiring |
| route-coordinate span | the full coordinate range, tiles plus any border positions | `X_WIDTH` / `Y_WIDTH`, and the range check in `route_compute` |
| tile region | which stretch of that span holds tiles | the collective clip |

`sim/tools/gen_tb_top.py:354` derives each router's neighbours from `X_DIM`/`Y_DIM` and a linear
node index. If that were widened to the route span the generator would wire routers that do not
exist, so it keeps the physical array.

`ref_model/c_model/include/router/router.hpp:69` rejects a `dst_id` outside
`cfg.mesh_x_dim`/`cfg.mesh_y_dim`. Those fields come to mean the route span, or a peripheral fails
the range check of the router forwarding toward it.

All three are stated in the topology file. None is inferred.

## Design

### Routing needs no new logic

`route_compute` steers X first and ejects when both coordinates match (`router.hpp:65-74`). A
peripheral west of the mesh is reached by "keep going west", and at the westmost router west is the
port it hangs on. The decision is already correct, because the router cannot tell a peripheral from
another router in that direction.

What changes is that `cfg.mesh_x_dim` / `cfg.mesh_y_dim` mean the route span rather than the router
count. A field that keeps its name and changes its meaning is the easiest thing to miss in review,
so it is called out here rather than left to the diff.

### Attachment

A peripheral occupies the boundary router port that is tied off today. Router degree stays 5 and no
router differs from its neighbours.

**A peripheral NI attaches exactly the way a tile NI attaches, at a different port index.** The
fabric already declares all five ports with one signal set:

```systemverilog
// noc_fabric_mesh_2x2_vc1.sv:52,60
localparam int unsigned LINK_PORTS = 5;  // LOCAL + N/E/S/W
// Per-network per-node per-port arrays (LOCAL + N/E/S/W uniformly).
logic [LINK_PORTS-1:0] tx_req_valid [4];
logic [DAT_NUM_VC-1:0] tx_dat_crdvalid [4][LINK_PORTS];
```

A tile's `ni_wrap` binds to `[i][RP_LOCAL]` (`:109-119`). A peripheral's binds to `[i][RP_WEST]`,
or whichever direction it sits on, with the same port map and the same tx/rx crossing. Ready,
valid, flit and DAT credit are per-port already, so the plumbing exists; what is removed is the
tie-off that zeroes it.

There is no second kind of link and no new credit scheme. The far side is the same NI a tile has.

`noc_fabric_<topo>.sv` today zeroes REQ/RSP ready returns and DAT credit returns on boundary
directions (`noc_fabric_mesh_2x2_vc1.sv:153,207`) and makes a valid flit on one a `$fatal` (`:233`).
The generator learns which boundary ports are populated from the topology file: an entry whose
coordinate lies outside the tile region names one boundary router and one direction. Those ports get
the full link wiring. The rest keep the tie-off and the `$fatal`, which now keys off "unpopulated
boundary port" rather than "boundary port".

**That check stays.** It is the only thing that catches a packet routed somewhere that does not
exist, and it needs a fault-injection proof it still fires once the wiring became conditional.

### A peripheral node is an NI plus an endpoint

The same pair a compute node has: `ni_wrap` on the router's boundary port, with a
`user_node_endpoint` behind it. `user_node_endpoint` is the AXI-side test endpoint, not the NI, so
naming it alone would leave the packetiser out.

No new block. Both directions follow: outbound because the SAM resolves the peripheral's window,
inbound because the peripheral's NMU consults the same SAM as any tile. Inbound is the capability
that does not exist today.

### The peripheral's own identity

A peripheral NI is constructed with the coordinate its topology entry states, exactly as a tile's NI
is. Three places consume it and all three are already correct once the constructor argument is:

| site | what it does with the coordinate |
|---|---|
| `nmu/packetize.hpp:196` | stamps `src_id_` into every request it emits |
| `nsu/depacketize.hpp:56,181` | derives its node coordinate from `src_id` and rebases arriving addresses onto it |
| `nsu/meta_buffer.hpp:21` | captures the requester's `src_id` so the response routes back |

The return path needs no new mechanism, but it works **only** if the peripheral's `src_id` is the
coordinate the topology states. A peripheral stamped with an invented id would route its responses
to the wrong node, and the failure would look like a fabric bug.

### Collectives are clipped to the tile region

A multicast names its members as a coordinate wildcard: the set is
`{v : v & ~mask == anchor & ~mask}`, a block of `2^k` values aligned to `2^k` per axis. Tiles that do
not start at 0 are therefore not expressible: with tiles at `1..4`, `{1,2,3,4}` is not aligned, and
the only covering block is `[0,7]`, which names the peripheral and three coordinates that do not
exist.

Two obvious fixes do not work, and both were checked rather than assumed.

| attempted fix | why it fails |
|---|---|
| the peripheral's leaf declines to eject a replica | `CollectB` join is stateless: every router recomputes the expected input set from the B header (`simple_router.hpp:450`). A member that does not respond is still counted, and the collect hangs |
| concentration, so the peripheral shares a coordinate and differs by port | the multicast predicate has no port dimension. Upstream's `floo_route_xymask.sv` never mentions `port_id`, and neither does the port of it in `route_mask.hpp` |

**The member set is the mask rectangle intersected with the tile region.**

`RouterConfig` gains `tile_min` and `tile_max` per axis, beside `mesh_x_dim` / `mesh_y_dim`:

| field | meaning |
|---|---|
| `mesh_x_dim`, `mesh_y_dim` | route span. The range check |
| `tile_min`, `tile_max` | **inclusive** coordinates of the first and last tile on that axis, matching the existing `dst_min` / `dst_max` convention. The collective clip |

The algorithm in `route_mask.hpp` is one clamp on each path, before the existing comparisons:

```
fork:  dst_min = max(anchor & ~mask, tile_min)    dst_max = min(anchor | mask, tile_max)
join:  src_min = max(src    & ~mask, tile_min)    src_max = min(src    | mask, tile_max)
```

**Local membership is unchanged.** A router only ever sits at a tile coordinate, so the
`coord_matched` test at `route_mask.hpp:108,150` already answers correctly. Only the forwarding
bounds move, which is exactly what stops a worm travelling out to the border and stops the join
expecting an input from there. Fork and join clamp with the same two numbers, so the member set is
identical wherever it is recomputed.

An empty intersection (`min > max`) is a source error, not a runtime condition. The source rejects
it; a router that sees one aborts, in the style of the guards already in that file.

`addr_trans.hpp`'s guard changes from "the highest wildcard member is inside the mesh" (`:338`) to
"the clipped set is non-empty and every member of it is a tile". For the source to compute the same
clip, `SpaceCoords` (`ni/address_map.hpp:43`) gains the tile region beside its counts, stated rather
than inferred, which is what its own comment already argues for: recovering a dimension as
`1 << len` over-permits every dimension that is not a power of two. `tile_min` / `tile_max` in
`RouterConfig` and the tile region in `SpaceCoords` are read from the same topology entries, which
is what makes source and router agree by construction rather than by convention.

These are elaboration-time parameters, **not control registers.** The values are a property of the
topology and never change after synthesis. Making them writable would convert a structural guarantee
into a runtime convention: one router configured differently from the rest would break the member
set on one side of the fork/join pair, and the symptom would look like a fabric bug rather than a
configuration error.

What it costs and what it buys:

| | |
|---|---|
| flit header | unchanged |
| AWUSER interface | unchanged, still an address mask confined to the node-index field |
| router config | two inclusive coordinates per axis |
| peripheral placement | **unconstrained. Any edge** |
| side effect | fixes the pre-existing case that a non-power-of-two axis cannot express a full row: a 3-wide row encodes as `[0..3]` and clips to `[0..2]` |

**Deliberately not built:** a mode bit separating tile-only multicast from full-span multicast.
Peripherals are not collective targets in this round, and `collective_op` is 2 bits whose values 2
and 3 are reserved today (`addr_trans.hpp` aborts on them), so adding the mode later costs no header
bits.

### Address map

**A peripheral is an entry in the same address space as the tiles**, at the border coordinate.
That one decision is what keeps the rest small, and it is checked below against a worked example
rather than asserted.

**One rule changes: a row is strided to a power of two, not to the number of entries.**

```
base(x, y) = base_zero + ((y << X_WIDTH) | x) * size
```

where `X_WIDTH = clog2(route span)`. Today's packing accumulates in list order
(`address_map.py:61,75`), which produces the same result only when the row length happens to be a
power of two. Deriving the base from the coordinate makes the identity structural instead of
accidental, and it is what lets the address coordinate field and the route coordinate be the same
number.

`rebase_node_coords` (`ni/address_map.hpp:60`) writes an NSU's own coordinate into that field, so
one numbering is not a preference here, it is what makes rebasing correct without a conversion.

The cost is the rounding: a row of `n` positions occupies `2^clog2(n)` slots. Three positions
occupy four. That is a static waste paid once, against a second numbering that would be paid on
every read of the code.

**The coordinate walk moves to the tile region.** `declare_space_coords` (`sam_yaml.hpp:35-60`)
validates a space by walking every coordinate in it and requiring an entry at each
(`addr_trans.hpp:189-205`). Two things follow from that walk, and an earlier draft of this spec got
both wrong by claiming the checks were untouched.

The walk requires the rectangle to be **fully populated**. With tiles at `x = 1..2` and a peripheral
on only one of two rows, `(0,1)` has no entry, `space_entries` is 5 against `x_count * y_count` of
6, and validation fails. It does not fail loudly: `x_count` stays 0, `declared()` returns false, and
the space is "simply not a collective target" per its own comment. Collectives would turn
themselves off and nothing would say so.

The walk also requires a **uniform aperture**, `e->size == origin->size`. A peripheral whose window
is not tile-sized fails that, which is every real memory controller.

Requiring the border ring to be fully populated would fix both and is rejected: it would mean
inventing filler entries for coordinates with no NI behind them, so the SAM would name nodes the
fabric ties off. The `$fatal` and the address map would disagree about what exists.

So the walk covers the tile region instead:

| check, `addr_trans.hpp` | today | after |
|---|---|---|
| `:189` entry count | `x_count * y_count` | the tile count |
| `:192` origin coordinate bits are zero | origin is `(0,0)` | origin is `(tile_min_x, tile_min_y)`, and its coordinate bits equal that |
| `:197-199` the walk bounds | `[0, x_count) x [0, y_count)` | `[tile_min, tile_max]` on each axis |
| `:203` uniform aperture | every entry in the space | every **tile**. A peripheral need not match a tile's size, but see the bound below |

`x_range.len` still sizes to the route span, because the field has to hold a border coordinate. So
`declare_space_coords` needs both numbers: the span for the field width, the tile region for the
walk. Those are the same two numbers the clip already needs, which is why this costs no new concept.

**An unpopulated border coordinate simply has no entry.** It is not a filler and not a legal
target: a request naming it misses the SAM, which `SamTable::translate` already treats as fatal.
Nothing new is needed to make it loud.

**Entry order is part of the contract.** `declare_space_coords` reads its stride from the first two
entries of a class (`sam_yaml.hpp:50`), so entries are listed in coordinate order and the first two
are adjacent tiles. The worked example below assumes it; the loader should enforce it rather than
leave it to the author.

**A peripheral sharing the tile space fits within one coordinate slot.** Its base is fixed at its
coordinate, so a window larger than the slot runs into the next one and `addr_trans.hpp:149`
rejects it as an overlapping range. Smaller or equal is fine; larger is not. The check is loud, so
this is a bound to know rather than a trap.

**Per-declaration base stays out of this round, but it is what a real peripheral will need.** It
existed to stop peripherals widening the tiles' node-index field, which the route-span field makes
moot. The remaining reason to want it is the size bound above: a memory controller fronting far
more than one slot cannot share the tile space and needs its own, with its own base. This round's
peripheral is tile-sized, so that is deferred rather than dismissed.

### Worked example

Two tiles wide, two rows, one peripheral on the west of each row.

```
        x=0      x=1     x=2
 y=1   [P]      [T]     [T]
 y=0   [P]      [T]     [T]
```

Route span `x` is `0..2`, so `X_WIDTH = clog2(3) = 2` and a row strides four slots.
`Y_WIDTH = clog2(2) = 1`. Every entry is `0x100000`, so the offset field is 20 bits.

| node | `base` | bit[22] = y | bit[21:20] = x |
|---|---|---|---|
| P (0,0) | `0x000000` | 0 | `00` |
| T (1,0) | `0x100000` | 0 | `01` |
| T (2,0) | `0x200000` | 0 | `10` |
| P (0,1) | `0x400000` | 1 | `00` |
| T (1,1) | `0x500000` | 1 | `01` |
| T (2,1) | `0x600000` | 1 | `10` |

`0x500000` is `0b0101` in bits [23:20], so `y = 1`, `x = 1`. `0x600000` is `0b0110`, so `y = 1`,
`x = 2`. The field is the coordinate.

`declare_space_coords` reads `stride = 0x100000` from the first two entries, a power of two, giving
`offset = 20`, `x_range = {20, 2}`, `y_range = {22, 1}`.

The walk covers the tile region, `x = 1..2` and `y = 0..1`, so the entry count it checks is
`4 == 2 * 2` and its origin is `T(1,0)` at `0x100000`, whose coordinate bits read `x = 1, y = 0`.
The two peripherals are entries but not walked, so they need not match the tile size, within the
one-slot bound. `(3,0)` is padding beyond the span and is never visited.

**The multicast that the clip exists for.** Anchor `T(1,0)`, target the tile row `{(1,0), (2,0)}`.
Wildcarding the x field gives `mask_x = 0b11`, and the raw wildcard set is

```
{v : v & ~3 == 1 & ~3 == 0}  =  {0, 1, 2, 3}
                                 ^        ^
                            peripheral   does not exist
```

Today that is rejected outright: `(anchor_x | mask_x) = 3 >= x_count = 3`. With the clip,

```
dst_min = max(1 & ~3, tile_min) = max(0, 1) = 1
dst_max = min(1 |  3, tile_max) = min(3, 2) = 2      ->  {1, 2}
```

which is exactly the two tiles. The peripheral and the non-existent coordinate are both removed by
the same clamp.

**Rebasing.** `T(2,1)` receives a replica whose address names the anchor. `rebase_node_coords`
writes its own `(2, 1)` into bits [22:20], giving `0x6xxxxx` with the low twenty bits untouched. No
conversion, because the field is the coordinate.

### Considered and rejected: collectives in tile-index space

Expressing the wildcard over tile indices `0..N-1` instead of route coordinates is attractive,
because indices are power-of-two aligned by construction and the clip would not be needed at all.
It fails on `nsu/depacketize.hpp:50-57,181-184`: the NSU rebases an arriving address using the
coordinate it was constructed with, and writes it into the address coordinate field. Under an
index-space scheme that field holds indices while the constructor argument is a route coordinate,
so a tile at route `x = 1` whose index is 0 writes the wrong value and the rebased address names
the wrong tile. Silently.

Making it work needs the conversion at four sites, `route_mask` fork and join,
`collective_translate`, and that rebase. Three of them are loud on error and the fourth is not.
This project has been bitten twice by a field whose meaning depends on context, so the record is
kept here to stop the idea being re-proposed.

### Coordinate width follows the topology

`X_WIDTH` and `Y_WIDTH` are computed from the route span per topology, on **both** `ref_model/` and
`rtl/`, bit for bit, rather than fixed at 4 each.

`addr_trans.hpp:358-361` carries the invariant that stays:

```cpp
static_assert(X_WIDTH + Y_WIDTH == COLLECTIVE_MASK_WIDTH,
              "collective_mask must be one node id wide (X|Y) -- specgen drift");
```

A collective mask is one node id wide because the mask is over the coordinate bits themselves, not a
list of targets. Widen the coordinate and the mask widens with it.

The second assert in that pair, `X_WIDTH + Y_WIDTH <= 8`, is a container limit and goes. A generated
`node_id_t` sized from `X_WIDTH + Y_WIDTH` replaces the `uint8_t` that carries a node id or a
collective mask: `RouterConfig` (`router_types.hpp:17`), the `route_mask.hpp` API, SAM entries and
`collective_translate`'s return (`addr_trans.hpp:24,261`), NSU metadata (`meta_buffer.hpp:21`), and
the matching guard at `route_mask.hpp:43-46`. The SV side gets the same typedef from specgen.

**The real bound is the header field accessor**, which passes values as `uint64_t`, so
`X_WIDTH + Y_WIDTH <= 64`. That is a limit of the accessor rather than of the design, and it is far
above any mesh this project will build. It is stated so that "the width follows the topology" is not
read as "there is no limit anywhere".

The accepted cost of per-topology widths, recorded so it is not rediscovered:

| | today | after |
|---|---|---|
| `ni_flit_constants.h` | one global file | one per topology |
| `NOC_REQ/RSP/DAT_FLIT_WIDTH` | fixed | per topology |
| `FlitMarshalT<WIDTH_BITS>` | one instantiation, `dpi_marshal.hpp:39,107` | per topology |
| c_model binary | serves every topology | built per topology |
| ctest | one run | multiplied by topology count |
| generated package, filelist, build rules | one set | per topology |

## Validation

The dummy peripheral is an `ni_wrap` plus a `user_node_endpoint` with `axi_sim_mem` behind its NSU
and a file master on its NMU, identical to a compute node. Two directed checks:

| # | check | new today |
|---|---|---|
| 1 | a tile writes the peripheral's window and reads it back | no, this is an ordinary SAM target |
| 2 | the peripheral writes a tile's window and reads it back | **yes, nothing outside the mesh originates traffic today** |

Check 2 is the round's deliverable. A run where only check 1 passes has not exercised the mechanism,
because check 1 never requires the peripheral to stamp a `src_id` or to consult the SAM.

The unpopulated-boundary `$fatal` needs a fault-injection proof it still fires, per the standing rule
that a checker's silence counts only after it has been shown to fire.

A multicast over a full tile row, on a topology whose tiles do not start at 0, is the check that the
clip works. Without the clip that mask is rejected at the source; with it the replicas reach every
tile in the row and no peripheral.

## Out of scope

Which peripherals, how many, and on which edges. The mechanism must not assume an answer.

Whether peripherals are multicast or collective targets. They are not, and the clip makes that
structural rather than a convention. The reserved `collective_op` values leave the door open at no
header cost if that changes.

`hotspot_boundary` becomes meaningful once an edge has a target, but the pattern is not restored in
this round.
