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

The subtraction lands in the address decode at the source. Routers compare unsigned and are
identical whether or not a border exists.

**There is no "peripheral" concept.** `hbm` is declared exactly like `cluster`, same fields, same
code path. What differs is that each endpoint declaration carries its own independent address
base, and an array index is a linear offset inside that declaration's region:

```python
# routing.py, AddrRange.set_arr()
case (m,):    self.start = self.base + size * m
case (m, n):  self.start = self.base + size * (m * arr_dim[1] + n)
```

## Design

### Attachment

A peripheral occupies the boundary router port that is tied off today. Router degree stays 5 and
no router becomes different from its neighbours.

`noc_fabric_<topo>.sv` currently ties off every boundary direction and makes a valid flit on a
tied direction a `$fatal`. The generator derives the populated boundary ports from the topology
file: an entry whose coordinate lies on the border ring names one boundary router and one
direction. Those ports are wired to a peripheral, the rest keep the tie-off and the `$fatal`.
**That check stays.** It is the only thing that catches a packet routed somewhere that does not
exist.

### A peripheral is a `user_node_endpoint`

Same NMU and NSU pair, same address windows, same DPI wrappers. No new block, and both directions
work without further design: outbound because the SAM resolves the peripheral's window, inbound
because the peripheral's NMU consults the same SAM as any tile.

Inbound is the capability that does not exist today.

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

Addresses do not move with coordinates. `address_map.py` packs bases in list order
(`base(i) = base(i-1) + size(i-1)`) and carries the coordinate alongside, so a shift changes
`dst_id` and nothing else.

### Address map

Adopt FlooNoC's per-declaration base. A group of entries may declare its own `base`; without one
the running accumulator continues as today. The five existing topology files are unchanged.

The consequence that matters: compute tiles keep their own contiguous node-index bit field inside
their own region, and peripherals get theirs inside a separate region. Neither perturbs the other.
The `address_map:` comment in each topology file states why that field has to stay contiguous:
multicast replica addresses differ only in node-index bits. A single accumulator shared with
peripherals would have widened that field and moved the multicast mask layout. Independent bases
avoid it without introducing a third address space.

### Coordinate width scales per topology

Decided: `X_WIDTH` and `Y_WIDTH` are computed per topology on **both** `ref_model/` and `rtl/`,
bit for bit, rather than fixed at 4 each.

The accepted cost, recorded so it is not rediscovered:

| | today | after |
|---|---|---|
| `ni_flit_constants.h` | one global file | one per topology |
| `NOC_REQ/RSP/DAT_FLIT_WIDTH` | fixed | per topology |
| `FlitMarshalT<WIDTH_BITS>` | one instantiation, `dpi_marshal.hpp:107-109` | per topology |
| c_model binary | serves every topology | built per topology |
| ctest | one run | multiplied by topology count |

What it buys: a 4x4 mesh carries `2 + 2` coordinate bits per id instead of `4 + 4`, so `REQ`
drops 8 b, and the 16x16 ceiling stated in `constants.yaml` stops being a fixed property of the
flit.

## Validation

The dummy peripheral is a `user_node_endpoint` with `axi_sim_mem` behind its NSU and a file master
on its NMU, identical to a compute node. Two directed checks:

| # | check | new today |
|---|---|---|
| 1 | a tile writes the peripheral's window and reads it back | no, this is an ordinary SAM target |
| 2 | the peripheral writes a tile's window and reads it back | **yes, nothing outside the mesh originates traffic today** |

Check 2 is the round's deliverable. A run where only check 1 passes has not exercised the
mechanism.

The unpopulated-boundary `$fatal` needs a fault-injection proof it still fires once boundary ports
are conditionally wired, per the standing rule that a checker's silence counts only after it has
been shown to fire.

## Out of scope

Which peripherals, how many, and on which edges. The mechanism must not assume an answer.

`COLLECTIVE_MASK_WIDTH` is 8, so a mesh plus a populated border can exceed what one multicast mask
covers. Peripherals are not multicast targets in this round and the interaction is not designed
here.

`hotspot_boundary` becomes meaningful once an edge has a target, but the pattern is not restored
in this round.
