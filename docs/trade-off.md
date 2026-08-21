# Response-ordering trade-off record

Rationale for the NI's AXI response ordering. `docs/nmu-spec.md` (RoB, virtual
networks) and `docs/nsu-spec.md` (meta buffer, response VC selection) specify the
as-built mechanisms. This record explains the choice behind them.

## The problem

AXI requires same-ID responses in issue order toward the master. A lossless
packet-switched NoC guarantees that a packet arrives, and nothing about when or
in what order, so the design must add the ordering itself, in the fabric or at
the endpoint. Reordering has three sources:

| source | mechanism |
|---|---|
| latency variation | round-trip time differs across destinations, and across time for one destination under congestion |
| path divergence | adaptive or multi-path routing gives two packets of one flow different routes, and the later-issued one can arrive first |
| cross-VC overtaking | a flow split across VCs sits in separate queues, and the common arbiters (round-robin, matrix, separable, wavefront) grant without regard to arrival order (on-chip-networks Ch 6) |

AXI same-ID order decomposes into point-to-point order (same source and
destination, issue order) plus a cross-destination window. Point-to-point order
follows from deterministic routing plus an unsplit flow (on-chip-networks
Ch 7). Global order, a single total order seen by all nodes, is out of scope.

The precise requirement is that a flow must not split across VCs. The stronger
reading, one VC identifier end to end, over-constrains. A packet's own flits
cannot reorder: the head allocates the VC, body and tail inherit it, and the
wormhole lock keeps them contiguous (on-chip-networks Ch 5). Order between
packets of one flow survives whenever the VC assignment is deterministic and
refuses on a full queue instead of spilling to another VC. Per-hop VC changes
stay legal as long as all packets of the flow change the same way at each hop.

## Textbook solutions

| scheme | ordering mechanism | cost |
|---|---|---|
| stall | one VC per flow, injection waits until the prior same-ID transaction drains | zero reorder storage, cross-destination transactions serialize |
| in-order arbitration | fabric arbiters grant by arrival age across VCs | re-serializes the flow and gives back the throughput multi-VC bought, plus per-port age tracking, rare in practice |
| endpoint reorder buffer | the fabric applies no ordering, an endpoint RoB re-sorts by sequence number | largest buffer, covering the intra-destination window as well as the cross-destination one, and the endpoint must reserve its space before injection |

VCs relieve head-of-line blocking by letting a later packet overtake a blocked
one. Ordering forbids overtaking within a flow. A flow therefore cannot spread
across VCs for throughput and stay ordered, unless an in-order arbiter
re-serializes it and returns most of the throughput the extra VCs bought.

## The shipped compromise

The shipped design sits outside this menu. Hardware area cost and performance
drove it: the fabric keeps same-destination order in-network, and `nmu::Rob`
with a bypass path handles cross-destination reordering at the endpoint.

| piece | mechanism |
|---|---|
| routing | XY dimension-order, one fixed path per source-destination pair |
| fixed VC id, request | `nmu::VcAllocator` reuses the ID's recorded VC when an `ordering_req = 0` flit repeats its `(dst_id, id)`. A blocked fixed VC waits instead of rerouting |
| fixed VC id, response | `nsu::VcAllocator` maps R to `(dst_id ^ rid) % num_vc`, stateless. Full or no-credit refuses, never spills |
| endpoint reorder | `nmu::Rob` re-sorts only same-ID responses interleaved across destinations. Ordering toward the master lives in the NMU because only the NMU knows its own issue order |
| identity echo | `nsu::MetaBuffer` holds `{src_id, upstream_id, ordering_req, ordering_tag}` per response; `nsu::Packetize` stamps them onto B/R. Neither reorders, and the fabric never reads either field |

The bypass path does the area work. An in-order response is by construction the
next to release, so it drains without taking a slot: idle-ID and
same-destination-streak requests leave with `ordering_req = 0` and skip slot
allocation end to end. This buys two things. The RoB shrinks beyond what
fabric-side same-destination ordering alone allows, because it holds only the
responses that can arrive out of order rather than one entry per outstanding
transaction. And a long same-`(id, dst)` burst flows through without clogging
the pool, so slots stay free for the cross-destination traffic that needs
reordering.

`RobMode::Disabled` is the stall scheme in this design, with zero reorder storage. Each ID keeps
an outstanding counter and a latched ordering-domain key `{dst_id, dst_port_id, AXI class}`.
Same-key successors are admitted up to `NMU_MAX_TXNS_PER_ID`; a key change waits until that ID is
idle. The port term is required because a tile and a peripheral may share one router coordinate,
and the class term is required because Narrow R and Data R return on different physical networks.
RTL selects the R implementation with the elaboration-time `READ_ROB_ENABLED` parameter and
`generate if`, not a preprocessor conditional. The B side has no mode. It remains a per-ID
metadata-only RoB, so different IDs are not placed into one global response order and no B data
SRAM is needed.

| mode | fabric guarantee | endpoint storage | stalls |
|---|---|---|---|
| `RobMode::Enabled` | same-`(dst, id)` responses in order | RoB sized to the cross-destination window, bypass traffic takes no slot | only on slot exhaustion |
| `RobMode::Disabled` | same ordering domain remains in order | per-ID counter and key, no response data | ordering-domain change waits for the ID to become idle |

## Invariants

- The ordering guarantee must hold on the response network, the direction the
  master observes. Determinism on the request network alone proves nothing
  about R/B arrival order.
- `ordering_req = 1` traffic enters the network only with a reserved slot. Admission
  gates on RoB space up front, because a response stalled in-fabric with
  nowhere to land would block every flit behind it. Slot reservation before
  injection is the fabric's deadlock gate.
- The same-ordering-domain bypass stays sound only while same-`(dst, port, class, id)` responses
  cannot split across VCs. The fixed VC id provides that. A round-robin spread
  of the streak breaks it, which is why each streak stays on one VC per
  network.
- No safe design pairs a fabric that may reorder same-destination responses
  with an endpoint that performs no reordering. Dropping the fixed VC id grows
  the RoB back to the endpoint-reorder-buffer scheme's size.

## Open costs

- A flow cannot spread across VCs, so head-of-line blocking within a flow
  remains. This is the recurring cost of the small RoB.
- No sweep exists for `max_txns_per_id` (default 32, `[TBD]`) or `r_rob_depth`
  (default 32, expressible to 256 via `R_ROB_DEPTH`).
- The approved Disabled-mode counter/key policy and the `dst_port_id` term in Enabled-mode bypass
  are not yet implemented in the C++ reference model.

## SAM destination decode

The first RTL milestone implements one generated table contract and one shared
pure-combinational wrapper, `ni_sam`. The selected configuration's `endpoints:` block is the sole
source; build-only `topology_pkg.sv` owns the typed rules and constant `SAM`, and the wrapper reuses
the pinned `common_cells` `cc_addr_decode`. There is no project-owned second decoder, runtime topology
multiplexer, writable table, or default route. A lookup returns destination node, destination port,
AXI class, collective enable, and X/Y coordinate layout while forwarding the global AXI address
unchanged.

YAML order is the architectural priority. Overlap is legal, and the first authored matching rule
wins. The pinned primitive instead grants its highest matching array index, so generation reverses
the expanded rule array. This keeps the policy in generated constants and adds no priority network
around the primitive. Offset decode can replace the range comparators only if table area or timing
becomes a measured problem; it is not part of this contract.

AW and AR use separate elaboration-time register-slice controls: `AW_SAM_REG_TYPE` and
`AR_SAM_REG_TYPE`, both default 0. Value 0 bypasses the slice, value 1 adds a simple output
register, and value 2 adds a full skid buffer with registered backpressure. Keeping the channels
independent allows the longer AW collective path to be cut without adding AR latency. These knobs
are RTL timing controls and do not belong in the topology YAML.

Table decode also selects collective coordinate metadata with the matched SAM rule. Only a range
explicitly authored with `en_collective: true` receives nonzero X/Y selectors; false or absent is
unicast-only. Each collective-capable address space has one internally uniform `node_stride`, but
Config and Memory may use different strides and coordinate bit positions. This preserves
address-map flexibility without adding a datapath search: the SAM result already identifies the
required X/Y slices. A future table-free offset decoder would instead require one global
coordinate field.

PPA cost is explicit: NMU has one comparator bank each for AW and AR, and NSU has one AW bank whose
result is functionally qualified only for multicast. The pinned decoder has no enable port, so this
contract makes no unmeasured claim that the NSU bank is physically clock-gated or free of internal
switching on unicast traffic. Comparator count and priority fan-in scale linearly with
`SAM_NUM_RULES`; there is no SAM storage register, queue, extra arbitration layer, width expansion,
or mandatory pipeline stage. The existing independent AW/AR register-slice controls remain the
only timing cuts. Reusing the typed wrapper reduces duplicated control logic but does not share a
physical comparator bank across concurrent channels. Any input-isolation, table-free, or
shared-bank optimization requires measured timing/area pressure and must preserve
one-request-per-cycle availability, authored priority, and independent AW/AR progress.

## External AXI interface

Each NMU and NSU has exactly one 512-bit AXI4 interface. The SAM classifies its transactions into
Narrow or Data NoC traffic; those classes do not create separate physical AXI ports. A Narrow
transfer packetizes only the addressed 64-bit lane and restores that lane at the destination.
Supporting a second AXI interface is outside the architecture, not a deferred RTL option.

The shared AXI face does not imply a shared NoC request scheduler. After the AXI channel CDC and
SAM classification, the NMU uses independent REQ and DAT Write pipelines, channel assignment and
`noc_clk` class FIFOs.
REQ may emit one Narrow write flit while DAT emits one Data write flit in the same cycle; blocking
one network does not block the other after classification. Each network keeps its own AW-to-WLAST
wormhole lock.

W has no address or class field, so an AW-order context FIFO records the class and route of every
accepted AW. Each accepted W beat uses the context at the FIFO head and is steered to that
burst's REQ or DAT pipeline; the context retires only on WLAST. Parallel NoC drain may therefore
reorder already-buffered packets across the two physical networks, but it cannot interleave W beats
or attach a W beat to a later AW. The shared AXI source still accepts at most one AW and one W beat
per cycle.

This parallelism does not relax an AXI ordering domain. AXI guarantees request order for the same
channel, ID and destination, while it gives no ordering guarantee across different peripheral
regions or memory locations ([AMBA AXI ordering model, Section A6.1](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/IHI0022H_amba_axi_protocol_spec.pdf)).
Narrow and Data are distinct address spaces and are already separate terms in the NMU ordering key.
Within one class and destination, requests remain on one physical network. Across classes, the
physical arrival order may differ; the NMU's per-ID B ordering state still returns write responses
in issue order. A requester that needs ordering across otherwise unordered regions must wait for
the earlier response before issuing the dependent transaction.

The surveyed reference reaches the same network-level parallelism with separate Narrow and Wide
AW/W state machines and separate request/wide arbiters ([AW/W selection](https://github.com/pulp-platform/FlooNoC/blob/2fa02eb23c1babef9a8f714715ea7c78de98c364/hw/floo_nw_chimney.sv#L1311-L1373),
[REQ arbitration](https://github.com/pulp-platform/FlooNoC/blob/2fa02eb23c1babef9a8f714715ea7c78de98c364/hw/floo_nw_chimney.sv#L1375-L1409),
[wide arbitration](https://github.com/pulp-platform/FlooNoC/blob/2fa02eb23c1babef9a8f714715ea7c78de98c364/hw/floo_nw_chimney.sv#L1448-L1479)).
Its two AXI class ports provide separate W streams; this design instead requires the AW-order
context FIFO because both classes share one AXI W channel. The current C++ NMU already has separate
REQ and DAT arbiter/allocation instances, but simultaneous same-cycle egress lacks a dedicated
test.

The reverse NSU NoC-to-AXI merge uses independent REQ Narrow and DAT Data ingress queues followed
by one work-conserving AW selector. A sole admissible class wins immediately; simultaneous classes
use round robin. Each accepted AW records `{class, burst_beats}` in a W-order FIFO, while the shared
W output serves only the FIFO head class through its final beat. AW may therefore admit later
transactions before an earlier burst completes its W data. This retains AW outstanding concurrency
and prevents class starvation. Its cost is intentional W head-of-line blocking when the head
class's next W beat is late; allowing the other class to bypass would break the accepted AW order
on the one downstream W channel.

The surveyed reference has separate Narrow and Wide downstream AXI class ports, so it does not
perform this shared-port merge. The current C++ NSU is the implementation reference for the merge:
it independently drains REQ and DAT ingress, round-robins simultaneous AW classes, records class
and burst length in `w_order_`, and blocks W behind the FIFO head. Direct tests for simultaneous
AW fairness and blocked-head W behavior are still missing.

The NoC carries a fixed 3-bit ID field (`NOC_ID_WIDTH`), so REQ/RSP/DAT links are fixed at
136/126/633 bits. The external `AXI_ID_WIDTH` is independently legal from 1 to 8 and is remapped
at the endpoint. The remap allocates one of eight NoC IDs to each distinct live external ID,
backpressures only an unseen ID when all eight are live, and restores the original AXI ID on B/R.
The generated field positions, flit containers, router ports and NoC class FIFOs therefore remain
one coherent fixed-width contract; `SRC_ID` and `SRC_PORT_ID` remain NI identity fields.

This trades at most eight live external-ID mappings per direction for fixed router wires, buffers,
crossbars, DPI records, and flit storage. No router or NoC FIFO pays for the maximum 8-bit external
ID. The endpoint table is supplied by the approved AXI remap primitive, so no project-local mapping
table or allocator is introduced. The surveyed reference likewise composes flits from parameterized packed header and payload types
([type definitions](https://github.com/pulp-platform/FlooNoC/blob/2fa02eb23c1babef9a8f714715ea7c78de98c364/hw/include/floo_noc/typedef.svh#L34-L81)).

## NSU downstream ID mapping

The surveyed reference design carries the request source in the packet header and preserves that
header beside the original AXI ID for response routing, but indexes the downstream Response Queue
by AXI ID alone. Two sources that present the same AXI ID therefore share one
downstream ordering stream. This is inferred from its [header
type](https://raw.githubusercontent.com/pulp-platform/FlooNoC/master/hw/include/floo_noc/typedef.svh),
[request/response path](https://raw.githubusercontent.com/pulp-platform/FlooNoC/master/hw/floo_axi_chimney.sv),
and [metadata buffer](https://raw.githubusercontent.com/pulp-platform/FlooNoC/master/hw/floo_meta_buffer.sv).

Three policies were considered:

| policy | downstream mapping key | consequence |
|---|---|---|
| AXI-ID-only | `noc_id` | smallest table and matches the surveyed reference, but unrelated sources using the same ID serialize |
| strict pass-through | none; drive `noc_id` directly | no allocation state, but cannot preserve independent source concurrency and cannot narrow the ID width |
| source-aware dynamic mapping | `{src_id, src_port_id, noc_id}` | preserves independent source streams at the cost of a bounded mapping table |

The design adopts source-aware dynamic mapping. `src_port_id` is part of the key because this
design carries it separately from `src_id`; omitting it would merge two endpoints attached to the
same node. Narrow and Data class are not key terms because both classes terminate at the same
external AXI interface and therefore share its ordering domain.

Allocation is identity-preferred. An existing key reuses its mapped downstream ID. A new key uses
`noc_id` when that value fits `NSU_AXI_ID_WIDTH` and is free; otherwise it receives the lowest free
downstream ID. A full table stalls only new keys. The write and read directions use separate
tables: AW allocates and B retires, while AR allocates and RLAST retires. A reference count keeps a
mapping live while transactions of that key remain outstanding. The tables live in `noc_clk`:
request allocation occurs before AW/AR enter their AXI async FIFOs, and response retirement occurs
after B/R leave those FIFOs. The FIFOs carry the mapped AXI ID; live mapping state never crosses.

`NSU_AXI_ID_WIDTH` defaults to the 3-bit NoC-carried `NOC_ID_WIDTH` and is legal from 1 to 8.
`NSU_MAX_ACTIVE_IDS` defaults to 8 and is legal from 1 through `2**NSU_AXI_ID_WIDTH`; it counts
live mappings, not ID bits. A wider external `AXI_ID_WIDTH` is compressed by the endpoint
remapper before entering the NI. This follows the surveyed separation between input ID width,
output ID width, and maximum unique IDs while retaining all eight default NoC IDs without
serialization.

`NSU_MAX_OUTSTANDING` independently sizes each read/write Response Queue, defaults to 32, and is
legal for power-of-two values from 1 through 256. This follows the surveyed separation between
transaction capacity and unique-ID capacity: one live mapping may own several outstanding
transactions, so `NSU_MAX_ACTIVE_IDS` cannot size the transaction records.

For multicast AW address replacement, the NSU follows the approved receiver-side table method.
It enables `ni_sam` only for multicast AW, obtains the matched range's X/Y coordinate offsets and
widths, and replaces only those bits with the local node coordinate. The lookup neither reroutes
nor reclassifies the request; `axi_ch` already carries the Config/Data class selected by the NMU.
Unicast bypasses lookup. A miss or a rule without explicit collective enable is malformed input,
not an unchanged-address fallback. This avoids adding coordinate metadata to every flit and
supports different layouts across SAM ranges at the cost of an AW-side comparator table in each
NSU.

The target block is named **Response Queue** (`nsu_response_queue` in RTL), and one stored record
is a `response_entry_t`. `ResponseHeader` is not used for this state because an entry also contains
ID, ordering, class, collective and narrow-read context; `response_header_t` remains reserved for
an actual NoC response header. The current C++ class remains `MetaBuffer` until the reference-model
alignment work begins.

The frozen entry fields are `src_id`, `src_port_id`, `noc_id`, `ordering_req`, `ordering_tag`,
`is_data`, `local_addr`, `len`, `size`, `burst`, `collective_op`, and `collective_mask`. Queue valid,
mapped downstream ID, mapping reference count, and the front-read beat counter remain separate
state because each is owned at another lifetime granularity. A single uniform NI request record
was rejected: carrying AW-only USER/collective fields through AR timing cuts and queues increases
stored width without adding information. The adopted channel-specific packed records keep those
bits only on AW while preserving common named ordering-domain and response-entry types. Packed
typing changes no combinational depth or register count beyond the fields listed.

## NI CDC and Router VC ownership

The adopted CDC boundary reuses the five AXI channel FIFOs. AW, W and AR cross toward the NI core;
B and R cross in the opposite direction. Each entry is one AXI channel record. There is no
additional complete-flit CDC stage. After the five FIFOs, SAM, ordering, packetization,
depacketization, channel assignment, credit management and all NoC class queues run in `noc_clk`.

This matches the surveyed [five-channel AXI CDC](https://github.com/pulp-platform/axi/blob/master/src/axi_cdc.sv),
which instantiates one dual-clock FIFO for each AXI channel. The owner-provided bridge RTL uses the
same five-FIFO topology but currently drives all five from one clock, so it is architectural
precedent rather than a CDC implementation. The surveyed NoC NI instead demultiplexes Read and
Write VCs before buffering and returns credit by consumed VC
([source](https://github.com/pulp-platform/FlooNoC/blob/2fa02eb23c1babef9a8f714715ea7c78de98c364/hw/floo_nw_chimney.sv#L252-L284)).
That placement is intentionally not copied: this target puts VC FIFOs only in the Router.

The four NoC-side queues are REQ, RSP, DAT Write and DAT Read. They are synchronous `noc_clk`
class FIFOs, not CDC or per-VC FIFOs. `NOC_FIFO_DEPTH`, default 8 and any positive power of two,
is their common entry count. Separate per-class depth parameters are not introduced.

| Flow | Storage owner | Backpressure |
|---|---|---|
| NI to Router REQ/RSP | NI class FIFO, then Router input FIFO | ready/valid |
| NI to Router DAT | NI class FIFO, then Router LOCAL per-VC input FIFO | NI sender counter per VC, seeded by `NOC_ROUTER_VC_DEPTH` |
| Router to NI REQ/RSP | Router output, then NI class FIFO | ready/valid |
| Router to NI DAT | Router output, then NI DAT Write or DAT Read class FIFO | ready/valid |
| Router to Router DAT | downstream Router per-VC input FIFO | per-VC credit in both directions |

The AXI-to-NoC assigner owns DAT VC selection. It reads the sender-side per-VC credit counters,
applies `NOC_DAT_VC_MODE`, stamps the selected `vc_id`, and decrements that counter on send.
`DataW` inherits its owning `DataAw` VC through WLAST. The credit state is not a FIFO: the credited
slots reside in the Router LOCAL input VC FIFOs. The NI has no per-VC pending or ingress queue.

Router-to-NI DAT ejection uses ready/valid because the NI receiver has shared class storage, not a
separately provisioned FIFO per VC. Combining per-VC credit and ready on this direction is rejected:
two authorities would define one transfer. Keeping per-VC receive credit would instead require
partitioned NI VC storage, which is outside the adopted ownership boundary.

| CDC partition | Benefit | Decision |
|---|---|---|
| Five AXI channel async FIFOs | reuses required protocol queues; all NI processing runs in `noc_clk` | adopted |
| Additional complete-flit async FIFOs | crosses already-packetized records | rejected as duplicate buffering |
| NI per-VC ingress FIFOs | symmetric credit protocol | rejected; VC storage belongs only to Router |

The reset contract is coordinated rather than independently recoverable. Integration fans out one
active-low system reset through separate `ACLK` and `noc_clk` reset synchronizers, then supplies
`ARESETn` and `noc_rst_n` to the NI. Assertion is asynchronous; deassertion is synchronous to each
destination clock, and release skew is legal. The NI has no `sys_rst_n` port. Any system reset
flushes the CDC FIFOs and all transaction state; one-sided reset and in-flight replay are not
supported.

The surveyed [five-channel AXI CDC](https://github.com/pulp-platform/axi/blob/master/src/axi_cdc.sv)
exposes distinct source- and destination-domain resets. Its supporting
[CDC library](https://github.com/pulp-platform/common_cells) lists a separate clearable FIFO for
one-sided reset, confirming that independent recovery requires extra reset coordination. The
surveyed NoC NI uses one clock/reset domain. This target therefore keeps the ordinary FIFO and
places the two reset synchronizers at system integration.

`NI_DAT_VC_DEPTH` is removed from the target. `NOC_ROUTER_VC_DEPTH`, default 8 and any power of two
at least 2, is both the Router input VC FIFO depth and the NI-to-Router DAT credit seed.
`AXI_FIFO_DEPTH`, default 8 and any power of two at least 2, is the common entry count of the five
AXI channel async FIFOs. These queues absorb clock-ratio variation and AXI backpressure; they do
not store transaction lifetime state.

All production FIFO depths use power-of-two entry counts and have no architectural maximum.
Synchronous FIFOs permit depth 1 unless a block-level relation raises the minimum; Router input VC
FIFOs require at least 2 because `almost_full_offset < depth`; AXI asynchronous FIFOs require at least 2
for Gray-pointer CDC. `NOC_ROUTER_OUTPUT_FIFO_DEPTH` is also defaulted to 8. These defaults are
`[TBD]` sizing points, not PPA optima; implementation signoff must sweep representative legal
depths before changing them. Pros: pointer/occupancy logic and memory inference stay uniform, and
customer sizing is not capped arbitrarily. Cons: a non-power-of-two depth that could save entries
for a specific workload is excluded, and default 8 may consume more area than the final measured
requirement.

The current C++ model remains a known divergence: it is single-clock, its `VcAllocator` owns
per-VC pending queues, and its LOCAL DAT receive path returns per-VC credits. Target alignment is a
separate implementation task.

## NSU downstream ID mapping consequences

For writes, the class selected by AW selects REQ or DAT Write for the whole burst. Existing
AW-order metadata preserves the AW/W association; the split does not permit W beats or later write
bursts to bypass an active burst. For responses, the Response Queue lookup by `BID` or `RID`
restores context before packetization selects RSP or DAT Read. A write entry retires only when its
complete B flit enters the RSP class FIFO. A read entry retires only when its complete RLAST flit
enters the RSP or DAT Read class FIFO. Earlier R beats retain the same entry. Each completed transaction
decrements the mapping reference count, and the downstream ID becomes free only when that count
reaches zero.

After classification, separate REQ/RSP/DAT Write/DAT Read FIFOs prevent one NoC class from consuming
another class's queue capacity. Before classification, Narrow and Data beats share their AXI channel
FIFO, so a blocked head can delay the other class. The design also cannot preempt a Data write burst
already using the single AXI W channel or a Data R beat held by the external AXI slave. Removing
those cases requires more queues before classification or another AXI interface; neither is adopted.

This policy has the following advantages:

- Different sources using the same `noc_id` can remain independently outstanding and may complete
  without artificial same-ID serialization at the downstream AXI interface.
- ID values remain unchanged in the common no-collision case, while independent
  external `AXI_ID_WIDTH` and downstream `NSU_AXI_ID_WIDTH` values remain legal.
- Storage scales with `NSU_MAX_ACTIVE_IDS`, not with the Cartesian product of all source and ID
  widths.
- Deterministic lowest-free allocation makes cycle-level verification reproducible.

The costs are:

- The NSU needs associative key comparison, free-ID selection, reference counts and Response
  Queue lookup instead of direct ID wiring.
- Five AXI channel async FIFOs per AXI interface add pointer and synchronizer area, but reuse the
  protocol queues that the interface already requires instead of adding a second CDC stage.
- A source collision can change the downstream ID value, so this is not strict pass-through even
  when the two configured widths match.
- Table exhaustion backpressures a previously unseen key; sizing therefore affects achievable
  multi-source concurrency.
- The current C++ reference model uses AXI-ID-only mapping and must be aligned before this policy
  becomes an implementation requirement.

Revisit this choice only if synthesis shows the bounded associative table on the timing-critical
path, or workload measurements show that same-`noc_id` traffic from different sources does not
benefit from independent completion. The fallback is AXI-ID-only mapping with per-ID response
entry FIFOs; strict pass-through is valid only when ID widths match and source-level
serialization is acceptable.

## AXI QoS to NoC QoS survey

Status: selectable shared or Read/Write-split DAT VC allocation approved. The first RTL transports
AXI QoS attributes but does not implement a NoC QoS policy. A DAT-only QoS extension is recorded
below and deferred.

AXI defines 4-bit `AWQOS` and `ARQOS` identifiers but leaves their exact system use
implementation-defined. The protocol recommends treating a larger value as higher priority and
defines zero as no QoS participation. A component may select a higher-QoS transaction only when
AXI ordering imposes no conflicting requirement; ordering takes precedence over QoS. See the
[AMBA AXI protocol specification, QoS signaling](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/IHI0022H_amba_axi_protocol_spec.pdf).

Confirmed implementations use two distinct steps: compress `AxQOS` into a small NoC traffic
class, then arbitrate class-specific queues or virtual channels. They do not normally allocate one
VC for every one of the 16 AXI values.

| Case | Confirmed behavior | Relevance |
|---|---|---|
| Public hard-memory NoC | the upper two `AxQOS` bits select one of four NoC urgency levels; zero is lowest and three highest | direct precedent for `AxQOS` compression rather than 16 VCs; [QoS mapping](https://www.intel.com/content/www/us/en/docs/programmable/768844/23-3/quality-of-service-qos-support.html) |
| Public programmable NoC | each port has eight credit-controlled VC FIFOs; high-priority requests precede low-priority requests, while per-VC tokens apportion service among eligible requests | precedent for separating priority from bandwidth fairness; [packet switch](https://docs.amd.com/r/en-US/pg313-network-on-chip/NoC-Packet-Switch) and [VC arbitration](https://docs.amd.com/r/en-US/pg313-network-on-chip/Virtual-Channel-Arbitration) |
| Surveyed open-source wide NoC | complete AXI AW/AR payloads, including `qos`, are transported; its optional wide-link VC mode separates write and read into two VCs, but its router arbitration does not consume `AxQOS` | transport and read/write separation are reusable; NoC QoS policy is not; [packetization](https://github.com/pulp-platform/FlooNoC/blob/2fa02eb23c1babef9a8f714715ea7c78de98c364/hw/floo_nw_chimney.sv#L1222-L1267), [VC count](https://github.com/pulp-platform/FlooNoC/blob/2fa02eb23c1babef9a8f714715ea7c78de98c364/hw/floo_nw_chimney.sv#L155-L158) and [VC arbiter ports](https://github.com/pulp-platform/FlooNoC/blob/2fa02eb23c1babef9a8f714715ea7c78de98c364/hw/floo_vc_arbiter.sv#L11-L30) |
| Open-source AXI NoC building blocks | AXI muxes used to construct a mesh NoC use round-robin arbitration and do not rank requests by `AxQOS`; a memory endpoint adapter does compare read and write QoS, but that decision is outside the NoC router | proof that AXI QoS transport is not equivalent to NoC QoS; [NoC architecture](https://arxiv.org/abs/2308.00154), [mux arbitration](https://github.com/pulp-platform/axi/blob/4da15979747f326bde2f9869c64e587ce599772c/src/axi_mux.sv#L264-L280) and [endpoint arbitration](https://github.com/pulp-platform/axi/blob/4da15979747f326bde2f9869c64e587ce599772c/src/axi_to_detailed_mem.sv#L292-L301) |
| Open-source protocol-independent NoC | AXI AW, W, AR, R and B use five fixed virtual networks to break protocol dependencies; `AxQOS` does not select the virtual network | protocol-deadlock classes and QoS classes are separate concerns; [AXI virtual-network map](https://github.com/ucb-bar/constellation/blob/c1b42cd0c7c7d6e4fc1ed9bf1f86fbd3b31ac510/src/main/scala/protocol/AXI4.scala#L291-L301) |

`NOC_DAT_VC_MODE` selects one of two elaboration-time policies and defaults to `SHARED`.
`DAT_NUM_VC` remains the only VC count parameter, with a legal range of 1 to 8 and a default of 2.

| Mode | Legal `DAT_NUM_VC` | Eligible VC set |
|---|---|---|
| `SHARED` | 1 to 8 | `DataAw`, `DataW` and `DataR` may use every VC |
| `READ_WRITE_SPLIT` | 2, 4, 6 or 8 | lower half for `DataAw`/`DataW`, upper half for `DataR` |

`DataW` inherits its owning `DataAw` VC in both modes. Split mode is system-wide: the NI allocator
and every DAT router output VA apply the same class mask when assigning or restamping `vc_id`.
Restricting only injection would allow a later hop to move a flit into the wrong class set. There
are no independent Read or Write VC-count parameters.

The DAT VC pool is not part of the protocol-deadlock proof. As in the surveyed
[wide-channel mapping](https://github.com/pulp-platform/FlooNoC/blob/main/docs/floonoc/links.md#narrow-wide-axi-to-req-rsp-wide-mapping),
`DataAr` uses REQ, `DataAw` / `DataW` and `DataR` use DAT, and `DataB` uses RSP. A Data-AR can
create a Data-R dependency from REQ to DAT; accepted Data-AW/Data-W can create a Data-B dependency
from DAT to RSP; narrow requests create a direct REQ-to-RSP dependency. No response creates a
dependency back to REQ or DAT. The resulting message dependency graph is therefore acyclic:

```text
REQ -> DAT -> RSP
```

One DAT VC in `SHARED` mode is therefore legal and protocol-deadlock-free. The default of two is a
performance policy. `SHARED` avoids reserving half of the DAT buffers for an idle direction, at
the cost of possible Read/Write head-of-line blocking within one VC. `READ_WRITE_SPLIT` removes
that cross-class blocking, at the cost of stranded capacity under asymmetric traffic.

Both modes keep the same physical buffering. The NI has one DAT Write and one DAT Read class FIFO;
the Router alone owns per-VC input FIFOs. Split mode changes the eligible VC masks applied by the
NI sender and Router VA, not NI storage. Its isolation and possible stranded capacity therefore
occur in Router VC capacity, not in duplicated NI queues.

The first RTL retains `AWQOS` and `ARQOS` on the AXI interface and transports them unchanged in
the AW and AR flit payloads. It does not add QoS to the 48-bit NoC header, map `AxQOS` to a VC, or
rank router requests by `AxQOS`. DAT VC allocation remains credit-aware, with round-robin selection
among the mode-eligible VCs allowed by the ordering rules. `DataW` inherits the VC selected for its
owning `DataAw`, and the wormhole grant remains locked through WLAST. Router arbitration continues
to use round robin. QoS adds no width beyond the selected ID-width layout. The current C++ model
implements only the `SHARED` candidate set and requires alignment for `READ_WRITE_SPLIT`.

The deferred DAT-only QoS design keeps QoS metadata separate from `vc_id`. It adds the complete
4-bit `AxQOS` value as router-visible header metadata, increasing the common header from 48 to
52 bits and, at fixed `NOC_ID_WIDTH = 3`, REQ/RSP/DAT widths from 136/126/633 to
140/130/637 bits. `DataAw` and every owning
`DataW` flit carry `AWQOS`; the NSU Response Queue preserves `ARQOS` and restores it on each
`DataR` flit. At each packet boundary, an output arbiter selects the highest eligible QoS value,
round-robins ties, and holds the winner through tail. This is best-effort priority: it does not
guarantee bandwidth, bounded latency, or freedom from low-priority starvation. Aging belongs only
with a future bounded-progress requirement; weighted service belongs only with a bandwidth-share
requirement. Extending QoS to REQ/RSP is a separate architecture decision.

Direct `AxQOS`-to-`vc_id` mapping is rejected for this extension. Default `AxQOS = 0` traffic would
collapse onto VC0 and waste the remaining VCs. `READ_WRITE_SPLIT` partitions by AXI direction, not
QoS, and does not change this decision. The C++ model and generated wrappers now take the approved
`NOC_DAT_NUM_VC = 2` default; model implementation of the non-default split mode remains separate
from QoS.

## Production shared primitive policy

Production RTL obtains synchronous FIFO, asynchronous FIFO, and reusable register-slice storage
from the exact external revision in `rtl/Bender.yml`; source and license evidence is recorded only
in the Provenance section of `docs/verification-environment.md`. Project adapters may translate
types and handshakes but may not reproduce storage arrays, pointers, CDC synchronizers, or generic
register-slice state. A custom primitive is rejected unless a concrete library gap and replacement
semantics are approved in this file before implementation. Block-specific architectural state is
outside this restriction.

For register slices, implementation starts by comparing the reference-only source behavior and
tests, then maps the required semantics onto the production-approved primitive. Interface-specific
`*_REG_TYPE` parameters use 0 for bypass, 1 for a simple register, and 2 for a skid buffer. Each
parameter keeps its separately approved default; this rule does not introduce a global default or
permit reference-only source in the production build.

The approved lookup order is exact-contract reuse, not brand preference. Complete AXI-interface
register units may use the approved AXI library. Internal packed ready/valid paths do not assemble
AXI behavior from AXI-Stream interfaces. Synchronous class FIFOs use `cc_fifo`; the five AXI CDC
channels use `cc_cdc_fifo_gray`; SAM uses `cc_addr_decode`. The owning block maps handshakes and
checks legal depth values. No project FIFO wrapper or duplicate wrapper-only regression is kept.

This choice avoids the extra AXI-Stream sidebands and output-prefetch capacity semantics observed
in the surveyed stream FIFO. It also avoids adopting a context-specific local FIFO whose full-rate
behavior depends on caller-side gating and whose CDC pointer contract differs from the production
requirement. The selected primitives keep exact power-of-two storage ownership and an already reviewed
CDC implementation. The cost is that each block top must expose the primitive push/pop mapping;
that mapping is local wiring, not a second storage abstraction.

## Model-facing REQ/RSP held-valid adaptation

REQ and RSP retain standard held ready/valid semantics: a transfer occurs only on
`valid && ready`, and the source keeps valid plus the complete flit stable while stalled. The C++
model emits one-cycle pop strobes, so the verification-only NMU REQ, NSU RSP, and Router REQ/RSP
egresses capture those strobes with the approved `common_cells` `spill_register`. The existing DPI
output register is treated as a pending input stage: another model pop is blocked until that stage
has entered the spill register. Model-facing Router ingress converts each wire handshake back to
one C++ injection pulse. DAT does not enter this path and remains credit-controlled.

This follows the upstream stream-cut precedent, which instantiates `spill_register` per elastic
channel rather than changing ready/valid into a pulse protocol
([`floo_cut.sv`](https://github.com/pulp-platform/FlooNoC/blob/cb7b2ba3fd4b7eac340a4117ffba05c2a9757699/hw/floo_cut.sv#L43-L69)).
A standalone flow-control normalizer and a project-local FIFO are rejected. The adaptation stays
in `ref_model/top/*_wrap.sv`; no production module under `rtl/` contains it.

Pros: stalled flits cannot be dropped or changed; randomized ready stalls remain meaningful; the
storage and handshake implementation comes from the pinned approved primitive; REQ/RSP wire
semantics match production RTL without changing the C++ cores. Cons: the model-facing REQ/RSP path
adds one wire cycle, the pending DPI register can introduce an input bubble, and model-to-model
Router composition must handshake-qualify ingress before recreating the C++ pulse. These are
verification timing costs, not production microarchitecture.

## Rectangular unicast and square collective signoff

`MESH_X_DIM` and `MESH_Y_DIM` are independently selected from {2, 4, 8, 16}. Unicast routing is
supported and verified on rectangular as well as square meshes. The first RTL target guarantees
multicast/collective operation only when the dimensions are equal; rectangular collective support
is deferred even though the current reference model exercises some rectangular route masks.

Pros: customers can size unicast fabrics to non-square floorplans without paying for unused
routers, while collective implementation and signoff stay aligned with the intended square-mesh
workload. Cons: enabling collectives can require a different topology, and reference-model success
on a rectangular collective case is not production conformance evidence. Rectangular collective
support should be added only with explicit product demand and complete fork/join signoff.

## Canonical AXI AW payload

`AWUSER` is part of the canonical `ni_signals_pkg::axi_aw_t` payload. The temporary
`nmu_sam_aw_t` wrapper is removed; `nmu_sam` and the NMU request path consume `axi_aw_t`
directly.

This keeps the AXI channel record self-contained and prevents a SAM-specific type from
leaking into generic request-path, CDC, FIFO, and testbench boundaries. The cost is explicit:
`axi_aw_t` grows from 80 to 138 bits, and records containing it grow by 58 bits. That storage
and CDC-width increase is required because AWUSER must remain associated with AW through the
request path; dropping or carrying it in a parallel untyped sideband would make the channel
contract easier to misuse.
