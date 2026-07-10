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
| `rob_idx` | `ROB_IDX_WIDTH` = 5 | `ni_packet.json:82-83` (`width_param`) | NMU `Packetize` | same |

The NSU stores both in its meta buffer and replays them onto the B/R response
(`nsu/meta_buffer.hpp`, `nsu/packetize.hpp:92-93,107-108`). The NoC never reads them.

Mode is per direction: `mode_w_`, `mode_r_` (`rob.hpp:107`), from `NmuConfig::write_rob_mode` /
`read_rob_mode` (`nmu.hpp:129-130`). FlooNoC carries the same split as `BRoBType` / `RRoBType`
(`floo_pkg.sv:301,305`). **The wrap layer collapses it**: `nmu_wrap.hpp:69-70` assigns one
`rob_mode` to both, and the DPI entry point (`cmodel_dpi.cpp:400-406`) exposes a single
`rob_enabled` int. A B-only RoB -- FlooNoC's cheap configuration, since B carries no payload --
is unreachable from the co-sim.

## 2. `RobMode::Disabled` -- as built

The shipped default (`cmodel_nmu_create`, `cmodel_dpi.cpp:395-398`).

| structure | type | size | HW counterpart |
|---|---|---|---|
| `write_outstanding_` | `array<bool, 256>` | 256 b | 256 FF |
| `read_outstanding_` | `array<bool, 256>` | 256 b | 256 FF |
| `w_burst_credit_` | `uint32_t` | -- | counter |

**INPUT** `push_aw` refuses while `write_outstanding_[id]` is set (`rob.hpp:198`).
**COMPUTE** On a successful downstream push the flag is set and the flit leaves with
`rob_req = 0` (`rob.hpp:199,202`). `push_ar` mirrors it (`rob.hpp:236-240`).
**OUTPUT** `pop_b` clears the write flag; `pop_r` clears the read flag on `last`
(`rob.hpp:253-254,267-270`).

One transaction in flight per AXI ID, so no response can overtake another of the same ID and
no reorder storage is needed. Ordering is a property of the interlock, not of the network.

`w_burst_credit_` gates W beats behind their AW (`rob.hpp:208`). A single counter suffices
because AXI4 W beats follow AW issue order strictly -- there is no `WID`.

**Gap against FlooNoC `NoRoB`.** FlooNoC admits multiple outstanding transactions per ID as
long as the destination is unchanged, bounded by a per-ID counter
(`floo_rob_wrapper.sv:139`: `push = ax_valid_i && (!in_flight || ax_dest_i == prev_dest) &&
!counter_full`). Ours stalls even when the destination is unchanged. Strictly more
conservative, strictly less throughput, and the difference has never been measured.

## 3. `RobMode::Enabled` -- as built

One constant, `ROB_CAPACITY = 1u << ni::header::ROB_IDX_WIDTH` = 32 (`rob.hpp:80`), sizes
eight of the twelve structures below. The other four are sized by the AXI ID space, 256.

| structure | type | per-entry content | HW counterpart |
|---|---|---|---|
| `write_entries_` | `array<WriteEntry, 32>` | `{occupied, ready, axi_id, BBeat}` | B RoB. Metadata only; no payload exists on B. |
| `read_entries_` | `array<ReadEntry, 32>` | `{occupied, ready, axi_id, RBeat}` | R RoB. `RBeat` carries `RDATA_WIDTH` = 256 b of data. |
| `free_write_entries_` | `bitset<32>` | free flag | allocation bitmap |
| `free_read_entries_` | `bitset<32>` | free flag | allocation bitmap |
| `write_order_by_id_` | `array<deque<BeatRange>, 256>` | `{base, len_plus_1}` | per-ID program-order list |
| `read_order_by_id_` | `array<deque<BeatRange>, 256>` | same | same |
| `read_arrival_offset_` | `array<uint8_t, 32>` | beat counter, keyed by burst base | per-burst arrival counter |
| `read_range_len_` | `array<uint8_t, 32>` | burst length, keyed by base | bound for the above |
| `committed_b_queue_` | `deque<CommittedBEntry>` | released beat | output FIFO |
| `committed_r_queue_` | `deque<CommittedREntry>` | released beat | output FIFO |
| `committed_b_pending_` | `array<uint8_t, 32>` | release refcount | per-slot counter |
| `committed_r_pending_` | `array<uint8_t, 32>` | release refcount | per-slot counter |

### Allocation

**INPUT** `push_aw(b)` / `push_ar(b)`.
**COMPUTE** `push_aw` takes exactly one slot: `find_consecutive_free(free_write_entries_, 1)`
(`rob.hpp:184`). `push_ar` takes `len + 1` **linearly consecutive** slots
(`rob.hpp:216-219`), refusing outright when `len + 1 > 32` (`rob.hpp:218`) or when no
consecutive run exists.
**OUTPUT** The run's base index becomes `rob_idx` on the outbound flit, with `rob_req = 1`
(`rob.hpp:187-188,222-223`). The `{base, len+1}` pair is appended to that ID's order deque.

A write burst returns one B regardless of length, so the write pool admits 32 outstanding
writes. A read burst consumes one slot per beat, because each slot *is* the storage for one
beat of `rdata`. So the same constant bounds writes by transaction and reads by beat.

### Release

**B** `pop_b_staged` marks slot `rob_idx` ready, then drains that ID's order deque from the
head while the head slot is ready (`rob.hpp:300-306`). Out-of-order arrivals wait.

**R** `pop_r_staged` reads `rob_idx` as the **burst base**, not the beat index. The NSU stamps
every beat of a burst with the same base, so beat *i* lands at
`base + read_arrival_offset_[base]` (`rob.hpp:331-339`). A burst is released only when **all** its
beats are ready (`rob.hpp:353-362`).

That whole-burst gate is stricter than AXI4 and stricter than FlooNoC. AXI4 permits read data
for different `ARID`s to interleave on the R channel -- `RID` distinguishes them; only *write*
data interleaving was removed, along with `WID`. Beats of one burst must stay in order, nothing
more. FlooNoC releases one beat at a time and frees each slot as its beat leaves
(`floo_rob.sv:250-266`), and when the next beat has not arrived it drops back to `RoBWrite` and
forwards a different ID's response directly (`floo_rob.sv:287-297`). Our RoB holds every slot of
a burst until its last beat lands. This costs latency and blocks slot recycling mid-burst; it
buys nothing AXI4 asks for.

`commit_b_exit` / `commit_r_exit` free a slot when its release refcount reaches zero
(`rob.hpp:377-395`).

Mixing is forbidden: an Enabled-mode RoB that receives a `rob_req = 0` response aborts
(`rob.hpp:284-287,323-326`).

## 4. Invariants that hold today and are written nowhere

1. **Every admitted transaction consumes at least one slot.** There is no bypass path.
2. Therefore the total length of all order deques, summed over all 256 IDs, is at most 32.
   `std::deque` is unbounded; the slot pool bounds it.
3. Therefore `committed_b_queue_.size()` and `committed_r_queue_.size()` are each at most 32.
4. Therefore **the slot pool is the outstanding limit.** B: 32 transactions. R: 32 beats.

Admission in Enabled mode is blocked only by slot-allocation failure (`rob.hpp:182,219`). The
`s1_aw_.full()` / `s1_ar_.full()` checks upstream are ordinary queue backpressure, not a
transaction-count limit.

### A read burst longer than the pool wedges the port

`push_ar` returns `false` for any `len + 1 > ROB_CAPACITY` (`rob.hpp:218`) and there is no other
path. AXI4 permits `len` up to 255. A 256-beat `INCR` read therefore never leaves the AXI slave
port in `RobMode::Enabled`: not slowly, never. `test_rob.cpp:278-293` pins the `return false`; the
regression matrix records the consequence as an exclusion (`AX4-BUR-003`, `len 256`, "rob
capacity"). Neither names it as a limitation.

FlooNoC does not have it, and bypass is the reason. `floo_rob.sv:336-355` checks
`rob_free_space > ax_len_i` **only on the `ax_rob_req_o` path**; a transaction that needs no
reordering is admitted at any length. So the first read of an ID, however long, passes a 64-entry
RoB untouched.

Bypass clause 1 is therefore not only an area optimization. It is what makes long bursts
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
unconditionally; `rob_req` is merely a bit inside the entry (`floo_rob.sv:443-446`). Invariant 2
above -- the slot pool bounds the order lists -- must therefore be re-derived once a bypass exists.

Under clause 1 alone it survives. A bypassed entry is admitted only onto an empty list, so an ID
holds at most one of them, and every other entry of that ID owns a slot. Per-ID list length is at
most `1 + depth`; the total at most `NumIds + depth`. Under clause 2 it does not survive: one ID may
bypass repeatedly while its destination holds steady, and the list grows without bound. That is why
FlooNoC carries `MaxTxnsPerId`, enforced as `ax_gnt_o = !fifo_full[ax_id_i]` (`floo_rob.sv:414`).

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
| `BRoBSize` | B RoB depth, flip-flops (`OnlyMetaData = 1` skips the SRAM, `floo_rob.sv:122`) | `floo_pkg.sv:303` | `1 << ROB_IDX_WIDTH` |
| `RRoBSize` | R RoB depth, `tc_sram_impl(.NumWords(RRoBSize), .DataWidth($bits(axi_data_t)))` (`floo_rob.sv:123-126`) | `floo_pkg.sv:307` | the same constant |
| `MaxTxnsPerId` | per-ID status FIFO depth | `floo_pkg.sv:299` | absent; implied by invariant 2 |
| RoB `NumIds` | per-ID status FIFO count | `2**AxiIdWidth`, `floo_rob.sv:53-54` | 256, inherited from `AWID_WIDTH = 8` |
| `MaxTxns` | meta buffer depth on the **subordinate** side (`floo_axi_chimney.sv:811-816`, guarded by `EnSbrPort`) | `floo_pkg.sv:288` | already modelled as NSU `meta_buffer.max_outstanding` = 32 |

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

| # | as built | what it becomes in RTL | the choice nobody made |
|---|---|---|---|
| 1 | B and R pools share `ROB_CAPACITY` | two independent memories, one of them SRAM | B costs ~11 b/entry, R costs 256 b/entry. Sizing them together sizes the cheap one after the expensive one. |
| 2 | `find_consecutive_free` (`rob.hpp:163-175`), first-fit linear scan | a 32-bit priority-encoder chain, evaluated every cycle | Three different allocators with three different reuse rules. Ours: any consecutive run, found by scan, so eight scattered free slots refuse an 8-beat burst. FlooNoC `floo_rob`: one `lzc` over an allocation bitmap (`floo_rob.sv:155-164`), O(1), but slots below the high-water mark cannot be reused until it clears. `floo_simple_rob`: a wrapping ring pointer (`floo_simple_rob.sv:126-137`), O(1), no fragmentation, bursts may straddle the wrap. Nobody chose ours. |
| 3 | `array<deque<BeatRange>, 256>` | 256 parallel FIFOs, or one shared linked-list store | FlooNoC builds `fifo_v3 [NumIds-1:0]` for the RoB status table (`floo_rob.sv:450-465`) and an `id_queue` with a shared `CAPACITY` for the meta buffer (`floo_meta_buffer.sv:146-151`). The two differ by roughly an order of magnitude in area at 256 IDs. |
| 4 | `ReadEntry` holds `axi::RBeat` by value | 256 b of `rdata` and ~11 b of metadata, in different memories | FlooNoC splits them: SRAM for `rob_wdata`, flip-flops for `rob_meta_q` (`floo_rob.sv:122-153`). |
| 5 | `mode_w_` and `mode_r_` exist but the wrap ties them | `BRoBType` and `RRoBType`, independently selectable | A B-only RoB is FlooNoC's cheap point and we cannot express it end to end. |
| 6 | a read burst releases only when every beat is ready (`rob.hpp:353-362`) | hold the whole burst, or stream it | AXI4 permits read data of different `ARID`s to interleave (`RID` distinguishes; only write interleaving and `WID` were removed). FlooNoC streams beat by beat and frees each slot as it leaves (`floo_rob.sv:250-266`), interleaving another ID's response when the next beat is late (`floo_rob.sv:287-297`). Ours neither streams nor recycles. |

## 7a. One comment that is wrong

`rob.hpp:33-36` states the tick order as "drain B/R before forwarding AW/W/AR -> response frees IDs in
same cycle, request can use freed IDs after". That holds for standalone `AxiSlavePort::tick()`. The
integrated `Nmu::tick()` runs the request side **first** (`nmu.hpp:291-294`) and drains the response
side after (`nmu.hpp:296-311`). No `Rob` behaviour depends on the order, but the comment has been
quoted as if it did.

## 8. Not modelled

- **Storage class.** No structure distinguishes SRAM from flip-flops, so the model reports no
  area and cannot rank the two pools by cost.
- **Bounded per-ID order lists.** `std::deque` grows on demand. Nothing in the model would
  notice a `MaxTxnsPerId` violation, because the bound does not exist.
- **Allocator timing.** `find_consecutive_free` is O(32) per call and free of cost in C++.
- **`RobMode` per direction.** Reachable in `NmuConfig`, unreachable through `NmuWrap`.
- **Bypass.** `rob_req` is always `1` in Enabled mode and always `0` in Disabled mode. The wire
  field is present and the NSU replays it faithfully; nothing ever sets it per transaction.

## 9. References

**Ours.** `src/c_model/include/nmu/rob.hpp`, `nmu/nmu.hpp:126-138`, `wrap/nmu_wrap.hpp:66-71`,
`src/dpi/cmodel_dpi.cpp:373-406`, `specgen/generated/json/ni_packet.json:77,82-83`,
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
