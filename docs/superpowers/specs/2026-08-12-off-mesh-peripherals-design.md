# Off-mesh peripherals

## Why

`docs/noc-target-spec.md` was written without a way to reach the mesh from outside it. Every node
is a tile and nothing originates traffic from beyond the grid, so there is no host interface, no
memory controller, and no answer to what coordinate something outside the grid would even have.
`hotspot_boundary` was dropped from the pattern set for the same reason: nothing at an edge was
worth targeting.

This round builds the mechanism only. What gets attached, how many, and on which edges is a later
decision, and nothing here should constrain it.

## What was surveyed

FlooNoC, from source. Three findings shape this design.

**An off-grid endpoint is not given an off-grid coordinate.** It hangs off the boundary router
port that a plain mesh ties off, and its coordinate is derived:

```python
# floogen/model/network.py, compile_ids()
node_xy_id = graph.nodes[neighbor]["id"] + XYDirections.to_coords(edge["dst_dir"])
```

West of router `(0, y)` gives `(-1, y)`. The router array is not enlarged and `degree` stays 5
(`floogen/examples/axi_mesh_xy.yml`).

**Negative coordinates never reach hardware.** The whole space is normalised once, at generation:

```python
# gen_xy_routing_info()
min_x = min(ni.id.x for ni in ni_nodes)
xy_id_offset = Coord(x=min_x, y=min_y)
num_x_bits = clog2(max_x - min_x + 1)
# gen_sam():  dest -= xy_id_offset
```

The subtraction lands in the address decode at the source, so routers compare unsigned.

**There is no "peripheral" concept.** `hbm` is declared exactly like `cluster`, same fields, same
code path. What differs is that each endpoint declaration carries its own independent address
base, and an array index is a linear offset inside that declaration's region:

```python
# routing.py, AddrRange.set_arr()
case (m,):    self.start = self.base + size * m
case (m, n):  self.start = self.base + size * (m * arr_dim[1] + n)
```

## Three quantities that are currently one

The design rests on separating three things the code treats as a single pair of numbers today.

| quantity | what it is | who uses it | changes with a border |
|---|---|---|---|
| physical router array | how many routers exist, per axis | fabric generator, for neighbour wiring | no |
| route-coordinate span | physical span plus any populated border ring | `X_WIDTH` / `Y_WIDTH`, and the range check in `route_compute` | yes |
| per-router coordinate | that router's own normalised `(x, y)` | each router's config, the SAM | shifts by the offset |

`sim/tools/gen_tb_top.py:354` derives each router's neighbours from `X_DIM`/`Y_DIM` and a linear
node index. If `X_DIM` were widened to the route span, the generator would wire routers that do
not exist. It keeps the physical array.

`ref_model/c_model/include/router/router.hpp:69` rejects a `dst_id` outside
`cfg.mesh_x_dim`/`cfg.mesh_y_dim`. Those fields must come to mean the route span, or a tile at the
far edge fails its own range check once the tiles shift.

## Design

### Routing needs no new logic, only new meanings

`route_compute` steers X first and ejects when both coordinates match
(`router.hpp:65-74`). A peripheral west of the mesh is reached by "keep going west", and at the
boundary router west is the port the peripheral hangs on. The decision is already correct.

What changes is the meaning of two existing fields, `cfg.mesh_x_dim` and `cfg.mesh_y_dim`, from
router count to route span, and the value of `cfg.x` / `cfg.y`, which become the router's
normalised coordinate. A field that keeps its name and changes its meaning is the easiest thing to
miss in review, so both are called out here rather than left to the diff.

### Attachment

A peripheral occupies the boundary router port that is tied off today. Router degree stays 5 and
no router differs from its neighbours.

A populated boundary link is wired **identically to an inter-router link**: the same ready/valid
in both directions, the same DAT credit seed and return path, the same depth. This is the
simplifying rule of the whole design, and it holds because the thing on the far side is the same
NI a tile has. There is no second kind of link.

`noc_fabric_<topo>.sv` today zeroes REQ/RSP ready returns and DAT credit returns on boundary
directions (`noc_fabric_mesh_2x2_vc1.sv:153,207`) and makes a valid flit on one a `$fatal`
(`:233`). The generator learns which boundary ports are populated, from the topology file: an
entry whose coordinate lies on the border ring names one boundary router and one direction. Those
ports get the full link wiring. The rest keep the tie-off and the `$fatal`, which now keys off
"unpopulated boundary port" rather than "boundary port".

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

A peripheral NI is constructed with its normalised coordinate exactly as a tile's NI is. Three
places consume it and all three are already correct once the constructor argument is right:

| site | what it does with the coordinate |
|---|---|
| `nmu/packetize.hpp:196` | stamps `src_id_` into every request it emits |
| `nsu/depacketize.hpp:56,181` | derives its node coordinate from `src_id` and rebases arriving addresses onto it |
| `nsu/meta_buffer.hpp:21` | captures the requester's `src_id` so the response routes back |

The return path therefore works with no new mechanism, but it works **only** if the peripheral's
`src_id` is its normalised coordinate. A peripheral stamped with an unnormalised or invented id
would route its responses to the wrong node, and the failure would look like a fabric bug.

### Coordinates

| step | rule |
|---|---|
| derive | peripheral coordinate = boundary router coordinate + direction vector |
| normalise | subtract the per-axis minimum, so the axis starts at 0 |
| size | `X_WIDTH = clog2(x span)`, `Y_WIDTH = clog2(y span)`, per topology |
| apply | the offset is applied where the SAM is generated, not in the router |

An axis with no peripherals has minimum 0, so its offset is 0 and its coordinates are unchanged.
**No tile in the five existing topologies changes coordinate.** A topology that adds a west border
shifts its tiles from `x = 0..n-1` to `x = 1..n`, and only that topology.

Their coordinate *widths* do change, because width now follows the mesh rather than a fixed 4:
`mesh_2x2` goes to `1 + 1` bits and `mesh_4x4` to `2 + 2`. The values are the same, the fields
holding them are narrower.

**Standing bound: `X_WIDTH + Y_WIDTH <= 8`.** `dst_id` is a `uint8_t` and the collective mask is
tied to that sum (`addr_trans.hpp:121,358`). Per-topology widths make small meshes cheaper but do
not lift the ceiling, so a border ring is supported only where the resulting span still fits.
Lifting it is a separate piece of work and is not attempted here.

### Address map

Adopt FlooNoC's per-declaration base. A group of entries may declare its own `base`; without one
the running accumulator continues as today. The five existing topology files stay textually
unchanged, but **both loaders gain a real explicit-base model**: `sim/tools/address_map.py:17,61`
and `ref_model/c_model/include/nmu/sam_yaml.hpp:108,129`, which today both state that there is no
base and derive every one by accumulation.

Two validators also have to widen. `address_map.py:72,89` rejects a coordinate outside
`x_dim`/`y_dim` and requires every mesh node exactly once per space. Those become "inside the
route span" and "every declared node exactly once per space".

The consequence that matters: compute tiles keep their own contiguous node-index bit field inside
their own region, and peripherals get theirs inside a separate region. Neither perturbs the other.
The `address_map:` comment in each topology file states why that field has to stay contiguous:
multicast replica addresses differ only in node-index bits. A single accumulator shared with
peripherals would have widened that field and moved the multicast mask layout. Independent bases
avoid it without introducing a third address space.

Addresses do not move when coordinates shift. `address_map.py:61,75` packs bases in list order and
carries the coordinate alongside, so a shift changes `dst_id` and nothing else.

### Coordinate width scales per topology

Decided: `X_WIDTH` and `Y_WIDTH` are computed per topology on **both** `ref_model/` and `rtl/`,
bit for bit, rather than fixed at 4 each.

The accepted cost, recorded so it is not rediscovered:

| | today | after |
|---|---|---|
| `ni_flit_constants.h` | one global file | one per topology |
| `NOC_REQ/RSP/DAT_FLIT_WIDTH` | fixed | per topology |
| `FlitMarshalT<WIDTH_BITS>` | one instantiation, `dpi_marshal.hpp:39,107` | per topology |
| c_model binary | serves every topology | built per topology |
| ctest | one run | multiplied by topology count |
| generated package, filelist, build rules | one set | per topology |

What it buys: a 4x4 mesh carries `2 + 2` coordinate bits per id instead of `4 + 4`, so `REQ`
drops 8 b, and the 16x16 ceiling stated in `constants.yaml` stops being a property of the flit.

## Validation

The dummy peripheral is an `ni_wrap` plus a `user_node_endpoint` with `axi_sim_mem` behind its NSU
and a file master on its NMU, identical to a compute node. Two directed checks:

| # | check | new today |
|---|---|---|
| 1 | a tile writes the peripheral's window and reads it back | no, this is an ordinary SAM target |
| 2 | the peripheral writes a tile's window and reads it back | **yes, nothing outside the mesh originates traffic today** |

Check 2 is the round's deliverable. A run where only check 1 passes has not exercised the
mechanism, because check 1 never requires the peripheral to stamp a `src_id` or to consult the SAM.

The unpopulated-boundary `$fatal` needs a fault-injection proof it still fires, per the standing
rule that a checker's silence counts only after it has been shown to fire.

## Out of scope

Which peripherals, how many, and on which edges. The mechanism must not assume an answer.

Whether peripherals are multicast or collective targets. They are not in this round, and the
`COLLECTIVE_MASK_WIDTH` interaction is not designed here.

Lifting `X_WIDTH + Y_WIDTH <= 8`.

`hotspot_boundary` becomes meaningful once an edge has a target, but the pattern is not restored
in this round.
