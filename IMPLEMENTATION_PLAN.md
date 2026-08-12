# NI ID architecture — narrow the NI-facing AXI ID to 3 bits

## Why

`AXI_ID_WIDTH = 8` makes `rob.hpp:264-265` build `2**8 = 256` per-ID order lists, each
`NMU_MAX_TXNS_PER_ID = 32` deep, behind a shared pool of 32. Stage 5d measured the consequence:
at four ids the pool reaches 30 of 32 while the deepest per-ID list reaches 12, so almost all of
that structure is unreachable. In RTL terms it is 256 x 32 entries of order-tracking for a
machine that can have 32 transactions in flight.

FlooNoC does not have this problem because it keeps the NI-facing id narrow. Every shipped
config uses `InIdWidth = 3` (`floo_test_pkg.sv:48,56,65`; `floo_synth_params_pkg.sv:64,83,92`),
and there is no outstanding counter on that path — `axi_aw_queue_ready_in = aw_rob_ready_out`
(`floo_axi_chimney.sv:296`) is the only gate — so its 8 x 32 is genuinely reachable.

| | now | target |
|---|---|---|
| NI-facing AXI id width | 8 b | **3 b** → 8 ids |
| per-ID order lists | 256 | 8 |
| transactions per id | 32 | 32 |
| total outstanding | 32, capped by the shared pool | 256 write, 8 read in `RobMode::Disabled` |

## Stage A — decisions, taken

### `NMU_OUTSTANDING_DEPTH` is removed

FlooNoC's `MaxTxns` and `MaxUniqueIds` are consumed by `floo_meta_buffer`, and the chimney
instantiates that inside `gen_mgr_port` (`floo_axi_chimney.sv:1042-1076`) — the AXI manager port
side, which is our NSU. `NSU_META_BUFFER_MAX_OUTSTANDING = 32` already ports it
(`nsu/meta_buffer.hpp:70-74`). The subordinate-port side, the one that injects into the network
and corresponds to our NMU, has only the RoB ready and the per-ID `MaxTxnsPerId`. So the NMU pool
is ours, and `rob.hpp:196-201` cites the NSU-side structure as its origin.

Bound after removal: `NMU_MAX_TXNS_PER_ID` x `2**AXI_ID_WIDTH` = 32 x 8 = 256 writes, and
1 x 8 = 8 reads in `RobMode::Disabled`, the co-sim default (`nmu.hpp:155-163`), where the read
path takes the single-outstanding interlock instead. Nothing becomes unbounded: a bypassed push
allocates no RoB slot but still takes an order-list entry (`rob.hpp:431,534`), and narrow
bypassed reads take an `ar_lane_meta_` entry (`rob.hpp:529-532`).

Raising it to 256 instead was rejected. Both limits are per-direction, so a pool of 256 equals
the per-ID bound exactly and can never refuse a push the per-ID gate would have allowed.

Keep `write_txns_` / `read_txns_`: they back the retire-side unmatched-response guards
(`rob.hpp:616,629`) and the hwm telemetry. Delete only the two `>= outstanding_depth_` gates
(`rob.hpp:381,460`) and the parameter plumbing.

### `axi_id_remap`, 8 unique ids, sized 5 b -> 3 b

`user_node_endpoint.sv:193` sets `XBAR_SLV_ID_W = ID_WIDTH - 1`, so the tile crossbar's id width
is derived from the NI's. That coupling is what this round removes: `XBAR_SLV_ID_W` becomes
`AXI_INITIATOR_ID_WIDTH` = 4, and the crossbar master port becomes `4 + $clog2(2)` = 5 b, the 32
distinct ids the round assumed but the RTL did not implement. `tile_mst` is declared at `ID_WIDTH`
today (`user_node_endpoint.sv:206`) and moves to 5 b with it; the remap bridges 5 b to the NI.

| parameter | value | why |
|---|---|---|
| `AxiSlvPortIdWidth` | 5 | the crossbar's master-port width |
| `AxiMstPortIdWidth` | `AXI_ID_WIDTH` | 8 during C, 3 after B |
| `AxiSlvPortMaxUniqIds` | 32 during C, 8 after B | `axi_id_remap` requires `AxiMstPortIdWidth >= $clog2(AxiSlvPortMaxUniqIds)` |
| `AxiMaxTxnsPerId` | 32 | matches `NMU_MAX_TXNS_PER_ID`, so the remap table never binds ahead of the NMU. Oversized for `RobMode::Disabled` reads, which hold 1 per id — harmless, the NMU backpressures |

`axi_id_remap` stalls a transaction whose id finds no free downstream id rather than erroring, and
its documentation states that upstream may legally exceed `AxiSlvPortMaxUniqIds` in flight. So 32
folding into 8 is correct by construction and needs no telemetry to justify. `axi_iw_converter.sv:106-109`
selects `axi_id_remap` exactly when `AxiSlvPortMaxUniqIds <= 2**AxiMstPortIdWidth`; the other
branch, `axi_id_serialize`, collapses distinct ids into one order stream and is what this avoids.

**Prerequisite**: `axi_id_remap.sv` is not vendored. `sim/dv/axi-0.39.7/src/` carries only
`axi_id_prepend.sv`. Stage C vendors it and adds it to `sim/build_config.mk`, checking its
`common_cells` dependencies against the existing filelist.

### `remap_downstream_id` collapses to the wrong width

`nsu/meta_buffer.hpp:60-64` collapses the downstream id to all-ones of `AXI_INITIATOR_ID_WIDTH`,
which is 15 and needs 4 b. The NSU master port is `ID_WIDTH` wide, so at 3 b that value does not
fit. FlooNoC sets the non-atomic id to `'1` of the output id width. It becomes all-ones of
`AXI_ID_WIDTH`, and `docs/nsu-spec.md:294`, `docs/known-limitations.md:11` and
`tests/nsu/test_meta_buffer.cpp:72-80` all state the old constant `8'hFF`.

`constants.yaml:36` requires `INITIATOR_ID_WIDTH + $clog2(initiators per tile) <= ID_WIDTH`. After
the remap the two widths sit on opposite sides of a converter and 4 > 3 is correct, so the
constraint becomes `ID_WIDTH >= $clog2(MAX_UNIQUE_IDS)`.

### Open, and Stage D is what closes it

`docs/noc-target-spec.md:126-128` argues `DAT` deadlock freedom from "read data into
reorder-buffer space reserved at request issue". Bypassed reads reserve no slot, so that
invariant does not hold for them, and the shared pool is today the only thing bounding how many
are in flight. In `RobMode::Disabled` the read side is capped at 8 by the per-id interlock, well
under the present pool of 32, so the shipped default is unaffected; the exposure is
`RobMode::Enabled` only, where the read bound goes 32 -> 256. `mesh_4x4_vc4_rob` is the run that
would show it.

## Blast radius

| site | what changes |
|---|---|
| `specgen/source/constants.yaml` | `ID_WIDTH` 8 -> 3 and its §A5.3.5 index-budget comment; `OUTSTANDING_DEPTH` entry deleted; `META_BUFFER_MAX_UNIQUE_IDS` `allowed: [1, 256]` -> `[1, 8]` |
| `axi/types.hpp:25` | `static_assert(AXI_ID_SPACE == 256)` is the tripwire, and updating it is the point |
| `nmu/rob.hpp` | four `AXI_ID_SPACE` arrays 256 -> 8; the two outstanding gates and the ctor argument go |
| `nmu/vc_allocator.hpp:99-100` | two more `AXI_ID_SPACE` arrays |
| `nsu/meta_buffer.hpp:60-64,131-136` | collapsed-id width, three `AXI_ID_SPACE` arrays, the "256 buckets" comment |
| `nsu/depacketize.hpp:63-73` | `max_unique_ids` is legal only as 1 or `AXI_ID_SPACE`, so passthrough becomes 8 |
| `src/dpi/cmodel_dpi.cpp` | `cmodel_nmu_create_ex` loses its `outstanding_depth` argument; id handling stays `uint8_t`, which still holds 3 b |
| `src/sv/*_wrap.sv` | port widths, and with them the contract `rtl/README.md` records |
| generated flit | the id rides the per-channel payload, not the 44 b header — `specgen/generated/json/ni_packet.json:20` and `ni_flit_pkg.sv:78-111,168-195` carry the AW/AR/B/R widths, so `REQ` (137 b) and `RSP` (127 b) move and `DAT` does not |
| `sim/tb/user_node_endpoint.sv` | `XBAR_SLV_ID_W`, `tile_mst` width, `SLV_ID_LIMIT`, the remap instance |
| `sim/tb/axi_vip_types_pkg.sv:13` | VIP id type follows `AXI_ID_WIDTH_DFLT` |
| `sim/tools/gen_tb_top.py:701-766` | the `+outstanding_depth=` plusarg |
| `sim/verilator/Makefile:233-234` | forwards `OUTSTANDING_DEPTH` as a plusarg |
| `docs/nmu-spec.md:155,194,199,309` | the shared outstanding gate, its parameter row, and the `_ex` signature |
| `docs/noc-performance-parameters.md:26-30` | describes the NMU outstanding depth as a live master-side pool |
| `sim/tools/gen_test_patterns.py` | stimulus ids derive from `INITIATOR_ID_WIDTH` and do not change; `--ids-per-initiator` is now bounded by what the remap tracks, not by the id space |

## Stages

**A — decide the two open questions.** Complete, above.

**C — insert `axi_id_remap` at full width.** Vendor `axi_id_remap.sv`; `XBAR_SLV_ID_W` becomes
`AXI_INITIATOR_ID_WIDTH`; `tile_mst` narrows to 5 b; the remap bridges 5 b to `ID_WIDTH` = 8 with
`AxiSlvPortMaxUniqIds = 32`. No behaviour change expected — this is the control run, and Tier 2
must stay green. C precedes B because narrowing `ID_WIDTH` first would take `XBAR_SLV_ID_W` to
2 b under a stimulus that emits ids 0..15 (`gen_test_patterns.py:1082`), which
`user_node_endpoint.sv:332` asserts against.

Landed: `axi_id_remap.sv` vendored into `sim/dv/axi-0.39.7/src/` and added to `XBAR_SRC`
(`lzc` and `cf_math_pkg`, its only dependencies, were already in the filelist);
`XBAR_SLV_ID_W` 7 -> 4, new `XBAR_MST_ID_W` = 5 carrying `tile_mst` and the tile memory path;
new `noc_mst` bus at `ID_WIDTH` behind `i_noc_id_remap`. `mesh_2x2_vc1` and `mesh_4x4_vc1`
`neighbor` both DIRECTED PASS, and `Vtb_top_axi_id_remap_intf__pi118.h` in the built object tree
confirms the remap elaborated rather than being dropped.

Carried to B: the master-face VIP typedefs (`file_master_t`, `scoreboard_t`, `master_dv`) are
sized by `ID_WIDTH` and must move to `XBAR_SLV_ID_W`, since a 3 b VIP cannot carry the stimulus
ids 0..15. `a_mst_id_fits` becomes vacuous once it does, and the width enforces what it asserted.

**B — narrow the width.** `constants.yaml`, regenerate, fix the `static_assert`, drop
`OUTSTANDING_DEPTH`, set the remap to 3 b out and `AxiSlvPortMaxUniqIds = 8`, fix
`remap_downstream_id`. Let ctest say what else breaks.

**D — re-run Stage 5d's sweep** on the new shape. The interesting number is whether the per-ID
list ever binds now that there are 8 lists instead of 256, which is the question that started
this, and whether the write side's 32 -> 256 changes anything the `RobMode::Enabled` runs see.

## Verification

Tier 2 after C and after B. Stage 5d's `all_to_all` sweep is the acceptance for D — it is the
only stimulus that makes any depth bind. `make pytest` and `codegen.py --check` after B, since
the payload field widths move.

**Status**: Stage A and C complete, uncommitted. B not started.
