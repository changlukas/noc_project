# RTL architecture contract

This directory contains synthesizable production RTL only. The contract in this file is frozen
before implementation starts; the behavioral details remain authoritative in
`docs/nmu-spec.md`, `docs/nsu-spec.md`, and `docs/router-spec.md`. Interface sketches under
`docs/rtl/` are diagrams, are not build inputs, and do not override this contract.

No empty, tied-off, or non-elaborating module is accepted as a package deliverable. Each module
below enters `rtl/` only with its functional implementation, elaboration guards, and package DV.
One module lives in one file, under `nmu/`, `nsu/`, `router/`, or `common/`.

## Canonical parameters

`specgen/source/constants.yaml` is the source of truth. Code generation emits
`ni_params_pkg.sv` and `ni_params.h`; production RTL consumes the SV symbols. A wrapper-local
name such as `DAT_NUM_VC` is an alias of the canonical `NOC_DAT_NUM_VC` value, not a second
parameter source.

| Canonical parameter | Default | Legal values | Contract |
|---|---:|---|---|
| `AXI_ID_WIDTH` | 3 | 1..8 | REQ width is `133 + AXI_ID_WIDTH`; RSP width is `123 + AXI_ID_WIDTH`; DAT is 633 throughout the legal range |
| `NOC_DAT_NUM_VC` | 2 | 1..8 | DAT VC count and credit-vector width; REQ and RSP remain single-VC |
| `NOC_DAT_VC_MODE` | `NOC_DAT_VC_MODE_SHARED` (0) | `NOC_DAT_VC_MODE_SHARED` (0), `NOC_DAT_VC_MODE_READ_WRITE_SPLIT` (1) | One system-wide elaboration choice for NI allocation and DAT Router VA |
| `AXI_FIFO_DEPTH` | 8 | power of two, >= 2 | Common depth of the five AXI-channel CDC FIFOs in each NI |
| `NOC_FIFO_DEPTH` | 8 | positive power of two | Common depth of the REQ, RSP, DAT Write, and DAT Read class FIFOs in each NI |

`READ_WRITE_SPLIT` additionally requires `NOC_DAT_NUM_VC` in {2, 4, 6, 8}; the lower half is
eligible for `DataAw`/`DataW` and the upper half for `DataR`. Every production top fails
elaboration for an illegal value, an illegal mode/count combination, or a generated flit/credit
type whose width disagrees with these parameters. Flit-width parameters are derived values in
production RTL, not independently tunable knobs.

## Production tops

The production module names are `nmu`, `nsu`, and `router`. Their signal names and packed AXI
types follow the corresponding `*_wrap.sv` functional ports. The following are the only deltas
from the current single-clock model wrappers:

- `nmu` and `nsu` expose `ACLK`, `ARESETn`, `noc_clk`, and `noc_rst_n` instead of `clk_i` and
  `rst_ni`.
- No production top has `ctx_i` or any DPI port.
- NMU and NSU DAT receive replace `rx_dat_crdvalid_o` with scalar `rx_dat_ready_o`; DAT transmit
  retains `tx_dat_crdvalid_i`.
- Router DAT credit ports remain per-VC on N/E/S/W. At LOCAL output only,
  `tx_dat_crdvalid[LOCAL]` is replaced by scalar `tx_dat_ready_local`; LOCAL input still returns
  `rx_dat_crdvalid[LOCAL]` to the injecting NI.

| Top | Functional faces | Public production parameters |
|---|---|---|
| `nmu` | AXI slave `axi_req_i`/`awuser_i`/`axi_rsp_o`; TX REQ ready/valid; RX RSP ready/valid; TX DAT credit; RX DAT ready/valid | canonical AXI widths, `NOC_DAT_NUM_VC`, `NOC_DAT_VC_MODE`, `AXI_FIFO_DEPTH`, `NOC_FIFO_DEPTH`, NMU RoB/order parameters, AW/AR SAM register type |
| `nsu` | RX REQ ready/valid; TX RSP ready/valid; RX DAT ready/valid; TX DAT credit; AXI master `axi_req_o`/`axi_rsp_i` | canonical AXI widths, `NOC_DAT_NUM_VC`, `NOC_DAT_VC_MODE`, `AXI_FIFO_DEPTH`, `NOC_FIFO_DEPTH`, Response Queue parameters |
| `router` | five ports in fixed order LOCAL, NORTH, EAST, SOUTH, WEST; per-port REQ/RSP ready/valid; N/E/S/W DAT credit; LOCAL DAT asymmetry above | `NOC_DAT_NUM_VC`, `NOC_DAT_VC_MODE`, Router VC/output depths, mesh dimensions and this Router's coordinates; port count is fixed at five |

Topology and SAM records are generated elaboration-time constants. They are not runtime pins.
Production tops use ANSI port lists and flat, explicit clock/reset ports; internal stream records
may be typed, but no child exposes a DPI or verification-only type.

## Generated topology and SAM contract

The selected `sim/configs/<CONFIG>.yml` `endpoints:` block is the sole source of
SAM rules. The generator writes exactly one per-configuration package to
`$(BUILD_ROOT)/generated/<CONFIG>/topology_pkg.sv`. This file is a build
artifact: it is never committed, and no per-configuration golden RTL is kept in
the repository. Existing topology constants and the SAM declarations below
share this package, so one generated unit owns the complete elaboration-time
topology contract.

The package exports these exact public declarations:

| Declaration | Required shape |
|---|---|
| `SAM_NUM_RULES` | Number of expanded authored address ranges. |
| `SAM_MASK_SEL_WIDTH` | `$clog2(ni_flit_pkg::AXI_ADDR_WIDTH + 1)`; large enough to encode an address-bit offset or length. |
| `sam_mask_sel_t` | Packed struct with `offset` and `len`, both `SAM_MASK_SEL_WIDTH` wide. |
| `sam_idx_t` | Packed struct with `dst_id`, `dst_port_id`, `is_data`, `collective_en`, `mask_x`, and `mask_y`; `mask_x` and `mask_y` are `sam_mask_sel_t`. There is no `base_id`. |
| `sam_rule_t` | Packed struct with `idx`, `start_addr`, and exclusive `end_addr`. |
| `SAM` | `sam_rule_t [SAM_NUM_RULES-1:0]` constant rule array. |

Every field width comes from the canonical generated address, destination,
port, traffic-class, X-coordinate, and Y-coordinate definitions. The generator
shall not emit numeric width guesses or add public defaults. It shall diagnose
any mismatch between `$bits` of the package fields and those canonical types.
`is_data` is the one-bit projection of the canonical two-class Narrow/Data
encoding; it is not a separately sized public parameter. The destination layout
remains the canonical `{Y, X}` encoding, so generation also checks that the
destination width equals the generated X plus Y widths.

Rule expansion preserves YAML authorship order: endpoint declarations first,
then each endpoint's `addr_range` order, then array members in increasing
X-fast member order. The pinned `addr_decode` gives the highest matching array
index priority, so authored rule `i` is emitted at
`SAM[SAM_NUM_RULES-1-i]`. Overlap is legal; this reversal makes authored rule 0
win deterministically. Generation fails for a zero or negative size, a base or
size that is not 4 KiB aligned, an unrepresentable or overflowing base, end, or
stride expansion, `start_addr >= end_addr`, or a reference whose generated
destination, port, class, or endpoint membership is invalid. Overlap alone is
never a generation error. An unmapped address has no fallback rule.

The pinned decoder's functional priority is normative. Its simulation-only
dynamic checker still diagnoses a multiple match; `docs/known-limitations.md`
records the follow-on selective assertion-handling blocker. Implementations may
not resolve that tooling conflict by rejecting overlap, globally disabling
assertions, or substituting another decoder.

Coordinate selectors are emitted only when that YAML range explicitly contains
`en_collective: true`. For `false` or an absent key, the generated rule has
`collective_en == 0` and both selectors are exactly `{offset: 0, len: 0}` even
when its stride would make a layout inferable. A collective-enabled range whose
X/Y layout cannot be represented by the canonical address and coordinate widths
is a generation error.

### `ni_sam` wrapper contract

`rtl/common/ni_sam.sv` is the only SAM lookup wrapper. NMU and NSU instantiate
the same module. It has no clock, reset, state, queue, or ready/valid channel;
`lookup_en_i` merely qualifies a pure-combinational lookup.

The module receives `SAM_NUM_RULES`, `parameter type addr_t`, `parameter type
sam_mask_sel_t`, `parameter type sam_idx_t`, `parameter type sam_rule_t`, and the
typed constant `SAM` as required elaboration-time parameters, without functional
defaults. Its ports are:

```systemverilog
input  addr_t     addr_i,
input  logic      lookup_en_i,
output sam_idx_t  sam_idx_o,
output logic      lookup_valid_o,
output logic      lookup_error_o
```

The wrapper instantiates the production-pinned `common_cells` `addr_decode`,
passing `SAM` as `addr_map_i`, disabling its default index, and tying its default
index to zero. It does not reimplement comparison or priority logic. The
externally visible truth table is fixed:

| `lookup_en_i` | Match | `sam_idx_o` | `lookup_valid_o` | `lookup_error_o` |
|---:|---:|---|---:|---:|
| 0 | don't care | zero | 0 | 0 |
| 1 | yes | matched `idx` | 1 | 0 |
| 1 | no | zero | 0 | 1 |

Elaboration checks require `SAM_NUM_RULES > 0` and type compatibility between
`sam_rule_t`, `sam_idx_t`, `sam_mask_sel_t`, and the generated canonical types.
All owners pass the rule count, SAM-related parameter types, and `SAM` through
the hierarchy as elaboration-time parameters; no topology signal or writable
SAM storage is permitted.

NMU contains independent AW and AR `ni_sam` instances. Their result travels
with the corresponding request through the existing AW/AR timing cut;
`nmu_packetize` consumes `dst_id`, `dst_port_id`, and `is_data`. AW collective
handling may also consume `collective_en`, `mask_x`, and `mask_y`; W uses the
stored AW transaction metadata and never performs a second lookup.

NSU contains one `ni_sam` instance owned by `nsu_depacketize`. It enables the
lookup only for a multicast AW request and consumes only the coordinate-layout
result. Unicast AW, W, AR, and response traffic do not enable this lookup. A
multicast AW miss, or a hit with `collective_en == 0`, is an invalid incoming
packet rather than a default mapping.

Compilation order is `ni_params_pkg.sv`, `ni_flit_pkg.sv`, the selected generated
`topology_pkg.sv`, `rtl/common/ni_sam.sv`, and then its consumers. `make
clean-generated` removes `$(BUILD_ROOT)/generated/`, including every generated
`topology_pkg.sv`.

### Generator acceptance vectors

Generator tests use semantic assertions rather than committed golden RTL:

- `mesh_2x2.yml` expands to eight rules. Authored memory rule 0 is `SAM[7]`
  with `[0x000000000, 0x002000000)`, destination 0, port 0, `is_data == 1`,
  `collective_en == 1`, `mask_x == '{offset: 32, len: 1}`, and
  `mask_y == '{offset: 33, len: 1}`. Authored configuration rule 4 is `SAM[3]`
  with `[0x002000000, 0x002001000)`, destination 0, port 0, and
  `is_data == 0`.
- `mesh_4x4.yml` expands to 32 rules. Authored memory rule 0 is `SAM[31]` with
  X/Y selectors `{32,2}` and `{34,2}`. Authored memory rule 6 is `SAM[25]`
  with `[0x600000000, 0x602000000)` and destination `{Y:1, X:2}`. Authored
  configuration rule 16 is `SAM[15]` with
  `[0x002000000, 0x002001000)`, destination 0, port 0, and `is_data == 0`.
- A synthetic broad authored rule 0 overlapping a narrower authored rule 1
  shall decode the overlap to rule 0. Separate cases cover a miss and every
  invalid-range category above; explicit `false` and absent `en_collective`
  cases assert zero-length selectors.
- Repeated generation of either checked-in mesh is byte-identical. Tests parse
  or compile the generated package and assert declarations and field semantics;
  they do not compare against a committed per-configuration RTL file.

## Clock and reset ownership

System integration owns the common active-low reset and its two reset synchronizers. It supplies
`ARESETn` and `noc_rst_n`; both assert asynchronously and each deasserts synchronously to its own
clock. Release skew is legal. There is no `sys_rst_n` port, one-sided recovery, or in-flight
replay. Any reset flushes the complete NI transaction state.

| Logic | Clock/reset owner |
|---|---|
| NMU/NSU AXI-side FIFO endpoints | `ACLK` / `ARESETn` |
| NMU/NSU NoC-side FIFO endpoints and all packet, ordering, SAM, credit, and class-queue state | `noc_clk` / `noc_rst_n` |
| Router and every Router child | `noc_clk` / `noc_rst_n` |
| AXI CDC storage and pointer synchronizers | both NI domains through the shared CDC primitive; no other child crosses domains |

AW, W, and AR cross toward the AXI request consumer; B and R cross in the opposite direction.
For NMU this means AW/W/AR cross ACLK-to-noc and B/R noc-to-ACLK. For NSU the directions are
reversed.

## Network ownership

| Network/class | Producer to consumer | Flow control at NI/Router | Storage owner |
|---|---|---|---|
| REQ: `NarrowAw`, `NarrowW`, `NarrowAr`, `DataAr` | NMU -> Router mesh -> NSU | ready/valid | NI REQ class FIFO, then Router input FIFO |
| RSP: `NarrowB`, `NarrowR`, `DataB` | NSU -> Router mesh -> NMU | ready/valid | NI RSP class FIFO, then Router input FIFO |
| DAT Write: `DataAw`, `DataW` | NMU -> Router mesh -> NSU | credit on injection; ready/valid on ejection | NI DAT Write class FIFO; per-VC storage only in Router inputs |
| DAT Read: `DataR` | NSU -> Router mesh -> NMU | credit on injection; ready/valid on ejection | NI DAT Read class FIFO; per-VC storage only in Router inputs |

The NI sender owns the per-VC credit counters and VC selection; each credited slot belongs to the
destination Router input FIFO. Router-to-NI DAT ejection has one transfer authority,
`valid && ready`, and never adds an NI-side per-VC FIFO. N/E/S/W Router links remain symmetric
per-VC credit links.

## Shared foundation packages (Stage 2)

These adapters may map project records and ready/valid names onto the pinned primitive ports. They
must not implement storage arrays, pointers, synchronizers, or a second flow-control policy.

| Package/module | Child contract | State owner |
|---|---|---|
| `common/noc_sync_fifo` | One typed ready/valid input and output, `clk_i`/`rst_ni`, legal depth parameter; wraps the approved synchronous FIFO with non-fall-through behavior | shared primitive |
| `common/axi_async_fifo` | One typed source ready/valid face and one typed destination ready/valid face, separate clock/reset pairs; depth is `AXI_FIFO_DEPTH` and maps to the primitive's log-depth | shared CDC primitive |
| `common/noc_reg_slice` | Typed ready/valid stream; `REG_TYPE=0` bypass, 1 simple stream register, 2 full spill register; illegal values fail elaboration | shared register primitive for types 1/2; no state for type 0 |
| `common/ni_sam` | Qualified address -> typed SAM result plus valid/error; wraps the pinned `addr_decode` | no state; generated constant `SAM` remains the sole rule storage |

Custom reusable FIFO, CDC, or register-slice implementations are forbidden. Block-specific state
such as RoB entries, route latches, credit counters, and protocol FSMs is not a shared primitive
and remains owned by its block. If the pinned library cannot meet a required semantic, the gap,
reset behavior, proposed replacement, DV evidence, and license impact must first be approved in
`docs/trade-off.md`; implementation cannot precede that decision.

## NMU packages (Stage 3)

| Package/module | Inputs -> outputs | Exclusive responsibility |
|---|---|---|
| `nmu/nmu_axi_cdc` | AXI slave records across ACLK <-> noc_clk | Exactly five AXI-channel CDC instances; no SAM, ordering, or packet state |
| `nmu/nmu_sam` | accepted AW/AR -> destination, port, class, collective metadata | Instantiates the shared `ni_sam` for AW and AR, performs burst-footprint and collective validation/translation; AW and AR timing cuts use `noc_reg_slice` |
| `nmu/nmu_rob` | decoded AW/AR and returning B/R metadata -> ordered request/response streams | Per-ID order lists, B/R slot pools, `READ_ROB_ENABLED` behavior, ordering tags, collective admission |
| `nmu/nmu_packetize` | ordered AXI request records -> complete REQ or DAT flit records | Field mapping and AW-to-W metadata inheritance; no VC allocation or NoC queue |
| `nmu/nmu_channel_assign` | packetized requests plus Router credits -> TX REQ/TX DAT | REQ and DAT Write class FIFOs, worm lock through WLAST, mode-eligible DAT VC choice and sender credit counters |
| `nmu/nmu_depacketize` | RX RSP/RX DAT ready/valid -> decoded AXI B/R records for `nmu_rob` | RSP and DAT Read class FIFOs, channel legality/demux, and payload decode; owns `rx_dat_ready_o` |
| `nmu/nmu` | production faces above | Parameter guards and child wiring only; no duplicate queue or transaction state |

## NSU packages (Stage 3)

| Package/module | Inputs -> outputs | Exclusive responsibility |
|---|---|---|
| `nsu/nsu_depacketize` | RX REQ/RX DAT ready/valid -> AXI AW/W/AR records | REQ and DAT Write class FIFOs, flit validation, class/address reconstruction, AW-to-W association, and multicast-AW-only shared `ni_sam` lookup; owns `rx_dat_ready_o` |
| `nsu/nsu_axi_cdc` | AXI master records across noc_clk <-> ACLK | Exactly five AXI-channel CDC instances; no response metadata |
| `nsu/nsu_response_queue` | issued AW/AR context plus AXI B/R -> restored response context | Downstream-ID allocation/remap, per-ID ordering, burst tracking, retirement only at B or RLAST packet acceptance |
| `nsu/nsu_packetize` | restored B/R context -> complete RSP or DAT flit records | Response field mapping and collective metadata echo; no VC allocation or NoC queue |
| `nsu/nsu_channel_assign` | packetized responses plus Router credits -> TX RSP/TX DAT | RSP and DAT Read class FIFOs, mode-eligible DAT VC choice and sender credit counters |
| `nsu/nsu` | production faces above | Parameter guards and child wiring only; no duplicate queue or transaction state |

## Router packages (Stage 3)

| Package/module | Inputs -> outputs | Exclusive responsibility |
|---|---|---|
| `router/router_route_select` | header and local coordinates -> unicast route, fork mask, or join mask | Pure route/mask computation; no buffering, grant, or credit state |
| `router/router_simple_network` | five REQ or RSP ready/valid inputs -> five outputs | Single-VC input FIFOs, route latch, worm grants, multicast fork; RSP instance also owns CollectB join |
| `router/router_dat_input` | one DAT input link -> per-VC candidate streams and returned credits | Per-VC input FIFO and route latch; credit returns only when a slot is freed |
| `router/router_dat_output` | candidates for one output plus downstream flow control -> one DAT output | Round-robin/worm arbitration, system-wide VC-mode mask and VA, N/E/S/W credit counters, LOCAL ready handling, stage-3 output FIFO |
| `router/router_dat_network` | five DAT inputs -> five DAT outputs | Five input/output children, multicast fork coordination, and no additional storage |
| `router/router` | production five-port faces above | One REQ simple network, one RSP simple network, one DAT network, coordinate/parameter guards; no duplicate route or queue state |

All child boundaries carrying backpressure are typed ready/valid streams. Credit is confined to the
external DAT link and the DAT input/output children that own credit accounting.

## Model, RTL, and hybrid DUT selection

The testbench owns three independent elaboration choices: NMU implementation, NSU implementation,
and Router implementation. Each choice is either `MODEL` or `RTL`; no single global switch is
permitted. The model branch instantiates the existing `*_wrap` and alone allocates/connects its
64-bit `ctx`. The RTL branch instantiates the production top and has no handle signal, dummy port,
or DPI import. Selection logic is verification-only and stays above both branches.

The first NI comparisons are zero-hop compositions:

| Composition | Direct paths | Verification-only DAT adaptation |
|---|---|---|
| RTL NMU -> reference NSU | REQ direct; RTL NMU DAT Write credit path direct; RSP direct | reference NSU DAT Read credit sender -> RTL NMU ready receiver |
| Reference NMU -> RTL NSU | RSP direct; RTL NSU DAT Read credit path direct; REQ direct | reference NMU DAT Write credit sender -> RTL NSU ready receiver |

The DAT adapter is a flow-control bridge only. It preserves every flit bit and the original order,
never changes `vc_id`, never routes, packetizes, merges, forks, or models Router latency. Its
verification FIFO capacity equals the credit capacity advertised to the model sender; a credit is
returned only when that adapter slot is freed by the downstream ready/valid transfer. Overflow,
underflow, or a flit change is fatal. The adapter is excluded from production source lists.

Router signoff is a reference-driven differential harness, not a mesh prerequisite. Identical
flits and legal flow-control events drive reference and RTL instances. N/E/S/W compare directly;
LOCAL DAT ejection is normalized by the same flow-control-only rule above. Comparison is limited
to the conformance matrix in `docs/router-spec.md`; documented intentional cycle differences are
explicit exclusions.

Full mesh is the later integration gate, after both zero-hop NI pairs and the Router differential
harness pass.

## Production primitive dependency

`rtl/Bender.yml` fetches the approved shared primitive library at an exact commit. The production
build obtains its source order and transitive include files from that dependency; no primitive RTL
is copied into this repository. Exact source, revision, selected modules, and license are recorded
in the Provenance section of `docs/verification-environment.md`.
