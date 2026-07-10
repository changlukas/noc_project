# NMU RoB — FlooNoC alignment, sizeable pools, bypass clause 1 — design

Baseline: `docs/nmu-rob-microarchitecture.md` (as-built, 2026-07-10). Read it first. This spec changes
exactly what that document lists as an accident of C++.

Every shape `nmu::Rob` asserts should be a decision someone made. Where a decision exists in FlooNoC,
we take theirs unless we can say why not.

## Goal

1. **D1** `rob_idx` widens 5 → 8 bits, so a RoB pool can be sized up to 256 entries.
2. **D7** the first-fit slot scan becomes FlooNoC's leading-zero-count high-water allocator.
3. **D8** a reordered read burst releases one beat at a time instead of waiting for its last beat.
4. **D2** `b_rob_depth` / `r_rob_depth` become independent runtime parameters, settable from co-sim.
5. **D4** `max_txns_per_id` names the per-ID order-list depth, defaulted to FlooNoC's 32.
6. **D9** bypass clause 1: a transaction whose response cannot arrive out of order allocates no slot.

Success: a 256-beat `INCR` read completes through `RobMode::Enabled`; today it wedges the AR channel
permanently. `make test` green after every task. A burst co-sim run passes the scoreboard.

## Motivation

`push_ar` refuses any `len + 1 > ROB_CAPACITY` (`rob.hpp:218`) and has no other path.
`AxiSlavePort::forward_ar_to_packetizer_` (`axi_slave_port.hpp:153-158`) retries the same
`ar_q_.front()` and never pops it, so the oversized AR head-of-line-blocks every later AR, including
short ones on other AXI IDs. `arready` is driven from queue capacity alone (`nmu_wrap.hpp:188-190`),
so it drops once `ar_q_` fills behind the stuck head and never rises. AXI4 permits `len` up to 255
(`axi/protocol_rules.hpp:40`; the pulp VIP agrees, `axi_test.sv:703,782`).

The retired regression matrix recorded the symptom as an exclusion (`AX4-BUR-003`, "rob capacity");
`test_rob.cpp:278-295` still pins the `return false`. Neither names it a defect. No co-sim run has
ever driven a burst: `sim/verilator/Makefile:228-231` hardcodes `--len 0`.

FlooNoC's RoB accepts the same burst, because a transaction that needs no reordering never consults
`rob_free_space` (`floo_rob.sv:336-355`). Bypass is the fix, not a bigger pool.

## What the FlooNoC paper commits to

arXiv 2409.17606v2. Surveyed twice, independently (Codex, and this session's own extraction), 2026-07-10.

| claim | quote | where |
|---|---|---|
| RoB size | "The RoB has a size of **8 kB** implemented as SRAMs." | Fig. 10 caption |
| RoB area | "The NI without a RoB ... **25 kGE**. Compared to the previous RoB-based NI [22], this results in a **256 kGE area reduction, representing a 91% decrease**" | §VI-C |
| why a RoB is expensive | "with a single AXI4 burst capable of reaching up to 4 kB, the RoB must be sized to accommodate **several such bursts**" | §I |
| bypass clause 1 | "**The first response of a stream of transactions with equal TxnID is always in order and does not require allocation**" | §III-A |
| bypass clause 2 | "**Assuming deterministic routing in the network**, the responses of requests to the same destination will arrive in the same order as the requests were issued." | §III-A |
| `robIDx` semantics | "Apart from being a unique identifier, the **robIDx also acts as the index into the RoB**, where it should be stored." | §III-A |
| link widths | narrow 64-bit, wide 512-bit | §III-B |

Two consequences we must own.

**Clause 2 is unavailable to us, by the paper's own wording.** It rests on deterministic routing. Our
VC arbiter spreads one ID's packets across a VC pool by round-robin, ID-agnostically
(`nmu/vc_arbiter.hpp:10-13`); the router arbitrates VCs round-robin per output
(`router/router.hpp:240-250`, pointer bumped at `:275`); and the NSU round-robins the response VC too
(`nsu/vc_arbiter.hpp:8-11,92-95`). Two same-ID same-destination packets can therefore be reordered on
either network. FlooNoC's `NoRoB` NI rests on the same premise and is equally unavailable.

**VC allocation is a separate design round.** Porting `floo_vc_assignment.sv:70-88` means per-hop VC
assignment by next-hop direction, look-ahead routing, per-port VC counts fixed by the turn model
(`{2,4,2,4,4}` for a 5-port XY router, `floo_vc_router.sv:16-22`), `AllowVCOverflow = 0`, and the end
of `vc1/2/4/8` as a configuration axis. It also lives in `hw/deprecated/`. Out of scope here; tracked
in `docs/backlog.md`.

## Decisions

**D1. `rob_idx` widens from 5 to 8 bits.** `ROB_IDX_WIDTH: 5 → 8` in
`specgen/generated/json/ni_packet.json:15`. A spec parameter change, approved 2026-07-10.

The header absorbs it with no wire growth. `rob_idx` occupies `[28:24]`; the field above it is `rsvd`,
a derived padding field (`specgen/ni_spec/constants.py:225-248`) sized `HEADER_TOTAL_WIDTH` minus
everything else. After the change: `rob_idx [31:24]`, `RSVD_WIDTH` `27 → 24`. **No functional header
field moves. `HEADER_WIDTH` stays 56, `FLIT_WIDTH` stays 408, `FLIT_BYTES` stays 51.** The DPI
`static_assert(FLIT_WIDTH == 408)` (`cmodel_dpi.cpp:32`) is untouched.

Why 8 and not 7. The paper's RoB is 8 kB. Our R RoB entry holds one beat of the data bus,
`RDATA_WIDTH` = 256 b = 32 B, so 8 kB is 256 entries, which is exactly `2**8`. It is also the longest
burst AXI4 can express, because `len` is 8 bits. The two coincide for the same reason.

**D1a. `rob_idx` is an address, not a tag.** The paper says so, and both implementations agree: the
request stamps the base index of its allocated range, and the response side reconstructs beat *i* at
`base + i` (`floo_rob.sv:103,276`; ours `rob.hpp:222-223,331-339`). The index therefore advances by
`len + 1` per transaction, but its *width* still bounds the pool: `n` address bits address `2**n`
entries and no more. `tb_floo_rob.sv:38` ties them by hand: `logic [$clog2(BRoBSize)-1:0]`.

`Rob::ROB_CAPACITY` is renamed **`ROB_IDX_SPACE`** (`= 1 << ROB_IDX_WIDTH` = 256), mirroring the
adjacent `AXI_ID_SPACE`. The old name asserted a capacity it no longer sets.

**D1b. Seven `uint8_t` sites overflow once a range can hold 256 beats.** A read range holds
`n = len + 1` beats, up to 256. Today `n <= 32`, so every one of these is unreachable. D1 makes them
reachable; it does not introduce them.

| site | today | after | what happens at `n = 256` |
|---|---|---|---|
| `BeatRange::len_plus_1` (`rob.hpp:142`) | `uint8_t` | `uint16_t` | stores `0`; the first beat trips `arrival_offset < read_range_len_[base]` → `0 < 0` → abort |
| `read_range_len_` (`rob.hpp:153`) | `array<uint8_t, N>` | `array<uint16_t, N>` | same |
| `read_arrival_offset_` (`rob.hpp:152`) | `array<uint8_t, N>` | `array<uint16_t, N>` | wraps to `0` on the last beat |
| loop counter `i` (`rob.hpp:356`) | `uint8_t` | `std::size_t` | `i` never reaches 256 → **infinite loop** |
| loop counter `i` (`rob.hpp:363`) | `uint8_t` | `std::size_t` | same |
| `idx` (`rob.hpp:364`) | `uint8_t` | `std::size_t` | `head.base + i` truncates, writes the wrong slot |
| `arrival_offset` (`rob.hpp:353`) | `uint8_t` | `std::size_t` | **defence in depth, not a demonstrated defect.** It reads a `uint16_t` member back into a `uint8_t` by plain assignment, so no `-Wnarrowing`. Unreachable today: `n = 256` needs the whole pool, so the range is its ID's only entry, and the drain loop resets `read_arrival_offset_[base] = 0` in the same `pop_r_staged` call that receives the last beat. Two independent attempts to write a test that distinguishes the two types failed. Found by the Task-1 reviewer; this spec, two Codex passes and a Fable pass all missed it. |

The two loop counters are the dangerous pair: widening `len_plus_1` alone converts a silent truncation
into a hang. Widen the counters in the same commit.

The lesson from `arrival_offset`: widening a member is not enough. **Grep every local that reads one of
these members**, and prefer `std::size_t` for locals over matching the member's width. That row alone is
defence in depth -- the invariant that masks it (a 256-beat range owns the whole pool, so its offset is
reset synchronously) is exactly the kind of thing a later change to the drain loop would break silently.

`rob_idx` itself stays `uint8_t` everywhere (`AwHeaderMeta::rob_idx`, `ResponseMeta::rob_idx`,
`NmuRsp*Entry::rob_idx`, `CommittedBEntry::rob_idx`, `BeatRange::base`): an 8-bit index addresses
`0..255` exactly.

**D2. Depth is enforced through the allocation bitmap, not through container size.**
`std::array<Entry, ROB_IDX_SPACE>` and `std::bitset<ROB_IDX_SPACE>` keep their compile-time size; the
allocator never hands out an index `>= depth`. No dynamic allocation. The model of "we built a smaller
RoB" is exact.

`b_rob_depth` and `r_rob_depth` are independent, mirroring `BRoBSize` / `RRoBSize`
(`floo_pkg.sv:303,307`). A B entry is `{id, resp, user}` and needs no SRAM (FlooNoC's `OnlyMetaData`
skips it, `floo_rob.sv:122`); an R entry is 32 B of `rdata`. Sizing them together sizes the cheap one
after the expensive one.

**D2a. What `r_rob_depth` physically means.** Not "enough to hold the worst case". Nothing is.

| | entries | R RoB SRAM at 32 B/beat |
|---|---|---|
| today | 32 | 1 KiB |
| one 4 kB burst at `AxSIZE = 5` (128 beats) | 128 | 4 KiB |
| the longest burst AXI4 can express (`len = 255`) | 256 | 8 KiB |
| the paper's silicon | -- | **8 kB** |

FlooNoC's 8 kB cannot hold a 256-beat burst either: 8192 B / 64 B per beat on the wide link is 128
entries. **`r_rob_depth` is a policy: how much SRAM you will spend to keep a burst of a given length
reorderable.** Anything longer must bypass (D9) or stall. That is what clause 1 is for.

Defaults stay 32 / 32, so D1 and D2 together change no behaviour. The sweep picks the real values.

**D3. `max_txns_per_id` is a sizing parameter, not a safety requirement.** An earlier draft claimed
clause 1 destroys the bound on the per-ID order list and therefore forces `max_txns_per_id`. That is
wrong, and the head invariant below disproves it: clause 1 admits a bypassed entry only onto an empty
list, so an ID holds at most one. Every other entry of that ID allocates a slot. Hence

> per-ID list length <= `1 + depth`, and total entries <= `NumIds + depth`.

Still bounded, with no new parameter. FlooNoC needs `MaxTxnsPerId` because **clause 2** lets one ID
bypass repeatedly while the destination holds steady, which does make the list unbounded. We drop
clause 2, so we inherit the boundedness for free.

`max_txns_per_id` is included anyway, for the reason this round exists: the per-ID FIFO depth is a
hardware parameter (`fifo_v3 #(.DEPTH(MaxTxnsPerId))`, `floo_rob.sv:450-465`) and today the C++
`std::deque` names no depth at all. It is also a real policy cap on per-ID outstanding transactions.

**D4. `max_txns_per_id` defaults to 32, FlooNoC's value.** `floo_rob.sv:12` declares
`parameter int unsigned MaxRoTxnsPerId = 32'd32`. It is **not** `AXI_ID_SPACE`: a per-ID cap of 256
means "effectively unbounded", a configuration FlooNoC cannot express, and choosing it to avoid editing
tests would let the tests dictate the design.

Note `MaxTxnsPerId < RoBSize` in FlooNoC (32 vs 64). A single ID hits its per-ID cap before it can
exhaust the pool; the pool is exhausted by several IDs together. A test that wants to exhaust the pool
from one ID must construct the `Rob` with a larger cap explicitly.

The right value for a 256-ID space is `[TBD]` and needs the depth sweep. 32 over 256 IDs is 8192
outstanding, which moves the binding constraint elsewhere (NSU `meta_buffer.max_outstanding` = 32, or
the 16-deep `AxiSlavePort` queues). 32 is FlooNoC's number, not a measured one.

**D5. Bypass adds no backpressure class that `RobMode::Disabled` does not already have.** The rule is
not "reserve for every response" but "guarantee every response can be handled"
(`docs/floonoc/chimneys.md:18`). A bypassed response is by construction the head of its ID's order
list, so it forwards straight to the AXI manager.

A bypassed R burst is absorbed by `Depacketize::r_q_` and `pending_`
(`depacketize.hpp:19-21,50-54,100-107`), `AxiSlavePort::r_q_` (`axi_slave_port.hpp:84-87,133-136`), and
one held beat in the wrap (`nmu_wrap.hpp:206-218`). All are finite. If the manager holds `RREADY` low
they fill and backpressure reaches the response network. **This is not a claim that nothing stalls in
the NoC.** It is a claim that nothing new stalls: `RobMode::Disabled`, the shipped default, already
forwards every B and R through those same finite queues with no reorder storage
(`rob.hpp:251-255,265-271`).

**[UNVERIFIED]** No AMBA source was found obliging a manager to raise `RREADY` within any bound; the
wrap merely holds `RVALID` until it does. A manager that stalls forever deadlocks the fabric in every
mode. Global deadlock freedom under a stalled manager is not proven here and is not altered by this
change.

**D6. `RobMode::Disabled` is untouched.** Clause 1 lives on the Enabled path only.

**D6a. Do not rely on same-cycle response-drain-before-request-admit.** `rob.hpp:33-36` claims the tick
order is "drain B/R before forwarding AW/W/AR". That describes standalone `AxiSlavePort::tick()`. The
integrated `Nmu::tick()` runs the request side first (`nmu.hpp:291-294`) and the response drains after
(`nmu.hpp:296-311`). No decision in this spec depends on the order -- the head invariant is a property
of the list operations, not of when they run -- but the stale comment must not be used to justify one.
Correct it while editing `rob.hpp`.

**D7. The allocator becomes FlooNoC's leading-zero-count high-water mark.** `find_consecutive_free`
(`rob.hpp:163-175`) scans the free bitmap for the first run of `n` free slots. That is neither of
FlooNoC's two allocators, and no design document ever chose it.

FlooNoC keeps one allocation bit per **range**, set at the range's top index, and derives free space
from a single leading-zero count (`floo_rob.sv:145-164,353`):

```systemverilog
assign rob_next_free_idx = RoBSize - rob_free_space;
lzc #(.WIDTH(RoBSize), .MODE(1'b1)) i_lzc (.in_i(rob_alloc_q), .cnt_o(rob_free_space));
...
if (rob_free_space > ax_len_i) begin ... rob_alloc_d[rob_next_free_idx + ax_len_i] = 1'b1; end
```

In C++, over a `std::bitset<ROB_IDX_SPACE>` holding only range tops:

```
free_space = alloc.none() ? depth : depth - 1 - highest_set_index
next_free  = depth - free_space                     // 0 when empty, msb+1 otherwise
admit n    iff free_space >= n                      // FlooNoC writes free_space > len
on admit   alloc.set(next_free + n - 1)
on release alloc.reset(idx)                         // a no-op unless idx is a range top
```

**This is a stack, not a free list.** Space is recovered only from the top. If range A occupies `[0..3]`
and range B occupies `[4..5]`, A completing clears its marker at index 3 but the highest set bit is
still 5, so `free_space` does not grow and slots `0..3` stay unreachable until B completes too.

**Trade-off, stated plainly.** First-fit reuses slots the high-water mark cannot, so this change
*lowers* the effective capacity of a pool of the same depth under out-of-order completion. The
depth-versus-throughput curve will sit to the left of where first-fit would have put it. That is the
price of modelling the allocator that would actually be built, and it is the reason to model it: a
sweep run against an allocator nobody would synthesize measures nothing. `floo_rob` accepts the same
cost because `RoBSize = 64` and bypass keeps occupancy low. **Reordering is exactly the case that hurts
a stack**, so the effect will be visible under `uniform_random`.

The alternative, `floo_simple_rob`'s wrapping ring pointer (`floo_simple_rob.sv:126-137`), is O(1) and
never fragments, but its own header warns it "has a known bug for burst support, and is therefore only
advised to be used for B responses" (`floo_simple_rob.sv:10-12`). Not a candidate for the R RoB.

`find_consecutive_free` and its unit test are deleted. Nothing else calls them.

**D8. A reordered read burst releases one beat at a time.** `pop_r_staged` holds every beat of a burst
until its last beat lands (`rob.hpp:353-362`). FlooNoC emits each ready beat on its own handshake and
clears that beat's allocation bit as it goes (`floo_rob.sv:250-266`).

Note what "frees a slot" does and does not mean under D7. The allocation bitmap holds only range
**tops**, so `alloc.reset(idx)` is a no-op for every beat except the range's last, and even then
`free_space` grows only if that marker was the highest set bit. Per-beat release buys **latency**, not
early space recovery. The two decisions are consistent; the wording "frees each slot as its beat
leaves" is not, and is avoided.

Bypass forces the issue. A bypassed R burst has no slots and streams beat by beat down the
direct-forward path. Leaving the reordered path on whole-burst release would put **two release
disciplines inside one RoB**, an inconsistency this round would itself create. AXI4 asks for neither:
it requires only that beats of one burst stay in order, and permits read data of different `ARID`s to
interleave on R (`RID` distinguishes them; only *write* interleaving and `WID` were removed).

Cost of the old discipline: every beat of a burst waits for the burst's slowest beat. That is the
whole cost, and it is real.

Mechanism: a per-range **release offset**, `read_release_offset_[base]`, beside the existing per-range
`read_arrival_offset_[base]`. FlooNoC keys its equivalent by ID (`read_rob_idx_offset_q[NumIds]`,
`floo_rob.sv:177-180`); we key by range base, as `read_arrival_offset_` already does. It carries the
same information, because a range belongs to exactly one ID.

**D9. Bypass clause 1.** `rob_req = !order_by_id_[id].empty()`. Clause 1 subsumes FlooNoC's sticky
`ax_rob_req_q` (`floo_rob.sv:396`), which exists only to keep clause 2 honest.

**D9a. `rob_idx` is stamped `0` on a bypassed transaction.** FlooNoC stamps the would-be index --
`ax_rob_idx_o = ax_rob_idx_i` is unconditional (`floo_rob.sv:413`), `ax_rob_idx_i = rob_next_free_idx`
(`:103`) -- because the lzc runs every cycle regardless and the mux costs nothing. The receiver never
reads the field when `rob_req == 0` (`floo_rob.sv:292`). We stamp `0` and record in `BeatRange` that
`base` is meaningless unless `rob_req`.

**D9b. Guards FlooNoC does not have.** The direct-forward branch asserts that the head of the ID's list
is the bypassed entry; the constructor asserts `1 <= depth <= ROB_IDX_SPACE`. FlooNoC has neither
(`floo_rob_wrapper.sv:20,27` takes `RoBSize` and `rob_idx_t` as independent parameters with no RTL
assertion). A behaviour model should abort on a misconfiguration, not silently mis-index. Both are in
the style of the aborts already in `pop_b_staged`.

## Mechanism

### Request path

`BeatRange` gains the `rob_req` bit that FlooNoC keeps in its status-FIFO entry, and widens
`len_plus_1`:

```cpp
struct BeatRange {
    uint8_t  base;         // meaningful only when rob_req == 1
    uint16_t len_plus_1;   // up to 256
    bool     rob_req;
};
```

**INPUT** `push_aw(b)` / `push_ar(b)`, Enabled mode.
**COMPUTE**
1. `if (order_by_id_[b.id].size() >= max_txns_per_id_) return false;` — the `ax_gnt_o` gate.
2. `bool needs_rob = !order_by_id_[b.id].empty();` — clause 1.
3. `needs_rob == false` → no allocation, any length. `needs_rob == true` → `free_space >= n`, then
   `alloc.set(next_free + n - 1)`, with `n = 1` for AW and `len + 1` for AR.
**OUTPUT** flit carries `{rob_req, rob_idx}`; `rob_idx = 0` when `rob_req = 0`. Push
`{base, n, needs_rob}` onto the ID's order deque. Nothing mutates before the downstream
`push_*_with_meta` returns true.

The `n > r_rob_depth_` check moves **inside** `needs_rob`, exactly as FlooNoC consults `rob_free_space`
only on the `ax_rob_req_o` path. A bypassed burst of any length is admissible; that is the whole point.

Clause 1 buys the invariant the response path rests on:

> **Invariant.** An ID's order list holds **at most one** `rob_req == 0` entry, and while that entry is
> in flight it is the **head**.
>
> A bypassed entry is pushed only onto an empty list, so it enters at the head. Entries append at the
> tail and pop at the head, so it stays there. Any entry pushed while it is in flight sees a non-empty
> list and is therefore `rob_req == 1`.

So a `rob_req == 0` response forwards the instant it arrives, with no ordering check: nothing is ahead
of it. This is why FlooNoC's direct-forward path (`floo_rob.sv:292-297`) needs no comparison against the
RoB, while its in-order-RoB'd path (`:302`) must compare `rsp_rob_idx_i == st_rsp_rob_idx`.

### Response path

**INPUT** a B or R beat with `{rob_req, rob_idx}` echoed by the NSU.
**COMPUTE**
- `rob_req == 0`: forward directly, touch no slot. Pop the ID's list head on B, or on R `last`. Then
  **re-drain that ID**.
- `rob_req == 1`: mark the slot ready; drain the ID's list from the head.
**OUTPUT** committed queue, then `pop_b` / `pop_r`.

**The re-drain is not optional.** Popping the bypassed head can expose a robbed entry whose slot is
already `ready`. Nothing else would look at it: the next `pop_*_staged` finds the committed queue empty
and waits on a flit that never comes. The response is lost and the slot leaks. Both drain loops become
private helpers, `drain_ready_write_heads_(id)` / `drain_ready_read_heads_(id)`, called from both arms.

The read drain releases per beat (D8):

```
while list non-empty:
    head = front
    if !head.rob_req: break                       // waiting on a bypassed response
    while release_off < head.len_plus_1 and read_entries_[head.base + release_off].ready:
        commit beat head.base + release_off ; ++release_off
    if release_off < head.len_plus_1: break        // burst not drained yet
    reset arrival/release offsets ; pop_front()
```

### The slot-release path must learn that a beat may own no slot

A bypassed beat carries `rob_idx = 0` and owns nothing. It must never reach `commit_b_exit` /
`commit_r_exit`, whose first act is `assert(committed_b_pending_[rob_idx] > 0)` (`rob.hpp:377-395`).
Under `NDEBUG` the assert is gone and the call decrements a counter it never incremented, then frees
slot `0` while a live transaction holds it. Silent corruption.

| site | today | after |
|---|---|---|
| `Rob::CommittedBEntry` / `CommittedREntry` (`rob.hpp:64-73`) | `{beat, rob_idx, axi_id}` | add `bool rob_req` |
| `Nmu::advance_rsp_s2_b_` / `_r_` (`nmu.hpp:387,394`) | hardcodes `true` into the staged entry's last field | passes `b->rob_req` |
| `Nmu::push_rsp_b_to_axi_` / `_r_` (`nmu.hpp:317,323`) | `if (entry.rob_enabled) rob_.commit_b_exit(...)` | unchanged in shape; the field now means what its guard needs |

`NmuRspBEntry::rob_enabled` is misnamed: it never meant "`RobMode` is Enabled", it meant "this entry
owns a RoB slot". Rename it **`rob_req`**, matching the wire field.

**Correction (found during Task 7 execution, 2026-07-11).** Renaming the field is not enough. Task 5
renamed `NmuRspBEntry::rob_enabled` -> `rob_req` and added the `if (entry.rob_req)` commit guard, but
`Nmu::advance_rsp_s2_b_` / `_r_` still built the staged entry with a hardcoded `true`
(`s2_rsp_b_.accept({..., true})`), discarding `CommittedBEntry::rob_req`. Latent while `rob_req` was
true everywhere; the moment clause 1 produced the first `rob_req == 0`, a bypassed B reached
`commit_b_exit` and aborted on `assert(committed_b_pending_[rob_idx] > 0)` -- the exact slot-0
corruption three reviewers named as the top risk. Six review passes missed it because each checked
that the construction sites *set a value*, not that the value flowed from producer to consumer. The
producer must forward `b->rob_req` / `r->rob_req`.

### Parameters

The three live in `NmuConfig`, **not** in `port_params.yaml`. `load_nmu_port_params`
(`nmu/port_params.hpp`) has only test callers and none constructs a `Rob`, so a YAML block would have
zero consumers. Defaults sit in `wrap/wrap_defaults.hpp` beside `kMetaBufferMaxOutstanding`; the co-sim
overrides them through the DPI, which is how the depth sweep will drive them.

| layer | NSU today | NMU after |
|---|---|---|
| config struct | `NsuConfig::port_params.meta_buffer_max_outstanding` | `NmuConfig::{b_rob_depth,r_rob_depth,max_txns_per_id}` |
| default | `kMetaBufferMaxOutstanding` | `kRobBDepth = 32`, `kRobRDepth = 32`, `kRobMaxTxnsPerId = 32` |
| wrap ctor | `NsuWrap::init(..., max_outstanding)` | `NmuWrap::init(..., b_rob_depth, r_rob_depth, max_txns_per_id)` |
| DPI | `cmodel_nsu_create(name, src_id, num_vc, max_unique_ids, max_outstanding)` | `cmodel_nmu_create_ex(name, src_id, num_vc, rob_enabled, b_rob_depth, r_rob_depth, max_txns_per_id, config_path)` |
| plusarg | `+max_outstanding=` | `+b_rob_depth=` `+r_rob_depth=` `+max_txns_per_id=` |
| Makefile | `MAX_OUTSTANDING` | `B_ROB_DEPTH` `R_ROB_DEPTH` `MAX_TXNS_PER_ID` |

Constructor asserts: `1 <= depth <= ROB_IDX_SPACE`, `max_txns_per_id >= 1`. Fail-loud.

There is deliberately **no** assert relating `r_rob_depth` to burst length. A burst longer than
`r_rob_depth` is admissible via clause 1 and stalls behind a same-ID predecessor otherwise, which is
correct. FlooNoC has no such assert either.

## Blast radius

| file | change |
|---|---|
| `specgen/generated/json/ni_packet.json` | `ROB_IDX_WIDTH: 5 → 8` |
| `specgen/generated/{cpp,sv}/…`, `noc_types_pkg_*` | regenerate: `ROB_IDX_MSB` 28→31, `RSVD_LSB` 29→32, `RSVD_WIDTH` 27→24 |
| `specgen/tests/test_codegen.py:144` | comment references `rob_idx=7` minimal header |
| `nmu/rob.hpp` | `ROB_IDX_SPACE` rename; `uint16_t` widening; lzc allocator replaces `find_consecutive_free`; per-beat release + `read_release_offset_`; three ctor params + asserts; `BeatRange::rob_req`; clause-1 and `max_txns_per_id` gates; direct-forward branches; `drain_ready_*_heads_` helpers |
| `nmu/nmu.hpp` | `NmuConfig` fields; `NmuRsp*Entry::rob_enabled` → `rob_req`, no longer hardcoded `true` (`:387,394`); the `commit_*_exit` guards at `:317,323` gate on it |
| `wrap/nmu_wrap.hpp`, `wrap/wrap_defaults.hpp` | overrides + defaults |
| `dpi/cmodel_dpi.{h,cpp}` | `cmodel_nmu_create_ex` signature |
| `sim/tools/gen_tb_top.py` | plusargs + create call |
| `sim/verilator/Makefile`, root `Makefile` | `B_ROB_DEPTH` `R_ROB_DEPTH` `MAX_TXNS_PER_ID` `BURST_LEN` knobs; `--len $(BURST_LEN)` replaces the hardcoded `--len 0` |
| `c_model/tests/nmu/test_rob.cpp` | the whole behavioural contract |

`RobMode::Disabled` paths, `Packetize`, `Depacketize`, NSU, router: untouched. `FLIT_WIDTH` and the DPI
pack/unpack: untouched.

## Testing (TDD)

Fault injection first: before trusting the bypass, break clause 1 deliberately (force `rob_req = 1`
always) and confirm the 256-beat read test goes back to failing. A check that cannot fail proves
nothing.

| test | asserts |
|---|---|
| `Enabled_LzcAllocator_IsAStack` | A `[0..3]`, B `[4..5]`; release A fully; a 4-beat AR is refused although four slots are notionally free. Pins D7's cost so nobody "fixes" it later. |
| `Enabled_LzcAllocator_NonTopReleaseDoesNotGrowFreeSpace` | clearing A's top at index 3 while B's top at 5 is still set leaves `free_space` unchanged. The negative half of the row above. |
| `Enabled_LzcAllocator_ReusesFromTheTop` | after B also releases, the next base is `0`. |
| `Enabled_PerBeatRelease_HeadBurstStreams` | a 4-beat robbed burst releases beat 0 as soon as beat 0 is ready, before beats 1-3 arrive. Fails under whole-burst release. |
| `Enabled_PerBeatRelease_ClearsMarkerOnRangeTop` | only `commit_r_exit` on the range's **top** beat clears its allocation marker; earlier beats clear nothing. Whether `free_space` then grows depends on the high-water mark, which the row above pins separately. |
| `Enabled_PushAr_OversizedBurst_AdmittedViaBypass` | **inverts `test_rob.cpp:278-295`**. `len = 255` on an idle ID returns `true`, carries `rob_req = 0`, allocates zero slots. |
| `Enabled_PushAr_OversizedBurst_SecondSameIdRefusedNotWedged` | a second `len = 255` on the same ID returns `false` while the first is in flight, and `true` once it drains. Transient, not permanent. |
| `Enabled_MaxBurst_LenPlus1DoesNotOverflow` | `r_rob_depth = 256`, primed ID, `len = 255`: the robbed burst allocates 256 slots and every beat lands. Fails on `uint8_t` (D1b). |
| `Enabled_Clause1_FirstTxnPerIdAllocatesNoSlot` | free-space unchanged across the first `push_aw` of an ID; decremented on the second. |
| `Enabled_MaxTxnsPerId_Gate` | the `(max_txns_per_id + 1)`-th same-ID request returns `false` with free slots available. Distinguishes the new gate from pool exhaustion. |
| `Enabled_MaxTxnsPerId_1_MatchesDisabled` | `Enabled` with `max_txns_per_id = 1` emits the same flits, in the same order, with the same `rob_req = 0`, as `Disabled` on the same stimulus, and allocates zero slots. See D3. |
| `Enabled_MixedList_OrderPreserved` | ID with `[bypass, robbed]`; the robbed response arrives first, is buffered, and is released only after the bypassed one. Catches a missing re-drain. |
| `Enabled_BypassedBeat_ReleasesNoSlot` | a bypassed B and a bypassed R must not touch `committed_*_pending_` or the allocation bitmap. |
| `Enabled_Depth_Parameterized` | fixture over `depth in {1,2,4,8,16,32,64,128,256}`: allocation never returns an index `>= depth`; the next request fails; response order is depth-invariant. |
| `Enabled_DepthAssert` | death test on `depth = 0` and `depth = ROB_IDX_SPACE + 1`. |

Every `Enabled_*` test encodes the pre-bypass invariant that the first Enabled transaction owns
slot 0 with `rob_req = 1`. That includes the **new** allocator and per-beat-release tests above: after
clause 1, a fresh ID's first AR bypasses and allocates nothing, so `Enabled_LzcAllocator_IsAStack`
cannot start with range A at `[0..3]`.

All of them are migrated in the bypass task, not before. Priming an ID costs a slot *before* the bypass
exists and costs nothing *after*, so no single version of a test is green on both sides. The migration
inserts one in-flight bypassed transaction per ID under test; because it allocates nothing after clause
1 lands, every `rob_idx` expectation survives unchanged.

Co-sim: `make sim TB=tb_mesh_4x4_vc1_rob PATTERN=neighbor BURST_LEN=63` must pass the scoreboard. 64
beats exceeds a 32-slot pool and stays inside the 4 kB boundary at `--size 5`. Before the bypass lands
this run must hang; that hang is the co-sim-level fault injection. Then the four directed patterns at
`BURST_LEN=0`, unchanged.

## Out of scope (YAGNI)

- **Bypass clause 2**, and the `NoRoB` NI. Both rest on deterministic routing, which our VC arbiter
  breaks. The paper says so in §III-A.
- **VC allocation.** `floo_vc_assignment.sv:70-88` is a per-hop, turn-model scheme with per-port VC
  counts and look-ahead routing, in `hw/deprecated/`. Its own design round.
- `RobMode` per direction through the wrap (`nmu_wrap.hpp:69-70` ties them). FlooNoC's `BRoBType` /
  `RRoBType` are independent; a B-only RoB is its cheap point.
- per-ID order table structure, `NumIds`, `id_queue`, `axi_id_remap`, `AWID_WIDTH`.
- Any area model inside the c_model. Area is a function of the parameters, computed outside.
- Asymmetric depth defaults. `b_rob_depth = 32`, `r_rob_depth = 256` would match the paper's 8 kB, but
  the sweep should choose, not this spec.

All are recorded in `docs/backlog.md`.
