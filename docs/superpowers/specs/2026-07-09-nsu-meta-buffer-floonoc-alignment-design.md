# NSU meta buffer — FlooNoC alignment

Date: 2026-07-09
Status: Draft (brainstormed with user; two independent FlooNoC RTL surveys, one gvsoc survey, one Codex design pass and one Codex adversarial spec review, findings applied; pending user review)

## Goal

Make the NSU response path independent of system-wide-unique AXI IDs, by porting FlooNoC's
`floo_meta_buffer` semantics: a **downstream ID remap** driven by `max_unique_ids`, and a **shared
outstanding pool** replacing the current per-AXI-ID depth.

## Motivation

An AXI ID is a manager-local handle. A tile may host several managers, so the stimulus must not derive
the ID from the node ID. Today the NSU depends on the opposite.

`MetaBuffer` allocates one entry per AW/AR flit into a per-AXI-ID deque and frees it at B egress or R
`rlast` (`meta_buffer.hpp:33,51`, `depacketize.hpp:19-20`, `packetize.hpp:129,146`). Occupancy of
`write_[id]` is therefore the count of in-flight transactions carrying that ID **summed across every
source tile**. Two consequences:

| # | consequence | evidence |
|---|---|---|
| 1 | Many sources sharing one ID overflow a single deque and `abort()`. RoB Enabled admits same-ID requests from a per-beat slot pool rather than the Disabled single-outstanding interlock (`rob.hpp:30-40`), bounded by `ROB_CAPACITY = 32` (`rob.hpp:80`), against a depth of 16. | `meta_buffer.hpp:34-37,52-55` |
| 2 | The buffer's capacity is `per_id_depth x distinct_ids`, so admitted outstanding depends on the stimulus ID scheme rather than on the NSU. | `meta_buffer.hpp:69-71` |

Neither is a correctness bug today. Same-ID responses return in issue order (AXI4, IHI 0022, chapter
A5.3 "Transaction ID"; the exact sub-clause number is [UNVERIFIED] because the primary PDF was not
reachable, the chapter and the wording were confirmed against ARM developer documentation), so the
per-ID FIFO front always matches. The defects are a fail-loud capacity model and a stimulus coupling.

## Reference: FlooNoC's `floo_meta_buffer` (what we port, what we drop)

| element | FlooNoC | port? |
|---|---|---|
| `out_id = '1` when `MaxUniqueIds == 1` | `floo_meta_buffer.sv:89-91,365` | **port** |
| `out_id = MaxAtomicTxns + in_id` when `> 1` | `floo_meta_buffer.sv:138-139` | **port** (with `MaxAtomicTxns = 0`) |
| shared-capacity buffer, `DEPTH`/`CAPACITY = MaxTxns`, per direction | `floo_meta_buffer.sv:94,112,148,173` | **port** |
| buffered payload `{original in_id, hdr}`, `hdr.src_id` routes the response | `floo_axi_chimney.sv:178-181,476-477,504-506` | **port** (we already store `src_id`) |
| full pool asserts `inp_ready` low (backpressure) | `id_queue` / `fifo_v3` handshake | **port** |
| no runtime check that `in_id < MaxUniqueIds`, silent alias | `floo_meta_buffer.sv:66,138-139` | **port** (alias only adds ordering) |
| build assert `MaxUniqueIds + MaxAtomicTxns <= 2**OutIdWidth` | `floo_meta_buffer.sv:381` | **port** |
| atomic reserved-ID range `[0, MaxAtomicTxns)` | `floo_meta_buffer.sv:276-304,343` | **drop** (`AtopSupport = 0`) |
| `IdMinWidth` truncation | `floo_meta_buffer.sv:66` | **drop** (upstream width == downstream width) |

FlooNoC states the reason for collapsing (`docs/floonoc/chimneys.md:50`): the NI must store each
request's `src_id` for the response, and a FIFO is cheaper than an ID queue. The cost is spelled out in
`floo_pkg.sv:289-295`: a single downstream `txnID` is "effectively serializing incoming transactions
from all managers in the entire system".

**FlooNoC does not pack the node ID into the AXI ID.** Two independent surveys of `hw/`, `hw/tb/`,
`floogen/`, `docs/` in the surveyed checkout found no such packing, and the outgoing AXI ID is derived
from the incoming AXI ID alone (`floo_meta_buffer.sv:138-139,343,364-365`). The source is recovered from
the flit header field `hdr.src_id` (`floo_axi_chimney.sv:476-477,504-506`), which we already mirror as
`MetaEntry::src_id`. This is a searched-checkout result, not a proven negative over every generated
configuration.

`gvsoc-pulp/pulp/floonoc_v2/` carries no equivalent: its requests are `vp::IoReq` subclasses and the
response is recovered through the `src_ni` / `burst` pointers (`floonoc_network_interface_v2.cpp:725,752`).
There is no AXI ID handling and nothing to lift. The SV model is the only reference.

## Decisions

| # | decision | rationale |
|---|---|---|
| **A** — remap is a single-variable function of `upstream_id` | Preserves the concurrency invariant below. A remap that also reads `src_id` (`out_id = {src_id, in_id}`) would let two sources' bursts with the same `rid` be in flight together, breaking `VcArbiter::r_burst_vc_` (`vc_arbiter.hpp:116,136-137,144`). |
| **B** — capacity is a shared pool per direction, not per ID | The abort in consequence 1 disappears without touching the ID scheme. Matches `MaxTxns` (`floo_pkg.sv:288`, "the number of both incoming and outgoing transactions that can be handled by the network interface"). |
| **C** — pool full backpressures, never aborts, and the check sits on the per-channel drain | A full pool must stall only its own channel. Mirrors the NMU independent-drain fix (`nmu.hpp:74-95`). See the request-path section. |
| **D** — `max_unique_ids` valid values are `1` and `AXI_ID_SPACE` | A local restriction, **not** a FlooNoC property. Upstream and downstream widths are both 8 here, so FlooNoC's `IdMinWidth` truncation never fires and any value `> 1` degenerates to the identity map. A build assert rejects intermediate values instead of silently accepting a parameter that does nothing. FlooNoC's own assert (`floo_meta_buffer.sv:381`) bounds capacity, not the value set. |
| **E** — no `max_atomic_transactions` parameter | `AtopSupport = 0`. The value would always be 0. |
| **F** — flit ID field width is unchanged | FlooNoC's flit carries the full `InIdWidth` on all four channels (`floo_pkg.sv:436-442`; `OutIdWidth` never appears in a flit width computation, only in the subordinate-facing typedef at `typedef.svh:123-124`). What its configs narrow is the downstream AXI port: `5 -> 2` (`floogen/examples/mcast.yml:24,34`), `4 -> 2` (`floo_test_pkg.sv:56-57`), `3 -> 1` (`floo_synth_params_pkg.sv:92-93`). That narrowing is what `floo_meta_buffer` performs, and ours is pinned at 8. There is nothing to save in the packet. |

## Design

### Downstream ID remap

A free function, not a `MetaBuffer` method: it is pure, and the formula is worth testing alone.

```
uint8_t remap_downstream_id(uint8_t upstream_id, std::size_t max_unique_ids);
```

**INPUT** the manager's AXI ID carried in the AW/AR flit.
**COMPUTE** `max_unique_ids == 1` returns the all-ones constant (`0xFF` at `AXI_ID_WIDTH = 8`); otherwise
returns `upstream_id`.
**OUTPUT** the ID presented on the downstream AXI port.

### Capacity

`MetaBuffer` keeps its `AXI_ID_SPACE`-wide bucket array, keyed now by the **downstream** ID. Each
direction gains one occupancy counter bounded by `max_outstanding`. Under `max_unique_ids == 1` every
entry lands in the all-ones bucket, which reproduces FlooNoC's single FIFO without a second code path.

```
bool write_full() const noexcept;   // write_count_ == max_outstanding_
bool read_full()  const noexcept;
```

`allocate_write` / `allocate_read` take the downstream ID. The per-ID depth check and its `abort()` are
removed.

### Request path (`Depacketize`) — mirror the NMU independent-drain fix

Today `Depacketize` allocates the `MetaEntry` at **ingress**, inside the shared flit loop
(`depacketize.hpp:153-158,176-181`). A full pool would therefore stall the one thing every channel
shares, which is the shape of the NMU request-path deadlock fixed on 2026-07-04
(`docs/superpowers/specs/2026-07-04-nmu-request-hol-fix-design.md:35-39`).

The NMU fix keeps three independent `PipelineStage` registers and drains each one on its own, gating no
channel on another (`nmu.hpp:74-95`). Its S1 entry carries the beat together with its own metadata
(`AdmittedAw {beat, dst_id, local_addr, rob_req, rob_idx}`). The NSU already has the three independent
registers. The only divergence is where allocation happens.

**Move the allocation from ingress to the per-channel drain.**

`s1_aw_` / `s1_ar_` change element type from a bare beat to a beat plus its header metadata, mirroring
`AdmittedAw`:

```
struct PendingAw { axi::AwBeat beat; uint8_t src_id, rob_req, rob_idx; };
struct PendingAr { axi::ArBeat beat; uint8_t src_id, rob_req, rob_idx; };
```

**INPUT** flits from `NocReqIn`.
**COMPUTE**, two stages, no cross-channel gating:

```
tick()  (ingress)  decode the flit, place beat + metadata into its own S1 register.
                   No MetaBuffer access. Stalls into pending_ only on its own S1 full,
                   exactly as today.

pop_aw()           s1_aw_ empty        -> nullopt
                   meta_.write_full()  -> nullopt        (backpressure, entry stays in S1)
                   otherwise           -> downstream_id = remap_downstream_id(...)
                                          meta_.allocate_write(downstream_id, {...})
                                          beat.id = downstream_id
                                          s1_aw_.take(), return beat

pop_ar()           same against read_full() / allocate_read
pop_w()            unchanged
```

**OUTPUT** AXI beats carrying the downstream ID, plus one `MetaEntry` per AW and AR.

A full write pool now stalls only `pop_aw`. `pop_ar` keeps handing reads to `AxiMasterPort`
(`axi_master_port.hpp` calls each `drain_*_from_depkt` independently). Allocation runs exactly once
because it happens at `s1_*.take()`, so a stalled entry is re-examined, never re-allocated. The old
double-allocate hazard, where a retried `pending_` flit could re-enter `meta_.allocate_write`, cannot
arise once ingress no longer touches the MetaBuffer.

`pending_` stays. Request flits arrive serialized on one NoC link, so an AR queued behind an AW waits
for that AW. That is the wire, not a modelling defect. The NMU has no equivalent because its source is
three independent push interfaces, not a single popped FIFO (`router/req_in.hpp:26` exposes `pop_flit()`
and no peek). Residual blocking is therefore bounded by the S1 registers, as it is today, and no ingress
resource waits on a downstream that waits back on it: the response network is separate from the request
network, `AxiMasterPort` drains responses before requests (`axi_master_port.hpp:83-95`), and the NSU
`WormholeArbiter` carries no AW-to-W pairing lock (`nsu.hpp:154-156`).

### Response path (`Packetize`)

**INPUT** a B or R beat whose `id` is the downstream ID.
**COMPUTE** `peek_write(b.id)` / `peek_read(b.id)` yields the entry. The flit payload `bid` / `rid` is
set from `MetaEntry::upstream_id`, the flit header `dst_id` from `MetaEntry::src_id`.
**OUTPUT** a response flit addressed to the requesting tile, carrying the manager's original AXI ID.

`commit_write` fires on B, `commit_read` on `rlast`, both unchanged.

### MetaEntry

```
struct MetaEntry {
    uint8_t src_id;       // requesting tile, becomes the response flit dst_id
    uint8_t upstream_id;  // manager's original AXI ID, restored into bid / rid
    uint8_t rob_req;
    uint8_t rob_idx;
};
```

`upstream_id` is required because a collapsed downstream ID carries no information. FlooNoC stores the
same pair (`meta_buf_t = {axi_in_id_t id; hdr_t hdr;}`, `floo_axi_chimney.sv:178-181`).

### Concurrency invariant

**Between the first beat and the `rlast` of an R burst, no other R beat carrying the same restored `rid`
reaches `VcArbiter::push_flit`.**

Note what this does *not* say. Two R bursts with the same `upstream_id` may be outstanding at one NSU at
the same time, and under `max_unique_ids = 1` sixteen of them routinely are. Their **beats** are what
cannot interleave.

The proof runs through the downstream ID. The remap is a function of `upstream_id` alone (Decision A),
so equal `upstream_id` implies equal downstream ID. The subordinate serializes read data per downstream
ID: the modeled `AxiSlave` keeps read bursts in per-ID queues and drains only the front burst of each
(`axi_slave.hpp:167-173,241-258,584-600`), and the co-sim `axi_rand_slave` picks one non-empty ID and
emits its front burst to completion (`sim/dv/axi-0.39.7/src/axi_test.sv:1410-1455`). Therefore
`VcArbiter::r_burst_vc_[rid]`, stamped on the first beat and released on `rlast`
(`vc_arbiter.hpp:116,144,150-153`), is never contended.

The invariant rests on the subordinate honouring same-ID read-data ordering, and on the remap ignoring
`src_id`. A `src_id`-dependent remap (`out_id = {src_id, in_id}`) would let two sources' bursts with the
same restored `rid` carry different downstream IDs, which the subordinate is then free to interleave.
`r_burst_vc_[rid]` would be contended and the burst-to-VC binding would break. This is why Decision A is
not merely a simplification.

## Naming

`max_unique_ids` keeps FlooNoC's name at the user's request. The rest is translated to the repo's
snake_case, full-word convention.

| FlooNoC | here |
|---|---|
| `MaxUniqueIds` | `max_unique_ids` |
| `MaxTxns` | `max_outstanding` |
| `MaxAtomicTxns` | not introduced |
| `IdMinWidth` | not introduced |
| `meta_buf_t.id` | `MetaEntry::upstream_id` |

`wrap/poc_defaults.hpp` is renamed `wrap/wrap_defaults.hpp` and its `kPoC*` constants lose the prefix.
The header holds the co-sim defaults, not proof-of-concept values. Rename only, no value changes,
separate commit.

## Parameters

| parameter | value | replaces |
|---|---|---|
| `max_unique_ids` | 1 | none (behaviour today equals 256) |
| `max_outstanding` | 32 | `per_id_depth` |

Both live in `nsu.meta_buffer` of `config/port_params.yaml` and in `wrap/wrap_defaults.hpp`. This
converges the **values** of an existing split-brain, not its sources: the co-sim path used
`kPoCMetaBufferPerIdDepth = 16` (`poc_defaults.hpp:21`) while the ctest path used `per_id_depth: 4`
(`port_params.yaml:34`), despite `axi_master_port.hpp:39` naming the YAML the single source of truth.
After this change both say 32, but the wrap layer still hardcodes and the YAML is still read by exactly
one test (`tests/integration/test_request_response_loopback.cpp:160`). Collapsing the two sources is a
separate round.

Because no single configuration path is authoritative, the `max_unique_ids` guard belongs where they all
converge: the `Depacketize` constructor. A default-constructed `NsuConfig` leaves the field 0, which
would otherwise read as the identity remap.

Build assert: `max_unique_ids == 1 || max_unique_ids == AXI_ID_SPACE`.

### Why the subordinate-facing ID width stays 8

`OutIdWidth` is an independent integrator parameter in FlooNoC, not a value derived from
`MaxUniqueIds`. Its own synthesis package pairs `MaxUniqueIds: 1` with `OutIdWidth` of 3, 2 and 1 in
three different configs (`floo_synth_params_pkg.sv:35,64-65,83-84,92-93`). Collapsing the ID does not
oblige anyone to narrow the port.

`floo_meta_buffer.sv:381` constrains it only from below: `MaxUniqueIds + MaxAtomicTxns <= 2**OutIdWidth`.
With `max_atomic_transactions = 0` that gives 1 bit at `max_unique_ids = 1`, and **8 bits at
`max_unique_ids = AXI_ID_SPACE`**.

`max_unique_ids` is a knob, flipped to `AXI_ID_SPACE` for the VC throughput round. The subordinate port
width is a compile-time constant. Narrowing it to 1 bit would pin `max_unique_ids = 1` permanently and
make that round impossible. Eight bits is the narrowest width that serves both settings, which is the
width we already have.

Under `max_unique_ids = 1` the upper seven downstream ID bits are then constant. That is free in a
behavioural model and is exactly the slack FlooNoC leaves in its own `AxiCfg` (`InIdWidth: 3`,
`OutIdWidth: 3`).

## Consequences

- `max_unique_ids = 1` orders every manager against every other at each subordinate. This is FlooNoC's
  documented default, and it is AXI-legal: aliasing two IDs adds ordering the manager did not request,
  and removes the subordinate's freedom to reorder them.
- **CORRECTED 2026-07-09.** This section originally read: "The bottleneck moves from the fabric to the
  subordinate. The VC comparison round must set `max_unique_ids = AXI_ID_SPACE` before measuring." That
  claim had no evidence and is withdrawn. FlooNoC never asserts a subordinate throughput loss
  (`docs/floonoc/chimneys.md:50`: the serialization "should not cause any big performance problems"),
  and our co-sim subordinate is a zero-wait `MAPPED` pulp `axi_rand_slave`
  (`sim/tb/user_node_endpoint.sv:193-196`) whose uniform service latency makes in-order same-ID return
  free. `max_unique_ids` constrains **ordering**, not bandwidth. See `docs/architecture.md`, "What
  `max_unique_ids` is, and is not".
- **The recorded `sim-saturation` series (vc1=1248, vc2=1710, vc4=1916, vc8=1935 bits/cyc) is still
  invalidated**, but for a different reason: it predates the capacity-model change, ran RoB Disabled with
  `IDS_PER_TILE=16`, and its configuration was never recorded alongside it. Whether `max_unique_ids`
  moves a saturation curve is a question to measure, not to assume.
- The static footprint of `MetaBuffer` (two 256-entry deque arrays) is unchanged. It is a C++ modelling
  artifact, not an RTL cost, and shrinking it is not in scope. The model therefore reproduces neither the
  FIFO area saving that `max_unique_ids = 1` buys in RTL, nor the `id_queue` cost that `> 1` pays.

## Observation, not a proposal

FlooNoC's manager AXI ports are 3 to 5 bits wide (8 to 32 IDs). Ours is 8 (256 IDs), which is wide by
that standard and is what makes the per-ID `MetaBuffer` array 256 entries deep. Narrowing the manager
port would shrink both the flit ID field and that array. It is an interface change, out of scope here,
and recorded only so a future round does not mistake the flit field for the thing worth narrowing.

## Non-goals

- No change to the flit format, `specgen` `AXI_ID_WIDTH`, the SV interface, or the DPI marshalling.
- No atomics, no `IdMinWidth` truncation, no `src_id`-bearing downstream ID.
- No change to `VcArbiter`, `Rob`, or the NMU.
- No per-channel ingress FIFOs. The `pending_` stash is retained: a serialized NoC link genuinely
  delivers an AR behind an AW, and the dead `aw_q_depth` / `w_q_depth` / `ar_q_depth` members
  (`depacketize.hpp:65-67`) stay dead. Moving allocation to the drain is what removes the cross-channel
  gate.

## Verification

The Verilator co-sim is the primary gate. Unit tests cover only what co-sim cannot observe directly.

| level | check |
|---|---|
| unit | `remap_downstream_id` over `max_unique_ids in {1, AXI_ID_SPACE}` x the ID boundary values. Pure function, one parameterized case. |
| unit | `MetaBuffer` shared pool: allocate to `max_outstanding`, assert `*_full()` reports back, assert no abort. This is the behaviour the old per-ID depth could not express. |
| co-sim | `make sim TB=tb_mesh_4x4_vc1 PATTERN=hotspot` under `max_unique_ids = 1`. Sixteen distinct upstream IDs collapse onto one downstream ID at the hotspot NSU: the serialized path at its worst, and the case where a lost or mis-restored `upstream_id` corrupts the readback. Gate: scoreboard clean, non-vacuous, no timeout. |
| co-sim | Same under `max_unique_ids = AXI_ID_SPACE`, which must reproduce today's behaviour on the wire. |
| regression | The four directed patterns plus `constrained_random` on `mesh_4x4_vc1`. Full ctest. |

The hotspot run is what proves the design. It exercises the collapsed remap, the shared pool under
sixteen-way many-to-one pressure, the `upstream_id` restore on every response, and the independent
`pop_aw` / `pop_ar` drain, all against a scoreboard that checks written data against readback. A
dedicated deadlock unit test is not carried: a stall shows up as a co-sim timeout, and the per-channel
drain removes the cross-channel gate that would cause one.

Fault injection first, per project practice: force `remap_downstream_id` to return `upstream_id` while
`max_unique_ids = 1`, confirm the hotspot co-sim reports a scoreboard mismatch, then revert. A checker
that has never fired has not been verified.

## Files

| file | change |
|---|---|
| `src/c_model/include/nsu/meta_buffer.hpp` | shared pool, `*_full()`, `MetaEntry::upstream_id`, drop the abort, add free function `remap_downstream_id` |
| `src/c_model/include/nsu/depacketize.hpp` | S1 elements carry beat + header metadata; allocation, remap and beat-ID rewrite move from `tick()` to `pop_aw` / `pop_ar`, each gated on its own pool |
| `src/c_model/include/nsu/packetize.hpp` | restore `upstream_id` into `bid` / `rid` (today both carry the downstream ID, `packetize.hpp:90,105`) |
| `src/c_model/include/nsu/nsu.hpp` | ctor plumbing |
| `src/c_model/include/nsu/port_params.hpp` | `meta_buffer_max_outstanding`, `meta_buffer_max_unique_ids` |
| `src/c_model/config/port_params.yaml` | `max_outstanding: 32`, `max_unique_ids: 1` |
| `src/c_model/include/wrap/nsu_wrap.hpp` | pass both parameters |
| `src/c_model/include/wrap/wrap_defaults.hpp` | renamed from `poc_defaults.hpp`, `kPoC*` prefix dropped |
| `src/c_model/include/wrap/{nmu,router}_wrap.hpp`, `src/dpi/cmodel_dpi.cpp`, `tests/wrap/test_nmu_wrap.cpp` | follow the rename |
| `src/c_model/tests/nsu/test_meta_buffer.cpp` | rewrite for the shared pool and the remap function; drop the per-ID depth cases |
| `src/c_model/tests/nsu/{test_nsu_depacketize,test_nsu_packetize,test_nsu}.cpp` | fix up for the new `pop_aw` / `pop_ar` signatures and the restored `bid` / `rid`. Add no new cases, the hotspot co-sim is the gate |
