# Master-face R backpressure

## Why

`docs/noc-target-spec.md` argues `DAT` deadlock freedom from read data landing in reorder-buffer
space reserved at request issue. Bypassed reads reserve no slot, so the invariant does not hold
for them, and the NI id-width round removed the shared outstanding pool that had been the only
aggregate bound on how many such reads are in flight. `RobMode::Enabled` therefore goes from 32
to 256, worst case all 256 unreserved.

That exposure cannot be measured on this testbench. pulp's `axi_file_master` never stalls its R
channel: `wait_r` is a resident forked task that consumes a beat whenever `r_outst` is non-empty
(`sim/dv/axi-0.39.7/src/axi_test.sv:2577-2586`). The NMU always sinks R, R never backs up into
`DAT`, and the dependency cycle cannot form. A clean run says nothing about deadlock freedom.

This work gives the master face the ability to stop accepting read data. The goal is functional
verification, not measurement.

## What is already here

`axi_delayer` is vendored and this testbench already instantiates one, in front of each tile
memory. Its R path drives `mst_req_o.r_ready` from a `stream_delay` stall unit
(`sim/dv/axi-0.39.7/src/axi_delayer.sv:108-118`), which is exactly the signal that has to go low.
`StallRandomOutput` covers the response direction, `B` and `R` together.

`sim/tools/gen_tb_top.py:159-167` already carries the profile mechanism for the memory instance:
a named profile dict, a module-level constant selecting one, and `_STALL_RANDOM_MAX_CYCLES` = 15
from `lfsr_16bit`'s 4-bit reload. This design reuses that shape rather than adding a second one.

## Wiring

One `axi_delayer_intf` between the file master and the tile crossbar.

```
file_master -> master_dv (AXI_BUS_DV)        monitors and scoreboard stay here
                   |  `AXI_ASSIGN
               mst_pre_delay (AXI_BUS)
                   |  axi_delayer_intf
               mst_post_delay (AXI_BUS)
                   |  the always_comb that today reads master_dv
               mst_flat_req / mst_flat_rsp + the four id signals
                   |
               tile_axi[0] -> tile crossbar -> tile_mst[NMU_TARGET]
                   -> i_noc_id_remap -> noc_mst -> master_axi_req_o.rready -> NMU
```

`master_dv` does not move, so `axi_scoreboard` and the VIP monitors keep their present view and
the transaction-level check is unchanged. The flattening repoints from `master_dv` to
`mst_post_delay`, so the procedural checkers that count handshakes see what actually reached the
crossbar. The delayer delays and never drops or reorders, so both views carry the same
transactions.

The four master-face id signals are driven from two different places and only two of them move:

| signal | today | after |
|---|---|---|
| `mst_awid`, `mst_arid` (`:124-125`) | `master_dv.aw_id` / `.ar_id` | `mst_post_delay.aw_id` / `.ar_id` |
| `mst_bid`, `mst_rid` (`:301-302`) | `tile_axi[0].b_id` / `.r_id` | unchanged, already downstream of the delayer |

Leaving the request pair on `master_dv` is the failure this table exists to prevent. The checkers
sample an id when the post-delay `awvalid` or `arvalid` handshakes, and a delayed handshake does
not carry the same cycle's pre-delay id.

`axi/assign.svh` is already included at `sim/tb/user_node_endpoint.sv:27`.

## Parameters

| where | what |
|---|---|
| `sim/tools/gen_tb_top.py` | `_MST_BACKPRESSURE_PROFILES = {"ideal": (0, 0), "random": (1, _STALL_RANDOM_MAX_CYCLES)}` and `_MST_BACKPRESSURE = "random"` |
| generated `tb_top_<topo>.sv` | `MST_STALL_RANDOM_OUTPUT` and `MST_FIXED_DELAY_OUTPUT` localparams, emitted beside the `MEM_*` pair |
| `sim/tb/user_node_endpoint.sv` | the same two as module parameters, forwarded to `axi_delayer_intf` |

Elaboration-time parameters, no plusarg and no Make variable. Changing the profile means editing
`gen_tb_top.py` and rebuilding, the same as `_MEM_LATENCY`.

The request side is hardwired off: `STALL_RANDOM_INPUT(1'b0)`, `FIXED_DELAY_INPUT(0)`. Stalling
`AW` / `W` / `AR` at the master face is injection-rate control, which `INJECTION_MODE` already
owns. Two knobs on the same quantity would let a regression carry a shape neither one names.

## Default and its cost

`_MST_BACKPRESSURE` defaults to `"random"`. Every gate carries backpressure, so the knob cannot
rot into a mechanism nobody turns on, and every regression exercises a consuming master that
sometimes cannot keep up.

This changes the timing of every existing gate. A gate that fails once backpressure is on is a
finding about the fabric or about a checker, and is handled as this round's work. It is not a
reason to change the default.

`sim-injection-sweep` is the exception and must run with `_MST_BACKPRESSURE = "ideal"`. Its
purpose is to find where the fabric saturates, and a consumer that stalls first measures the
consumer. This mirrors the reasoning already recorded for `_MEM_LATENCY` at
`gen_tb_top.py:149-153`, and it is a documented requirement rather than a new mechanism.

## Known limitation

`axi_delayer_intf` does not expose `stream_delay`'s `Seed`, and `axi_delayer` does not forward
one, so every node's LFSR starts from the same state. The LFSR advances per handshake, so
instances decorrelate as soon as their traffic differs, and only highly symmetric traffic such as
`neighbor` on a square mesh keeps them aligned. Reaching independent per-node stall patterns
would mean bypassing the component and wiring `stream_delay` directly. That is not done, and this
limitation is recorded instead.

## Acceptance

| # | check |
|---|---|
| 1 | Under `"random"`, `master_axi_req_o.rready` is observed low while `master_axi_rsp_i.rvalid` is high. The knob is proven to act before its presence is treated as coverage |
| 2 | Under `"ideal"`, `stream_delay` takes its `gen_pass_through` branch and the path is wires |
| 3 | Tier 2 green with backpressure on: `mesh_2x2_vc1` and `mesh_4x4_vc1` `PATTERN=neighbor` |
| 4 | `mesh_2x2_vc1 PATTERN=multicast` green, with `+mcast_fault` and `+decerr_fault` still tripping their checkers |
| 5 | The `"ideal"` requirement for `sim-injection-sweep` is written where the sweep is run from |

## Out of scope

The DMA-driven testbench is a separate top built from real units. This testbench stays
DV-oriented and the delayer is a permanent fixture of it, not a stand-in for the DMA.

Stage D of the NI id-width round runs after this lands. This design makes its result meaningful
and does not itself measure anything.
