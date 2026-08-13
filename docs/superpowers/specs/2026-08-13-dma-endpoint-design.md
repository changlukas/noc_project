# A standard DMA on the master face

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
| `AxiIdWidth` | 4, `ni_params_pkg::AXI_INITIATOR_ID_WIDTH_DFLT` | the crossbar's slave port is that wide, and `i_noc_id_remap` already folds it to the NI's 3 b on the way out. No second remap |
| `NumAxInFlight` | at most 64, `MaxMstTrans` | what one initiator may have in flight. Above it the tile memory's delayer throttles; overflow stalls rather than errors, so exceeding it costs throughput silently |
| `DataWidth` | 512 | the tile's AXI |

`BufferDepth`, `TFLenWidth` and `MemSysDepth` are genuinely free. FlooNoC runs 16, 32 and 0; those
are its numbers, read from its testbench, not derived for this fabric, and the plan states what it
picks and why rather than inheriting them silently.

### The ID interaction that makes the job field matter

`options_t.axi_id` is per request, and the legalizer drives the AW channel's `id` straight from it
(`idma_legalizer_rw_axi.sv:339`), so a per-job ID is honoured to the wire. Nothing in iDMA
allocates or rotates IDs: `NumAxInFlight` sizes internal request FIFOs and is independent of the ID
value, so a job stream that reuses one ID issues same-ID traffic and leans on downstream ordering
and backpressure.

Downstream here is an NMU whose shipped `RobMode::Disabled` enforces per-ID single-outstanding. A
single-ID job stream therefore serialises at the NI regardless of `NumAxInFlight`, which would read
as a fabric throughput result when it is a stimulus artefact. That is the concrete reason the job
format gains the field.

`sim/tb/dma/dma_node_endpoint.sv` is a new file rather than a mode inside the existing endpoint.
The existing one is named-dependent on `axi_file_master` in its injection-mode `case`, its
`run_ar_after_b` sequencing and its `b_total != file_master.num_writes` epilogue, and its
scoreboard's write-then-read golden model does not hold for a DMA.

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

## Open at plan time

Three things this document does not settle, listed so the plan answers them rather than
discovering them.

| | |
|---|---|
| Backdoor reach | the comparison reads two nodes' `axi_sim_mem` arrays from one place. Whether the testbench hierarchy exposes both, and from where, is not established here |
| Retirement versus in flight | comparing after a DMA retires a job assumes nothing of that job is still moving. iDMA reports on its own response channel, which is upstream of the NI's response path and of the memory's write acceptance, so the plan states what it waits on beyond `rsp_valid` |
| `BufferDepth`, `TFLenWidth`, `MemSysDepth` | free parameters with no derivation yet, see above |

## Gate

One new co-sim target on an existing topology, isolated by directory so the six existing gates do
not see it. It passes when every job retires and every destination region matches its source.

Reaching it is the round's whole claim: a standard AXI manager, configured by someone else, moved
data across this fabric and the data arrived intact.
