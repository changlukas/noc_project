# Router RTL verification plan

This plan maps the approved Router contract to verification work that precedes and accompanies RTL
implementation. It assigns tests, assertions, coverage, reference-model obligations, and the
R0/R1/R2 acceptance split. It does not define or supply production RTL.

## 1. Authority and entry gates

The sources of truth, in precedence order, are:

1. `AGENTS.md` and the frozen production hierarchy, interfaces, ownership, and package boundaries
   in `rtl/README.md`;
2. `docs/router-spec.md`, with generated constants from `specgen/source/` and generated packet
   layouts;
3. `docs/nmu-spec.md` and `docs/nsu-spec.md` for the two Router-to-NI boundary contracts;
4. this plan, which assigns evidence but does not add behavior;
5. the project-owned C++ Router, only within the aligned comparison scope in Section 9.

Issue #9's interface and parameter contract is present in this branch's history and in the first
three sources above. A later mismatch is resolved in favor of those approved sources, not issue
text, a prior implementation, or current model behavior.

The following are entry gates for Router RTL implementation or differential checking:

- generated SV/C++ parameter and packet constants pass `codegen.py --check`;
- production sources follow the frozen `router_route_select`, `router_simple_network`,
  `router_dat_input`, `router_dat_output`, `router_dat_network`, and `router` ownership split;
- no DPI handle, reference-tree path, model queue, or verification-only normalizer enters the
  production source closure;
- each compared C++ behavior has passed the alignment gate in Section 9;
- every assertion has passed its named fault-injection red test from Section 7;
- unresolved values remain `[TBD]` and are not converted into checker constants.

The approved three-cycle model-facing zero-load latency is cycle-exact. Arbitration, lock, flow
control, and conservation are property-based. Packet delivery, fork multiplicity, join reduction,
and network isolation are end-to-end checks. Section 4 gives the complete classification.

The R0/R1/R2 names below are DV levels. They are unrelated to restrictions R1 and R2 in
`docs/router-spec.md` Section 2.10.

## 2. Verification levels

| Level | DUT and purpose | Principal evidence |
|---|---|---|
| R0 | one Router child or shared primitive in isolation | directed and constrained-random unit tests, local assertions, independent route/VC predictors, parameter expected-fail tests |
| R1 | production `router` top at one coordinate, all five ports and all three networks | block scoreboards, simultaneous-network pressure, reset, latency/throughput, collective and congestion tests |
| R2 | RTL and reference Router instances driven side by side | identical pin-level stimulus in the aligned lockstep scope, direct cycle comparison, normalized LOCAL DAT event comparison, and explicit divergence counters |

R0 closes ownership and local invariants before integration. R1 is the authoritative Router block
signoff because it covers target-only reset, asymmetric LOCAL DAT flow control, split VC mode, and
all legal parameter builds. R2 is a conditional executable cross-check. R2 must pass before any
full-RTL mesh run; a mesh result cannot waive an R0, R1, or R2 failure.

### 2.1 Acceptance trace

| Required behavior | Planned evidence |
|---|---|
| XY unicast and boundary ejection | R0-ROUTE-01 through R0-ROUTE-05, R1-TOP-01, RTR-A04 |
| collective fork and join | R0-COL-01 through R0-COL-09, R1-TOP-06/07, RTR-A09 through RTR-A13 |
| ready/valid stability | R0-SIM-01/02, R1-TOP-02, RTR-A01/RTR-A02/RTR-A24 |
| arbitration order and fairness | R0-ARB-01 through R0-ARB-05, R1-TOP-04, RTR-A07/RTR-A08 |
| wormhole locking | R0-ARB-06 through R0-ARB-09, R1-TOP-05, RTR-A05/RTR-A06 |
| DAT VC FIFO and VA | R0-DAT-01 through R0-DAT-09, R1-TOP-03, RTR-A14 through RTR-A17 |
| fixed VC | R0-DAT-10/11, R1-TOP-03, RTR-A15/RTR-A16 |
| credit conservation | R0-CRD-01 through R0-CRD-08, R1-TOP-08, RTR-A18 through RTR-A20/RTR-A25 |
| reset | R0-RST-01 through R0-RST-04, R1-TOP-09, RTR-A21 |
| illegal parameters | Section 6 expected-fail matrix, RTR-A22 |
| three-network independence | R1-TOP-10 through R1-TOP-12, RTR-A23 |
| reference/RTL hybrid | R2-HYB-01 through R2-HYB-10 and Section 11 |
| reference classification and license handling | Section 10 and `docs/verification-environment.md`, Provenance |

### 2.2 Approved-spec trace

| Router specification | Planned evidence |
|---|---|
| SPEC 1, interface | R0-IF-01 exact port/type/elaboration binding and R1 top build |
| SPEC 2, reset | R0-RST-01 through R0-RST-04, R1-TOP-09, RTR-A21 |
| SPEC 3, routing | R0-ROUTE-01 through R0-ROUTE-05, R1-TOP-01, RTR-A04 |
| SPEC 4, zero-load latency | R1-TOP-01 and R2-HYB-01 cycle-exact samples |
| SPEC 5, bit transparency | R0-DAT-12, R0-SIM-01, R1 scoreboards, R2 comparison points |
| SPEC 6, VC handling | R0-DAT-03 through R0-DAT-11, RTR-A14 through RTR-A16 |
| SPEC 7, credit decrement | R0-CRD-01/02/05, RTR-A18/RTR-A19 |
| SPEC 8, credit pulse discipline | R0-CRD-03/04/08, RTR-A20/RTR-A25 |
| SPEC 9, never send without credit | R0-CRD-01/06, RTR-A19 |
| SPEC 10, wormhole non-interleave | R0-ARB-06/08, R1-TOP-05, RTR-A05/RTR-A06 |
| SPEC 11, lock persistence | R0-ARB-07/09, RTR-A05/RTR-A06 |
| SPEC 12, arbitration order | R0-ARB-01/04, R1-TOP-04, RTR-A07 |
| SPEC 13, VC independence | R0-DAT-05/06, R1-TOP-03, RTR-A14/RTR-A19 |
| SPEC 14, output FIFO | R0-DAT-08, RTR-A17 |
| SPEC 15, multi-output evaluation | R1-TOP-14 and RTR-A26 enforce one flit per output and concurrent independent outputs; the model-only same-FIFO multi-read cycle is R2-X02 |
| SPEC 16, fairness | R0-ARB-05, R1-TOP-04, RTR-A08 |
| SPEC 17, boundary silence | R1-TOP-13, RTR-A27 |
| SPEC 18, network independence | R1-TOP-10 through R1-TOP-12, RTR-A23 |
| SPEC 19, parameter legality | Section 6 and RTR-A22 |
| SPEC 20, multicast fork | R0-COL-01/03/04/05/06, R1-TOP-06, RTR-A09 through RTR-A11/RTR-A28 |
| SPEC 21, CollectB join | R0-COL-02/03/07/08/09, R1-TOP-07, RTR-A12/RTR-A13/RTR-A28 |

R0-IF-01 elaborates the frozen top and each child with generated packet and credit types, checks
the fixed port order and widths independently of DUT declarations, and rejects a model/DPI or
verification-only port on a production module.

## 3. Testbench and checking model

The R1 harness instantiates one production Router at an independently selected legal coordinate.
Each of REQ, RSP, and DAT has five independent input agents and five independent output agents.
REQ/RSP agents obey ready/valid and can stall each output independently. DAT link agents maintain
one shadow capacity counter per VC on N/E/S/W, inject only with capacity, and return delayed credits
only for recorded dequeues. The LOCAL DAT sink uses ready/valid and never fabricates a credit port.

The predictor is project-owned and independent of DUT control state. It computes:

- XY unicast direction from source-independent destination coordinates and destination port;
- fork output sets and join expected-input sets from the approved mask equations;
- legal candidate inputs per output and the exact VC-major/input-minor round-robin order;
- preferred and eligible output VC sets, fixed-VC preservation, tail-only overflow, and lock state;
- expected CollectB contributor set, replica equality, survivor, and response precedence;
- per-link and per-VC occupancy using only accepted boundary events.

The predictor must not call DUT route, mask, VA, arbitration, or reduction functions. Generated
packet constants may be shared, but expected field values are decoded independently. Every accepted
input gets a unique sequence tag held outside the flit. Scoreboards track the expected output set,
per-output order, accepted branch mask, and terminal completion. They compare every flit bit except
the one target-authorized DAT `vc_id` rewrite, whose expected value is predicted separately.

A positive test passes only after its declared input count was accepted, its expected output and
credit counts were observed, every scoreboard and lock drained, and all required coverage witnesses
occurred. A watchdog is only a failure mechanism. Random arbiters and sinks become bounded-fair
during drain; liveness is never claimed while the environment withholds service forever.

## 4. Check semantics

| Contract area | Cycle-exact | Property-based | End-to-end |
|---|---|---|---|
| unicast route | three-cycle zero-load output appearance | selected direction is the approved XY direction; no illegal turn | accepted flit appears exactly once at the predicted output |
| REQ/RSP flow control | held valid/payload compared every stalled cycle | transfer only on `valid && ready`; idle bus is zero; no loss/duplication | ordered accepted sequence drains under randomized stalls |
| arbitration/fairness | grant order at each eligible packet boundary | work conservation, pointer update only on tail, bounded grant opportunities | all persistent contenders complete with predicted packet order |
| wormhole lock | owner/VC checked each active cycle | no interleave; empty owner retains lock; tail releases | packets arrive whole and in order |
| fork | branch accept bitmap checked each cycle | no duplicate branch; pop/credit exactly once after all branches | one complete worm per selected branch, none elsewhere |
| join | readiness and selected survivor checked on each reduction opportunity | exact expected set, qualification, equality, priority, worm-boundary hold | contributors consume once and yield one complete result |
| DAT FIFO/VA | stage occupancy transitions and output FIFO admission checked per cycle at R0 | legal VC, eligible set, fixed VC, tail-only overflow, lock continuity | flit and packet order across pressure and contention |
| DAT credits | send/return/counter deltas checked per cycle on N/E/S/W | range, no send at zero, return iff slot freed, conservation | all seeded credits restored after drain |
| LOCAL DAT | held valid/payload checked per cycle | ready/valid only; no LOCAL external-credit ownership | accepted sequence and backpressure recovery |
| reset | outputs/state sampled on every reset/release edge | asynchronous assertion effect, synchronous deassertion use, no stale transfer | pre-reset work is discarded and post-reset traffic completes |
| illegal parameters | elaboration must fail before time advances | each guard names the violated approved constraint | not applicable |
| network independence | simultaneous transfer witnesses | no state, pressure, or credit crosses a network | each of seven nonempty active-network combinations drains independently |

Cycle-exact comparison is suspended during reset and starts at the first common post-release sampling
edge. An output stalled by the testbench is checked for stability, not for a new latency deadline.
Fairness bounds count eligible packet-boundary grant opportunities, not raw cycles; an open worm or
a blocked downstream is not an opportunity.

## 5. Directed and constrained-random tests

### 5.1 R0 route selection and masks

| Test | Stimulus | Required evidence |
|---|---|---|
| R0-ROUTE-01 | every source/destination pair on each legal mesh geometry, including 2x4 and 4x2 | X mismatch chooses E/W before any Y move; Y and LOCAL follow only after X matches |
| R0-ROUTE-02 | each edge coordinate and each legal boundary destination port | ejection uses the named boundary face; interior use of a boundary face is rejected |
| R0-ROUTE-03 | self destination and LOCAL port | route is LOCAL with no external turn |
| R0-ROUTE-04 | random flits with every non-routing field toggled | route is independent of payload and non-routing header fields |
| R0-ROUTE-05 | reserved port and out-of-mesh coordinates | fail-loud checker/guard fires before a transfer |
| R0-COL-01 | exhaustive legal masks on small square geometries | fork mask equals the independently enumerated member-tree edges |
| R0-COL-02 | same mask/source space as R0-COL-01 | join mask equals actual unicast return-path contributors |
| R0-COL-03 | zero mask | both functions reduce to the approved unicast behavior |

### 5.2 R0 simple networks, arbitration, and collectives

| Test | Stimulus | Required evidence |
|---|---|---|
| R0-SIM-01 | single flits through every legal input/output pair with random output stalls | exact content, held valid/payload, no loss, duplication, or wrong output |
| R0-SIM-02 | fill to ready threshold, hold blocked, then drain | ready deasserts without overflow and every accepted flit drains in order |
| R0-ARB-01 | all candidates continuously request one output with one-flit packets | exact input-minor round-robin sequence wraps |
| R0-ARB-02 | requester arrives after winner is selected but before output ready | frozen winner is not stolen |
| R0-ARB-03 | only one candidate remains eligible | no avoidable bubble at an available output |
| R0-ARB-04 | requesters enter and leave around each packet boundary | pointer changes only after accepted tail and next eligible scan is exact |
| R0-ARB-05 | every contender persists under bounded-fair service | each wins within the number of eligible packet-boundary opportunities |
| R0-ARB-06 | multi-flit worms from competing inputs | no packet interleave and release occurs on accepted tail only |
| R0-ARB-07 | locked owner becomes temporarily empty | output idles without losing or transferring the lock |
| R0-ARB-08 | body/tail changes destination bits | latched route remains authoritative until tail |
| R0-ARB-09 | one output is locked while another is free | independent output progresses without corrupting the lock |
| R0-COL-04 | one-, two-, and multi-branch forks including LOCAL | bit-identical worms appear once on every selected branch |
| R0-COL-05 | one fork branch stalls across head/body/tail | completed branches are not replayed; input pops only after all branches accept |
| R0-COL-06 | disjoint trees operate concurrently | both progress and conserve their own branch state |
| R0-COL-07 | CollectB contributors arrive in every permutation | no result before the full qualified expected set; each contributor consumes once |
| R0-COL-08 | OKAY, DECERR, and each possible first SLVERR position | whole survivor and first-SLVERR-in-route-order rule are exact; DECERR is not elevated |
| R0-COL-09 | join becomes ready while output is mid-worm | join waits at the worm boundary, then has strict priority without stealing frozen ownership |

The overlapping-tree wedge test remains negative restriction evidence: it demonstrates why the
external non-overlap rule is required, but a detected forbidden-stimulus wedge is not a Router
functional failure and cannot count as positive coverage.

### 5.3 R0 DAT input, output, and network

| Test | Stimulus | Required evidence |
|---|---|---|
| R0-DAT-01 | independently fill and drain every input VC | FIFO order and occupancy are independent per input/VC |
| R0-DAT-02 | every legal turn and every legal input/output VC pair | VC-major then input-minor selection is exact |
| R0-DAT-03 | preferred VC has credit | nonfixed head selects its preferred VC |
| R0-DAT-04 | preferred VC full, multiple eligible alternatives vary | tail-only packet chooses the highest-index credited eligible VC |
| R0-DAT-05 | preferred and every eligible alternative full | head stalls without consuming input or changing state |
| R0-DAT-06 | alternate candidate is eligible when first candidate's VA fails | output remains work-conserving in the same cycle |
| R0-DAT-07 | head selects nonpreferred VC, then body/tail arrive | the worm remains on that output VC until accepted tail |
| R0-DAT-08 | simultaneous output-FIFO dequeue and admission | no bubble, overflow, underflow, or incorrect credit delta |
| R0-DAT-09 | all approved VC modes and traffic classes | eligible sets match the approved mode; class partitions never cross |
| R0-DAT-10 | fixed head on every legal VC | output `vc_id` is unchanged and no spill to another VC occurs |
| R0-DAT-11 | fixed VC has zero credit while another has credit | packet stalls until its fixed VC recovers |
| R0-DAT-12 | toggle every flit field while selecting each VA outcome | every bit is preserved except the independently predicted nonfixed `vc_id` rewrite |
| R0-CRD-01 | seed, send to zero, and return one credit per VC | counter remains in range and send stops exactly at zero |
| R0-CRD-02 | simultaneous send and return on the same VC | counter delta is zero with both events recorded |
| R0-CRD-03 | simultaneous independent events across VCs | no cross-VC accounting |
| R0-CRD-04 | fork one input flit to multiple outputs | exactly one upstream credit returns after the input slot is freed |
| R0-CRD-05 | long random legal traffic followed by drain | seed equals in-flight plus available capacity at every sample and after drain |
| R0-CRD-06 | duplicate return and send-at-zero negative cases | the corresponding assertion fires in separate red runs |
| R0-CRD-07 | LOCAL input/output pressure | LOCAL uses ready/valid where specified and no external LOCAL credit is created or consumed |
| R0-CRD-08 | one dequeue at each input port/VC with all other traffic idle | the registered credit pulse appears at the exact approved boundary latency and lasts one cycle |

### 5.4 R0 reset

| Test | Stimulus | Required evidence |
|---|---|---|
| R0-RST-01 | reset from idle with assertion at multiple clock phases | externally visible valid/credit outputs quiesce and state clears |
| R0-RST-02 | reset with every FIFO class nonempty | queued work, locks, branch masks, pointers, and counters return to approved reset state |
| R0-RST-03 | reset during a stalled REQ/RSP output and during a DAT worm | no stale pre-reset flit transfers after release |
| R0-RST-04 | deassert at arbitrary phase, then inject first legal item | state use begins only after synchronized release and first post-reset item completes once |

## 6. R1 top-level and parameter evidence

| Test | Stimulus | Required evidence |
|---|---|---|
| R1-TOP-01 | all legal unicast turns at corner, edge, and interior coordinates | exact route, transparency, and three-cycle zero-load latency on all networks |
| R1-TOP-02 | independent random stalls on all REQ/RSP outputs | held-valid contract, input backpressure, exact order, and full recovery |
| R1-TOP-03 | all legal DAT turns, modes, fixed values, and pressure combinations | per-hop VA, output FIFO behavior, and class/fixed eligibility are exact |
| R1-TOP-04 | many-to-one traffic from all inputs and VCs | exact two-level arbitration, work conservation, and bounded-opportunity fairness |
| R1-TOP-05 | mixed packet lengths, locked-empty gaps, and independent outputs | no interleave or lock leak; uncongested outputs progress |
| R1-TOP-06 | legal disjoint multicast trees on square meshes with asymmetric branch stalls | branch synchronization, one upstream dequeue, and whole-flit replication |
| R1-TOP-07 | legal CollectB trees on square meshes, error mixes, and mid-worm arrival | exact contributor set, reduction, strict priority, and boundary hold |
| R1-TOP-08 | saturate DAT links, withhold and return credits in random batches | all per-VC conservation equations hold and every seed is restored after drain |
| R1-TOP-09 | assert reset in every nonidle state class | target reset semantics, quiescence, state discard, and post-reset recovery |
| R1-TOP-10 | drive each network alone while the other two are blocked | no cross-network state or backpressure |
| R1-TOP-11 | exercise all seven nonempty active-network combinations | each network reaches an accepted transfer and drains independently |
| R1-TOP-12 | saturate one network while the other two carry backpressure-free traffic | no avoidable bubble or credit/ready coupling across networks |
| R1-TOP-13 | place DUT at each coordinate class and target every absent direction | every boundary output remains silent on every network |
| R1-TOP-14 | target distinct outputs from all serviceable inputs in the same cycle | each output transfers at most one flit and independent outputs operate concurrently without requiring model-style same-FIFO multi-read |

Separate elaboration builds cover every legal set named by `docs/router-spec.md` and generated
constants: mesh geometries, coordinate classes, DAT VC count/mode combinations, input and output
FIFO depths, and ready-slack settings. Unicast positive tests include rectangular power-of-two
geometries; multicast/collective positive tests use square geometries only. Pairwise legal
combinations are sufficient except that all VC counts cross all VC modes and all FIFO depth
endpoints. Current generated defaults are always included.

Expected-fail elaboration jobs independently violate:

- unsupported mesh dimensions or coordinates outside the selected mesh;
- DAT VC count outside its approved range or not representable by the packet field;
- split mode with a disallowed VC count;
- input/output FIFO depth outside its approved range;
- ready slack outside the approved relation to input depth;
- any fixed port count or derived width inconsistent with generated packet types.

Each job must fail because RTR-A22 reports the intended constraint. Compiler failure for an
unrelated syntax, missing-file, or type error is not evidence. The ready-slack numerical
calibration remains `[TBD]` as specified; verification may test approved settings but may not infer
a production default from simulation.

A rectangular Router remains a legal elaboration for unicast, so no Router parameter guard can
infer that collective traffic will be used. The verification/config flow rejects a multicast or
collective run when the selected dimensions differ; this is an acceptance-policy check, not
RTR-A22 evidence.

## 7. Assertion plan and fault-injection evidence

Assertions live in production modules only when they are synthesis-safe protocol guards; richer
white-box properties may be bound from DV. Fault injection uses a separate negative test target and
testbench `force`/mutation of the sampled condition. It never adds a fault mux, parameter, or
conditional path to production RTL. Each red run enables exactly one mutation, must report the
named assertion, and is forbidden from reporting PASS.

| Assertion | Property | Named fault-injection red test and planted fault |
|---|---|---|
| RTR-A01 | REQ/RSP output valid remains asserted until handshake | RTR-FI-A01: drop valid for one stalled cycle |
| RTR-A02 | REQ/RSP and LOCAL DAT payload remains stable while valid and not ready | RTR-FI-A02: flip one payload bit during a stall |
| RTR-A03 | an idle output bus is all zero and no unknown reaches an active interface | RTR-FI-A03: force a nonzero idle flit bit |
| RTR-A04 | each head's selected route is legal and one-hot | RTR-FI-A04: force a second route bit on one head |
| RTR-A05 | worm owner, route, and selected VC remain stable before accepted tail | RTR-FI-A05: change owner on a body flit |
| RTR-A06 | a lock releases only on its accepted tail and never leaks past tail | RTR-FI-A06: suppress release on one accepted tail |
| RTR-A07 | each RR pointer changes only for an accepted packet tail | RTR-FI-A07: advance the pointer on an accepted head |
| RTR-A08 | an eligible candidate is granted within the declared number of eligible packet-boundary opportunities | RTR-FI-A08: mask one persistent candidate for a full arbitration rotation |
| RTR-A09 | fork `done_mask` is a subset of the expected mask and grows only on branch acceptance | RTR-FI-A09: set one unaccepted branch bit |
| RTR-A10 | a completed fork input pops exactly once, only after every expected branch | RTR-FI-A10: assert input pop one branch early |
| RTR-A11 | a forked input returns exactly one upstream credit for its one freed slot | RTR-FI-A11: duplicate the fork completion credit pulse |
| RTR-A12 | CollectB fires only with the exact expected, fully qualified contributor set | RTR-FI-A12: assert reduction grant with one contributor absent |
| RTR-A13 | CollectB replicas agree and the survivor follows the approved response precedence | RTR-FI-A13: alter one replica field while forcing reduction |
| RTR-A14 | every active DAT input/output VC index is in range and mode-eligible | RTR-FI-A14: force an out-of-range output `vc_id` |
| RTR-A15 | `fixed_vc` keeps the wire VC unchanged | RTR-FI-A15: restamp one fixed head to a different VC |
| RTR-A16 | a nonfixed worm selects VC on its head and retains it through tail; overflow is tail-only | RTR-FI-A16: change output VC on one body flit |
| RTR-A17 | no FIFO overflows or underflows, including simultaneous fill/drain | RTR-FI-A17: force dequeue while the selected FIFO is empty |
| RTR-A18 | each N/E/S/W DAT credit counter remains within zero and its seed depth | RTR-FI-A18: inject a duplicate downstream credit at full count |
| RTR-A19 | a DAT flit is sent only when its selected N/E/S/W output VC has credit | RTR-FI-A19: force valid with the selected counter at zero |
| RTR-A20 | an upstream DAT credit pulse corresponds to exactly one freed input VC slot | RTR-FI-A20: pulse credit without an input dequeue |
| RTR-A21 | reset clears valids, FIFO/lock/fork/join/pointer state, and re-seeds credit state | RTR-FI-A21: retain one lock bit across reset |
| RTR-A22 | every static parameter and coordinate constraint is legal at elaboration | RTR-FI-A22: build one job with each illegal-parameter case from Section 6 |
| RTR-A23 | REQ, RSP, and DAT state transitions depend only on their own network inputs and reset | RTR-FI-A23: force one RSP RR-pointer transition on a REQ-only cycle |
| RTR-A24 | each REQ/RSP input environment holds valid/payload until the Router accepts it | RTR-FI-A24: withdraw one input valid while its ready is low |
| RTR-A25 | each DAT credit bit is a one-cycle pulse at the approved latency for one freed slot | RTR-FI-A25: stretch one credit pulse for a second cycle |
| RTR-A26 | each output has at most one owner and emits at most one flit per cycle | RTR-FI-A26: force two internal owner/grant bits for one output |
| RTR-A27 | every absent boundary direction remains invalid on every network | RTR-FI-A27: force one boundary output valid high |
| RTR-A28 | collective op/channel, branch/expected set, continuation, and contributor qualification are legal | RTR-FI-A28: inject a collective continuation with a mismatched branch set |

RTR-A08's monitor counts opportunities only when the contender is valid, route/VC eligible, the
output is serviceable, and no prior worm owns it. The count resets on the candidate's accepted
head or on loss of eligibility. This makes the fairness property non-vacuous without claiming
progress under permanent downstream blockage.

## 8. Coverage model

The functional coverage model contains the following mandatory bins and crosses:

- route: input port, output port, coordinate class, X/Y/LOCAL decision, boundary ejection, and
  legal turn;
- arbitration: contender count, winning input, VC, pointer wrap, sole-eligible, frozen winner,
  packet length, locked-empty interval, and output pressure;
- collective: fork branch count/mask, LOCAL branch, stalled branch position, join contributor set,
  arrival permutation, response class, first-SLVERR position, and mid-worm hold;
- DAT: input VC, preferred output VC, selected output VC, fixed flag, VA outcome, traffic class,
  VC mode, credit level at zero/one/full, and simultaneous send/return;
- buffering: every FIFO empty/almost-full/full transition, simultaneous enqueue/dequeue, and each
  ready-slack/depth relation included by the approved matrix;
- reset: idle, FIFO-nonempty, stalled output, active worm, partial fork, partial join, and nonzero
  credit-state assertion points;
- independence: seven nonempty active-network masks crossed with pressure on each other network;
- hybrid: every comparison point, every runtime exclusion witness, every configuration-exclusion
  manifest entry, and both direct and normalized paths.

Illegal bins reject forbidden XY turns, out-of-range VCs, and collectives that name a boundary
port. Each mandatory bin must be hit by a passing test, and each assertion must have its red-test
witness. Code, branch, expression, toggle, and aggregate functional-coverage percentage targets
are `[TBD]` until the project approves them; no invented percentage can gate signoff.

## 9. Reference-model obligations

The project-owned C++ Router is a conditional oracle. Existing tests already provide executable
evidence for route computation, fork/join masks, zero-load latency, flit transparency, worm locks,
round-robin behavior, same-cycle FIFO fill/drain, VC allocation, fixed VC, chained-router credit
conservation, fairness, collectives, adapter behavior, and wrapper boundaries. Before R2, the
specific tests used as oracle evidence must pass at the pinned revision.

| Existing model evidence | Obligation before R2 |
|---|---|
| `RouterRouteCompute.*` and `RouteMask*` | retain exhaustive direction/range and fork/join member-set checks |
| `RouterDatapath.ZeroLoadLatencyIsThreeTicks` and `SimpleRouterDatapath.*Latency*` | retain core timing checks and separately prove the held-output wrapper cycle |
| `RouterWormhole.*`, `SimpleRouterWormhole.*`, and `NocWormholeArbiter.*` | retain lock, gap, tail, frozen-winner, and malformed-worm checks |
| `RouterVcArbitration.*`, `RouterFairness.*`, and `RouterVa*` | retain exact RR/VA/fixed/overflow/credit cases for the aligned SHARED instance |
| `RouterCredit.*`, `RouterDatapath.CreditDecrementAtGrantAndPulseAfterDequeue`, and adapter tests | retain per-VC counter and chained conservation checks |
| `RouterFork*`, `SimpleRouterFork*`, and `SimpleRouterJoin*` | retain branch synchronization, restriction-wedge, contributor, survivor, and qualification checks |
| `RouterWrap.*` and `sim/tools/test_model_egress_hold.py` | retain physical direction, LOCAL credit, and REQ/RSP held-egress checks |

Two alignment tests are still obligations rather than existing evidence: M-ALIGN-01 must witness
the same-input/multiple-output model tick used by R2-X02, and M-ALIGN-02 must drive REQ, RSP, and
DAT simultaneously with independent stalls and prove model-wrapper network independence. R2 may
not claim either exclusion or alignment property until its named test is non-vacuous.

| Area | Aligned comparison | Required obligation or exclusion |
|---|---|---|
| unicast route and masks | direction and mask values | compare exhaustive legal coordinates/masks; invalid-input death tests remain diagnostic rather than RTL timing oracles |
| REQ/RSP datapath | accepted flit bits/order and model-facing three-cycle zero-load latency | use the held-valid wrapper; exclude internal queue occupancy and direct-core two-tick mode |
| arbitration and worm lock | winner sequence, packet lock, tail release | compare only when one input FIFO read per cycle is sufficient; tag the optimistic multi-output case below |
| fork | branch set, full-flit replication, branch completion, single input pop | event sequence compares everywhere; cycle comparison excludes a same-input multi-output tick |
| join | expected set, qualification, whole survivor, response precedence, worm-boundary hold | add/retain directed permutations and disagreement death tests before using the model result |
| DAT SHARED mode | preferred mapping, tail overflow, fixed VC, lock, credit events | cycle compare the current generated aligned instance; do not infer split-mode behavior |
| DAT split mode | none | R1 independent predictor/property evidence only until the model is aligned |
| external DAT links | N/E/S/W credit protocol and flit events | compare counters and events; target LOCAL flow control is normalized and not cycle-exact |
| reset | startup initialization only | all mid-run and nonempty reset evidence is target-only R0/R1 |
| parameter widths | current generated model types | other legal widths/geometries use R0/R1 until model containers and wrapper types are aligned |
| three-network independence | separate Router instances exist in the wrapper | add a focused model test with simultaneous traffic and independent stalls before claiming R2 isolation evidence |

The known optimistic behavior permits one input FIFO to supply different outputs in one model tick.
It is retained as a model capability and explicit R2 exclusion, not imposed as a multi-read RTL
microarchitecture. A focused model sentinel must continue to demonstrate this case so that the
exclusion cannot become vacuous. Any new model/target mismatch stops differential signoff until it
is either fixed or approved and added to Section 11.4; the harness may not silently widen matching
windows.

## 10. Reference classification and license handling

Pinned revisions, inspected filenames, per-candidate classification, interface/behavior/dependency
deltas, and license obligations are recorded only in the Provenance section of
`docs/verification-environment.md`. This plan refers to those entries by source ID.

- P3-F01 through P3-F06 are `adapt` candidates. Any later use requires a separate reviewed change,
  retains upstream notices, marks modifications, and removes unsupported generic behavior rather
  than carrying it into production.
- P3-F07 through P3-F31 are `rewrite` or out-of-scope candidates because their interface,
  behavior, storage ownership, flow control, packet types, or dependency closure conflicts with
  the frozen target. They must not be copied into the production implementation.
- P3-T01/P3-T02 are `adapt` for test intent only; no source is copied by this issue.
- P2 remains reference-only. General testing ideas may inform project-owned DV, but its RTL must
  not be copied, adapted, or instantiated.

No candidate qualifies for `reuse as-is`: none simultaneously meets the frozen interface,
behavior, dependency, and reset requirements. Issue #10 copies, adapts,
instantiates, or edits no third-party RTL and leaves every notice unchanged.

## 11. R2 Router differential co-simulation

### 11.1 Composition and identical stimulus

R2 instantiates one RTL Router and one reference `RouterWrap` at the same coordinate. Neither is
connected to another Router or a mesh. A single stimulus coordinator owns a canonical, seeded trace
and drives both instances:

- REQ/RSP input `valid` and flit bits are identical each cycle. Lockstep cases launch an item only
  when both inputs can accept it, so the same item transfers on the same edge and cannot be accepted
  twice by one instance. Separate R0/R1 tests prove valid persistence against arbitrary ready.
- REQ/RSP output `ready` is the same bit for both instances on every port and cycle.
- DAT input valid, flit, and VC are identical each cycle. The coordinator sends only when both
  per-instance shadow input-credit accounts permit the same transfer.
- DAT N/E/S/W returned-credit vectors are identical on every cycle. Lockstep sinks return a credit
  only after the matching pair of output events has been observed and after the same seeded delay.
- the same LOCAL DAT ready schedule drives the RTL sink and the reference normalizer's sink side.
- reset assertion and startup release stimulus are common; target-only reset cases remain R0/R1.

If a supposedly aligned ready, flit, or credit event differs before a common transfer, the
coordinator stops that lockstep case and reports a differential failure; it does not mask the
difference by delivering the item on separate cycles. In the R2-X06 event test only, the
coordinator keeps identical input valid low until both instances are ready, records each unequal
ready cycle under R2-X06, and then performs one common transfer. Each cycle records the seed, input
item index, ready vector, credit vector, output events, and active exclusion tag for replay.

### 11.2 LOCAL DAT normalizer

The reference model exposes credit flow control at LOCAL DAT while the target exposes ready/valid.
A verification-only bounded normalizer receives reference LOCAL flits without changing any bit or
order, presents its oldest entry as held ready/valid, and returns the corresponding model credit
only when the common LOCAL ready schedule frees that entry. Its capacity equals the credits it
advertises. Overflow, underflow, bit change, reordering, fabricated credit, or credit returned for
an occupied entry is fatal. The normalizer adds no routing, arbitration, VC selection, or target
timing and is excluded from every production source list.

### 11.3 Comparison points

The harness compares:

1. accepted input item index, complete flit, port, network, and VC at every common transfer;
2. per-cycle REQ/RSP output valid, complete flit, and handshake on all five ports in lockstep cases;
3. per-cycle DAT N/E/S/W output valid/flit and input credit-return vectors in lockstep cases;
4. LOCAL DAT accepted output sequence after normalization, including exact bits and order;
5. per-output packet boundaries, fork multiplicity, join contributor consumption and survivor;
6. input/output/credit conservation totals and empty final state after drain.

R2 uses both comparison modes. Lockstep tests require exact cycle identity at direct interfaces.
Event tests compare ordered accepted streams only for a case carrying a specific approved
exclusion tag. Both modes still require exact flit bits, output identity, multiplicity, order, and
final conservation.

### 11.4 Approved cycle-divergence exclusions

| Tag | Excluded equality | Equality still required |
|---|---|---|
| R2-X01 | reference LOCAL DAT credit timing, normalizer residence, and LOCAL output cycle | normalized accepted flit bits/order/count and normalizer conservation |
| R2-X02 | exact cycles when one model input FIFO supplies multiple outputs in one tick, including collective fanout | per-output worms, branch set, no duplication, input-pop count, and final conservation |
| R2-X03 | split VC mode | no R2 claim; complete R0/R1 eligible-set, fixed-VC, and credit evidence is required |
| R2-X04 | mid-run/nonempty reset and post-reset cycle alignment | no R2 claim; complete R0/R1 reset evidence is required |
| R2-X05 | non-current model field widths and unaligned parameter builds | no R2 claim; complete legal R0/R1 build and functional evidence is required |
| R2-X06 | internal FIFO occupancy and the provisional REQ/RSP ready-threshold cycle | accepted sequence, no overflow, held output, bounded capacity, and full drain |

An exclusion is active only in the named test and only while its triggering witness counter is
nonzero. Every unlisted cycle mismatch fails. Each exclusion has a dedicated test that proves the
relaxed comparison path is exercised, and at least one nearby mutation must still fail the
remaining equality checks. Adding or broadening an exclusion is a specification decision and
requires approval; it is not a test-debug action.

### 11.5 R2 test matrix

| Test | Stimulus | Required evidence |
|---|---|---|
| R2-HYB-01 | one unicast flit for every legal input/output route on all networks | direct outputs are cycle- and bit-exact; LOCAL DAT is event-exact |
| R2-HYB-02 | multi-flit worms with identical REQ/RSP ready and DAT credit delays | lockstep ownership, tail release, held outputs, and ordered completion match |
| R2-HYB-03 | all-to-one contention with one-flit and mixed-length packets | exact arbitration sequence and fairness opportunities match |
| R2-HYB-04 | current aligned SHARED-mode preferred, overflow, stall, and fixed-VC cases | direct DAT VC/flit/credit events match cycle by cycle |
| R2-HYB-05 | legal forks with one-, two-, and multi-branch sets | direct branches match; R2-X01/X02 are tagged only when their witnesses occur |
| R2-HYB-06 | CollectB arrival permutations, errors, and worm-boundary holds | contributor consumption and complete survivor match |
| R2-HYB-07 | simultaneous independent REQ/RSP/DAT traffic and pressure | all three direct event streams match and each network has nonzero progress |
| R2-HYB-08 | directed runtime cases for R2-X01, R2-X02, and R2-X06; manifest audit for R2-X03 through R2-X05 | runtime witnesses are nonzero, configuration exclusions name their R0/R1 evidence, and all nonexcluded checks remain armed |
| R2-HYB-09 | corrupt one compared flit, duplicate one normalized event, and alter one credit separately | each red run fails its intended checker and cannot report R2 PASS |
| R2-HYB-10 | long reproducible random aligned trace followed by bounded-fair drain | all comparison points are nonzero, all scoreboards drain, and credits return to seed |

R2 PASS requires nonzero accepted traffic on every network and port class exercised by the test,
at least one stalled ready/valid interval, at least one zero-credit interval per exercised DAT VC,
one contended arbitration, one multi-flit worm, one legal fork, one legal join, and empty final
scoreboards. Exclusion counters are reported separately and never count as ordinary matched cycles.

## 12. Package gates and signoff

| Package | Required evidence before its implementation issue is complete |
|---|---|
| `router_route_select` | R0-ROUTE-01 through R0-COL-03; RTR-A03/A04; route/mask coverage |
| `router_simple_network` | R0-SIM, R0-ARB, and R0-COL-04 through R0-COL-09; RTR-A01 through RTR-A13 |
| `router_dat_input` | R0-DAT-01 and relevant FIFO/reset tests; RTR-A14/A17/A20/A21 |
| `router_dat_output` | R0-DAT-02 through R0-DAT-11 and R0-CRD; RTR-A05 through RTR-A08 and RTR-A14 through RTR-A19 |
| `router_dat_network` | multi-input/output DAT, fork coordination, LOCAL boundary, and conservation tests; no duplicate storage ownership |
| `router` | all R1 tests, parameter expected-fail builds, all assertion red tests, mandatory functional coverage, then all R2 tests |

The block is accepted only when:

- focused R0 and R1 suites pass from a clean build with all mandatory bins hit;
- every RTR-A assertion has its named failing red test and passes in positive regression;
- all legal parameter builds elaborate and each illegal build fails for its intended guard;
- R2 direct interfaces pass cycle-exact comparison, normalized/excluded cases satisfy only the
  enumerated exceptions, and all non-vacuity/conservation gates pass;
- generated-source drift, lint, and repository tests pass;
- the repository clean target is run after inspecting build, test, or simulation results and no
  generated artifact remains.

Only after this gate may the full-RTL `2x2 verify` run begin. The `4x4 verify` milestone remains the
later campaign integration gate; neither mesh run changes the Router block acceptance criteria.
