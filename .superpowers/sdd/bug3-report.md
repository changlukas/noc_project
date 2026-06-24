# Bug3 Fix Report: per-VC link credit monitor + configurable LOCAL credit depth

## Bug A — per-VC credit monitor

**Root cause**: `link_perf_monitor.sv` had scalar `credit` (one counter, seeded `BUFFER_DEPTH`).
`gen_tb_top.py` OR-collapsed `[NUM_VC-1:0] credit` → scalar `valid`, and passed no `vc_id`.
At NUM_VC > 1 a hotspot draining VC0 while VC1 is idle causes false underflow on the VC1 budget.

**Fix**: `link_perf_monitor.sv`
- Added `parameter int NUM_VC = 1`.
- Replaced scalar `credit` with `int unsigned credit [NUM_VC]`, each reset to `BUFFER_DEPTH`.
- Added `input logic [$clog2(NUM_VC < 2 ? 2 : NUM_VC)-1:0] vc_id` (flit's VC this cycle).
- Added `input logic [NUM_VC-1:0] credit_pulse` (per-VC slot freed — no OR-collapse).
- Per-VC update each cycle: `credit[v] += credit_pulse[v]; if (valid && vc_id==v) credit[v] -= 1`.
- `flit_count` remains total. `stall_cyc` counts cycles with `!valid && any_vc_zero`.
- Assertion is per-VC: `!(valid && credit[vc_id] == 0)`.

**Fix**: `gen_tb_top.py` (link_perf_monitor instantiation block)
- Added `.NUM_VC(NUM_VC)` to each monitor instance.
- `vc_id` decoded from flit header bits [21:19] (VC_ID_LSB=19, VC_ID_MSB=21 from ni_flit_constants.h): `.vc_id(n{i}_link_{net}_out_flit[RP_{d}][21:19])`.
- `credit_pulse` wired directly to the full `[NUM_VC-1:0]` vector (no `|`): `.credit_pulse(n{i}_link_{net}_out_credit[RP_{d}])`.

## Bug B — LOCAL sender credit depth

**Root cause**: `nmu_wrap.hpp`/`nsu_wrap.hpp` called `enable_noc_credit(kPoCChannelModelDepth)` (= 64).
Router LOCAL input VC FIFO depth is `NOC_ROUTER_VC_DEPTH` (= 4, from `ni_params.h`).
A sender credit of 64 > actual buffer depth 4 breaks the credit flow-control invariant.

**Fix**: `nmu_wrap.hpp` and `nsu_wrap.hpp`
- Added `#include "ni_params.h"` (already present in `router_wrap.hpp`).
- Changed: `enable_noc_credit(kPoCChannelModelDepth)` → `enable_noc_credit(static_cast<std::size_t>(::ni::NOC_ROUTER_VC_DEPTH))`.
- `kPoCChannelModelDepth` (64) retained in `poc_defaults.hpp` for the ChannelModel stub — not deleted.
- Single source: `specgen/source/constants.yaml` → `ni_params.h` (NOC_ROUTER_VC_DEPTH) → both wraps.

## Depth provenance

`constants.yaml` `noc.ROUTER_VC_DEPTH.default = 4` → codegen → `ni_params.h::ni::NOC_ROUTER_VC_DEPTH = 4`
→ `router_wrap.hpp` (`c.vc_depth`), `nmu_wrap.hpp` (`enable_noc_credit`), `nsu_wrap.hpp` (`enable_noc_credit`),
   `link_perf_monitor.sv` (via `ROUTER_VC_DEPTH` localparam in tb_top/fabric).
All four points now share a single source.

## Gate results

| Gate | Result |
|------|--------|
| multi-VC hotspot (mesh_4x4_vc4, hs=0, 4 txn/node) | PASS: scoreboard clean, 64 reads 0 mismatches |
| depth-8 hotspot (ROUTER_VC_DEPTH=8, same run) | PASS: scoreboard clean |
| constants.yaml + generated files restored to default | DONE |
| vc1 sim-regress | 6/6 PASS |
| vc2 sim-regress | 6/6 PASS |
| vc4 sim-regress | 6/6 PASS |
| vc8 sim-regress | 6/6 PASS |
| ctest (make check) | 545/545 PASS |
| gen_tb_top.py --check (mesh_4x4_vc4) | exit 0 |
| axi_master.hpp untouched | CONFIRMED |
| git status (only 4 real files) | CLEAN |

## Changed files

- `sim/sv/link_perf_monitor.sv` — per-VC monitor
- `sim/tools/gen_tb_top.py` — per-VC wiring (vc_id decode + full credit_pulse vector)
- `c_model/include/wrap/nmu_wrap.hpp` — LOCAL credit seeded from NOC_ROUTER_VC_DEPTH
- `c_model/include/wrap/nsu_wrap.hpp` — LOCAL credit seeded from NOC_ROUTER_VC_DEPTH
