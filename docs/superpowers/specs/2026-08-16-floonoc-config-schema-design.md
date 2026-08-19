# FlooNoC-shaped Configuration Schema — Design

**Goal:** replace this project's topology YAML with FlooNoC's configuration schema, so
one testbench serves many configurations and every configuration is described by one
declarative config file.

**Principle, stated by the owner:** copy FlooNoC as far as it goes; invent as little as
possible. Every field below is either lifted from FlooNoC or listed in "Additions" with
the reason FlooNoC cannot express it.

**Sources.** FlooNoC facts are read from the local checkout at `E:\05_NoC\FlooNoC`
(`floogen/model/*.py`), not from the published documentation. An earlier draft of this
spec cited a GitHub survey and carried five wrong field claims; every citation here was
re-derived from that checkout.

**Status:** design. Not approved. The "Decisions required" section is the gate.

---

## 1. What this replaces

Today: eight hand-written testbenches under `sim/tb/`, a four-field `ifeq` table in
`sim/build_config.mk` mapping a configuration to its geometry, VC count, RoB mode and
YAML, and a topology YAML that hand-lists 32 address tiles per 4x4 mesh.

**Each testbench differs from its nearest sibling by exactly one line**, and from a distant
one by at most three. Two configurations of different geometry differ in their `import`;
two of the same geometry differ in `DAT_NUM_VC` or in `READ_ROB_ENABLED`;
`tb_mesh_2x2_vc1` against `tb_mesh_4x4_vc8_robless` differs in all three. Verified by
diffing with comments and blank lines stripped. An earlier draft said "every pair differs
by exactly one line", which is false across the whole set. Both peripheral and non-peripheral wrappers pass
`N_PERIPH`, `PERIPH_NODE` and `PERIPH_PORT`; on a non-peripheral configuration the package
supplies zeros, so those parameters are not a difference between them.

| element | now | after |
|---|---|---|
| testbench | 8 files, 53-65 lines each | 1 file |
| shared body | `sim/tb/noc_tb_top.sv` | unchanged, becomes *the* testbench |
| fabric | `ref_model/top/noc_fabric.sv` | unchanged |
| config → build mapping | `ifeq` table, 4 fields | deleted; the config file is the mapping |
| address map | 32 hand-listed tiles | 2 array-expanded ranges |
| VC count, RoB mode | testbench localparams | `constants.yaml` only — see decisions 3 and 4 |

---

## 2. Copied from FlooNoC

| element | source | what we take |
|---|---|---|
| `Network` | `model/network.py:54-62` | `name`, `description`, `network_type`, `protocols`, `endpoints`, `routers`, `connections`, `routing` |
| `name` names the package | `network.py:47` — "used to name the generated files (e.g. `floo_<name>_pkg.sv`)" | several config files sharing a `name` share a package. **This is the mechanism that lets one testbench serve many configs.** |
| `RouterDesc` | `model/router.py:15-20` | `name`, `array`, `tree`, `xy_id_offset`, `auto_connect: bool = True`, `degree` |
| automatic mesh links | `model/graph.py`, via `Graph.add_nodes_as_array(..., connect=auto_connect)` | a 2D router array gets N/E/S/W inter-router links with **no `connections:` entry**. Only endpoint↔router links are listed. |
| `EndpointDesc` | `model/endpoint.py:17-25` | `name`, `description`, `array`, `num`, `addr_range: List[AddrRange]`, `xy_id_offset`, `mgr_port_protocol`, `sbr_port_protocol` |
| subordinate test | `endpoint.py:87-89` — `is_sbr()` is `sbr_port_protocol is not None` | an endpoint contributes to the SAM only if it declares `sbr_port_protocol` |
| `AddrRange` | `model/routing.py:382-391` | `start`, `end`, `size`, `base`, `arr_idx`, `arr_dim`, `rdl_name`, `rdl_as_mem`, `en_collective: bool = False`, `desc` |
| array expansion | `routing.py:440-449` — `start = base + size * (m * arr_dim[1] + n)` | one range declaration expands to N per-instance ranges over an `array:` endpoint, row-major |
| SAM derivation | `network.py:686` `gen_sam()` | the address map is derived from endpoint declarations, not hand-listed; upstream `RouteMap` rejects overlapping ranges, but the later approved project contract deliberately does not adopt that rejection: project overlap is legal and authored-first |
| `en_collective` | `routing.py:377` — "marks this range as a multicast/collective destination" | replaces our AWUSER-mask-only expression of the same idea |
| `ConnectionDesc` | `model/connection.py:19-31` | `src`, `dst`, `src_range`, `dst_range`, `src_idx`, `dst_idx`, `src_lvl`, `dst_lvl`, `dst_dir: int`, `src_dir: int`, `allow_multi`, `bidirectional` |
| `XYDirections` | `routing.py:245-252` | `NORTH=0, EAST=1, SOUTH=2, WEST=3, EJECT=4` — `dst_dir`/`src_dir` are these values |
| `RouteAlgo` | `routing.py:24-37` | `XY`, `YX`, `ID`, `SRC`. **There are no mirrored variants**; an earlier draft claimed otherwise. |
| `Routing` | `routing.py:798-841` | `route_algo`, `use_id_table`, `rob_idx_bits`, `port_id_bits`, `num_vc_id_bits`, `decouple_rw`, `vc_impl`, `collective`. The class is named `Routing`, not `RoutingDesc`; an earlier draft had the wrong name. |
| `use_id_table` names our `decode` modes | `routing.py:804` | with `XYRouting` and `use_id_table: false`, "the XY coordinates are automatically derived as address offsets... requires that all endpoint addresses are contiguous and of the same size". Same intent as our `decode: table\|offset`, and `addr_offset_bits` comes with it — but **not the same constraints**; see 3.5. |

**FlooNoC's own method for derived values.** `Routing` holds authored fields and
elaboration-filled ones in one model, the latter `Optional` with `None` defaults:
authored are `route_algo`, `use_id_table`, `rob_idx_bits`, `port_id_bits`,
`num_vc_id_bits`, `decouple_rw`, `vc_impl`, `collective`; derived are `sam`, `table`,
`num_endpoints`, `num_id_bits`, `num_x_bits`, `num_y_bits`, `num_route_bits`,
`addr_width`, `collective_sam`. Any field we add follows that shape.

**FlooNoC's `Network` has no `params` field.** An earlier draft copied one; it does not
exist. Both `Network` (`network.py:52`) and `AddrRange` (`routing.py:380`) set
`extra="forbid"`, so every addition below is a real model change, not a free-form key.

---

## 3. Additions — two fields FlooNoC cannot express, one deviation our own decision creates,
and one guarantee FlooNoC does not carry

### 3.1 `space` on `AddrRange`

**FlooNoC has no semantic region-kind field.** `AddrRange` carries `desc`, `rdl_name`,
`rdl_as_mem` — free text and SystemRDL codegen metadata, no NoC-level meaning
(`routing.py:382-391`).

**Why we need it.** `docs/noc-target-spec.md` §5 makes the SAM's address space select the
AXI class: config space selects Narrow, memory space selects Data, independently of
destination decode. The model implements exactly that — `sam_yaml.hpp` parses
`tile.space`; `addr_trans.hpp` stores both `Space` and `AxiClass`; `SamTable::translate()`
returns the matched entry's class; `axi/types.hpp` records that the class is resolved once
at NMU packetize and carried in the flit's `axi_ch` so the response path never decodes an
address again.

**Why block-granular routing is insufficient.** The NMU must choose Narrow or Data *before*
the request reaches the destination tile's local crossbar. A single per-node block range
loses that too early. Collectives tighten it: `collective_translate()` validates
eligibility, coordinate fields and burst footprint against the request's space, and
`NsuWrap` loads per-space coordinate ranges for address rebasing.

Values `memory`, `config`, `peripheral`; default `memory` when absent, matching
`address_map.py`'s `t.get("space", "memory")`.

### 3.2 `stride` on `AddrRange`

FlooNoC's expansion places member `k` at `base + size * k` (`routing.py:449`) — stride
equals size. Our stride is `block_size` (`0x100000000`) while the memory aperture is
`0x2000000` and the config aperture `0x1000`, two apertures per node at different offsets
within the block.

`stride` keeps FlooNoC's array mechanism rather than discarding it; the alternative is
hand-listing 32 ranges, which is what we are trying to stop doing. Absent, it defaults to
`size` — FlooNoC's existing behaviour — so a config that does not need it reads exactly as
a FlooNoC config does.

### 3.3 `num_vc` — withdrawn

An earlier draft made this a config-file field. **It is not.** The owner's rule places a
DUT parameter where it is defined, and `noc.DAT_NUM_VC` is already defined in
`specgen/source/constants.yaml` (default 1, allowed up to 8). The config files do not
carry it; changing the VC count is an edit to that file and a rebuild.

The FlooNoC evidence that made this look like an addition still holds — FlooNoC has no
authored N-way VC count, only a header field width (`routing.py:836`) and an RTL-derived
count capped at 2 (`hw/floo_nw_router.sv:90-91`) — but the conclusion drawn from it was
wrong. Nothing is added to the schema. See decision 3 and its cost in 8.1.

The RoB mode is withdrawn for the same reason; see decision 4.

### 3.4 Peripheral attachment — withdrawn, with one constraint on the reader

An earlier draft made this a fourth field. Decision 6 settled it as FlooNoC's existing
`dst_dir`, read as *which port* rather than as a coordinate offset. No field is added.

**The constraint that makes this safe.** FlooNoC uses `dst_dir` two ways: as a directed
router port (`network.py:411-434`) and, for a non-router NI, to derive that NI's
coordinate through `XYDirections.to_coords()` (`network.py:354-363`, `routing.py:260-267`).
Our peripherals share their host router's coordinate, so **our reader must take the port
meaning and must NOT apply the coordinate derivation** for a peripheral endpoint. Doing
both would place the peripheral one step outside the mesh. FlooNoC's own SAM uses the NI
id rather than `dst_dir` when it builds address rules (`network.py:686-699`), so nothing
downstream of ours depends on the offset either.

This also retires a latent defect: `noc_fabric.sv`'s `periph_rp()` currently derives the
direction from the coordinate and assumes the host router sits on the named edge, so an
interior coordinate resolves silently to EAST or NORTH.

### 3.5 Offset-mode validation

`use_id_table: false` gives us the *mode*; it does not give us its guarantees. FlooNoC's
non-table path is a bit-slice and nothing more — `id_o.x = addr_i[XYAddrOffsetX +: ...]`,
`id_o.y = addr_i[XYAddrOffsetY +: ...]` (`hw/floo_id_translation.sv:74-77`). No space
concept, no coverage check, no uniform-aperture validation, no ordering validation.

Our offset mode requires that every present tile space has declared coordinate ranges and
that all spaces share one range pair (`sam_yaml.hpp:101-117`), and those ranges come from
`declare_space_coords`, which validates origin, dimensions, uniform stride, uniform
aperture, lookup reachability, and raster-order `dst_id` (`addr_trans.hpp:242-290`).

So the field name and the two modes are copied; the validation behind `offset` is ours and
has to survive the migration. It is the constraint that keeps `stride` (3.2) from being set
inconsistently across a node's two ranges — the invariant §5 flags.

### 3.6 Protocol labels become presence markers

Decision 2 omits `protocols:`, but `EndpointDesc.mgr_port_protocol` /
`sbr_port_protocol` cannot go with it: `is_sbr()` is `sbr_port_protocol is not None`
(`endpoint.py:87-89`), and `gen_sam()` walks only subordinate endpoints, so an endpoint
without it contributes no SAM rules at all.

FlooNoC resolves those labels against the `protocols:` list (`network.py:477-479`,
`:494-496`). With the list gone, they resolve to nothing. So they become **presence
markers our reader deliberately ignores** — a real deviation from the schema we are
copying, created by our own decision rather than by a gap in FlooNoC, and listed here so
it is not mistaken for an oversight. The reader must carry an explicit line saying so, or
the next reader will look for a `protocols:` block that was never written.

---

## 4. Consumers — what must keep working

Two readers. The second is why a generate-time-only answer is not an answer.

| reader | file | when | reads |
|---|---|---|---|
| Python | `sim/tools/address_map.py` `pack(address_map, x_dim, y_dim)` | generate time | **only the address map** — `tiles[].{x,y,size,space}`, `block_size`, `peripherals[].{x,y,face,size}`. Dimensions arrive as arguments, not from the YAML. |
| Python | `sim/tools/gen_tb_top.py` | generate time | `topology.num_vc` (`:599`, `:688`), plus the dimensions it passes to `pack()` |
| **C++ SAM** | `ref_model/c_model/include/nmu/sam_yaml.hpp` `load_sam_table` | **simulation runtime, via `+sam_config`** | `topology.{x_dim,y_dim}` (`:147-154`) and `address_map` (`:166-208`). **Not** `topology.name`, **not** `topology.num_vc`. |

Downstream of `pack()`, unchanged in contract: `gen_test_patterns.py` derives per-node
destination addresses, `gen_dma_jobs.py` derives DMA job addresses, `gen_tb_top.py` emits
the topology package the testbench imports.

---

## 5. Established, not open

**The RoB mode lives in `constants.yaml`** (decision 4), and FlooNoC's own method agrees:
`rob_idx_bits` is in `Routing` (`routing.py:833`), but the actual RoB enable/type is a
**chimney** parameter (`hw/floo_nw_chimney.sv:43-45`, `docs/floonoc/chimneys.md:66-68`) —
the chimney being FlooNoC's equivalent of our NMU/NSU. Our switch is likewise an NMU
parameter (`nmu.hpp:161`, `:245`).

**`block_size` carries a cross-space invariant.** `docs/noc-target-spec.md:363-365` and
`:523-524` state the node stride is one value shared by every space. Turning it into a
per-range `stride` is only safe if that invariant stays explicit somewhere — otherwise two
ranges of one node could be given different strides and nothing would object.

---

## 6. Worked example

`sim/configs/mesh_4x4.yml`. FlooNoC field names throughout; the three additions marked. No
`num_vc`, no RoB mode, no `protocols:` — all three live in `constants.yaml`.

```yaml
name: mesh_4x4
description: "4x4 mesh of tiles"
network_type: "axi"            # decision 1

routing:
  route_algo: "XY"
  use_id_table: true           # our `decode: table`. false + addr_offset_bits is `offset`,
                               # whose validation is ours to keep — see 3.5
  collective:
    en_narrow_multicast: true
    en_wide_multicast: true

endpoints:
  - name: "tile"
    array: [4, 4]
    mgr_port_protocol: ["axi"]           # see 8.2 — the name resolves to nothing now
    sbr_port_protocol: ["axi"]           # required: is_sbr() gates SAM participation
    addr_range:
      - base: 0x0
        size: 0x2000000
        stride: 0x100000000              # ADDITION 3.2
        space: memory                    # ADDITION 3.1
        en_collective: true
      - base: 0x2000000
        size: 0x1000
        stride: 0x100000000
        space: config
        en_collective: true

routers:
  - name: "router"
    array: [4, 4]
    degree: 5
    # auto_connect defaults true: N/E/S/W inter-router links are implicit

connections:
  - src: "tile"
    dst: "router"
    src_range: [[0, 3], [0, 3]]
    dst_range: [[0, 3], [0, 3]]
    dst_dir: 4                           # XYDirections.EJECT
```

A peripheral configuration adds a second endpoint, attached by an explicit direction:

```yaml
endpoints:
  - name: "peripheral"
    num: 4
    sbr_port_protocol: ["axi"]
    addr_range:
      - base: 0x1000000000        # 4x4 value; a 2x2 mesh's is 0x400000000
        size: 0x100000
        stride: 0x100000
        space: peripheral
        # no en_collective: peripherals are unicast destinations only

connections:
  - src: "peripheral"
    dst: "router"
    src_idx: [0, 1, 2, 3]
    dst_idx: [4, 11, 1, 14]
    dst_dir: 3                 # WEST port on this router, not a coordinate offset
```

---

## 7. Config file layout

With the VC count and the RoB mode in `constants.yaml`, one file describes one geometry:

```
sim/configs/
    mesh_2x2.yml   mesh_2x2_periph.yml   mesh_4x4.yml   mesh_4x4_periph4.yml
```

FlooNoC keeps its configs flat in `floogen/examples/`. Whether we add a directory layer
is decision 7 — with one file per geometry there is nothing for a directory to group.

---

## 8. Decisions — settled

The owner settled these one at a time. The rule they articulated governs the whole
schema:

> **A parameter is defined in exactly one file.** A value that varies per configuration
> is selected in the config file; a value that does not vary lives only in
> `specgen/source/constants.yaml`. Duplicating a definition across files is not allowed.

| # | decision | resolution |
|---|---|---|
| 0 | do we run floogen? | **No.** We adopt the schema shape and write our own readers. FlooNoC's pydantic validation is a reference, not a runtime constraint — so `network.py:395-405`'s rejection of `axi` with a nonzero `num_vc_id_bits` does not bind us. `noc_fabric.sv` and `noc_tb_top.sv` are kept. |
| 1 | `network_type` | **`axi`** — determined by fact, not preference. `constants.yaml:60-70` records that the shared endpoint carries the data class at 512 b and **the narrow class rides the addressed 8 B lane of the same port**. One AXI interface per endpoint. FlooNoC's `narrow-wide` means two separate interfaces (occamy's `mgr_port_protocol: ["narrow_in", "wide_in"]`), which we do not have. |
| 2 | `protocols:` | **Omitted.** The four AXI widths do not vary per configuration, so they stay in `constants.yaml` as the single source with its drift gate. FlooNoC makes the block required; we deviate, because copying it would put the same numbers in nine files with nothing comparing them. The same reasoning removes `rob_idx_bits`, `port_id_bits` and `num_vc_id_bits` from `routing:`: the flit header layout does not vary per geometry and is already defined in `specgen/generated/json/ni_packet.json` (`VC_ID_WIDTH: 3`, `DST_PORT_ID_WIDTH: 2`, `SRC_PORT_ID_WIDTH: 2`). FlooNoC carries one `port_id_bits` where we carry a separate dst and src width, so the field was never a one-to-one copy either. `route_algo` and `use_id_table` stay — they describe the configuration and have no specgen definition. |
| 3 | `num_vc` | **`constants.yaml` only. Not in the config files.** VC count is a DUT parameter, and the owner's rule puts it where it is defined. |
| 4 | RoB mode | **`constants.yaml` only**, by the same rule. FlooNoC's own precedent agrees: its RoB enable/type is a chimney parameter (`hw/floo_nw_chimney.sv:43-45`), the chimney being its NMU/NSU, and `constants.yaml` already has an `nmu:` section. |
| 5 | grouping key | **Superseded.** With `num_vc` and the RoB mode out of the config files, the eight configurations collapse to four geometries and there is one file per geometry. Nothing is left to group. See 8.1. |
| 6 | peripheral attachment | **`dst_dir`, no new field.** Our reader reads it as *which port* rather than as a coordinate offset. This also removes a latent defect: `noc_fabric.sv`'s `periph_rp()` currently derives the direction from the coordinate and assumes the host router sits on the named edge, so an interior coordinate resolves silently to EAST/NORTH. An explicit direction retires that derivation. Cost: one field whose meaning differs from FlooNoC's — recorded in the reader, not left implicit. |
| 7 | config directory layer | **Flat**, as FlooNoC keeps `floogen/examples/`. Four files, no directories. |
| 8 | the VC sweep | **The VC dimension is removed.** `sim-injection-sweep` sweeps injection rate only, at whatever VC count `constants.yaml` currently holds. Four curves means four edits and four sweeps. This works without accumulation logic because each run writes its own `result.csv` and `plot_injection_sweep.py` globs them. |

### 8.1 What decisions 3 and 4 cost

Four config files replace eight configurations:

```
sim/configs/mesh_2x2.yml   mesh_2x2_periph.yml   mesh_4x4.yml   mesh_4x4_periph4.yml
```

Three consequences follow, and each needs handling in the plan rather than discovering:

1. **`sim-injection-sweep` breaks.** It sweeps `SWEEP_VCS ?= 1 2 4 8` in one command
   (`sim/Makefile`). With the VC count in a tracked file, a sweep would have to edit
   `constants.yaml` four times mid-run. It needs a different shape.
2. **The RoB/RoBless comparison changes form.** `tb_mesh_4x4_vc8_robless.sv`, added in
   this campaign, stops being a configuration; comparing the two modes becomes two
   edits and two builds rather than two runs.
3. **The `ifeq` table disappears entirely** — `TB_NUM_VC`, `TB_READ_ROB` and the
   `TB_TOPOLOGY_YAML` field added this campaign all lose their reason to exist. This is
   the intended outcome, reached sooner than planned.

---

## 9. Migration impact

| area | work |
|---|---|
| schema | write the config files; extend `Network` and `AddrRange` (both `extra="forbid"`) |
| Python reader | rewrite `address_map.py`'s `pack()` for array-expanded ranges |
| **C++ reader** | rewrite `sam_yaml.hpp`'s `load_sam_table` for the same, **including array expansion at runtime** |
| testbench | collapse eight testbenches to one; delete the `ifeq` table entirely, including the `TB_NUM_VC`, `TB_READ_ROB` and `TB_TOPOLOGY_YAML` fields |
| build | config selection replaces `TB=`; package name comes from `name:` |
| sweep | remove the VC dimension from `sim-injection-sweep` (decision 8) |
| constants | `num_vc` and the RoB mode become the only home for those two values; the testbench stops declaring them |
| docs | `README.md`, `docs/verification-environment.md`, `docs/nmu-spec.md`, `docs/router-spec.md` |

### The largest risk, named

**The C++ runtime reader's array-expansion parity with the Python one.** Not the
testbench deletion, which fails loudly at compile time. `sim/tools/address_map.py:1-5`
states that it mirrors the C++ SAM contract; the runtime loads the SAM from YAML through
`+sam_config`. If the two expansions diverge, the generated package and the generated
stimulus agree with each other and disagree with the runtime address translator — a run
that decodes to the wrong node while every artifact looks self-consistent.

Any plan built on this spec must gate that parity directly rather than inferring it from
a passing run: expand the same config with both readers and compare the resulting rule
sets, rather than trusting a scoreboard that only sees the two halves that agree.

Survives unchanged: the `sim-gen` / `sim-build` / `sim-run` split, the tracked stimulus
example, `ref_model/top/noc_fabric.sv`, `sim/tb/noc_tb_top.sv`.

Obsoleted: the eight testbenches and the `ifeq` table, roughly 450 lines. Also obsoleted,
sooner than this campaign planned: `tb_mesh_4x4_vc8_robless.sv` and the fifth table field,
both added earlier in this same branch.
