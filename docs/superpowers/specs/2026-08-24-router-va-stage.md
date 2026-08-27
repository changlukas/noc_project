# DAT router: VC allocation as its own stage

Design for `router::Router` (DAT). `SimpleRouter` (REQ/RSP) is unchanged.

## Decision

The DAT router pipeline follows the canonical input buffered VC router (On-Chip Networks 2nd ed.
ch 6): BW, RC + VA, SA + ST, LT. A head flit passes all four. A body or tail flit skips RC + VA
and inherits the route and output VC its head obtained. Head 4 cycles per router, and each
following flit of the packet leaves one cycle after the flit ahead of it, so a packet of n flits
leaves n minus 1 cycles after its head. A single flit packet is a head and a tail, 4 cycles. A
body measures 3 only when it enters an empty FIFO after its head's SA grant.

| Point | Ruling | Source |
|---|---|---|
| VA grants one output VC to the head. The VC must be free (held by no packet) and credited (`credit_[out][vc] > 0`). A single flit packet (`flit_tail = 1`): preferred VC first, else the highest index other free credited VC, else idle and retry. A worm head (`flit_tail = 0`): preferred VC only, else idle and retry. | ch 6 VA grants output VCs to heads. The credit term is a deviation from the textbook (VA free only, SA credit): DAT traffic is mostly single flit R, and a head holding a zero credit VC while it waits in SA would block every other packet that prefers that VC. The single flit overflow and the worm head preferred only rule are today's FVADA rules moved to VA. Worm heads never overflow because two `fixed_vc = 0` worms to one destination must stay on one VC to keep write order (the RoB reorders responses only). FlooNoC selects per flit on `vc_not_full`, never holds. | textbook, `router.hpp:324-345`, `nmu-spec.md` ordering domain, `floo_vc_assignment.sv:96-116` |
| Credit is not reserved at VA. SA serves a held VC only through its holder, so nobody else can spend that VC's credit between the holder's VA and SA. | `router.hpp:502-511` | |
| SA checks credit on the held VC for every flit. A flit with no credit waits in SA and keeps the VC. | ch 6 SA grants the crossbar per flit | textbook |
| The output VC is held from the VA grant until the tail passes SA. Tail deallocates. | ch 6, ch 5 | textbook |
| Within one cycle SA runs before VA. A VC a tail frees in cycle t is allocatable by VA in cycle t. | back to back single flit packets on one input VC would otherwise run at one flit per two cycles. In RTL this is the SA grant to VC free to VA allocate combinational chain | reviewer finding |
| Fork head VA is per branch. Each branch output runs the VA rule on the parked head independently and holds its VC once obtained. The head leaves the input FIFO when every branch has granted it (the fork pop pass, unchanged). | today's F6 semantics (every branch locked at its own grant, `fork_done_` incremental join) moved to VA. The VA credit term keeps the hold window equal to today's. An atomic all branch VA would need a cross output arbitration order and can livelock between overlapping forks | round 1 and round 2 review |
| VA arbitration per output: when several idle input VCs want one output in the same cycle, input VC major from `in_vc_rr_[out]`, input minor from `rr_[out]`, one VA grant per output per cycle. Both pointers advance on a VA grant only, never on a failed VA. `vc_rr_[out]` stays in SA and advances on every SA grant. VA is not gated by the output FIFO, SA is. | keeps SPEC 12 and SPEC 16 backed | reviewer finding |
| Within one cycle SA pops before VA runs, so VA sees the post pop FIFO front. One pop per input VC per cycle follows from VA: only the front flit of an input VC is eligible and only its assigned output can pop it. | the C++ model evaluates stages in reverse order already | `router.hpp:477-484` |
| The input FIFO slot frees one cycle later than today (SA instead of the merged stage), so the credit round trip grows by one cycle. `NOC_ROUTER_VC_DEPTH` 8 covers it. Measured, not tuned. | `router-spec.md` 2.7 | round 2 review |
| One input VC FIFO pops at most one flit per cycle. SPEC 15 (several pops of one input VC FIFO in a cycle) is withdrawn. | route and VC now belong to the input VC, only its front flit is eligible. Closer to RTL (`docs/known-limitations.md`, the multi pop row) | user ruling |
| Low load bypass, speculative VA, lookahead RC: not in this change. | user ruling | |
| FlooNoC `floo_vc_router` runs SA then VA in one stage and locks the input to output pairing, not a VC. Not followed. | `floo_vc_router.sv:277-302, 334-341` | |

## Stages

| Stage | Who | State touched | Action per cycle |
|---|---|---|---|
| 1 BW | every flit | `input_fifo_[in][vc]` | file the registered flit by header `vc_id` |
| 2 RC + VA | head at the FIFO front of an input VC in state `idle` | per input VC: `route` (one output, or a branch set for a fork), `out_vc` per branch, state | route compute, then per branch output pick a VC by the VA rule above. Unicast success: record `out_vc`, set `wormhole_[out][out_vc] = (in, vc)`, state `active`. Fork: each branch that succeeds records its `out_vc` and locks, the input VC is `active` once one branch holds, branches still without a VC retry VA on the parked head each cycle. Unicast failure: state stays `idle` |
| 3 SA + ST | front flit of every input VC in state `active` | `credit_[out][out_vc]`, `fork_done_`, output FIFO | per output, output FIFO below depth: round robin from `vc_rr_[out]` over its VCs whose holder has a front flit and `credit_[out][vc] > 0`, grant one, decrement credit, stamp `vc_id`, push the output FIFO, schedule the upstream credit pulse. Unicast pops now. Fork marks `fork_done_` and the fork pop pass pops when every branch granted, as today. Tail grant clears `wormhole_[out][out_vc]` for that branch and, when every branch has granted the tail, the input VC returns to `idle` |
| 4 LT | output FIFO front | link | one flit per output per cycle |

Per input VC state is the textbook G, R, O, C set: G in {idle, active} plus `head_parked`, true
from the VA grant until that flit leaves the input FIFO; R = route (branch set), recomputed from
the parked flit's header rather than stored; O = output VC per branch; C per output VC in
`credit_` as today. While the head is parked, SA skips its continuation checks and a fork's
remaining branches may still allocate their own VC off that same head. The textbook waiting VC
and waiting credit states are the idle retry and the SA wait.

An `active` input VC whose FIFO is empty keeps its VCs and waits, as the lock does today. The
`fixed_vc = 0` continuation check (locked `out_vc` equals the recomputed preferred VC) stays in
SA. The F9 branch set check stays in SA.

## What moves from today

| Today (stage 2 does RC, VA, credit, grant in one cycle) | After |
|---|---|
| unlocked slot scan per output: first head whose `vc_assignment` lands on this VC | VA per input VC the cycle before. SA scans the held VCs only |
| lock set at grant of the head | lock set at VA. The head is still at the input FIFO front |
| `vc_assignment` credit gate, FVADA overflow for tails only | VA rule: free and credited, preferred first, overflow for single flit packets only, worm heads preferred only. No VA at SA |
| tail steal guard | structurally impossible, removed |
| `fixed_vc = 1`: VC verbatim, credit gated, waits while the slot is locked | same, checked at VA |
| fork: each branch locks at its own grant | each branch locks at its own VA |
| same input VC FIFO may pop twice in a cycle (SPEC 15) | one pop per input VC per cycle |

Credit accounting, credit pulses, the fork pop pass, `locked_branch_set`, `SimpleRouter` and all
NI code are unchanged.

## Latency

Only `DataR` (single flit) and the AW + W worm ride DAT. `DataAr` rides REQ and `DataB` rides
RSP, both `SimpleRouter`, unchanged.

| Packet | Per router | mesh_4x4 zero load probe, 3 hops, 4 routers | Before |
|---|---|---|---|
| `DataR` | 4 | 16 | 12 |
| AW + 32 W | head 4, each W one cycle behind the flit ahead of it | head 16, W stream unchanged | head 12 |

Scenario 2 zero load data read moves from 37 to 41, data write from 38 to 42. Within the target
range DAT 3 to 5 per hop (`docs/noc-target-spec.md:812`). Throughput: SA still grants one flit per
output per cycle, body flits pipeline at one per cycle, and the SA before VA rule keeps back to
back single flit packets at one per cycle.

## Verification

| Test | Change |
|---|---|
| `RouterDatapath.ZeroLoadLatencyIsThreeTicks` | `ZeroLoadLatencyIsFourTicks` |
| new `RouterDatapath.BodyFlitsFollowHeadOneCycleApart` | 3 flit worm: head tick 4, body 5, tail 6 |
| new `RouterDatapath.BackToBackSingleFlitsOnePerCycle` | 8 single flit packets on one input VC arrive one per cycle after the first |
| new `RouterVa.HeadWaitsForHeldVc` | preferred VC held by another packet, other VCs uncredited: head stays idle. A credited free other VC: head takes it |
| new `RouterVa.ZeroCreditVcIsNeverHeld` | preferred free, credit 0, no other credited VC: head stays idle, `wormhole_[out][*]` empty |
| new `RouterVa.ForkBranchesLockAtVaIndependently` | two branch fork, one branch VC held: the free branch locks and waits, the held branch locks when freed, pop after both grant |
| `RouterVaFvada.PreferredThenHighestIndexOverflowThenStall` | keep, +1 tick, worm head still stalls on a full preferred VC |
| `RouterVaDeath.LockedOutputVcVsPreferredMismatchAborts` | keep, the continuation check stays valid because worm heads never overflow |
| `RouterVaWorkConserving.HeadVaFailAlternateCandidateGrantedSameTick` | rewrite: VA failure is a held VC |
| `RouterWormhole.CreditBlockedTailDoesNotOverflowToAnotherVc` | keep |
| `kPipelineDepth` and hard coded ticks in `test_router.cpp`, fork and chain tests | +1 per router for heads |
| the SPEC 15 multi pop test | delete |
| co-sim gates | directed uniform_random and bit_complement burst 32, continuous vc2 and vc8 uniform_random |
| Scenario 2 zero load rerun | data read 41, write 42, narrow unchanged |

## Docs

`docs/router-spec.md` 2.4 (stage table, per router head 4 then one flit per cycle, SA before VA, one pop per
input VC), 2.5 (VA a cycle before SA, VA rule, VA arbitration pointers), 2.6 (lock at VA, fork
atomic), R10 (head 4, one flit per cycle after), SPEC 4, SPEC 10 to 13, SPEC 15 withdrawn, SPEC 16, the worked
example in 2.9 and the multi pop example in 2.4. SPEC 4 becomes DAT head 4 then one flit per cycle at the
wrapper pins. `docs/noc-target-spec.md` 7.4 as built line. `docs/known-limitations.md` multi pop
row removed.
