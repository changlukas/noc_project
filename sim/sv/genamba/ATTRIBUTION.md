# OSS Attribution — sim/sv/genamba/

AMBA AXI verification IP from the gen_amba project, vendored for the gen_amba
integration feasibility spike (used as golden AXI master BFM + memory model under
Verilator). Files are vendored unmodified from upstream **except** for project
patches to two files (`mem_test_tasks.v` and `axi_master_tasks.v`), documented
below. Patch motivations split three ways: Verilator simulator compatibility
(watcher removal, B-latch reads), a plain wide-bus correctness bug (get_mask
offset width — wrong on any simulator at `WIDTH_DS > 16`), and detection
strength (error_flag escalation).

## Upstream

- Source: https://github.com/adki/gen_amba_2021
- Commit: 4ba7903c569b25439f8d89362662c77b9f20a31e
- License: 2-clause BSD

## Files

| This repo                                | Upstream path                                   | Status     |
|------------------------------------------|--------------------------------------------------|------------|
| `sim/sv/genamba/mem_axi.v`            | `gen_amba_axi/verification/ip/mem_axi.v`         | Unmodified |
| `sim/sv/genamba/mem_axi_beh.v`        | `gen_amba_axi/verification/ip/mem_axi_beh.v`     | Unmodified |
| `sim/sv/genamba/mem_axi_dpram_sync.v` | `gen_amba_axi/verification/ip/mem_axi_dpram_sync.v` | Unmodified |
| `sim/sv/genamba/axi_master_tasks.v`   | `gen_amba_axi/verification/ip/axi_master_tasks.v`| **Modified** (B-channel latch read in `axi_master_write_b`; `error_flag` escalation in `_write_b` + `_read_r`; see "Modifications" below. `_read_r` is otherwise upstream-pristine — burst drains bypass it via `bfm_drain_r` in `genamba_master_bfm.sv`.) |
| `sim/sv/genamba/mem_test_tasks.v`     | `gen_amba_axi/verification/ip/mem_test_tasks.v`  | **Modified** (offset-width fix; see "Modifications" below) |
| `sim/sv/genamba/axi_tester.v`         | `gen_amba_axi/verification/ip/axi_tester.v`      | **Modified** (capture-counter glue block; test sequence untouched — see below) |

## Modifications

### `mem_test_tasks.v` — `get_mask` offset-width fix

Upstream `get_mask` (around line 200) declares:

```verilog
reg [3:0] offset;       // 4-bit; holds 0..15
offset = addr % WIDTH_DS;
```

The `[3:0]` width is sufficient for AXI buses with `WIDTH_DS ≤ 16` (data ≤ 128 bits),
which matches the upstream `axi_tester.v` reference design (`WIDTH_DA=32`, `WIDTH_DS=4`).
For wider buses where `WIDTH_DS > 16`, the modulus result silently truncates: e.g. at
`WIDTH_DA=256` (`WIDTH_DS=32`), `addr=0x10` should give `offset=16`, but the 4-bit
register truncates to `0`. Subsequent `mask << (offset*8)` then applies the wrong
byte-lane mask, producing false `mem_test` mismatches.

Project fix: widen `offset` to explicit `[4:0]` (5-bit, covers `WIDTH_DS` up to 32 =
256-bit bus). Minimum-impact correctness patch; no behavioural change at upstream-
tested widths.

Discovered during T2 of `docs/superpowers/plans/2026-06-08-genamba-role1-testbench.md`.
Worth upstreaming to `gen_amba_2021` as a PR.

### `mem_test_tasks.v` — `error_flag` watcher removal

Upstream defines an `always @(*)` watcher block immediately after the `error_flag`
declaration:

```verilog
reg error_flag=0;
always @ ( * ) begin
   if (error_flag==1) begin
        repeat (50) @ (posedge ACLK);
        $finish(2);
   end
end
```

Under Verilator 5.036, regs (including `error_flag` and the top-level `ARESETn`)
are randomized at construction; the watcher can observe both as 1 before any
initial block runs, falsely firing `$finish(2)` at ~495 ns regardless of whether
a real mismatch occurred. Tried suppressing via `--x-initial 0`, `--x-assign 0`,
runtime `+verilator+rand+reset+0`, and an `if (ARESETn && error_flag==1)` guard
on the watcher — none worked (the randomization affects `ARESETn` too).

Project fix: remove the watcher block. The project's BFM-side wrapper tasks check
`if (error_flag) $fatal(...)` explicitly after each vendored test call, providing
equivalent failure-detection without the race. The `error_flag` register itself
is retained (still used by the explicit checks).

Discovered during T2 (same plan). The race is Verilator-specific; the watcher
works fine under Icarus / ModelSim. Worth flagging upstream as a Verilator
compatibility issue.

### `axi_master_tasks.v` — B-channel snapshot-latch reads

Upstream `axi_master_write_b` reads the B-channel `BID`/`BRESP` signals as raw
input wires immediately after `@(posedge ACLK)` synchronization:

```verilog
while (BVALID==1'b0) @ (posedge ACLK);
BREADY <= #LD 0;
if (BID!=awid) $display(...);    // reads BID procedurally
```

Under Verilator `--timing`, procedural code resumed from `@(posedge ACLK)` reads
post-NBA signal values — i.e. the NEXT cycle's perspective of the registered
output. The NMU bridge implements the AXI4 §A3.2.1 held-latch pattern correctly
(`nmu_shell_adapter.hpp:150-162` holds the response in `held_b_` until BREADY is
seen; the SV-side registers `bvalid_q`/`bid_q` in `nmu_wrap.sv` deassert via NBA
one cycle after the handshake). So in the next cycle BID=0 — which is what the
vendored task ends up reading, even though the handshake-cycle value (correctly
captured by a parallel `always @(posedge ACLK)` monitor) is correct. This is a
Verilator-specific procedural-vs-NBA race; the same code works under Icarus /
ModelSim.

Project fix: introduce a snapshot-latch register in `genamba_master_bfm.sv`
(non-vendored, project code) that captures B-channel signals on every handshake
cycle via a clean `always @(posedge ACLK)` block:

```verilog
reg [WIDTH_ID-1:0] b_id_latch;
reg [1:0]          b_resp_latch;
always @(posedge ACLK) begin
    if (BVALID && BREADY) begin b_id_latch <= BID; b_resp_latch <= BRESP; end
end
```

Then patch the vendored `axi_master_write_b` to (1) wait one additional
`@(posedge ACLK)` so the latch's NBA settles, then (2) read the latches
(`b_id_latch`, `b_resp_latch`) instead of the raw input wires. The latch
registers themselves are project code, so the vendored modification is limited
to the in-task signal name swap plus the extra `@(posedge)` wait.

Discovered during T3 (debug DBG monitors in `tb_genamba.sv` showed BID=0x39 at
the handshake cycle, while the vendored task's procedural read returned 0).

**R-channel handling — not a vendored patch.** The same race affects R-channel
`RID`/`RDATA`/`RLAST` reads, plus a structural mismatch with the per-beat
procedural loop (each iter consumes 2 cycles vs. one R beat/cycle from the
bridge — Task B blen=4 hangs at the second-to-last beat). Rather than further
patching `axi_master_read_r`, the project's `bfm_drain_r` adapter task in
`sim/sv/genamba_master_bfm.sv` bypasses the vendored task entirely: it reads
from a parallel-captured shadow array (`r_shadow[256]`) indexed by a blocking
read-side counter (`r_shadow_ridx`). The vendored `axi_master_read_r` stays in
the Task A call path only (via `mem_test` → `axi_master_read`), modified only
by the `error_flag` escalation below.

### `axi_master_write_b` / `axi_master_read_r` — error_flag escalation

Upstream protocol checks in both tasks (BID/BRESP in `_write_b`; RID/RRESP/
RLAST in `_read_r`) report failures with `$display` only — the simulation
continues and exits 0, so a B/R-channel ID or response error is invisible
unless someone greps the log.

Project fix: each check additionally sets `error_flag = 1` (the flag is
upstream's own fail-fast mechanism, declared in `mem_test_tasks.v`). The
check conditions and messages are unmodified — only the escalation is added.
The project-side traps convert the flag into a `$fatal` + non-zero exit:
`test_baseline_mem_test` (after `mem_test`) and `bfm_drain_b` (after
`axi_master_write_b`) in `genamba_master_bfm.sv`.

Verified by fault injection: a deliberate `bfm_drain_b` with a wrong AWID
makes the BID check fire, sets `error_flag`, and the trap aborts the run
with a non-zero exit code.

### `<= #LD` flattening — edge-aligned BFM drives

Upstream drives every AXI output with `<= #LD` (`LD=1`). Under Verilator
`--timing` this is executed as a 1 ns coroutine suspension per statement
(not an IEEE intra-assignment delay), so consecutive assignments staircase
1..7 ns past the clock edge — waveforms showed every BFM-driven signal
off-edge by its statement ordinal, and chained calls could drift past the
next edge entirely.

Project fix: all 50 `<= #LD` sites in `axi_master_tasks.v` are flattened to
plain `<=` (NBA at the current edge). All BFM-driven signals now transition
exactly on clock edges (verified: every VCD transition lands on a posedge
timestamp).

Two latent races surfaced by the (denser) flattened timing, both fixed with
condition-based waits instead of fixed delays:

- `axi_master_write_b`: the procedural BVALID poll resumes post-NBA (one
  wire-cycle early); a BREADY deassert issued from there lands at the head
  of the next timestep, before any always_ff samples the handshake —
  aborting it. The patched task now detects handshake completion via the
  `b_count` capture counter (`genamba_master_bfm.sv`) and deasserts BREADY
  only afterwards; the stale fixed one-extra-cycle wait is removed.
- Inter-task R-shadow sync: a bare `r_shadow_ridx = r_shadow_widx` barrier
  raced straggler R beats still held by the bridge when the previous task
  returned. `bfm_r_barrier` (project-owned, `genamba_master_bfm.sv`)
  re-asserts RREADY to flush stragglers, waits for 4 quiet edges, then
  syncs the pointer; all six test-task barriers use it.

### Request-side counter handshakes + serialized write (wait_valid era)

When the NMU adapters moved to one-shot wait_valid ready pulses (ready high
for exactly one wire cycle per AW/AR), the vendored `while (xREADY==0)`
polls broke the same way the B poll had: a post-NBA coroutine resume reads
the ready one wire-cycle early, and the VALID deassert issued from that
point lands before the DUT samples the handshake. All three request waits
(`axi_master_write_aw`, per-beat `axi_master_write_w`,
`axi_master_read_ar`) now wait on capture counters
(`aw_count`/`w_count`/`ar_count` in `genamba_master_bfm.sv`, incremented by
the always_ff that samples true wire values) — the same proven pattern as
`b_count`.

`axi_master_write` upstream forks AW/W/B concurrently (AXI-legal). Project
decision: serialized to AW (handshake completes) → W → B, matching the
adapter-layer Tasks B-G and the conservative ordering preferred for
waveform review.

### `axi_tester.v` — capture-counter glue block

The patched task waits in `axi_master_tasks.v` reference handshake capture
counters (`aw_count`/`w_count`/`ar_count`/`b_count`) and B-response latches
(`b_id_latch`/`b_resp_latch`) that the including module must declare.
`axi_tester.v` gets the same declarations + capture `always` block as
`genamba_master_bfm.sv`, inserted right after the task `include`s. The
upstream initial test sequence (SINGLE_TEST / BURST_TEST / *_MEM plusarg
stages) is untouched — `tb_genamba_tester` runs it as-is as the
pure-referee mode (`make run-genamba-tester`).

### Known upstream issue (unpatched)

`axi_master_write_multiple_outstanding` indexes `reg_addr[idx]` from its
concurrent AW loop where `reg_addr[idy]` (the W-loop index) is intended,
which can mis-strobe narrow/unaligned writes. Left as-is: the project BFM
does not call this task; fix it before first use.

## Notes

Only the point-to-point spike IP is vendored (golden master tasks + memory model);
the gen_amba crossbar generator and RTL are not included — they belong to the later
crossbar/role-2 effort. 2-clause BSD permits redistribution with this notice.
