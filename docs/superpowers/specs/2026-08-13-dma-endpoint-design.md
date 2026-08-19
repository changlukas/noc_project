# A standard DMA on the master face

> **Amendment, 2026-08-13, written after the round shipped.** Three statements below did not survive
> implementation. The body is left as it was decided.
>
> | section | what it says | what the round established |
> |---|---|---|
> | *The reorder buffer becomes the default* | both DAT deadlock conditions hold after this round, iDMA supplying the second | Neither the cycle nor the second condition is reached. The shipped jobs have each node read its OWN window, which the tile crossbar answers at m0/m1, so no read crosses the fabric and no AR reaches the NMU: a gate run reports `read_txns_hwm=0` on every node. `docs/known-limitations.md` records the gap as open: Stage B does NOT close it, and closing it needs jobs sourced from another node's window, which nothing emitted at that date |
> | *Settled before planning*, the `mon_w_valid_o` / `mon_w_last_o` paragraph | connect them, and let the check wait on the observed event | Removed. Both read 0 at every posedge under Verilator 5.048, counted two ways in an instrumented run. The top gates on job retirement instead, and the write monitor is a recorded limitation |
> | *Gate* | a standard AXI manager, configured by someone else, moved data across this fabric | Write path only: 16 transfers of 1 KiB, byte-exact against an address-derived preload. Not evidence about the fabric read path, the `RobMode` flip or the address-decode gate |

Put pulp iDMA where the stimulus replayer is, and find out whether a real AXI manager runs on this
fabric. The traffic change is secondary: `axi_file_master` replays a transaction list this project
wrote, so every AXI shape the fabric has ever seen is one this project chose. A DMA picks its own
bursts, its own outstanding depth and its own alignment, so this is an AXI conformance test of the
NoC that the existing gates structurally cannot be.

Deliverable: a separate top with its own gate. The six existing co-sim gates, ctest and pytest are
untouched.

## What is already settled

Established by reading the sources and running them, recorded in
`.superpowers/sdd/ai-dataflow/survey-idma.md`. Not re-argued here.

| | |
|---|---|
| File closure | 18 new files, all present at tags already pinned here |
| Verilator | the closure elaborates on 5.048 with `--timing` at 512 b / 48 b / 3 b / 58 b, 0 errors |
| Collectives | `idma_req_t.user` reaches `aw_user`, so a DMA can issue them. Not used this round |
| Reference integration | FlooNoC instantiates the same backend, joins the two ports with `axi_rw_join`, and feeds an address-decoding crossbar |
| Cost | one `passthrough_stream_fifo` per byte lane per DMA. 512 b is 64, so a 4x4 mesh is 1024. FlooNoC pays 1152 and does not mitigate it |

## Vendoring

iDMA has two tags per release. `v0.6.5` commits the generated RTL under `target/rtl/`, so
`git show` reaches it. `v0.6.5-src` does not, and regenerating needs bender, which this repo has
not got. The directory pins `v0.6.5`.

| package | files |
|---|---|
| `sim/dv/idma-0.6.5/` | 12: `idma_pkg.sv`, `include/idma/{typedef,guard}.svh`, `target/rtl/idma_{backend,legalizer,transport_layer}_rw_axi.sv`, and 6 under `src/backend/` |
| `sim/dv/axi-0.39.7/src/` | +1: `axi_rw_join.sv` |
| `sim/dv/common_cells-1.37.0/src/` | +5: `stream_fork`, `stream_fifo_optimal_wrap`, `stream_fifo`, `fall_through_register`, `passthrough_stream_fifo` |

The six additions to existing packages go into their existing directories, at the tags those
directories already name. `sim/dv/README.md` gains a row for iDMA and its file list, and the
existing `axi` and `common_cells` rows gain the new files.

## The endpoint

The tile crossbar in `user_node_endpoint.sv` already decodes a master's addresses into per-space
local memories and a default port onto the NoC. That is the same shape FlooNoC builds for its DMA.
So the endpoint is one substitution:

```
idma_backend_rw_axi ──axi_rw_join──> tile crossbar ┬── memory space  -> local memory
   (read port, write port)                          ├── config space  -> local memory
                                                    └── default       -> NMU, the NoC
```

The backend exposes read and write as two AXI manager ports; `axi_rw_join` makes them one. Nothing
downstream of the crossbar changes, and neither does the generated fabric, the router wraps or
`gen_tb_top.py` — the endpoint's port list is unchanged.

**One AXI interface, not two.** FlooNoC's narrow-wide configuration carries two physical AXI
interfaces per endpoint and instantiates a DMA on each, which is why it pays 1152 FIFOs for 16
nodes. Here the three networks are internal to the NI, which classifies by address space, so one
DMA reaches both traffic classes by choosing an address. One DMA per node, 64 FIFOs each.

### Parameters the crossbar already fixes

The tile crossbar states what a master on its slave port may do, so three of the backend's
parameters are not free choices.

| backend parameter | value | why |
|---|---|---|
| `AxiIdWidth` | 4, `ni_params_pkg::AXI_ID_WIDTH_DFLT` | the crossbar's slave port is that wide, and `i_noc_id_remap` already folds it to the NI's 3 b on the way out. No second remap |
| `NumAxInFlight` | at most 64, `MaxMstTrans` | what one initiator may have in flight. Above it the tile memory's delayer throttles; overflow stalls rather than errors, so exceeding it costs throughput silently |
| `DataWidth` | 512 | the tile's AXI |

`BufferDepth`, `TFLenWidth` and `MemSysDepth` are genuinely free. FlooNoC runs 16, 32 and 0; those
are its numbers, read from its testbench, not derived for this fabric, and the plan states what it
picks and why rather than inheriting them silently.

### The ID field

`options_t.axi_id` is per request, and the legalizer drives the AW channel's `id` straight from it
(`idma_legalizer_rw_axi.sv:339`), so a per-job ID is honoured to the wire. Nothing in iDMA
allocates or rotates IDs: `NumAxInFlight` sizes internal request FIFOs and is independent of the ID
value, so a job stream that reuses one ID issues same-ID traffic and leans on downstream ordering.

The NMU's per-ID structures are a reorder buffer slot and a meta buffer bucket per ID. A single-ID
stream reaches one of each, so the field is a coverage requirement: without it the round would
report that a DMA runs on this fabric while never touching seven eighths of the structures that
carry its responses.

`sim/tb/dma/dma_node_endpoint.sv` is a new file rather than a mode inside the existing endpoint.
The existing one is named-dependent on `axi_file_master` in its injection-mode `case`, its
`run_ar_after_b` sequencing and its `b_total != file_master.num_writes` epilogue, and its
scoreboard's write-then-read golden model does not hold for a DMA.

## The reorder buffer becomes the default, and that is the interesting part

`RobMode::Disabled` is what ships (`nmu_wrap.hpp:77`, `nmu.hpp:155`), selected the other way only
by a topology name ending `_rob`. The target spec describes the other one: its response path is
"packet buffer, reorder buffer, depacketizer" with an 8 KB reorder buffer (§3). So `Enabled`
becomes the default in this round, and the implementation catches up with the specification.

**That wakes a dormant limitation, and the DMA is what supplies the missing half.**
`known-limitations.md` records that the target spec derives DAT deadlock freedom from read data
landing in reorder-buffer space reserved at request issue, while a bypassed read reserves nothing,
so the argument does not cover it. Its "when it bites" is two conditions, and today neither holds:
nothing constructs an `Enabled` build, and reaching the cycle needs a consumer that backpressures
R, which `axi_file_master` is not — its `wait_r` is a resident forked task that consumes a beat
whenever one is owed, so the NMU always sinks R.

Both conditions hold after this round. The default supplies the first. iDMA supplies the second:
its read side is gated by the per-lane buffer's ready, which is gated by the write side draining,
so a congested write path stops it accepting R. The previous round's master-face backpressure work
closed by saying that forcing the dependency cycle needed a directed hold on `RREADY` while writes
accumulate, and that its random 15-cycle stall was not it. A DMA does it as a matter of course.

This is not a reason to avoid either change. It is the most valuable thing the round can produce,
and it is written down here so that a hang in this configuration is read as the known gap in the
deadlock argument rather than as a DMA integration fault.

**The two changes are staged rather than simultaneous.** The default flips first and the six
existing gates are re-baselined against it; only then does the DMA arrive. One round and one
re-baseline of the new gate either way, but a failure after the second step has one new variable
rather than two. Whichever step turns a gate red, the other step is not a suspect.

## Jobs

FlooNoC's job file is ten plain-text fields read by `read_jobs`. Two changes.

| field | source | why |
|---|---|---|
| `length`, `src_addr`, `dst_addr` | FlooNoC | |
| `max_src_len`, `max_dst_len` | FlooNoC | burst legalization bounds |
| `aw_decoupled`, `rw_decoupled` | FlooNoC | coupling knobs the backend already takes |
| `src_protocol`, `dst_protocol` | FlooNoC | both AXI here, kept so the format matches upstream |
| `num_errors` | FlooNoC | always 0; the error path is out of scope |
| **`axi_id`** | **added** | FlooNoC leaves `idma_job.id` at `'0`, so every transfer uses one ID. This fabric has 3 b IDs, a per-ID reorder buffer and a per-ID meta buffer; a single-ID stimulus exercises none of it |

`sim/tools/gen_dma_jobs.py` emits the files. It needs this project's address map, which
FlooNoC's generator has no knowledge of: a job's `src_addr` and `dst_addr` are SAM addresses, so
the emitter derives them from the topology the same way `gen_test_patterns.py` does, reusing
`address_map.pack` rather than restating the formula.

`sim/tb/dma/idma_job_driver.sv` reads a file and drives `idma_req_t` valid/ready. The alternative,
filling the struct in an `initial` block, is rejected: it makes the stimulus a compile-time
constant, where today `+stim_dir=` changes a run without recompiling.

## Checking

FlooNoC checks no data. Its memory is `axi_rand_slave` without `MAPPED`, so a read returns
randomised bytes and comparison is structurally impossible; both mesh testbenches contain no
assertion at all, and even iDMA-reported errors are printed rather than failed. Their DMA
testbench measures throughput. That part is not reusable.

The check comes from iDMA's own repository instead, where `test/include/tb_tasks.svh` compares
memory regions through a backdoor into `axi_sim_mem`'s array.

**Per job: the destination region equals the source region.** The source is preloaded with a
pattern that is a function of its address, so a byte landing at the wrong offset is caught rather
than matching by luck. The comparison is a backdoor read of both endpoints' memory arrays after
the owning DMA reports the job complete, so it needs no readback traffic and does not perturb what
it measures.

**Run completion** replaces the existing two-phase barrier: the run ends when every DMA has
retired every job it was given. A DMA that retires fewer jobs than its file holds is a failure,
which is the DMA-side equivalent of the existing vacuity check.

## What this round does not do

- No collectives from the DMA. The path exists and is recorded; using it is a later round.
- No DRAM timing model. The endpoint keeps `axi_sim_mem` behind the crossbar.
- No replacement of `axi_file_master`. Both masters exist, in separate tops, and the existing
  gates keep the one they have.
- No AI dataflow patterns. Those need a traffic generator on top of the job emitter and are the
  round after this one.

## Settled before planning

Five things the design left open. Four are closed, one is reduced to a measurement.

**The backdoor reaches, and needs nothing built.** The path is
`tb_top_<topology>.g_endpoint[n].u_endpoint.g_tile_mem[t].i_mem.i_sim_mem.mem[addr]`, two generate
levels deep. Verilator's support for that was the doubt, so it was run rather than argued: a probe
of the same shape, a function naming two different iterations and both reading and writing the
array, elaborates on 5.048 with `--timing` and zero errors. No DPI backdoor, no per-endpoint
exported task.

**The comparison cannot race the write.** iDMA's completion waits for the write response, not the
last W beat: `idma_axi_write.sv:267` drives `w_dp_valid_o` from `write_rsp_i.b_valid`, and the
backend raises `rsp_valid` on `w_dp_rsp_valid & w_last_burst`. On this side `axi_sim_mem` writes
`mem[]` on W acceptance and only queues B after the last W beat, so by the time iDMA sees B the
destination array already holds the data.

Connect the memory's own `mon_w_valid_o` / `mon_w_last_o` anyway. They exist on
`axi_sim_mem_intf` and `user_node_endpoint.sv:552-553` leaves them unconnected; wiring them in the
DMA endpoint turns "the ordering argument says it has landed" into an observed event, which is
what the check should wait on.

**Two of the three free parameters are decisions, one is a measurement.**

| | value | why |
|---|---|---|
| `BufferDepth` | 3 | the source's own recommendation for misaligned transfers, and the only hard rule is `> 1`. FlooNoC's 16 is its number, and at 64 FIFOs per DMA the difference is not free |
| `TFLenWidth` | fixed by the job emitter's longest transfer | the bound is 12 to `AddrWidth`. Today's `REGION_BYTES` of `0x1000` needs 12; a whole `0x100000` tile window needs 20 |
| `MemSysDepth` | 0 to start | iDMA calls it the memory system's depth. Here that is a round trip through the NoC, and this repo has no constant for it. Settling it means measuring cycles from issue at the joined port to return, across DMA, NI, fabric, NI, crossbar, delayer, memory. Starting at 0 costs throughput, not correctness |

**The flip moves numbers, not expectations.** `RobMode` selects the R path only; B always runs
through the reorder buffer. What necessarily changes is the counters the generated top prints —
`read_slot_hwm`, `order_list_hwm`, `write_txns_hwm`, `read_txns_hwm` and the AR admission clauses,
since `Disabled` reads do not count AR admission branches at all. R beat ordering and latency change
only where same-ID reads can overlap, because `Disabled` refuses a second same-ID AR while one is
outstanding.

The scoreboard is not affected: it compares read data against golden writes, and is not written as
a latency or occupancy checker. So re-baselining the six gates is reading new numbers, not
re-deriving what they should be.

**ctest should not move.** 21 cases construct an NMU without setting the mode and so inherit it,
across `test_nmu`, `test_nmu_dat_face`, `test_nmu_credit`, `test_nmu_wrap`, `test_dat_merge_wrap`,
`test_cmodel_dpi` and `test_ni_router_chain`. None asserts something that necessarily changes: they
use single ARs, distinct IDs, or completion and readback assertions rather than exact latency or
the `Disabled` same-ID interlock. The one test that pins RoBless behaviour sets `Disabled`
explicitly. A test that does move is therefore a finding, not a re-baseline.

## Gate

One new co-sim target on an existing topology, isolated by directory so the six existing gates do
not see it. It passes when every job retires and every destination region matches its source.

Reaching it is the round's whole claim: a standard AXI manager, configured by someone else, moved
data across this fabric and the data arrived intact.
