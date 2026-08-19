# NSU RTL verification plan

This plan defines the behavior-level verification contract for the target NSU before RTL exists.
It maps the approved NSU architecture to the S0 and S1 work packages, identifies the C++ model
alignment needed for trustworthy comparison, and defines the zero-hop hybrid co-simulation gate.
It does not define an RTL microarchitecture.

## 1. Authority and open gates

The verification oracle is applied in this order:

1. `docs/noc-target-spec.md`, the target overlays in `docs/nsu-spec.md`, and the approved decisions
   in `docs/trade-off.md`;
2. the wrapper-facing target contract and canonical parameter table in `rtl/README.md`;
3. generated types and constants from `specgen/source/`;
4. the current C++ model and its tests, only where they agree with items 1 to 3;
5. the issue text and external references.

Two naming and behavior gates must be resolved before their dependent RTL package can pass:

- The issue's “response-header FIFO” means the approved **Response Queue**
  (`nsu_response_queue`) containing `response_entry_t` records. `ResponseHeader` is not accepted as
  a second state structure or as a shortened record: routing, original ID, ordering, class,
  collective, and narrow-read context must remain together.
- The target uses source-aware dynamic downstream-ID mapping keyed by
  `{src_id, src_port_id, noc_id}` with separate read and write tables. The AXI-ID-only rule in the
  current-model acceptance section of `docs/nsu-spec.md` describes `MetaBuffer`, not target RTL.
  S0 differential comparison is gated on the selective model alignment in Section 9.

The target downstream-ID contract is approved. `NSU_AXI_ID_WIDTH` defaults to `AXI_ID_WIDTH = 3`
and is legal from 1 to 8. `NSU_MAX_ACTIVE_IDS` defaults to 8 and is legal from 1 through
`2**NSU_AXI_ID_WIDTH`; therefore the default 3-bit interface supports 1 through 8 live mappings.
The external initiator may use the independently wider `INITIATOR_ID_WIDTH`; the endpoint remapper
compresses it to the NoC-carried `AXI_ID_WIDTH` before the request reaches the NI.
`NSU_MAX_OUTSTANDING` defaults to 32 and is a power of two from 1 through 256; it independently
sizes the read and write Response Queues and is not an alias for the live-mapping count.

## 2. Verification levels

| Level | DUT scope | Primary evidence | Exit condition |
|---|---|---|---|
| S0 | `nsu_depacketize` and `nsu_response_queue` | directed and constrained-random unit tests, local assertions, transaction-level reference comparison after alignment | every request class reconstructs correctly; write association and downstream-ID lifetime rules hold under backpressure and response interleaving |
| S1 | `nsu_axi_cdc`, `nsu_packetize`, `nsu_channel_assign`, and `nsu` top | child-unit tests, block-level random tests, assertion/coverage results, and reference-NMU-to-RTL-NSU hybrid co-sim | all five AXI channels remain independent, response physical networks make independent progress, credit and CDC/reset invariants hold, and hybrid pass is non-vacuous |

S0 proves that accepted NoC requests become the correct AXI transactions and that enough context is
retained to route responses. S1 proves the temporal boundaries around that behavior. Exact pipeline
latency is not compared unless a later approved specification adds a target-RTL latency requirement.

### 2.1 Acceptance trace

| Required behavior | Planned evidence |
|---|---|
| shared generated SAM and multicast-AW-only coordinate lookup | S0-SAM-01 through S0-SAM-06, NSU-A11 |
| AW/W/AR reconstruction and Narrow/Data request scheduling | S0-REQ-01 through S0-REQ-09 |
| Response Queue lifetime and legal downstream-ID mapping | S0-RQ-01 through S0-RQ-11, NSU-A04 through NSU-A06 |
| independent AXI channels | S0-REQ-07, S1-AXI-01/02, S1-CDC-02 |
| concurrent B/R scheduling and independent RSP/DAT progress | S0-RQ-09, S1-RSP-01 through S1-RSP-04 |
| credit-gated DataR | S1-DAT-01 through S1-DAT-07, NSU-A09 |
| CDC and coordinated reset | S1-CDC-01 through S1-CDC-05, NSU-A10 |
| illegal parameters | Section 6 elaboration-positive and expected-fail matrix |
| behavior under backpressure and response interleaving | Sections 4, 5, and the two sustained scenarios in Section 8 |
| reference classification and license preservation | Section 10 and the Provenance table in `docs/verification-environment.md` |
| reference NMU to RTL NSU zero-hop hybrid | Section 11, including F0 adaptation, independent stalls, comparison scope, and non-vacuous counters |
| no speculative RTL | plan and provenance documentation only; no production module is created or instantiated by issue #12 |

## 3. Testbench model and checking

The unit environment contains:

- independent REQ and DAT request drivers with legal packet constructors;
- independent AXI AW, W, and AR sink agents, each with its own ready generator;
- independent AXI B and R source agents that obey held-valid and legal per-ID ordering;
- independent RSP ready and per-VC DAT credit agents;
- accepted-transfer monitors at every boundary;
- a request scoreboard, a Response Queue/mapping predictor, a response packet predictor, and
  conservation counters.

Scoreboards operate on handshakes, not attempted transfers. For ready/valid, acceptance is
`valid && ready`. For NSU DAT injection, acceptance is a valid strobe on a VC whose sender counter
was nonzero immediately before the edge. The scoreboards retain full-width field values and compare
them only after the corresponding acceptance event.

Random stalls are generated independently per channel. A random test is invalid unless its required
stall, concurrency, mapping, and completion coverage bins are hit. A watchdog may fail a wedged run,
but reaching the watchdog or simply running for a fixed cycle count cannot produce PASS.

## 4. S0 request and Response Queue tests

### 4.1 Shared SAM and multicast address layout

| ID | Stimulus and pressure | Required observation |
|---|---|---|
| S0-SAM-01 | unicast AW, W, AR, and idle cycles with arbitrary address-bus values | `ni_sam.lookup_en_i` is zero; result/valid/error are zero; no inactive traffic can alter an address or create an error |
| S0-SAM-02 | multicast AW hits collective-enabled memory and config ranges while AXI AW is independently stalled | lookup enables only for that AW; `nsu_depacketize` uses the matched X/Y `{offset,len}` fields, changes only coordinate bits, and holds the complete translated AW stable while stalled |
| S0-SAM-03 | multicast AW misses, then hits a rule with explicit `en_collective: false`, then one with the key absent | each packet is rejected as malformed with no default layout; false/absent package entries have `collective_en=0` and both selector lengths zero |
| S0-SAM-04 | checker-owned broad authored rule 0 overlaps narrower rule 1 at the multicast address | the shared wrapper selects authored rule 0, proving the emitted reverse array preserves first-match behavior with the pinned highest-index priority |
| S0-SAM-05 | generate and elaborate the checked-in 2x2 and 4x4 configurations with the same `ni_sam` unit used by NMU | exact declarations, rule counts, indices, ranges, destination/port/class, collective flags, and X/Y selectors match N0-SAM-11 through N0-SAM-13 in `docs/nmu-verification-plan.md`; both generations are byte-identical and no golden RTL is committed |
| S0-SAM-06 | exercise zero/negative, non-4-KiB-aligned, overflowing, inverted, invalid membership/port/class, incomplete coverage, and unrepresentable collective-enabled ranges, then run `make clean-generated` | every invalid case fails generation, legal overlap succeeds, and `$(BUILD_ROOT)/generated/` contains no generated package after cleanup |

### 4.2 Request reconstruction and scheduling

| ID | Stimulus and pressure | Required observation |
|---|---|---|
| S0-REQ-01 | one Narrow AW/W burst on REQ; vary legal address, ID, burst fields, strobes, WLAST, ordering, source/port, and collective context | AXI AW and every W beat are bit-accurate; unicast address is unchanged, while multicast replaces only the coordinate field returned by `ni_sam` and never subtracts a region base; the Response Queue write entry contains the complete context |
| S0-REQ-02 | one Narrow AR on REQ, including unaligned narrow-lane cases | AXI AR is bit-accurate and its unicast address remains unchanged; the read entry retains address/burst context needed to select every returned narrow lane |
| S0-REQ-03 | Data AW/W on DAT and Data AR on REQ | all three AXI channels reconstruct bit-accurately; class affects NoC response selection but not the downstream AXI ordering domain |
| S0-REQ-04 | REQ and DAT present admissible AWs in the same cycle, then continuously replenish both; repeat after both have waited behind full write-admission state | the first simultaneous tie follows reset state; grants alternate on accepted simultaneous-class AWs; rejected/stalled attempts do not rotate priority; either class wins immediately when it is the only admissible class |
| S0-REQ-05 | accept several AWs from alternating classes before completing earlier W bursts | AW continues independently; the W-order FIFO records `{class, burst_beats}` in accepted-AW order |
| S0-REQ-06 | with the W-order head belonging to one class, delay that class's next W while the other class has W available | the later class does not bypass; W resumes from the head class and changes class only after the exact final beat |
| S0-REQ-07 | hold each of AWREADY, WREADY, and ARREADY low separately and in combinations while other channels remain ready | the blocked channel holds valid and fields stable; unrelated channels continue until their own capacity limit |
| S0-REQ-08 | fill each logical request/class FIFO and the W-order FIFO independently | ready propagates only from the exhausted resource; no accepted request is lost, duplicated, overwritten, or consumed twice |
| S0-REQ-09 | alternate single-beat and multi-beat writes, then inject WLAST early, late, or without an accepted AW in negative tests | legal bursts produce exactly `AWLEN + 1` W transfers; each illegal sequence fires its intended checker |

Tests S0-REQ-04 and S0-REQ-06 close the two direct coverage gaps in the current C++ scheduler tests.
The testbench must not infer fairness from a final transaction count; it observes grant order while
both classes are continuously eligible.

### 4.3 Response Queue and downstream-ID mapping

| ID | Stimulus and pressure | Required observation |
|---|---|---|
| S0-RQ-01 | allocate one AW and one AR for every request class | both directions retain requester, port, NoC ID, ordering fields, and class; write records also retain collective context and read records retain lane/burst context; tables are independent |
| S0-RQ-02 | repeat one mapping key with several outstanding transactions | the mapped downstream ID is reused and its reference count changes once per accepted request/completed response |
| S0-RQ-03 | present a new key whose NoC ID fits the downstream width and is free | identity mapping is selected |
| S0-RQ-04 | collide two sources or source ports on the same NoC ID, and present an out-of-range NoC ID when the downstream width is narrower | the first legal identity remains stable; the other key receives the lowest free downstream ID; no two live keys share an ID |
| S0-RQ-05 | fill a direction's mapping table, then present an existing key and a new key | the existing key can progress subject to queue capacity; only the unseen key stalls until an ID is freed |
| S0-RQ-06 | interleave legal B responses across mapped IDs and issue multiple transactions on one key | context is returned in the downstream ID's FIFO order; response packets restore the original ID and requester fields |
| S0-RQ-07 | interleave R beats from different mapped IDs, including multi-beat bursts | every beat uses the correct record; a record and mapping reference retire only when the complete RLAST response flit is accepted by its class FIFO |
| S0-RQ-08 | stall the RSP or DAT Read class FIFO during B, non-last R, and RLAST packetization | lookup state is non-destructive while stalled; B/last-R commit occurs only on class-FIFO acceptance, never on AXI acceptance or attempted NoC enqueue |
| S0-RQ-09 | accept B and R in the same cycle, including equal numeric BID/RID | separate direction tables return both contexts without collision or implicit priority |
| S0-RQ-10 | inject an unmapped BID/RID, an R beat after retirement, and reference-count underflow in checker-negative configurations | each violation is detected; no default or stale context can form a response packet |
| S0-RQ-11 | fill the approved Response Queue entry capacity in one direction using repeated and distinct live keys, then complete one transaction | every further AW or AR allocation in the exhausted direction stalls; no live entry is overwritten; W and the opposite direction continue; one accepted B or RLAST response-packet enqueue frees exactly one entry and releases a mapping only when its reference count reaches zero |

The mapping predictor is key-based and must not reuse the C++ `MetaBuffer` implementation until the
alignment gate closes. This keeps a shared implementation bug from appearing in both DUT and
scoreboard.

## 5. S1 channel, scheduling, credit, CDC, and reset tests

### 5.1 AXI and response-channel independence

| ID | Stimulus and pressure | Required observation |
|---|---|---|
| S1-AXI-01 | independently stall AW, W, and AR after all three are active | each output holds its own valid/data; progress on one channel neither consumes nor changes another channel |
| S1-AXI-02 | independently pause B and R sources, with legal response interleaving across IDs | accepted beats are neither coupled nor reordered; both CDC paths can accept on the same ACLK edge |
| S1-RSP-01 | make B and NarrowR continuously available while RSP is ready | the RSP scheduler is work-conserving and bounded-fair between the two eligible streams; one RSP flit transfers per cycle at most |
| S1-RSP-02 | provide DataB and DataR together | DataB uses RSP and DataR uses DAT; both physical outputs may transfer in the same `noc_clk` cycle |
| S1-RSP-03 | stall RSP while DAT has credit, then exhaust DAT credit while RSP is ready | one physical network's stall does not stop the other; backpressure reaches only the source/class queue that has exhausted capacity |
| S1-RSP-04 | interleave NarrowR and DataR bursts from different IDs, including concurrent B traffic | every response uses the saved class and original ID; beats of each AXI read burst remain ordered and RLAST retires exactly once |
| S1-BUF-01 | fill REQ, RSP, DAT Write, and DAT Read class FIFOs one at a time | each FIFO provides its approved `NOC_FIFO_DEPTH`; full in one class cannot consume another class's storage |

“Independent” permits pressure to propagate through a genuinely shared bounded resource, such as a
full AXI CDC FIFO or the one downstream W channel. It does not permit a control dependency that
waits for an unrelated channel handshake.

### 5.2 DataR scheduling and credit

| ID | Stimulus and pressure | Required observation |
|---|---|---|
| S1-DAT-01 | reset, then send DataR while varying credit returns per VC | every sender counter starts at `NOC_ROUTER_VC_DEPTH`, decrements once per accepted flit, increments once per credit pulse, and stays in range |
| S1-DAT-02 | drive a DataR whose approved fixed-hash VC has zero credit while another eligible VC has credit | the flit waits and is retried on its required VC; it never spills to another VC |
| S1-DAT-03 | interleave multi-beat DataR bursts with equal and different `(dst_id, rid)` keys | every beat of one burst receives the same hash-selected VC and `fixed_vc=1`; distinct keys follow the specified hash independently |
| S1-DAT-04 | hold all eligible DataR credits at zero until the DAT Read FIFO fills, while B/NarrowR traffic continues | no zero-credit DAT send occurs; pressure reaches AXI RREADY through the finite buffers; RSP continues to make progress |
| S1-DAT-05 | return credits after a witnessed zero-credit interval | the exact blocked sequence drains without loss, duplication, counter overflow, or permanent stall |
| S1-DAT-06 | run `SHARED` across legal VC counts and `READ_WRITE_SPLIT` across its legal even counts | DataR uses all VCs in `SHARED` and only the upper-half mask in split mode; mode changes neither packet fields nor response ordering |
| S1-DAT-07 | continuously offer DataR while its required VCs remain credited and the DAT output is otherwise free; add simultaneous RSP load in a second phase | after pipeline fill, DAT accepts one flit per `noc_clk` cycle with no avoidable bubble; independent RSP progress neither inserts a DAT bubble nor consumes DAT credit |

The current C++ model implements only `SHARED`. `READ_WRITE_SPLIT` is therefore checked against the
approved mask/hash rules, not by differential model output. A staged implementation that supports
only `SHARED` cannot claim full canonical-mode signoff; it must report split-mode coverage as open
and must never silently behave as `SHARED` when configured for split mode.

### 5.3 Five-channel CDC and coordinated reset

| ID | Stimulus and pressure | Required observation |
|---|---|---|
| S1-CDC-01 | run equal, ACLK-faster, `noc_clk`-faster, and non-integral clock relationships with randomized initial phase | accepted AW/W/AR/B/R records cross exactly once, in channel order, with stable payload under destination stalls |
| S1-CDC-02 | concurrently saturate all five AXI channels under independent source and destination pauses | no cross-channel coupling, pointer corruption, loss, or duplication occurs; conservation holds per channel |
| S1-CDC-03 | preload all five CDC FIFOs and all four NoC class FIFOs, then assert the common system reset | every valid, queue entry, Response Queue entry, W-order entry, scheduler state, and sender credit state is flushed or restored to its reset value |
| S1-CDC-04 | deassert the two synchronized domain resets on different legal edges, then issue traffic only after each local reset is released | release skew creates no phantom transfer or stale response; normal traffic completes after both domains are active |
| S1-CDC-05 | assert reset while AW/W, B/R, RSP stalls, and a zero-credit DAT condition are all present | no pre-reset transaction is replayed; post-reset traffic completes from clean state |

One-sided reset recovery is outside the approved contract and is not a positive test. The
integration fixture always derives both local resets from one system reset, with asynchronous
assertion and domain-synchronous deassertion. The existing common-primitive regression remains a
prerequisite; S1-CDC tests prove the NSU wiring and transaction state around those primitives.

## 6. Parameter and interface guards

Elaboration-negative tests instantiate one bad setting at a time and require the expected `$fatal`.
A timeout or unrelated compile error is not a passing guard test.

| Parameter/contract | Positive matrix | Negative matrix |
|---|---|---|
| `AXI_ID_WIDTH` | 1, default, 8; generated REQ/RSP widths agree at every port | 0 and 9; any generated-width mismatch |
| `NSU_AXI_ID_WIDTH` | 1, default, 8 | 0 and 9 |
| `NSU_MAX_ACTIVE_IDS` | 1, default; `2**NSU_AXI_ID_WIDTH` at widths 1, default, and 8 | 0 and `2**NSU_AXI_ID_WIDTH + 1` |
| `NSU_MAX_OUTSTANDING` | 1, default, 256 | 0 and a positive non-power-of-two value |
| `NOC_DAT_NUM_VC` | 1, default, 8 in `SHARED`; 2, 4, 6, 8 in `READ_WRITE_SPLIT` | 0, 9; split with 1 or any odd count; out-of-range credit-vector width |
| `NOC_DAT_VC_MODE` | both approved encodings | any other encoding |
| `AXI_FIFO_DEPTH` | 2, default, 32 | 0 and a positive non-power-of-two value |
| `NOC_FIFO_DEPTH` | 1, default, 32 | 0 and a positive non-power-of-two value |
| `NOC_ROUTER_VC_DEPTH` | 2, default, 32 | 0, 1, and a positive non-power-of-two value |
| generated SAM | deterministic 2x2/4x4 packages, legal overlap, explicit collective true/false/absent, canonical field-width derivation | empty/invalid range or membership, overflow, incomplete required coverage, unrepresentable requested collective layout, or a field/type width mismatch; overlap is not negative |

The test records the instance path and expected diagnostic for each guard. This prevents an
unrelated fatal in another generated block from satisfying the test.

## 7. Assertion and checker plan

Assertions are local to the owner of the state they check. Ready/valid interfaces use standard
elastic-channel semantics: once valid is asserted, valid and payload remain stable until the
handshake; ready is not required to be registered unless the approved implementation contract
requires it. Every checker must have a fault-injection or illegal-stimulus test proving it can fail.

| ID | Owner | Invariant |
|---|---|---|
| NSU-A01 | depacketize plus unit-interface monitor | accepted ingress `axi_ch` is legal for its physical network; the environment checks the routing precondition that `dst_id`/port targets this NSU rather than requiring duplicate route decode in the DUT |
| NSU-A02 | AXI egress/RSP source | AW, W, AR, and RSP valid plus payload are held stable while stalled |
| NSU-A03 | request scheduler | no W transfers without an accepted AW-order entry; beat count is `AWLEN + 1`; WLAST is exact; non-head class cannot transfer |
| NSU-A04 | Response Queue | no read/write allocation overwrites a live record; lookup is live and direction-correct; commit cannot underflow |
| NSU-A05 | ID mapper | live keys map one-to-one to downstream IDs within each direction; reference count is nonzero for a live mapping; full-table admission blocks only a new key |
| NSU-A06 | response path | every accepted BID/RID has a live matching entry; only B or accepted RLAST response-packet enqueue retires one transaction; every emitted B/R flit has `flit_tail=1` |
| NSU-A07 | class FIFOs | occupancy remains in range; accepted input minus accepted output equals occupancy; no full overwrite or empty read |
| NSU-A08 | response schedulers | at most one RSP and one DAT flit transfer per cycle; B/NarrowR arbitration is work-conserving and bounded-fair while both remain continuously eligible |
| NSU-A09 | DAT assigner | selected VC is mode-eligible and equals the required DataR hash; `fixed_vc=1`; zero-credit send is impossible; an eligible head transfers whenever its required VC has credit and the output can accept; credit count stays within `[0, NOC_ROUTER_VC_DEPTH]` |
| NSU-A10 | CDC/reset | no output valid escapes reset; no transfer occurs from stale pre-reset state; each local state element is controlled by its documented domain reset |
| NSU-A11 | multicast SAM | lookup enable implies an accepted/present multicast AW; hit must be valid and collective-enabled before coordinate replacement; overlap resolves authored-first; miss has no default; unicast AW, W, and AR never enable lookup |

In addition to temporal assertions, build/source-list review checks that production NSU source
lists contain no verification adapter and that the NSU owns no per-VC pending FIFO. The latter is
an ownership check, not an assertion about an internal signal name.

## 8. Functional coverage and behavior-focused scenarios

Coverage is collected on accepted events. The S0/S1 regressions must close these crosses without
inventing a percentage target:

- physical ingress × `axi_ch` × Narrow/Data class × single/multi-beat;
- SAM lookup enabled/disabled × hit/miss/overlap × collective true/false/absent × 2x2/4x4 selector layout;
- AW arbitration state × eligible classes × selected class;
- W head class × other-class availability × head stalled/accepted × WLAST;
- mapping direction × identity/fallback/reuse/table-full × same/different source and port;
- response type B/R × class × original/mapped ID differs × class-FIFO stall;
- B and R concurrent acceptance × RSP and DAT concurrent transmission;
- B/NarrowR RSP contention × grant × downstream stall;
- DataR VC × mode × credit zero/nonzero × credit return × burst position;
- continuously eligible credited DataR × DAT transfer/no-transfer × simultaneous RSP progress;
- each AXI channel stall × each unrelated channel progress;
- clock relationship × randomized phase × CDC direction × FIFO near-full;
- reset with state nonempty × release order × first post-reset channel.

Two long-pressure scenarios are required beyond isolated channel tests:

1. Maintain both request classes, several IDs, independent AW/W/AR backpressure, and delayed head-W
   beats. Check accepted-AW order, work-conserving AW selection, intentional W head blocking, and
   unrelated AR progress.
2. Return B and interleaved R bursts from several mapped IDs while alternately stalling RSP and
   withholding DAT credits. Check independent B/R acceptance, per-ID context, exact RLAST
   retirement, simultaneous RSP/DAT progress, and recovery after both stall types.

Each scenario logs its seed and all pause-generator configurations so a failure is replayable.

## 9. Selective C++ reference-model alignment

The C++ model is like an executable RTL reference block: it is useful only when its state contract
matches the target. Alignment is deliberately selective; the model does not reproduce asynchronous
FIFO internals or an arbitrary production pipeline.

| Area | Current evidence/gap | Required alignment before use as oracle |
|---|---|---|
| Response tracking | `MetaBuffer` has per-ID FIFOs and correct peek/commit behavior, but keys by ID only | replace or front it with source-aware read/write Response Queue behavior, complete `response_entry_t` context, reference counts, identity-preferred/lowest-free mapping, and commit on accepted response-class enqueue |
| Downstream ID width | current beat types and collapse/pass-through tests assume the fixed model ID width and only two `max_unique_ids` modes | parameterize the compared downstream ID behavior after the canonical Response Queue parameters are approved; add collision, fallback, reuse, full-table, and legal/illegal-width tests |
| Narrow/Data request scheduler | current depacketizer has independent ingress, round-robin AW selection, `w_order_`, and blocked-head W behavior, but `Depacketize::pop_aw` rotates `aw_prefer_data_` before rejecting a full write pool | preserve the accepted scheduling policy, but advance round-robin state only on an accepted simultaneous-class AW; add direct simultaneous-AW fairness under admission stalls, sole-eligible work conservation, multiple-AW outstanding, and blocked-head W tests before differential S0 scheduling claims |
| B/R and RSP/DAT scheduling | current packetizer has separate B/R staging and class selection; current tests prove selected fields but not all simultaneous pressure cases | expose accepted events to the checker and add concurrent B/R plus independent RSP/DAT pressure tests; compare order/content, not internal tick count |
| Target NI buffering | current `VcAllocator` owns per-VC pending queues and DAT receive uses model-side credit | for target-conformance comparison, model four bounded class FIFOs and head-only credit gating with no NI per-VC storage; keep the F0 adapter for the model/target DAT receive mismatch |
| CDC/reset | model is single-clock | do not emulate pointer synchronizers or compare CDC latency in C++; use RTL primitive and NSU integration assertions/scoreboards |

Until an alignment row closes, tests use an independent behavioral predictor for that row and may
still use the model for packet encoding/decoding fields that have been shown equivalent. The current
fixed-cycle timing and model-only DAT receive-credit behavior are explicitly outside target RTL
comparison.

## 10. Reference classification and license handling

The source IDs, inspected revisions, allowed uses, and license obligations are recorded only in the
Provenance section of `docs/verification-environment.md`.

- Normative protocol sources define legal AXI behavior.
- Approved repository specifications and generated constants define this DUT.
- The project-owned C++ model is a comparison oracle only inside the aligned scope in Section 9.
- Production-approved external source P1 is an architectural/behavior reference for response
  tracking and packet scheduling. This plan copies or instantiates none of it.
- Reference-only external source P2 informs pause-generator, channel-independence, and CDC test
  intent. Its RTL must never be copied, adapted, or instantiated in production.

Any future reuse decision is a separate reviewed change. Original copyright and SPDX/license
notices must remain intact; no third-party notice may be removed or rewritten.

## 11. Reference-NMU to RTL-NSU zero-hop co-simulation

### 11.1 Composition and F0 adapter

The hybrid fixture drives the reference NMU from an AXI manager and connects it to RTL NSU without
a Router:

```text
AXI manager -> reference NMU -- REQ --------------------> RTL NSU -> AXI memory model
                 |               RSP <-------------------- |
                 +-- DAT Write -> F0 flow adapter -------> |
                 |               DAT Read + credit <------+
```

- REQ is direct ready/valid.
- RSP is direct ready/valid.
- RTL NSU DataR injection and its per-VC Router-credit return connect directly to the reference NMU
  receive side.
- The F0 verification-only adapter translates the reference NMU's DAT Write credit sender to RTL
  NSU ready/valid receive semantics. Its FIFO capacity equals the credit capacity advertised to the
  model. It returns one model credit only when the corresponding slot is freed by an RTL handshake.
  It holds RTL valid/flit stable while stalled.

The adapter cannot modify packet bits, order, VC, destination, or class; it cannot route or add a
Router pipeline. Overflow, underflow, a send without advertised capacity, or an input/output flit
mismatch is fatal. The adapter is excluded from production source lists.

Protocol-preserving pause controls gate the direct REQ/RSP handshakes without changing their
payloads or adding routing behavior. A lossless per-VC credit-delay control may delay, but never
drop, duplicate, or change, a reference-NMU DataR credit pulse. These controls provide the required
ready/valid and credit stalls while leaving the two endpoints' native protocols directly connected.

### 11.2 Random pressure

Every random run independently varies:

- reference-manager AW, W, and AR issue gaps;
- direct REQ and RSP ready/valid pause controls, including held-valid intervals;
- F0 DAT Write ready stalls, including filling and draining the adapter;
- per-VC DataR credit-delay and release, including a sustained zero-credit interval;
- AXI memory AWREADY, WREADY, and ARREADY stalls;
- legal B and R response latency, B/R concurrency, and R interleaving across different IDs;
- ACLK/`noc_clk` ratio and phase from the S1-CDC matrix.

Ready and valid agents obey the interface protocol while applying pressure. In particular, the
memory's B/R producer must hold payload stable when the RTL NSU is not ready, and a credit return is
tied to an actual freed destination slot rather than generated as an unconstrained pulse.

### 11.3 Comparison scope

The hybrid scoreboard compares accepted transaction behavior, not cycle identity:

- every accepted source AW/AR produces exactly one matching downstream AW/AR; unicast address is
  unchanged and multicast changes only the approved coordinate field, with all other fields equal;
- every source write's W beats appear once, in the accepted AW order, with exact data, strobe, and
  WLAST;
- downstream B/R response fields, data, beat order, and terminal RLAST return once to the original
  AXI source ID and transaction;
- emitted RSP/DAT flits have the correct class, requester/destination, port, ordering, collective,
  ID, payload, tail, fixed-VC, and eligible VC values;
- accepted counts conserve transactions and beats across each boundary after all queues drain.

Latency, internal downstream mapped-ID values, async-FIFO pointer timing, and adapter residence time
are excluded from end-to-end equality. Mapped IDs are nevertheless checked locally against the S0
mapping predictor and for legal AXI response lookup. The current reference NSU is not placed in this
path and cannot mask an RTL NSU mapping defect.

### 11.4 Non-vacuous PASS

A hybrid run passes only after all scoreboards and assertions are clean, all accepted work drains,
and counters prove that the run contained:

- accepted AW, W, AR, B, and R traffic, including both Narrow and Data transactions;
- transfers on REQ, RSP, DAT Write, and DAT Read;
- simultaneous admissible REQ/DAT request pressure and simultaneous AXI B/R acceptance;
- at least one cycle where RSP and DAT both make progress;
- a witnessed ready/valid stall on every boundary selected for randomized stalling, followed by a
  successful transfer of the held item;
- a witnessed zero-credit DataR interval followed by credit recovery and transfer;
- response interleaving across different IDs and at least one multi-beat R burst;
- identity mapping, mapping reuse, and mapping retirement; fallback mapping is also required when
  the configured source ID range exceeds the downstream width;
- at least one nonempty-state coordinated reset/restart scenario in the reset test class.

The normal regression uses deterministic directed cases plus logged constrained-random seeds. A
separate fault campaign corrupts one packet field, drops one accepted beat, duplicates one response,
lies about one credit, and violates one held-valid boundary in isolation; each planted fault must
make the corresponding checker fail. Fault runs are expected-fail evidence and can never contribute
to normal functional coverage.

The single-reference-NMU hybrid does not manufacture a second source identity. The simultaneous
source/port collision case remains mandatory in S0-RQ-04, where the unit driver can legally present
multiple requester keys to one NSU.

## 12. Package acceptance evidence

S0 is complete when Sections 4, 6 (applicable guards), 7 (S0 owners), 8 (S0 crosses), and the first
three relevant alignment rows in Section 9 pass from a clean tree. S1 is complete when all S0
evidence remains green and Sections 5 through 8, the remaining applicable alignment rows, and the
hybrid gate in Section 11 pass.

Each package records:

- exact DUT revision, generated-constant drift-check result, simulator/tool revision, seed, and
  parameter set;
- test/assertion/coverage result and expected-fail guard/fault result;
- accepted-transfer and non-vacuity counters;
- any intentional model exclusion, linked to one row in Section 9;
- a post-run repository clean result confirming no generated source, waveform, log, cache, or build
  artifact remains.

No waived assertion, unhit required behavior bin, unknown (`X`) comparison, watchdog termination,
or unapproved `[TBD]` parameter can satisfy an exit condition.
