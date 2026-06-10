# OSS Attribution — cosim/sv/genamba/

AMBA AXI verification IP from the gen_amba project, vendored for the gen_amba
integration feasibility spike (used as golden AXI master BFM + memory model under
Verilator). Files are vendored unmodified from upstream **except** for project
patches to two files (`mem_test_tasks.v` and `axi_master_tasks.v`) — all related
to Verilator `--timing` simulator compatibility, documented below.

## Upstream

- Source: https://github.com/adki/gen_amba_2021
- Commit: 4ba7903c569b25439f8d89362662c77b9f20a31e
- License: 2-clause BSD

## Files

| This repo                                | Upstream path                                   | Status     |
|------------------------------------------|--------------------------------------------------|------------|
| `cosim/sv/genamba/mem_axi.v`            | `gen_amba_axi/verification/ip/mem_axi.v`         | Unmodified |
| `cosim/sv/genamba/mem_axi_beh.v`        | `gen_amba_axi/verification/ip/mem_axi_beh.v`     | Unmodified |
| `cosim/sv/genamba/mem_axi_dpram_sync.v` | `gen_amba_axi/verification/ip/mem_axi_dpram_sync.v` | Unmodified |
| `cosim/sv/genamba/axi_master_tasks.v`   | `gen_amba_axi/verification/ip/axi_master_tasks.v`| **Modified** (B/R latch reads; see "Modifications" below) |
| `cosim/sv/genamba/mem_test_tasks.v`     | `gen_amba_axi/verification/ip/mem_test_tasks.v`  | **Modified** (offset-width fix; see "Modifications" below) |
| `cosim/sv/genamba/axi_tester.v`         | `gen_amba_axi/verification/ip/axi_tester.v`      | Unmodified (template/reference for the BFM signal environment) |

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

### `axi_master_tasks.v` — B/R channel snapshot-latch reads

Upstream `axi_master_write_b` and `axi_master_read_r` read the B-channel
`BID`/`BRESP` and R-channel `RID`/`RRESP`/`RLAST` signals as raw input wires
immediately after `@(posedge ACLK)` synchronization:

```verilog
while (BVALID==1'b0) @ (posedge ACLK);
BREADY <= #LD 0;
if (BID!=awid) $display(...);    // reads BID procedurally
```

Under Verilator `--timing`, procedural code resumed from `@(posedge ACLK)` reads
post-NBA signal values — i.e. the NEXT cycle's perspective of the registered
output. The NMU bridge implements the AXI4 §A3.2.1 held-latch pattern correctly
(`nmu_shell_adapter.hpp:150-162`): `bvalid_q`/`bid_q` are held until BREADY is
seen, then deasserted via NBA. So in the next cycle BID=0 — which is what the
vendored task ends up reading, even though the handshake-cycle value (correctly
captured by a parallel `always @(posedge ACLK)` monitor) is correct. This is a
Verilator-specific procedural-vs-NBA race; the same code works under Icarus /
ModelSim.

Project fix: introduce snapshot-latch registers in `genamba_master_bfm.sv`
(non-vendored, project code) that capture B/R-channel signals on every
handshake cycle via a clean `always @(posedge ACLK)` block:

```verilog
reg [WIDTH_ID-1:0] b_id_latch;
reg [1:0]          b_resp_latch;
reg [WIDTH_ID-1:0] r_id_latch;
reg [1:0]          r_resp_latch;
reg                r_last_latch;
always @(posedge ACLK) begin
    if (BVALID && BREADY) begin b_id_latch <= BID; b_resp_latch <= BRESP; end
    if (RVALID && RREADY) begin r_id_latch <= RID; r_resp_latch <= RRESP; r_last_latch <= RLAST; end
end
```

Then patch the vendored `axi_master_write_b` / `axi_master_read_r` to (1) wait
one additional `@(posedge ACLK)` so the latches' NBA settles, then (2) read the
latches (`b_id_latch`, `b_resp_latch`, `r_id_latch`, `r_resp_latch`, `r_last_latch`)
instead of the raw input wires. The latch registers themselves are project code
(in `genamba_master_bfm.sv`), so the vendored modification is limited to the
in-task signal name swap plus the extra `@(posedge)` wait.

Discovered during T3 (debug DBG monitors in `tb_genamba.sv` showed BID=0x39 at
the handshake cycle, while the vendored task's procedural read returned 0). The
issue is purely a Verilator simulator quirk; the bridge transports BID/RID/RLAST
correctly end-to-end (verified at every adapter / DPI / SV signal boundary by
Codex static trace + the SV DBG monitors).

### `axi_master_read_r` — shadow-array data capture

The B-channel latch fix above is sufficient for `axi_master_write_b` (which
checks BID/BRESP only once per write transaction). But for multi-beat read
bursts, the per-beat procedural `dataR[idx] = RDATA` read inside the vendored
loop is structurally broken under Verilator `--timing`:

```verilog
for (idx=0; idx<bleng; idx=idx+1) begin
    @ (posedge ACLK);
    while (RVALID==1'b0) @ (posedge ACLK);
    dataR[idx] = RDATA;          // post-NBA read — reads NEXT cycle
    @ (posedge ACLK);            // (previous patch's extra wait for latches)
    if (r_id_latch != arid) ...
end
```

Each iteration consumes 2 clock cycles (the `@(posedge)` at loop top + the
extra `@(posedge)` added by the prior latch patch). NSU/NMU drive R beats at
one beat/cycle with RREADY held high, so for `blen=4` the second-to-last
beat is the last RVALID-high cycle. By iter 2 the bus has gone idle —
`while (RVALID==1'b0)` loops forever. Task B blen=4 hangs after the first R
burst's RLAST=1; Task A blen=1 escapes because there's only one iteration.

Project fix: capture every RVALID-RREADY handshake's RDATA in a parallel
NBA-counter-indexed shadow array (`r_shadow[]` in `genamba_master_bfm.sv`),
then replace the vendored loop with a simple "wait for `bleng` new entries,
copy to dataR" task body. Per-beat ID checks reduce to per-burst checks
against `r_id_latch` / `r_last_latch` / `r_resp_latch` — sufficient for our
tests (same-ID bursts have identical RID per beat anyway; per-beat RLAST is
implicitly verified by RLAST=1 only on the final beat).

Discovered during T5 Task B blen=4 silent hang. The same race would manifest
in `axi_master_write_w` (W-beat loop) if our flow used RREADY-style W push;
the adapter task `bfm_post_w` sidesteps it by issuing exactly `blen` W beats
sequentially without waiting for WREADY drops between them.

## Notes

Only the point-to-point spike IP is vendored (golden master tasks + memory model);
the gen_amba crossbar generator and RTL are not included — they belong to the later
crossbar/role-2 effort. 2-clause BSD permits redistribution with this notice.
