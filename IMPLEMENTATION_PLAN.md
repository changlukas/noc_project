# NI ID architecture — narrow the NI-facing AXI ID to 3 bits

## Why

`AXI_ID_WIDTH = 8` makes `rob.hpp:241` build `2**8 = 256` per-ID order lists, each
`NMU_MAX_TXNS_PER_ID = 32` deep, behind a shared pool of 32. Stage 5d measured the consequence:
at four ids the pool reaches 30 of 32 while the deepest per-ID list reaches 12, so almost all of
that structure is unreachable. In RTL terms it is 256 x 32 entries of order-tracking for a
machine that can have 32 transactions in flight.

FlooNoC does not have this problem because it keeps the NI-facing id narrow. Every shipped
config uses `InIdWidth = 3` (`floo_test_pkg.sv:48,56,65`; `floo_synth_params_pkg.sv:64,83,92`),
and there is no outstanding counter on that path — `axi_aw_queue_ready_in = aw_rob_ready_out`
(`floo_axi_chimney.sv:376`) is the only gate — so its 8 x 32 is genuinely reachable.

| | now | target |
|---|---|---|
| NI-facing AXI id width | 8 b | **3 b** → 8 ids |
| per-ID order lists | 256 | 8 |
| transactions per id | 32 | 32 |
| total outstanding | 32, capped by the shared pool | 256 |

## What has to be decided, not just implemented

**Does `NMU_OUTSTANDING_DEPTH` survive?** FlooNoC has no such counter on this path. Keeping it at
32 in front of an 8 x 32 structure puts the total straight back to 32 and the change buys
nothing. Either it goes, or it rises to 256 and stops being the binding limit. Stage 5d's sweep
is the evidence to argue from: every depth from 32 down to 1 stalled cleanly, so removing the
cap is not a correctness risk, only a backpressure-shape change.

**Where does `axi_id_remap` sit, and how wide?** It becomes required, not optional:
`AXI_INITIATOR_ID_WIDTH = 4` gives one initiator 16 ids and a tile carries two initiators, so up
to 32 distinct ids must fold into 8. pulp's `axi_iw_converter` selects `axi_id_remap` for exactly
this case (`axi_iw_converter.sv:127-146`). Its `AxiSlvPortMaxUniqIds` is the number this round
has to put a value on, and Stage 5c's telemetry can measure the ids actually seen rather than
guessing.

## Blast radius

36 files reference `AXI_ID_WIDTH` or `AXI_ID_SPACE`. The load-bearing ones:

| site | what changes |
|---|---|
| `specgen/source/constants.yaml` | `ID_WIDTH` default 8 → 3; its comment describes the §A5.3.5 index budget and must be rewritten, since 3 b leaves no room for an index |
| `axi/types.hpp` | `static_assert(AXI_ID_SPACE == 256)` locks the current value deliberately — it is the tripwire, and updating it is the point |
| `nmu/rob.hpp:241` | two `std::array<..., AXI_ID_SPACE>` drop from 256 to 8 entries |
| `nsu/meta_buffer.hpp` | `remap_downstream_id` collapses to all-ones of `AXI_INITIATOR_ID_WIDTH`; check that still fits |
| `src/sv/*_wrap.sv` | port widths, and with them the contract `rtl/README.md` records |
| flit | the id field width, hence `specgen/source/` packet json and every generated output |
| `sim/tools/gen_test_patterns.py` | stimulus ids already derive from `INITIATOR_ID_WIDTH`, so they do not change — but `--ids-per-initiator` is now bounded by what the remap can track, not by the id space |

## Stages

**A — decide the two open questions**, with Codex review, before touching code. The pool question
changes what the rest of the work is.

**B — narrow the width.** `constants.yaml`, regenerate, fix the `static_assert`, let ctest say
what else breaks. Expect the per-id arrays and anything indexing by raw id.

**C — insert `axi_id_remap`** in `user_node_endpoint.sv` between the tile crossbar and the NMU,
sized from B's telemetry.

**D — re-run Stage 5d's sweep** on the new shape. The interesting number is whether the per-ID
list ever binds now that there are 8 lists instead of 256, which is the question that started
this.

## Verification

Tier 2 after B and after C. Stage 5d's `all_to_all` sweep is the acceptance for D — it is the
only stimulus that makes any depth bind. `make pytest` and `codegen.py --check` after B, since
the flit field width moves.

**Status**: Not Started
