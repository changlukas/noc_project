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

Two changes.

**Per-declaration base.** A group of entries may declare its own `base`; without one the running
accumulator continues as today. The five existing topology files stay textually unchanged, but both
loaders gain a real explicit-base model: `sim/tools/address_map.py:17,61` and
`ref_model/c_model/include/nmu/sam_yaml.hpp:108,129`, which today both state that there is no base
and derive every one by accumulation.

Compute tiles then keep their own contiguous node-index bit field inside their own region, and
peripherals get theirs inside a separate region. Neither perturbs the other. The `address_map:`
comment in each topology file states why that field has to stay contiguous: multicast replica
addresses differ only in node-index bits. A single accumulator shared with peripherals would have
widened that field. Independent bases avoid it without introducing a third address space.

**The address coordinate field spans the route span, not the tile count.** `rebase_node_coords`
(`ni/address_map.hpp:60`) writes a coordinate into the address's coordinate field, so that field and
the route coordinate are the same number. With tiles at `1..4` the field covers `0..4`, and the slot
at 0 belongs to the peripheral or to nothing. One numbering, no translation.

The cost is the reserved slot, accepted deliberately: two numberings would be paid for on every read
of the code, a reserved address slot is paid for once.

Three validators relax from "dense from `(0,0)`" to "dense within the stated tile region":
`address_map.py:72,91` and `addr_trans.hpp:115`. `sam_yaml.hpp:55` declares `x_count = x_dim` today
and must declare the tile region instead.

Addresses are packed in list order (`address_map.py:61,75`), so nothing about coordinates moves an
address.

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
