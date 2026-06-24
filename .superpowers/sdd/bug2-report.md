# bug2-report: link credit depth consistency fix

## Credit architecture trace

| Link type         | Receiving buffer         | Governing param       | Credit returned by                      |
|-------------------|--------------------------|-----------------------|-----------------------------------------|
| inter-router      | router input VC FIFO     | `ROUTER_VC_DEPTH`     | router.receive_credit() on VC drain     |
| router→NSU eject  | NSU slave VC buffer      | `SLAVE_VC_BUFFER_DEPTH` | nsu_wrap credit_return pulse          |

`link_perf_monitor.BUFFER_DEPTH` must equal the RECEIVING buffer depth. Before this fix the generator used `SLAVE_VC_BUFFER_DEPTH` for inter-router links, which matched only by coincidence (both defaulted to 4). When `ROUTER_VC_DEPTH` diverges from `SLAVE_VC_BUFFER_DEPTH` (e.g. depth=8), the monitor saw more credits returned than its initial count → underflow assertion fires.

## Changes

**`sim/tools/gen_tb_top.py`**
- `emit_fabric`: added `ROUTER_VC_DEPTH = ni_params_pkg::NOC_ROUTER_VC_DEPTH_DFLT` parameter to `noc_fabric_*` module header.
- `emit_fabric`: changed `link_perf_monitor` instantiation from `.BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH)` → `.BUFFER_DEPTH(ROUTER_VC_DEPTH)`.
- `emit_tb_top`: added `ROUTER_VC_DEPTH` localparam derived from `ni_params_pkg::NOC_ROUTER_VC_DEPTH_DFLT`.
- `emit_tb_top`: passes `.ROUTER_VC_DEPTH(ROUTER_VC_DEPTH)` to fabric instantiation.

**`c_model/include/nsu/depacketize.hpp`**
- Reverted the `ar_src` local variable extraction (cosmetic, identical behavior). Restored to inline `static_cast<uint8_t>(f.get_header_field("src_id"))` directly in the initializer list.

**`c_model/include/axi/axi_master.hpp`** — NOT TOUCHED (RAW-hazard fix kept as-is).

**`c_model/include/wrap/router_wrap.hpp`** — NOT TOUCHED (already correct: uses `NOC_ROUTER_VC_DEPTH`).

Generated artifacts re-emitted: `sim/sv/noc_fabric_*.sv`, `sim/sv/tb_top_*.sv` (all 4 topologies).

## Two-depth configurability proof

**Default (ROUTER_VC_DEPTH=4):**
```
[bench] running Vtb_top.exe (16 nodes) ...
PASS: scenario complete, scoreboard clean
[bench] correctness gate: PASS
latency: mean=42.22 p95=62 max=82 (n=64 txns, window 207 cyc)
```

**Depth=8 (constants.yaml ROUTER_VC_DEPTH default→8, codegen, rebuild):**
```
[bench] running Vtb_top.exe (16 nodes) ...
PASS: scenario complete, scoreboard clean
[bench] correctness gate: PASS
latency: mean=41.47 p95=61 max=74 (n=64 txns, window 199 cyc)
```

Constants restored to default (4) after depth=8 proof.

## Gate results

- `make check PYTHON3=python3` → **545/545 passed**
- `make sim-regress TOPOLOGY=mesh_4x4_vc1 PYTHON3=python3` → **6/6 passed**
- `git status` clean (no stale generated files)

## Concerns

None. The fix is minimal and surgical: one parameter added to the generator, one usage site changed. The c_model router already used `NOC_ROUTER_VC_DEPTH` correctly; the SV monitor was the only broken consumer.
