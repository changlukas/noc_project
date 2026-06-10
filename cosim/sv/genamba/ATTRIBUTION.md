# OSS Attribution — cosim/sv/genamba/

AMBA AXI verification IP from the gen_amba project, vendored for the gen_amba
integration feasibility spike (used as golden AXI master BFM + memory model under
Verilator). Files are vendored unmodified from upstream **except** for two patches
to `mem_test_tasks.v` (offset-width fix + watcher removal for Verilator compat),
documented below.

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
| `cosim/sv/genamba/axi_master_tasks.v`   | `gen_amba_axi/verification/ip/axi_master_tasks.v`| Unmodified |
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

## Notes

Only the point-to-point spike IP is vendored (golden master tasks + memory model);
the gen_amba crossbar generator and RTL are not included — they belong to the later
crossbar/role-2 effort. 2-clause BSD permits redistribution with this notice.
