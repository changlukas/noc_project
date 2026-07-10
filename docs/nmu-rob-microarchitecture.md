# NMU reorder buffer -- as-built microarchitecture

What `nmu::Rob` (`src/c_model/include/nmu/rob.hpp`) is today, structure by structure, and
which of its shapes are decisions and which are accidents of C++.

This is a description, not a proposal. Every row that a future RTL implementer would have to
choose is marked. Where the C++ has no buildable counterpart, it says so.

## 1. Placement and interface

Inline between `AxiSlavePort` and `{Packetize, Depacketize}`. Implements `RequestPacketizer`
(`push_aw` / `push_w` / `push_ar`) on the request side and `ResponseDepacketizer` (`pop_b` /
`pop_r`) on the response side.

Two header fields carry the RoB across the wire:

| field | width | declared | set by | consumed by |
|---|---|---|---|---|
| `rob_req` | 1 | `ni_packet.json:77` | NMU `Packetize` | NMU `Depacketize`, after the NSU echoes it back |
| `rob_idx` | `ROB_IDX_WIDTH` = 8 | `ni_packet.json:82-83` (`width_param`) | NMU `Packetize` | same |

The NSU stores both in its meta buffer and replays them onto the B/R response
(`nsu/meta_buffer.hpp`, `nsu/packetize.hpp:92-93,107-108`). The NoC never reads them.

Mode is per direction: `mode_w_`, `mode_r_` (`rob.hpp:139`), from `NmuConfig::write_rob_mode` /
`read_rob_mode` (`nmu.hpp:129-130`). FlooNoC carries the same split as `BRoBType` / `RRoBType`
(`floo_pkg.sv:301,305`). **The wrap layer collapses it**: `nmu_wrap.hpp:71-72` assigns one
`rob_mode` to both, and the DPI entry point (`cmodel_dpi.cpp:402-411`) exposes a single
`rob_enabled` int. A B-only RoB -- FlooNoC's cheap configuration, since B carries no payload --
is unreachable from the co-sim.

## 2. `RobMode::Disabled` -- as built

The shipped default (`cmodel_nmu_create`, `cmodel_dpi.cpp:396-400`).

| structure | type | size | HW counterpart |
|---|---|---|---|
| `write_outstanding_` | `array<bool, 256>` | 256 b | 256 FF |
| `read_outstanding_` | `array<bool, 256>` | 256 b | 256 FF |
| `w_burst_credit_` | `uint32_t` | -- | counter |

**INPUT** `push_aw` refuses while `write_outstanding_[id]` is set (`rob.hpp:251`).
**COMPUTE** On a successful downstream push the flag is set and the flit leaves with
`rob_req = 0` (`rob.hpp:252,255`). `push_ar` mirrors it (`rob.hpp:297-303`).
**OUTPUT** `pop_b` clears the write flag; `pop_r` clears the read flag on `last`
(`rob.hpp:316,330-331`).

One transaction in flight per AXI ID, so no response can overtake another of the same ID and
no reorder storage is needed. Ordering is a property of the interlock, not of the network.

`w_burst_credit_` gates W beats behind their AW (`rob.hpp:261`). A single counter suffices
because AXI4 W beats follow AW issue order strictly -- there is no `WID`.

**Gap against FlooNoC `NoRoB`.** FlooNoC admits multiple outstanding transactions per ID as
long as the destination is unchanged, bounded by a per-ID counter
(`floo_rob_wrapper.sv:139`: `push = ax_valid_i && (!in_flight || ax_dest_i == prev_dest) &&
!counter_full`). Ours stalls even when the destination is unchanged. Strictly more
conservative, strictly less throughput, and the difference has never been measured.

## 3. `RobMode::Enabled` -- as built

`rob_idx`'s addressable range is `ROB_IDX_SPACE = 1u << ni::header::ROB_IDX_WIDTH` = 256
(`rob.hpp:96`), which sizes every structure below to 256 entries. Each direction's actual pool
depth is a runtime ctor parameter -- `b_rob_depth_` / `r_rob_depth_`, default 32 -- so only the
low `depth_` entries of a 256-entry array are ever allocated (`rob.hpp:47-48,54-55,141-142`). The
per-ID order-list depth, `max_txns_per_id_`, is a third ctor parameter, also default 32
(`rob.hpp:48,56,143`).

| structure | type | per-entry content | HW counterpart |
|---|---|---|---|
| `write_entries_` | `array<WriteEntry, 256>` (`rob.hpp:169`) | `{occupied, ready, axi_id, BBeat}` | B RoB. Metadata only; no payload exists on B. |
| `read_entries_` | `array<ReadEntry, 256>` (`rob.hpp:170`) | `{occupied, ready, axi_id, RBeat}` | R RoB. `RBeat` carries `RDATA_WIDTH` = 256 b of data. |
| `alloc_write_` | `bitset<256>` (`rob.hpp:175`) | one bit per allocated range, set at the range's top | FlooNoC `rob_alloc_q` (`floo_rob.sv:146`) |
| `alloc_read_` | `bitset<256>` (`rob.hpp:176`) | same | same |
| `write_order_by_id_` | `array<deque<BeatRange>, 256>` (`rob.hpp:197`) | `{base, len_plus_1, rob_req}` | per-ID program-order / status list |
| `read_order_by_id_` | `array<deque<BeatRange>, 256>` (`rob.hpp:198`) | same | same |
| `read_arrival_offset_` | `array<uint16_t, 256>` (`rob.hpp:205`) | beat counter, keyed by burst base | per-burst arrival counter |
| `read_range_len_` | `array<uint16_t, 256>` (`rob.hpp:206`) | burst length, keyed by base | bound for the above |
| `read_release_offset_` | `array<uint16_t, 256>` (`rob.hpp:212`) | beats of the head burst released so far, keyed by base | per-burst release counter |
| `committed_b_queue_` | `deque<CommittedBEntry>` (`rob.hpp:215`) | released beat | output FIFO |
| `committed_r_queue_` | `deque<CommittedREntry>` (`rob.hpp:216`) | released beat | output FIFO |
| `committed_b_pending_` | `array<uint8_t, 256>` (`rob.hpp:217`) | release refcount | per-slot counter |
| `committed_r_pending_` | `array<uint8_t, 256>` (`rob.hpp:218`) | release refcount | per-slot counter |

`free_write_entries_` / `free_read_entries_` and `find_consecutive_free` (section 7 row 2, as
built through Task 5) are deleted; `alloc_write_` / `alloc_read_` replace them.

### Allocation

**INPUT** `push_aw(b)` / `push_ar(b)`.
**COMPUTE** Both first gate on the per-ID order-list depth: FlooNoC's `ax_gnt_o`
(`floo_rob.sv:414`), ported as `write_order_by_id_[b.id].size() >= max_txns_per_id_`
(`rob.hpp:226`) and the AR mirror (`rob.hpp:269`). **Bypass clause 1**: if that ID's order list
is empty, nothing in flight can overtake this response, so it needs no slot
(`needs_rob = !...empty()`, `rob.hpp:229,273`, ported from `floo_rob.sv:422-425`). Otherwise
`push_aw` takes one slot from `write_free_space()` (`rob.hpp:233-234`); `push_ar` takes `len + 1`
slots from `read_free_space()`, refusing if free space is short (`rob.hpp:277-278`). The
allocator is FlooNoC's `lzc` high-water stack (`floo_rob.sv:155-164`), ported as `alloc_write_` /
`alloc_read_` marking only the top of each allocated range and `write_free_space()` /
`read_free_space()` counting leading zero bits above it (`rob.hpp:106-113`, `highest_set`,
`rob.hpp:183-188`). Space returns only from the top (`commit_b_exit` / `commit_r_exit`,
`rob.hpp:463-481`); a hole below the high-water mark cannot be reused until the mark retreats
past it. `find_consecutive_free`'s any-consecutive-run scan is gone.
**OUTPUT** The allocated (or bypassed) base becomes `rob_idx` on the outbound flit, with
`rob_req = needs_rob` (`rob.hpp:236-238` AW, `rob.hpp:280-282` AR). The `{base, len+1, rob_req}`
triple is appended to that ID's order deque whether or not a slot was taken (`rob.hpp:246` AW,
`rob.hpp:293-294` AR).

A write burst still returns one B regardless of length; a read burst still consumes one slot
per beat, because each slot *is* the storage for one beat of `rdata`. Depth is no longer a
single shared constant: `b_rob_depth_` and `r_rob_depth_` are independent runtime parameters
(section 6).

### Release

**B** `pop_b_staged` marks slot `rob_idx` ready, then drains that ID's order deque from the
head while the head slot is ready and not itself bypassed (`drain_ready_write_heads_`,
`rob.hpp:336-345`, called from `rob.hpp:384,399`). Out-of-order arrivals wait.

**R** `pop_r_staged` reads `rob_idx` as the **burst base**, not the beat index. The NSU stamps
every beat of a burst with the same base, so beat *i* lands at
`base + read_arrival_offset_[base]` (`rob.hpp:434-454`). Release is now **per-beat**, not
whole-burst: `drain_ready_read_heads_` walks `read_release_offset_[base]` forward one beat at a
time while `read_entries_[base + offset]` is ready, pushing each beat onto `committed_r_queue_`
as it becomes available (`rob.hpp:347-364`). The order-list entry pops only once every beat of
its burst has been released (`rob.hpp:359-362`). This matches FlooNoC, which frees each slot as
its beat leaves and forwards another ID's response directly when the next beat is late
(`floo_rob.sv:250-266,287-297`); the earlier whole-burst gate (held every slot until the last
beat landed) is retired.

`commit_b_exit` / `commit_r_exit` free a slot when its release refcount reaches zero
(`rob.hpp:463-481`).

Bypass and slotted responses are not distinguished by rejecting one or the other outright: a
bypassed response must match the head of its ID's order list, or the model aborts
(`rob.hpp:379-382` B, `rob.hpp:420-423` R).

## 4. Invariants that hold today and are written nowhere

1. **At most one bypassed entry per AXI ID.** Bypass clause 1 admits an entry onto an order list
   only when that list is empty (`rob.hpp:229` AW, `273` AR), so a second transaction to the same
   ID always finds a non-empty list and takes a slot.
2. **Every other order-list entry of that ID owns a slot.** Combined with 1, a per-ID order list
   holds at most `1 + depth` entries, where `depth` is `b_rob_depth_` or `r_rob_depth_`.
3. Therefore the total length of all order deques, summed over all `AXI_ID_SPACE` = 256 IDs, is at
   most `NumIds + depth` = `256 + b_rob_depth_` (write) or `256 + r_rob_depth_` (read) -- not
   `depth` alone. `std::deque` is unbounded; this bound is asserted nowhere in code.
4. **The slot pool is no longer the outstanding limit.** It still bounds reorder storage for
   slotted transactions and still guarantees the NoC can always find somewhere to put a response
   (section 5); the per-ID outstanding count is `max_txns_per_id_` (`rob.hpp:226,269`),
   independent of pool depth.

Admission in Enabled mode is blocked by two independent checks: the per-ID order-list depth
(`rob.hpp:226,269`) and, for a non-bypassed transaction, slot-allocation failure
(`rob.hpp:233,277`). The `s1_aw_.full()` / `s1_ar_.full()` checks upstream are ordinary queue
backpressure, not a transaction-count limit.

### A read burst longer than the pool wedged the port -- fixed

Before bypass, `push_ar` returned `false` for any `len + 1` exceeding the pool depth, with no
other path, so a 256-beat `INCR` read never left the AXI slave port in `RobMode::Enabled`: not
slowly, never. Recorded until the fix only as a matrix exclusion (`AX4-BUR-003`, `len 256`, "rob
capacity"), never named a defect.

FlooNoC never had it, because of bypass: `floo_rob.sv:336-355` checks `rob_free_space >
ax_len_i` **only on the `ax_rob_req_o` path**; a transaction that needs no reordering is admitted
at any length. Bypass clause 1 (section 3) is the same fix here: `needs_rob` is false for the
first burst of an idle ID, so no slot check applies (`rob.hpp:273,277`) and the burst is admitted
at any length. `test_rob.cpp:1213-1229` covers the admit; `:1231-1248` confirms a second oversized
burst on the same, now non-idle, ID is still refused -- per-ID, not a channel wedge. Verified on
the wire: `docs/backlog.md`, "FIXED 2026-07-11" (a 64-beat burst that hung pre-fix drains
post-fix; all shorter patterns unaffected).

Bypass clause 1 was therefore not only an area optimization. It is what makes long bursts
admissible at all.

## 5. What the slot pool is, and is not

**It is the network's deadlock guarantee.** A response that arrives with nowhere to go stalls
in the NoC, and a stalled response blocks every flit behind it. FlooNoC states the rule
directly (`docs/floonoc/chimneys.md:18`):

> the NI still needs to ensure that all responses from the NoC can be handled, either by
> buffering them in the RoB or forwarding them to the on-chip protocol (if they are in
> order). Stalling the network is not an option, as this would inevitably lead to deadlocks.
> Therefore, the NI also needs to track or allocate space in the RoB and only inject new
> requests into the network if it can guarantee that the responses can be handled.

So "no free slot, no request" is not a coupling to be removed. It is the reason the fabric does
not deadlock. FlooNoC gates the same way (`floo_rob.sv:345`: `rob_free_space > ax_len_i`).

**It is not the only way to reach that guarantee.** A transaction whose response provably
cannot arrive out of order needs no slot. FlooNoC exploits this with two clauses in
`floo_rob_status_table` (`floo_rob.sv:422-433`): the first transaction of an ID needs no
reorder storage, and neither does a follow-on transaction to the same destination as the
previous one. Only the `else` branch allocates.

**A bypassed transaction still occupies an order-list entry.** FlooNoC pushes its status FIFO
unconditionally; `rob_req` is merely a bit inside the entry (`floo_rob.sv:443-446`). Section 4's
invariants 1-3 are this re-derivation, now that bypass clause 1 is shipped.

Under clause 1 alone it holds: a bypassed entry is admitted only onto an empty list, so an ID
holds at most one of them, and every other entry of that ID owns a slot. Per-ID list length is at
most `1 + depth`; the total at most `NumIds + depth`. Under clause 2 it would not hold: one ID
could bypass repeatedly while its destination holds steady, and the list would grow without
bound. That is why FlooNoC carries `MaxTxnsPerId`, enforced as `ax_gnt_o = !fifo_full[ax_id_i]`
(`floo_rob.sv:414`). Clause 2 is not implemented here (below).

The trade the two clauses make is RoB storage against status-table storage.

**The second clause does not hold here.** It assumes same destination implies in-order
arrival, which FlooNoC buys by hardwiring one virtual channel into the AXI router
(`floo_axi_router.sv:100,132`, request and response nets alike). Our VC arbiter spreads a
single ID's packets across a VC pool by round-robin, ID-agnostically
(`nmu/vc_arbiter.hpp:10-13`), and the router arbitrates VCs round-robin per output
(`router/router.hpp:240-250`, pointer bumped at `:275`), so two same-ID same-destination
packets can be reordered.
Restoring the clause would require pinning an ID's packets to one VC, which surrenders the
VC spread that a multi-VC fabric exists to provide.

Pinning the request VC would not be enough. The NSU round-robins the response VC too: B takes a fresh VC from the write pool with no pin at all, and the first beat of every read burst round-robins a new VC that the rest of that burst follows (`nsu/vc_arbiter.hpp:8-11,92-95`; `r_burst_vc_` is burst coherence, not an ID pin). Two same-ID responses can therefore be reordered on the way home even if their requests were not. Clause 2 needs per-ID VC pinning on **both** networks, which is strictly more than the NMU-side mechanism deleted on 2026-06-30. FlooNoC has never shipped clause 2
alongside multiple VCs: its VC router lives in `hw/deprecated/` and picks a VC from the
next-hop direction with free-VC overflow, never from the AXI ID
(`hw/deprecated/vc_router_util/floo_vc_assignment.sv:70-88`, `floo_vc_selection.sv:36-46`).

## 6. Parameters that are not parameters

FlooNoC names five sizes. We name none of them; each is either fused into another constant or
absent.

| FlooNoC | what it sizes | FlooNoC source | here |
|---|---|---|---|
| `BRoBSize` | B RoB depth, flip-flops (`OnlyMetaData = 1` skips the SRAM, `floo_rob.sv:122`) | `floo_pkg.sv:303` | `b_rob_depth_`, ctor param, default 32 (`rob.hpp:47,54,141`) |
| `RRoBSize` | R RoB depth, `tc_sram_impl(.NumWords(RRoBSize), .DataWidth($bits(axi_data_t)))` (`floo_rob.sv:123-126`) | `floo_pkg.sv:307` | `r_rob_depth_`, ctor param, default 32 (`rob.hpp:48,55,142`) |
| `MaxTxnsPerId` | per-ID status FIFO depth | `floo_pkg.sv:299` | `max_txns_per_id_`, ctor param, default 32 (`rob.hpp:48,56,143`), gates admission (`rob.hpp:226,269`) |
| RoB `NumIds` | per-ID status FIFO count | `2**AxiIdWidth`, `floo_rob.sv:53-54` | 256, inherited from `AWID_WIDTH = 8` |
| `MaxTxns` | meta buffer depth on the **subordinate** side (`floo_axi_chimney.sv:811-816`, guarded by `EnSbrPort`) | `floo_pkg.sv:288` | already modelled as NSU `meta_buffer.max_outstanding` = 32 |

All three are now plumbed end to end: `NmuConfig` fields (`nmu.hpp:133-136`) through `Nmu`'s ctor
(`nmu.hpp:289-290`) to `Rob`'s ctor (`rob.hpp:46-48`), from `NmuWrap::init` params
(`nmu_wrap.hpp:51-54,73-75`) with co-sim defaults in `wrap_defaults.hpp` (`kRobBDepth` /
`kRobRDepth` / `kRobMaxTxnsPerId`, lines 30-31,35) and DPI params on `cmodel_nmu_create_ex`
(`cmodel_dpi.cpp:402-411`), to a Makefile knob (`B_ROB_DEPTH` / `R_ROB_DEPTH` /
`MAX_TXNS_PER_ID`, `sim/verilator/Makefile:211-212,219`).

`MaxTxns` belongs to the NSU, not the NMU. `floo_axi_chimney.sv:872-873` asserts that a chimney
without a manager port carries no RoB at all; the RoB and the meta buffer sit on opposite faces
of the NI.

`rob_idx` width and RoB depth are separate parameters in FlooNoC -- `floo_rob_wrapper` takes
`RoBSize` and `rob_idx_t` independently (`floo_rob_wrapper.sv:20,27`), and no RTL assertion ties
them. `floo_simple_rob.sv:25` merely *defaults* `rob_idx_t` to `logic[$clog2(RoBSize)-1:0]`, and
`tb_floo_rob.sv:38` picks `$clog2(BRoBSize)` by hand. The only real constraint is
`2**$bits(rob_idx_t) >= max(BRoBSize, RRoBSize)`.

## 7. Decisions made by C++, not by a designer

Each row is a shape the model asserts and an RTL implementer would have to honour or overrule.

| # | status | as built | what it becomes in RTL | the choice |
|---|---|---|---|---|
| 1 | **CLOSED** | B and R pool depths are independent ctor parameters, `b_rob_depth_` / `r_rob_depth_` (`rob.hpp:47-48`), sharing only the 256-entry `ROB_IDX_SPACE` array bound (`rob.hpp:169-170`) | two independent memories, one of them SRAM, each sized to its own depth | Was fused into one `ROB_CAPACITY` constant; now expressible. B costs ~11 b/entry, R costs 256 b/entry, so sizing them independently lets the cheap one shrink without the expensive one. |
| 2 | **CLOSED** | `alloc_write_` / `alloc_read_` high-water bitsets, `write_free_space()` / `read_free_space()` as leading-zero count (`rob.hpp:106-113,175-176,183-188`) | one `lzc` over an allocation bitmap, O(1) | Ported from FlooNoC `floo_rob` (`floo_rob.sv:155-164`): slots below the high-water mark cannot be reused until it clears. `find_consecutive_free`'s any-consecutive-run scan is deleted. `floo_simple_rob`'s wrapping ring pointer (`floo_simple_rob.sv:126-137`) remains a documented alternative, not chosen. |
| 3 | open | `array<deque<BeatRange>, 256>` (`rob.hpp:197-198`) | 256 parallel FIFOs, or one shared linked-list store | FlooNoC builds `fifo_v3 [NumIds-1:0]` for the RoB status table (`floo_rob.sv:450-465`) and an `id_queue` with a shared `CAPACITY` for the meta buffer (`floo_meta_buffer.sv:146-151`). The two differ by roughly an order of magnitude in area at 256 IDs. |
| 4 | open | `ReadEntry` holds `axi::RBeat` by value (`rob.hpp:163-168`) | 256 b of `rdata` and ~11 b of metadata, in different memories | FlooNoC splits them: SRAM for `rob_wdata`, flip-flops for `rob_meta_q` (`floo_rob.sv:122-153`). |
| 5 | open | `mode_w_` and `mode_r_` exist but the wrap ties them (`nmu_wrap.hpp:71-72`) | `BRoBType` and `RRoBType`, independently selectable | A B-only RoB is FlooNoC's cheap point and we cannot express it end to end. |
| 6 | **CLOSED** | a read burst releases beat by beat via `read_release_offset_` (`rob.hpp:347-364`) | stream it, freeing each slot as its beat leaves | Matches FlooNoC (`floo_rob.sv:250-266,287-297`): AXI4 permits read data of different `ARID`s to interleave (`RID` distinguishes; only write interleaving and `WID` were removed). The earlier whole-burst gate held every slot until the last beat landed; retired. |

## 7a. One comment that was wrong -- CLOSED

`rob.hpp:35-38` used to state the tick order as "drain B/R before forwarding AW/W/AR -> response
frees IDs in same cycle, request can use freed IDs after", true only of standalone
`AxiSlavePort::tick()`. It now states the correct order: the integrated `Nmu::tick()` runs the
request side **first** (`nmu.hpp:297-301`) and drains the response side after
(`nmu.hpp:303-319`), and the comment says so (`rob.hpp:35-38`). No `Rob` behaviour depends on the
order.

## 8. Not modelled

- **Storage class.** No structure distinguishes SRAM from flip-flops, so the model reports no
  area and cannot rank the two pools by cost.
- **Allocator timing.** `highest_set` (`rob.hpp:183-188`) is a linear scan over the allocation
  bitset in C++, modelling FlooNoC's O(1) `lzc` combinational priority encoder
  (`floo_rob.sv:155-164`); the model reports no timing cost for either shape.
- **`RobMode` per direction.** Reachable in `NmuConfig`, unreachable through `NmuWrap`
  (`nmu_wrap.hpp:71-72`).

## 9. References

**Ours.** `src/c_model/include/nmu/rob.hpp`, `nmu/nmu.hpp:126-148`,
`wrap/nmu_wrap.hpp:51-54,71-75`, `wrap/wrap_defaults.hpp:30-31,35`,
`src/dpi/cmodel_dpi.cpp:373-411`, `sim/verilator/Makefile:211-212,219`,
`specgen/generated/json/ni_packet.json:77,82-83`,
`specgen/generated/cpp/ni_flit_constants.h:56,219`, `docs/architecture.md` section 2.

**FlooNoC** (`E:/05_NoC/FlooNoC`, read-only reference). `hw/floo_rob.sv`,
`hw/floo_simple_rob.sv`, `hw/floo_rob_wrapper.sv`, `hw/floo_meta_buffer.sv`,
`hw/floo_axi_chimney.sv`, `hw/floo_axi_router.sv:100,132`, `hw/floo_pkg.sv:279-355`,
`hw/test/floo_test_pkg.sv:44-65`, `hw/tb/tb_floo_rob.sv:30-38`, `docs/floonoc/chimneys.md`.

**AMBA AXI.** Read data for different `ARID`s may interleave on the R channel; write data
interleaving and `WID` were removed in AXI4. Arm, *Learn the architecture -- An introduction to
AMBA AXI*, "Transfer behavior and transaction ordering"
(`developer.arm.com/documentation/102202/0300`). Secondary source: the page renders client-side and
was read through a search summary, not fetched. `floo_rob.sv:250-266,287-297` depends on the rule
and is the corroborating implementation.

**pulp-platform/axi v0.39.7.** `src/axi_id_remap.sv` -- narrows a wide, sparsely used AXI ID
space to a dense one, parameterized by `AxiSlvPortMaxUniqIds` and `AxiMaxTxnsPerId`; a
transaction beyond the unique-ID limit stalls until another ID drains. FlooNoC depends on this
library (`Bender.yml:16`). It is the standard component for giving a RoB a small `NumIds`
without narrowing the NI's external AXI ID width.
