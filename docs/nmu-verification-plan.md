# NMU RTL verification plan

This plan maps the approved NMU contract to verification work that precedes and accompanies RTL
implementation. It defines tests, assertions, reference-model obligations, and acceptance evidence;
it does not define or supply production RTL.

## 1. Authority and entry gates

The sources of truth, in precedence order, are:

1. `AGENTS.md` and the frozen production hierarchy, ports, clock/reset ownership, network ownership,
   and package boundaries in `rtl/README.md`;
2. `docs/nmu-spec.md`, with generated constants from `specgen/source/constants.yaml` and the
   generated packet layout;
3. `docs/router-spec.md` and `docs/nsu-spec.md` for the two ends of the NMU network contracts;
4. this plan, which assigns verification evidence but does not override behavior;
5. the project-owned C++ model, only within the aligned comparison scope in Section 9.

Issue #9's interface and parameter contract is present in the branch history and in the first four
sources above. Any later mismatch is resolved in favor of those approved sources, not the issue
text or current model behavior.

The following are entry gates for RTL implementation or differential checking:

- generated SV/C++ parameter and packet constants pass `codegen.py --check`;
- the production source list contains only functional modules from the frozen package boundaries;
- no DPI handle, F0 adapter, model queue, or verification-only type enters a production module;
- every compared C++ behavior has passed the alignment gate in Section 9;
- any unresolved value is marked `[TBD]` and is not turned into a guessed checker constant.

Exact model tick count is not an RTL acceptance criterion. Cycle checks apply only where the
approved contract states latency or throughput, including SAM register-slice modes and sustained
one-flit-per-cycle behavior. Otherwise scoreboards compare accepted events, content, and order.

## 2. Verification levels

| Level | DUT and purpose | Principal evidence |
|---|---|---|
| N0 | one NMU child or shared primitive in isolation | directed and constrained-random unit tests, local assertions, parameter expected-fail tests, packet constants checked independently of the DUT encoder |
| N1 | production `nmu` top with AXI and NoC agents; both clocks and all physical faces present | end-to-end request/response prediction, same-ID ordering, REQ/DAT and B/R independence, CDC/reset stress, sustained-load and backpressure evidence |
| N2 | RTL NMU connected zero-hop to the reference NSU | transaction-level differential checking, randomized AXI/NoC pressure, F0 adaptation on the one mismatched DAT direction, and non-vacuous hybrid PASS |

N0 finds ownership and local invariant errors before integration. N1 is the authoritative NMU block
signoff level because its response injector can deliberately reverse responses from multiple
logical destinations. N2 proves interoperability with an executable endpoint; it does not replace
N1's multi-destination ordering tests or model a Router.

### 2.1 Acceptance trace

| Required behavior | Planned evidence |
|---|---|
| table SAM decode and unchanged global address | N0-SAM-01 through N0-SAM-06, NMU-A01 |
| AW/AR register-slice modes | N0-SAM-07 through N0-SAM-10, NMU-A02 |
| AW/W association and packet fields | N0-PKT-01 through N0-PKT-08, NMU-A03/NMU-A04 |
| REQ/DAT parallelism and independent pressure | N1-REQ-01 through N1-REQ-04, NMU-A05 |
| VC eligibility, credits, and fixed VC | N0-VC-01 through N0-VC-08, N1-DAT-01 through N1-DAT-04, NMU-A06 |
| per-ID ordering and always-enabled B RoB | N0-ROB-01 through N0-ROB-11, N1-ORD-01/02, NMU-A07/NMU-A08 |
| read RoB enabled and disabled | N0-ROB-12 through N0-ROB-18, N1-ORD-03/04, NMU-A07/NMU-A08 |
| same-ID different-destination overtaking evidence | Section 5.3 directed trace and N1-ORD-01 through N1-ORD-04 |
| B/R independence | N0-RSP-01 through N0-RSP-05, N1-RSP-01 through N1-RSP-04, NMU-A09 |
| five-channel CDC, reset, and release skew | N0-CDC-01 through N0-CDC-04, N1-CDC-01 through N1-CDC-06, NMU-A10 |
| illegal parameters and generated interface consistency | Section 6 expected-fail matrix, NMU-A11 |
| zero-hop RTL-NMU/reference-NSU hybrid | N2-HYB-01 through N2-HYB-08 and Section 11 |
| reference classification and license preservation | Section 10 and `docs/verification-environment.md`, Provenance |

## 3. Testbench and checking model

The N1 harness has five independent AXI channel agents, REQ/RSP ready/valid agents, and DAT agents
with asymmetric target flow control. The AXI driver may issue AW, W, and AR independently while
obeying AXI rules. The NoC response agent accepts requests into a transaction predictor, then emits
legal B, NarrowR, or DataR flits in a programmable physical order. It never derives its expected
packet by calling the DUT packetizer.

The predictor records, on accepted AW/AR:

- the first matching SAM rule, unchanged global address, destination node and port, traffic class,
  and collective context;
- AXI ID, burst fields, accepted request sequence, and the expected ordering-domain key
  `{dst_id, dst_port_id, AXI class}`;
- the unique admission branch, sticky state, `ordering_req`, and reserved tag/range when applicable;
- accepted AW order, remaining W beats, inherited route/order metadata, and selected fixed VC;
- expected B/R issue order per AXI ID and direction.

On accepted W, the predictor consumes only the oldest accepted AW's beat budget. On accepted NoC
output, it decodes every field with generated constants and checks the expected network and packet
boundary. On accepted response input, it records physical arrival order. On AXI B/R handshake, it
checks content and per-ID issue order. Separate conservation scoreboards cover the five CDC
channels, REQ, RSP, DAT Write, DAT Read, B slots, R slots, class FIFOs, and credits.

Every positive test has explicit completion counts. A watchdog is only a failure mechanism; reaching
the timeout or running a fixed number of clocks cannot produce PASS. Each assertion and scoreboard
has one fault-injection or illegal-stimulus test that demonstrates it can fail.

## 4. N0 request-side tests

### 4.1 SAM and register slices

| Test | Stimulus | Required evidence |
|---|---|---|
| N0-SAM-01 | addresses at `base`, interior, and `base + size - 1` for every generated memory, config, and peripheral entry | the authored first matching range supplies exact `dst_id`, `dst_port_id`, class, space coordinates, and unchanged address |
| N0-SAM-02 | addresses immediately below and above adjacent legal ranges | half-open boundaries select only the covering rule; a miss is rejected under the environment precondition rather than silently routed |
| N0-SAM-03 | simultaneous AW and AR lookups into different entries | both decoders accept and return their own complete metadata without shared priority or cross-coupling |
| N0-SAM-04 | legal FIXED, INCR, and WRAP bursts ending at the last byte of one entry | the complete burst footprint stays in one rule and is accepted; a footprint crossing an entry boundary is rejected before packet emission |
| N0-SAM-05 | unicast and legal collective AWs in each collective-capable space | unicast remains unchanged; collective destination/mask metadata follows the matched space; peripheral and malformed collective cases are rejected |
| N0-SAM-06 | generated table compared entry-by-entry with the configuration expansion used by the C++ loader | rule order, base, size, destination, port, class, and coordinate metadata agree; an overlapping or otherwise illegal authored map fails generation/validation |
| N0-SAM-07 | `AW_SAM_REG_TYPE=0` and `AR_SAM_REG_TYPE=0`, including output stalls | bypass is combinational; valid/payload remain protocol-correct and no item is lost or duplicated |
| N0-SAM-08 | each channel independently at type 1 under isolated and randomized stalls | one complete beat-plus-metadata record is held stable; unloaded latency is one cycle; any permitted post-backpressure bubble is recorded without loss or duplication |
| N0-SAM-09 | each channel independently at type 2 under continuous traffic and randomized stalls | registered backpressure holds payload stable and, after fill, sustains one accepted request per cycle without avoidable bubbles |
| N0-SAM-10 | all nine AW/AR mode pairs under simultaneous traffic | each channel follows only its own mode and pressure; no AW/AR coupling is introduced |

The decoder predictor performs an ordered table scan. The DUT may evaluate entries in parallel, but
the observable winner is the authored first match. Positive topology tests use legal non-overlapping
maps; priority is still checked at the unit boundary with a checker-owned rule vector, while the
production generator continues to reject overlapping authored maps.

### 4.2 Packetization and AW/W association

| Test | Stimulus | Required evidence |
|---|---|---|
| N0-PKT-01 | one transaction for every legal request `axi_ch` on its specified physical network | header and payload fields match generated offsets and widths; unused payload bits are zero |
| N0-PKT-02 | legal minima, maxima, and walking patterns for AXI fields, IDs, addresses, data, and strobes | field values round-trip bit-for-bit with no adjacent-field corruption |
| N0-PKT-03 | AW followed by `AWLEN + 1` W beats for single- and multi-beat Narrow/Data bursts | AW has `flit_tail=0`; only WLAST has `flit_tail=1`; every W copies its owning AW's destination, port, ordering tag/request, collective context, and fixed-VC context |
| N0-PKT-04 | accept several AWs of alternating class/ID/destination before presenting W | the shared W channel follows accepted AW order exactly; a later AW cannot claim an earlier W beat |
| N0-PKT-05 | delay W, pause either physical output, and refill AW independently | association survives arbitrary stalls; metadata retires only on accepted WLAST, not on attempted output |
| N0-PKT-06 | present W before any AW and inject early, missing, or extra WLAST in negative tests | protocol error is detected; no unrelated metadata is consumed |
| N0-PKT-07 | simultaneous admissible AW, W, and AR into independent staging paths | each accepted beat produces exactly one flit on the class-selected face; no state is shared accidentally |
| N0-PKT-08 | B, NarrowR, and DataR response flits with legal and reserved channel encodings | legal fields decode bit-accurately; wrong-face, reserved-channel, wrong-destination-port, and malformed-tail cases are detected |

## 5. N0/N1 ordering, channels, and response tests

### 5.1 RoB admission and storage

| Test | Stimulus | Required evidence |
|---|---|---|
| N0-ROB-01 | first AW/AR on each ID | idle branch alone fires, sticky clears, `ordering_req=0`, tag zero, and no slot is consumed |
| N0-ROB-02 | same-ID same-key streak in each direction | same-domain branch alone fires through `NMU_MAX_TXNS_PER_ID`; every request bypasses and retains the fixed VC required for AW/W |
| N0-ROB-03 | vary one key component at a time: destination, destination port, or class | the next same-ID request takes fallback, reserves storage, sets sticky, and carries `ordering_req=1` |
| N0-ROB-04 | after fallback, return to the latest or original key before the ID drains | sticky forces every later request to allocate; no same-key shortcut reopens early |
| N0-ROB-05 | fully retire the ID, then issue another request | order list and sticky reset; the new request takes idle bypass |
| N0-ROB-06 | exercise all IDs and both directions with interleaved requests | read and write order lists, counters, sticky flags, and pools are independent; different IDs may progress independently |
| N0-ROB-07 | allocate B entries to exact capacity, complete non-top entries, then the top | one slot per fallback AW; high-water free space returns only from the top; no live entry is overwritten |
| N0-ROB-08 | allocate R bursts of varied length, including exact-capacity cases | `ARLEN + 1` consecutive slots are reserved atomically; partial allocation never occurs; base tag and per-beat offsets are exact |
| N0-ROB-09 | fill robbed B/R responses before the older bypass response arrives | filled entries remain hidden until they reach the per-ID head; unrelated IDs continue |
| N0-ROB-10 | exhaust a pool or per-ID transaction cap while W is owed | new AW/AR admission stalls without state change; already admitted W may release its packet; opposite direction remains usable |
| N0-ROB-11 | collective AW at idle, then while any same-ID request is live | only the idle collective is admitted; no same-ID request streams past it until merged B retirement |
| N0-ROB-12 | `READ_ROB_ENABLED=1`, multi-beat reads returned out of physical order across IDs and destinations | per-ID AR order and beat order are restored; different IDs may interleave at transaction boundaries |
| N0-ROB-13 | enabled R pool receives each beat at `base + beat_index`, with response and AXI stalls | every beat occupies/releases exactly one reserved slot and retires the transaction only on RLAST |
| N0-ROB-14 | `READ_ROB_ENABLED=0`, same-ID same-key read streak | up to `NMU_MAX_TXNS_PER_ID` ARs are admitted with `ordering_req=0`; the outstanding count decrements only on accepted RLAST |
| N0-ROB-15 | disabled mode, change destination, port, or class while the ID is non-idle | the new AR stalls and emits no request flit until the prior same-key streak fully retires |
| N0-ROB-16 | disabled mode with other IDs, writes, B, and unrelated R traffic | only the blocked read ID is interlocked; B RoB and write behavior are unchanged |
| N0-ROB-17 | both read modes at parameter boundaries and with randomized response stalls | no counter, tag, slot, or order-list underflow/overflow; accepted-minus-retired accounting returns to zero |
| N0-ROB-18 | malformed or unallocated ordering tag, extra R beat, wrong bypass/robbed classification | the responsible checker fails before stale or aliased data reaches AXI |

### 5.2 REQ/DAT, VC, credit, and B/R independence

| Test | Stimulus | Required evidence |
|---|---|---|
| N0-VC-01 | credited unfixed DataAW under each legal VC count in `SHARED` | the first credited eligible VC in round-robin scan order wins, the pointer advances only on acceptance, repeated packets exercise every VC, and no uncredited send occurs |
| N0-VC-02 | DataAW in `READ_WRITE_SPLIT` at each legal even VC count | only the lower half is eligible; an illegal mode/count pair fails elaboration |
| N0-VC-03 | zero one, several, then all eligible credits, with varied round-robin pointer | the first credited eligible VC in scan order wins; all-zero stalls without losing or moving the head or pointer |
| N0-VC-04 | return delayed and bursty credit pulses, including a pulse that makes a zero counter usable in the current cycle | each pulse represents one freed Router slot; counters remain within `[0, NOC_ROUTER_VC_DEPTH]`, same-cycle availability is honored, and exact conservation holds |
| N0-VC-05 | ordering-bypassed AW/W stream while its fixed VC has zero credit and another VC is free | the stream stalls on its fixed VC and never spills; W inherits the same VC through WLAST |
| N0-VC-06 | fallback AW and all ARs | `fixed_vc=0`; AR remains on REQ and does not consume DAT credits |
| N0-VC-07 | continuously eligible credited DAT Write packets | after fill, the DAT class path accepts one flit per `noc_clk` cycle without an avoidable bubble |
| N0-VC-08 | source-list and hierarchy review plus storage accounting | the NMU owns class FIFO heads and sender counters only; per-VC pending storage exists only in Router inputs |
| N0-RSP-01 | B and R become releasable together | both independent AXI channel outputs may assert valid in the same ACLK cycle |
| N0-RSP-02 | stall BREADY while accepting R, then reverse | each payload holds stable under its own stall; the unrelated channel continues to its own finite-capacity limit |
| N0-RSP-03 | simultaneous RSP B/NarrowR and DAT DataR arrivals | both class FIFOs can accept/progress in one `noc_clk` cycle; DAT ready follows only DAT Read FIFO capacity |
| N0-RSP-04 | fill the RSP class FIFO while DAT Read continues, then fill DAT Read while RSP continues | backpressure is face-local; neither class consumes the other's storage or authority |
| N0-RSP-05 | sustain mixed B, NarrowR, and DataR with random response and AXI stalls | no loss, duplication, cross-class reordering, or hidden priority; all conservation counts return to zero |

| Test | Integrated N1 stimulus | Required evidence |
|---|---|---|
| N1-REQ-01 | build legal buffered Narrow and Data write backlogs while their outputs are stalled, then release both outputs together | both faces drain independently; every cycle in the bounded interval where both heads remain eligible transfers one REQ and one DAT flit, including at least one witnessed simultaneous transfer |
| N1-REQ-02 | exhaust DAT credits while REQ stays ready, then stall REQ while DAT is credited | the unblocked physical face continues; pressure crosses only through real shared AXI/AW-W capacity |
| N1-REQ-03 | mix AW/W/AR so REQ arbitration is continuously contested | an accepted AW locks only its own physical packet through WLAST; no AR or other AW interleaves inside it; each face applies its independent approved round-robin scan and advances its pointer only on a drained flit |
| N1-REQ-04 | same as N1-REQ-01 across register-slice and VC modes | parallelism is preserved by every legal configuration; throughput counters identify any avoidable bubble |
| N1-DAT-01 | random legal credit delays at low and near-full occupancy | no zero-credit send, over-credit, loss, duplication, or permanent stall |
| N1-DAT-02 | fixed-VC bypass streams interleaved with fallback traffic | bypass streams never change VC; fallback traffic may use only mode-eligible credited VCs |
| N1-DAT-03 | continuous DAT Write with simultaneous REQ and returning DAT Read pressure | ingress ready and egress credits remain separate; each direction makes progress when its own authority is available |
| N1-DAT-04 | long all-credit-zero interval followed by exact returns | the blocked ordered sequence drains exactly once and all counters return to their seeded values |
| N1-RSP-01 | B and R heads release together while both AXI ready signals toggle independently | same-cycle valid is observed; each channel transfers exactly its predicted sequence |
| N1-RSP-02 | sustained RSP and DAT Read input, alternating which class FIFO is near full | neither physical input creates head-of-line blocking in the other |
| N1-RSP-03 | B and NarrowR contend on RSP while DataR arrives on DAT | RSP bandwidth is shared only before depacketization; DataR is independent and B/R output order remains legal |
| N1-RSP-04 | simultaneous request issue, response arrival, and AXI retirement | state changes are acceptance-qualified; no channel waits for an unrelated handshake |

### 5.3 Same-ID different-destination overtaking evidence

N1-ORD-01 and N1-ORD-02 run with the B RoB, which is always enabled. N1-ORD-03 runs with the read
RoB enabled. For each direction the test issues request A and then request B with the same AXI ID
but different ordering-domain keys. A takes idle bypass; B takes fallback and receives a tag. The
NoC agent accepts both requests, injects B's legal response first, and then injects A's response.

The PASS artifact must contain all of the following, joined by transaction sequence number rather
than by waveform inspection alone:

1. request acceptance order `A < B`, decoded destinations/ports/classes, and the distinct keys;
2. branch witnesses `A: idle/bypass` and `B: fallback/ordering_req=1`, including B's legal tag or
   R-slot range;
3. physical response acceptance order `B < A`, proving that overtaking actually occurred;
4. a witness that B's response occupied RoB storage while no B/R beat for B was accepted on AXI;
5. AXI retirement order `A < B`, exact response/data content, and a final zero occupancy/count;
6. assertion/coverage hits for fallback allocation, out-of-order fill, blocked release, head release,
   and later release.

N1-ORD-01 covers single-beat B responses. N1-ORD-02 repeats write ordering while varying only
`dst_port_id` and only class, proving that the complete key participates. N1-ORD-03 covers R with
single- and multi-beat later requests completing physically first; it records every filled R slot
and every released beat.

N1-ORD-04 sets `READ_ROB_ENABLED=0`. It issues request A, then offers same-ID request C with a
different destination, port, or class. PASS requires C to remain unaccepted with no emitted request
flit until A's accepted RLAST makes the ID idle; C may then handshake and emit with
`ordering_req=0`. This is the disabled mode's positive ordering evidence: the illegal overtaking
opportunity is prevented at admission.
The always-enabled B RoB tests still run in both read-mode builds.

The zero-hop N2 composition has one reference target and therefore cannot create two independently
timed destinations. Its SAM and response tests supplement but do not satisfy this evidence; N1 is
the required acceptance source.

### 5.4 CDC and coordinated reset

| Test | Stimulus | Required evidence |
|---|---|---|
| N0-CDC-01 | exercise AW, W, AR from ACLK to noc_clk and B, R from noc_clk to ACLK, one channel at a time | exactly one destination transfer occurs for every accepted source item; order and every payload bit are preserved |
| N0-CDC-02 | run each channel with equal clocks, either side faster, non-integer ratios, randomized phase, and independent legal source/destination stalls | held-valid behavior, no loss/duplication, and source backpressure follow only finite FIFO capacity |
| N0-CDC-03 | drive empty, one-entry, almost-full, full, simultaneous pop/push, and wraparound cases at every legal depth | accepted-minus-emitted accounting equals occupancy; no full overwrite or empty read; traffic resumes after the primitive's safe pointer synchronization |
| N0-CDC-04 | assert the common reset asynchronously at randomized phases with each FIFO empty and nonempty, then deassert each domain reset synchronously with both legal release orders | no pre-reset item escapes; outputs stay quiescent until their local reset is released and new traffic is accepted; pointer/state convergence produces no phantom transfer |
| N1-CDC-01 | structural bind/lint of the production top | five and only five AXI channels cross domains through `nmu_axi_cdc`; no complete-flit CDC or other combinational cross-domain path exists |
| N1-CDC-02 | simultaneous traffic on all five channels under unrelated clocks and independent AXI/NoC pressure | all five scoreboards make progress independently and drain exactly once |
| N1-CDC-03 | continuous single-channel and mixed-channel traffic at each clock relationship | progress follows legal CDC visibility and the slower accepting side; pressure on an unrelated channel introduces no cross-channel stall |
| N1-CDC-04 | assert common reset with AW/W association, worm lock, credits, class FIFOs, RoB slots, sticky IDs, and B/R outputs nonempty | every listed transaction-state class flushes and no stale request, flit, response, or credit event appears after release |
| N1-CDC-05 | repeat reset with ACLK release before noc_clk and the reverse | release skew is tolerated; no transfer uses a domain still in reset and first post-reset traffic starts from empty state |
| N1-CDC-06 | assert reset between active clock edges and deassert at varied synchronizer phases | assertion is observed asynchronously and each local deassertion is synchronous; output protocol assertions remain clean throughout |

The harness always derives both domain resets from the one common reset. It does not test or claim
one-sided reset recovery, which the approved contract excludes.

## 6. Parameter and interface guards

Each guard has an elaboration-positive test at legal boundaries/defaults and an expected-fail test
for illegal values. The negative test passes only when the intended NMU instance path and diagnostic
are observed before time advances; an unrelated compile or simulator failure is not evidence.

| Parameter/relationship | Positive cases | Expected-fail cases |
|---|---|---|
| `AXI_ID_WIDTH` | 1, default, 8 | 0 and 9 |
| `NOC_DAT_NUM_VC` | 1, default, 8 | 0 and 9 |
| `NOC_DAT_VC_MODE` | 0 and 1 with a legal count | other encodings; split with odd count or count 1 |
| `AXI_FIFO_DEPTH` | 4, 8, 16 | representative non-members below, between, and above the legal set |
| `NOC_FIFO_DEPTH` | 4, 8, 16 | representative non-members below, between, and above the legal set |
| `NOC_ROUTER_VC_DEPTH` | 1, default, 16 | 0 and 17 |
| `NMU_ROB_B_DEPTH` | 1, default, 256 | 0 and 257 |
| `NMU_ROB_R_DEPTH` | 1, default, 256 | 0 and 257 |
| `NMU_MAX_TXNS_PER_ID` | 1, default, 256 | 0 and 257 |
| `READ_ROB_ENABLED` | 0 and 1 | other values |
| `AW_SAM_REG_TYPE`, `AR_SAM_REG_TYPE` | all independent pairs from 0, 1, 2 | any value outside 0..2 |
| generated interface widths | every legal ID/VC configuration derives the specified REQ/RSP/DAT and credit widths | any independently overridden or mismatched flit/type/credit width |
| generated SAM | each shipped valid topology | empty, overlapping, overflowing, uncovered, out-of-mesh, duplicate, illegal port/space, or otherwise invalid authored maps |

Other public AXI width guards are exercised at the canonical legal boundaries and sets stated in
`specgen/source/constants.yaml`; this plan does not repeat them as an independent parameter source.
Burst legality tests cover size, burst encoding, WRAP alignment/length, and 4 KB/region footprint
using the approved protocol checker rules.

## 7. Assertion and checker plan

The names below are verification identifiers, not proposed RTL signals. Assertions bind to public
handshakes or reviewed child-boundary events; they do not require an unapproved pipeline or FIFO
implementation.

| ID | Boundary | Property |
|---|---|---|
| NMU-A01 | SAM | each accepted AW/AR has one legal first-match result; output address is unchanged; complete metadata is acceptance-stable |
| NMU-A02 | ready/valid slices | valid and complete payload stay stable while stalled; accepted-minus-emitted occupancy remains in the legal mode capacity; type 2 is bubble-free under continuous ready traffic |
| NMU-A03 | AW/W association | no W is emitted without an accepted AW; beat count is `AWLEN + 1`; only WLAST retires metadata; inherited fields equal that AW |
| NMU-A04 | packet fields | every accepted flit uses a legal channel/face, generated field layout, exact tail, and legal destination port; no reserved encoding or nonzero unused payload escapes |
| NMU-A05 | physical request faces | REQ and DAT state/transfer authority are independent; each face transfers at most one flit per cycle; a locked AW packet admits only its W beats through WLAST |
| NMU-A06 | DAT assignment | VC is mode-eligible and credited; zero-credit send is impossible; counters stay in range; fixed-VC AW/W never change or spill; no NMU per-VC pending storage exists |
| NMU-A07 | order admission | exactly one admission branch applies; key includes destination, port, and class; sticky clears only when the ID becomes idle; collective admission is idle-only |
| NMU-A08 | RoB and disabled-read state | no live slot overwrite, unallocated fill, range overlap, underflow, or premature release; enabled responses retire per ID; disabled read key/counter blocks only a differing non-idle key |
| NMU-A09 | response paths | RSP and DAT Read FIFO conservation is independent; decoded B/R reaches only its direction; B and R outputs obey independent ready/valid holding |
| NMU-A10 | CDC/reset | five and only five AXI channels cross domains; no combinational cross-domain path exists; no stale pre-reset item transfers; all transaction, lock, metadata, slot, sticky, class-FIFO, and credit state flushes on reset |
| NMU-A11 | parameters/interfaces | each illegal parameter or generated-width mismatch terminates elaboration with the intended instance diagnostic |

Credit pulses are checked as freed-slot events, not as ready/valid. Environment assumptions require a
ready/valid source to hold valid and payload stable until handshake. A credit return may be delayed,
but the environment may not invent, duplicate, or return a credit for an unconsumed slot.

## 8. Coverage and behavior-focused scenarios

Functional coverage uses accepted events and assertion antecedents. Required crosses are:

- SAM entry/space/port × AW/AR × exact boundary/interior × register-slice mode × stall;
- admission branch × direction × key component changed × sticky state × read-RoB mode;
- B/R pool occupancy boundary × allocation success/failure × top/non-top retirement;
- AW class × W burst length/position × physical face × worm lock × output pressure;
- simultaneous REQ/DAT eligibility × transfer pair × credit/ready state;
- VC × mode × fixed flag × ordering request × credit zero/nonzero/return;
- physical response arrival order × AXI retirement order × B/R direction × burst position;
- simultaneous B/R valid × BREADY/RREADY combinations;
- each CDC channel × source/destination clock ratio/phase × empty/almost-full/full × stall;
- reset with each state class nonempty × assertion domain × release order × first post-reset transfer.

Two sustained scenarios are mandatory:

1. **Parallel request saturation.** Legal AXI traffic first fills both request-class backlogs while
   the physical outputs are blocked. REQ is then made ready and every required DAT credit is made
   available together. During the bounded window where both class heads remain continuously
   eligible, both faces transfer each cycle and simultaneous REQ+DAT cycles are witnessed. Separate
   long phases prove each face's own one-flit-per-cycle rate; adding AR traffic may share REQ but
   must not reduce free DAT utilization.
2. **Independent return saturation.** RSP and DAT Read remain continuously available while AXI B/R
   are ready. The two physical ingress faces and two AXI response channels must make independent
   progress; randomized pressure may cause only finite-capacity backpressure, not an avoidable
   cross-channel bubble.

No numeric frequency, area, or power target is approved, so those results remain `[TBD]`. The plan
does check the specified utilization invariants and reports FIFO/slot high-water marks and stall
reasons rather than inventing a PPA threshold.

## 9. Selective C++ reference-model alignment

The project-owned model is a conditional oracle. Alignment changes must preserve its role as a
transaction-level reference and add focused C++ tests before an area enters differential comparison.
They do not emulate RTL CDC implementation details or force the RTL to use model-internal queues.

| Area | Current usable behavior/gap | Required alignment before differential use |
|---|---|---|
| SAM | table lookup, unchanged address, YAML expansion, spaces and ports are modeled; RTL register types 1/2 are absent | retain table/order/config parity and burst-footprint checking; use the model only for mode 0 timing, and compare modes 1/2 against the approved slice semantics |
| ordering-domain key | Enabled bypass compares destination and class but omits destination port | key both read and write admission on `{dst_id, dst_port_id, AXI class}`; add one-component-at-a-time and sticky tests |
| B RoB | slot pool, tags, bypass/fallback, sticky behavior, and per-ID ordering are modeled | retain behavior; add the exact N1 overtaking trace counters/metadata needed for accepted-event comparison |
| enabled R RoB | per-beat slots, high-water allocation, bypass/fallback, and ordering are modeled, with the same missing port term | add destination port to the key and preserve per-beat fill/release tests before enabling comparison |
| disabled R path | model permits only one outstanding read per ID | implement same-key streaks up to `NMU_MAX_TXNS_PER_ID`, latch the complete key while non-idle, block only a key change, and decrement only on RLAST |
| VC ownership and modes | model allocators own per-VC pending queues and implement `SHARED` only | expose class-FIFO-head acceptance and Router-credit behavior with no NI per-VC storage; add split-mode masks and fixed-VC no-spill tests; do not compare model queue timing |
| LOCAL DAT receive | current model consumes DataR through symmetric credit flow control | keep the production target ready/valid; isolate the mismatch in F0 for hybrid comparison and never treat model receive-credit timing as DUT behavior |
| REQ/DAT parallelism | independent paths exist, but no shared-AXI test proves same-cycle egress | add a focused model test that records simultaneous REQ and DAT transfers; use it for functional capability, not exact RTL latency |
| B/R independence | model drains RSP and DAT response inputs independently but lacks target CDC/class-FIFO timing | compare decoded content and order after independent accepted inputs; exclude internal queue occupancy and exact cycle |
| CDC/reset | model is single-clock | do not emulate synchronizers or compare crossing latency in C++; verify CDC structure, dual-clock behavior, and reset in RTL N0/N1 |
| widths | model/DPI types are fixed at the present 3-bit-ID generated instance | compare only the aligned instance until beat types, packet offsets, containers, and wrappers are parameterized together; other legal widths remain RTL-only evidence |

Until each row's alignment test passes, its current-model result is diagnostic evidence, not an RTL
golden. All remaining exclusions stay recorded in `docs/known-limitations.md`.

## 10. Reference classification and license handling

Reference identities, pinned revisions, inspected files, classifications, and SPDX obligations are
recorded only in `docs/verification-environment.md`, Provenance. This plan uses those source IDs:

- **P1** is production-approved. Its RoB behavior and associated tests may inform a reviewed,
  project-owned architecture or DV predictor. This issue copies and instantiates no P1 RTL. Any
  later approved reuse must preserve every upstream copyright/SPDX notice and the recorded license.
- **P2** is reference-only. Only general test intent such as stall matrices, held-valid checks,
  asynchronous clock ratios, and reset stress may inform project-owned DV. P2 RTL may not be copied,
  adapted, or instantiated in production. Existing notices remain untouched.
- The C++ NMU is project-owned and follows the conditional-oracle rules in Section 9; it is not a
  substitute for RTL CDC or asymmetric LOCAL DAT flow control.

Review evidence records the source ID and revision, never a copied RTL fragment. Production and DV
source-list checks fail if a reference-tree path or the F0 adapter appears in a production closure.

## 11. RTL-NMU to reference-NSU zero-hop co-simulation

### 11.1 Composition and F0 adapter

The N2 composition is:

```text
AXI manager -> RTL NMU -- REQ ready/valid -----------------> reference NSU -> AXI memory
                       -- DAT Write credit ---------------->
                       <- RSP ready/valid ------------------
                       <- F0 <- reference DAT Read credit --
```

There is no Router, route stage, merge, fork, or modeled link latency. REQ and RSP connect directly
through protocol-preserving randomized pause gates. RTL NMU DAT Write already matches the reference
NSU credit receiver and connects directly; the harness may delay returned credits without changing
their count or VC. Only reference-NSU DataR transmit differs from RTL-NMU DAT receive and therefore
uses F0.

F0 is a verification-only bounded FIFO. Its capacity equals the total credit capacity advertised
to the reference sender. It accepts each credit-qualified reference flit with its original VC,
presents the oldest flit unchanged as held `valid` to the RTL NMU, and returns one credit for that
recorded VC only when the RTL `valid && ready` transfer frees the slot. Overflow, underflow, a
fabricated credit, a flit-bit change, or reordering is fatal. F0 never selects a VC or interprets
packet fields. It is excluded from every production source list.

The RTL NMU uses independent `ACLK` and `noc_clk`. The reference NSU and its AXI target tick on
`noc_clk`. Runs include equal clocks, AXI faster, NoC faster, non-integer ratios, and randomized
phase. CDC signoff remains N0/N1 RTL evidence; the single-clock reference is not a CDC oracle.

### 11.2 Random pressure and stimulus

Every run has a reproducible seed and independently randomized controls:

- AXI AW, W, and AR gaps, including AW-ahead, W-delayed, multiple accepted AWs, mixed reads/writes,
  IDs, legal burst forms, and Narrow/Data SAM boundaries that name the attached zero-hop target;
- AXI BREADY and RREADY stalls, both isolated and simultaneous;
- direct REQ ready stalls and RSP valid/ready pressure, with valid/payload held until handshake;
- delayed RTL-NMU DAT Write credit returns per VC, including witnessed all-eligible-zero intervals;
- RTL-NMU DAT Read ready stalls through finite internal capacity and F0, with legal reference
  credits returned only on freed adapter slots;
- downstream AXI memory AWREADY/WREADY/ARREADY and B/R response delays;
- coordinated reset with empty and nonempty state, randomized legal assertion time, and both legal
  domain-release orders.

Pressure generators have bounded fairness during completion phases. Separate directed tests hold a
boundary blocked long enough to fill the finite upstream storage and prove backpressure propagation;
random tests alone are not used to claim a full/empty boundary.

| Test | N2 composition stimulus | Required evidence |
|---|---|---|
| N2-HYB-01 | one single- and one multi-beat transaction in each Narrow/Data and read/write class | RTL request flits, reference-NSU AXI requests, memory effects, and returned RTL AXI responses match transaction content and order exactly |
| N2-HYB-02 | independently randomized AXI request gaps, B/R backpressure, REQ ready stalls, RSP pressure, and memory latency | every ready/valid source holds stable, every accepted item completes exactly once, and bounded-fair recovery drains all scoreboards |
| N2-HYB-03 | delay direct DAT Write credits and F0/DataR capacity separately, including all-credit-zero/full intervals | each direction backpressures independently, then recovers without credit, flit, or transaction mismatch |
| N2-HYB-04 | prefill Narrow/Data writes, release REQ/DAT together, and arrange concurrent B/R completion | simultaneous REQ+DAT and BVALID+RVALID witnesses occur; physical and AXI response channels remain independent |
| N2-HYB-05 | separate builds sweep all SAM register modes, both read-RoB modes, legal VC counts, and both VC modes | every applicable configuration reaches the same functional completion criteria; mode-specific latency/eligibility checks pass |
| N2-HYB-06 | equal and unrelated clock relationships plus coordinated reset at empty and nonempty states | five-channel CDC integrity and both legal reset-release orders pass; no pre-reset transaction escapes |
| N2-HYB-07 | checker-red runs corrupt one F0 bit, duplicate one accepted response, and violate one credit conservation event separately | each run fails only through its intended checker and cannot report hybrid PASS |
| N2-HYB-08 | long reproducible random sequences with mixed IDs, bursts, classes, and all pressure mechanisms | all non-vacuity counters are nonzero, no watchdog fires, every scoreboard drains, and all credit totals return to seed |

### 11.3 Comparison scope

The primary oracle is an independent AXI transaction scoreboard plus generated packet decoder:

- accepted AXI requests map to the exact expected REQ/DAT flits, physical face, unchanged address,
  AW/W association, packet fields, ordering metadata, and legal VC behavior;
- the reference NSU reconstructs legal AXI requests and its memory observes exact address, burst,
  data, and strobe content;
- returning B/R content, ID, response, beat order, and completion count match the originating AXI
  transactions;
- per-ID AXI order, B/R independence, and loss/duplication checks are mandatory.

Cycle equality, internal queue occupancy, C++ allocator choice, C++ single-clock latency, and F0
residence time are excluded. Split-mode and non-default-width DUT behavior is checked against the
approved rules unless the corresponding C++ alignment gate has closed. The hybrid's one reference
target does not prove multi-destination overtaking; Section 5.3 supplies that evidence.

### 11.4 Non-vacuous PASS

N2 reports PASS only if all applicable scoreboards/assertions are clean and the run records:

- at least one accepted and completed Narrow write, Data write, Narrow read, and Data read;
- every AXI request/response channel handshakes, every REQ/RSP/DAT physical direction transfers,
  and F0 enqueues, stalls while nonempty, dequeues, and returns credit;
- at least one multi-beat write and read, WLAST/RLAST retirement, two outstanding AWs before W
  completion, and more than one live AXI ID;
- at least one simultaneous REQ+DAT transfer and one simultaneous BVALID+RVALID cycle;
- a witnessed REQ stall with held payload, RSP stall with held payload, DAT Write all-credit-zero
  interval followed by recovery, and DAT Read/F0 pressure followed by recovery;
- each SAM register mode and both read-RoB modes in separate builds, all legal VC modes/counts in
  the parameter regression, and both legal reset-release orders;
- accepted request count equals reference-NSU AXI issue count, terminal response count equals AXI
  completion count, all transaction/FIFO/F0/credit scoreboards drain to zero, and credit totals
  return to their seeded values.

A dedicated red test corrupts one F0 flit bit or duplicates one accepted response and must fail the
intended checker. A run with zero transactions, no exercised stall, a stuck interface, undrained
state, or watchdog expiry cannot pass.

## 12. Package acceptance evidence

Each implementation package is reviewable only with the following focused evidence:

| Package | Required evidence before review |
|---|---|
| `nmu_axi_cdc` | N0-CDC suite, five-channel structural check, all clock/reset relationships, reset-flush assertions, legal-depth and expected-fail guards |
| `nmu_sam` | N0-SAM suite, generated-table parity, burst/collective legality, all AW/AR register-mode pairs, SAM/width guards |
| `nmu_rob` | N0-ROB suite, N1-ORD traces, both read modes, B-always-enabled proof, pool/capacity negatives, assertion fault injection |
| `nmu_packetize` | N0-PKT field/face/tail tests against generated constants, multi-AW W association, malformed-input negatives |
| `nmu_channel_assign` | N0-VC suite, N1-REQ/DAT stress, fixed-VC and credit conservation, split masks, no-per-VC-storage review |
| `nmu_depacketize` | N0-RSP suite, channel/face legality, independent RSP/DAT Read pressure, ready ownership and malformed-response negatives |
| `nmu` top | full N1 regression, parameter/interface guards, CDC/reset, sustained scenarios, coverage closure, clean production source list |
| hybrid selection/F0 | full N2 matrix and red tests, F0 conservation/bit transparency, no Router behavior, verification-only source-list proof |

For every package, the handoff records command, seed, parameter set, simulator, pass/fail counts,
assertion results, required coverage hits, and remaining approved exclusions. A clean compile without
the required accepted events is not package evidence.
