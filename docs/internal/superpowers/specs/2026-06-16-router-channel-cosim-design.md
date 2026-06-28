# RouterChannel Co-sim Design (bidirectional tb_top)

Date: 2026-06-16
Status: Approved pending user review
Scope: wire the production `RouterChannel` (sub-project B) into the Verilator wire-level
co-sim by replacing the `ChannelModel` stub in `tb_top`, and rewrite `tb_top` as a
2-node symmetric bidirectional testbench (each node = master + NMU + NSU + slave, both
directions running simultaneously). Sub-project C of the NI+router integration. The
`ChannelModel` co-sim path (used by `tb_genamba*`) stays; only `tb_top` is repointed.

## 1. Motivation

`RouterChannel` is validated end-to-end only by the pure-C++ `test_router_loopback.cpp`;
the SV co-sim (`tb_top`) still drives the `ChannelModel` stub. This brings the real router
fabric into the wire-level testbench. `tb_top` is simultaneously upgraded from a one-way
single-NI pipeline to the symmetric 2-node form the fabric actually models, so both nodes
issue requests and receive responses through the routers in one run.

Depends on the bit-32 address layout (`LOCAL_ADDR_BITS=32`, landed on this branch): local
address is `addr[31:0]`, destination coordinate decodes from `addr[39:32]`. Each scenario's
addresses carry the target node coordinate directly — no runtime address offset.

## 2. Topology

Two symmetric nodes `(0,0)` and `(1,0)`. Each direction is one request+response flow:

| Flow | Source master | addr coordinate | Target NSU/slave | addr range |
|---|---|---|---|---|
| A | node1.master | `(0,0)` | node0 | low (`addr[39:32]=0`) |
| B | node0.master | `(1,0)` | node1 | high (`addr[39:32]=1`, ≥ `0x1_0000_0000`) |

```
 master1─►nmu1─►[REQ R(1,0)]══►[REQ R(0,0)]─►nsu0─►slave0   Flow A req
master1◄─nmu1◄─[RSP R(1,0)]◄══[RSP R(0,0)]◄─nsu0◄─slave0   Flow A rsp
 slave1◄─nsu1◄─[REQ R(1,0)]◄══[REQ R(0,0)]◄─nmu0◄─master0  Flow B req
 slave1─►nsu1─►[RSP R(1,0)]══►[RSP R(0,0)]─►nmu0─►master0  Flow B rsp
```

One `RouterChannel` (4 routers + 8 adapters + 4 relays, from B) carries both flows. The two
request directions traverse opposite link segments and different `(input port → output
port)` pairs, so they do not overtake each other (B §6 cross-flow isolation).

## 3. Components

| File | Responsibility |
|---|---|
| `c_model/include/cosim/router_channel_shell_adapter.hpp` (new) | `RouterChannelShellAdapter`: owns one `RouterChannel`; exposes both nodes' NoC ports via new `RouterChannelInputs/Outputs`; `init/set_inputs/tick/get_outputs` mirror `ChannelModelShellAdapter` for 2 nodes |
| `cosim/c/cmodel_dpi.cpp` (modify) | `cmodel_router_channel_create/set_inputs/tick/get_outputs`; `ShellType::RouterChannel`; per-master scenario path + per-slave memory_base (see §5) |
| `cosim/sv/router_channel_wrap.sv` (new) | beta-tick DPI wrapper carrying both nodes' `noc_intf` bundles (mirrors `channel_model_wrap`) |
| `cosim/sv/tb_top.sv` (rewrite) | 2× {master, NMU, NSU, slave} + one `router_channel_wrap`; no wb2axip protocol checkers |
| `specgen/`-adjacent scenario generator (new) | emit the two coordinate-bearing variants per pattern (§6) |

`RouterChannelShellAdapter` reuses B's `RouterChannel` accessors: `nmu_req_out(node)`,
`nmu_rsp_in(node)`, `nsu_req_in(node)`, `nsu_rsp_out(node)`, `tick()`. Per-node tick mapping
mirrors the stub (NMU inject → `nmu_req_out(n).push_flit`; NSU inject → `nsu_rsp_out(n).push_flit`;
sample `nsu_req_in(n).pop_flit` / `nmu_rsp_in(n).pop_flit`; credit via `credit_avail(0)`),
applied to both `n ∈ {0,1}`.

## 4. NoC I/O struct

`RouterChannelInputs/Outputs` carry both nodes' NoC pins (vs the stub's single NMU + single
NSU). Per node `n`: inputs `{req_in_valid[n], req_in_flit[n], rsp_in_valid[n], rsp_in_flit[n]}`;
outputs `{req_out_valid[n], req_out_flit[n], rsp_out_valid[n], rsp_out_flit[n],
req_out_credit_return[n], rsp_out_credit_return[n]}`. Flit pack/unpack reuses the existing
`flit_from_bytes`/`flit_to_bytes`.

## 5. Harness generalization

The single-flow harness is half-generalized. What changes:

| Mechanism | Today | Change |
|---|---|---|
| `cmodel_done()` | iterates all `Master` handles, logical-AND | keep — already N-master safe |
| `g_scoreboard` | one shared `expected_` map | keep — flows write disjoint address ranges (low vs high), no collision |
| `cmodel_master_create(name)` | master re-loads single `g_scenario_yaml_path` | `cmodel_master_create(name, scenario_path)` — each master loads its own coordinate variant |
| `cmodel_slave_create(name)` | memory_base from `g_scenario.config` | `cmodel_slave_create(name, scenario_path)` — slave loads its variant; `memory_base`/`memory_size` come straight from that variant's config |

The generator (§6) is the single point that shifts address + `memory_base`; `cmodel_slave_create`
performs no offset arithmetic — node1.slave's high base is already baked into the node1 variant
it loads. Variant→instance pairing: the node0 variant drives `{node1.master, node0.slave}`
(Flow A, low addr); the node1 variant drives `{node0.master, node1.slave}` (Flow B, high addr).
Because Memory is a flat array indexed `addr - base_`, the node1 variant's `memory_base` must
equal its shifted base so the high range is in bounds.

`g_scenario` (via the unchanged `cmodel_init`) still supplies shared, address-independent
config; the per-instance variants supply addresses + base. The signature changes are contained:
`tb_top.sv` (rewritten here) is the only production caller; `test_cmodel_dpi.cpp` updated alongside.

**VC count.** This round pins `num_vc = 1` for the co-sim: the SV NoC wrappers fatal on
`NUM_VC != 1` (`nmu_wrap.sv`, `nsu_wrap.sv`), so single-VC is the only wire-level config today
and credit reduces to `credit_avail(0)`. Multi-VC co-sim (and the matching credit-vector
backpressure contract) is later work; the pure-C++ loopback already covers the `num_vc` grid.

## 6. Scenario generation

Per pattern, two coordinate-bearing variants drive the two masters:

- **node0 variant** (drives node1.master, targets `(0,0)`): existing scenario unchanged —
  its addresses already decode to `dst=(0,0)`.
- **node1 variant** (drives node0.master, targets `(1,0)`): every transaction `addr` and the
  `memory_base` shifted by `+0x1_0000_0000` (sets `addr[32]`).

A generator emits both into a stable layout (e.g. `sim/test_patterns/<pattern>/{node0,node1}/
scenario.yaml`). This round generates the wb2axip-compatible subset only; full 37-pattern
augmentation is later work (user-driven).

## 7. Verification

CosimIntegration becomes bidirectional. Per pattern in the subset, one `Vtb_top` run drives
node1.master from the node0 variant and node0.master from the node1 variant; both flows must
exhaust their transactions and the shared scoreboard must report zero mismatch. The `tb_top`
exit poll (`cmodel_done && cmodel_scoreboard_clean → $finish(0)`) is unchanged in logic.

**Non-vacuous-pass guard.** `cmodel_done` passes with ≥1 master, and `scoreboard_clean` only
checks `mismatch_count()==0`; so a wiring bug that creates one master, or a write-only pattern,
could PASS without exercising both flows or comparing any read-back. To close this, the run
must assert: (a) exactly two masters were created, and (b) `reads_checked_ > 0` for the run
(both flows performed read-back comparison). The DPI exposes the master count and
`reads_checked_` (extending the existing scoreboard dump), and the ctest asserts both; the
generated subset is restricted to patterns that issue reads.

**Coverage delta (honest).** Removing the two wb2axip protocol checkers drops AXI4 protocol-conformance
checking on the NMU-master and NSU-slave boundaries and the MAXSTALL/MAXRSTALL/MAXDELAY stall
induction — the scoreboard only compares read-back data, not protocol well-formedness or
ordering. This is a real reduction versus the prior single-flow tb_top, accepted for this round
and restored when the wb2axip protocol checker is re-integrated (checker-first). The generated subset is kept to the
prior wb2axip-runnable transactions so the *data-layer* set matches, now run through the real
fabric in both directions.

## 8. Errors and invariants

- **`src_id` = node coordinate** (B §8): NSU stamps a response `dst_id` from the request
  `src_id`, and the RSP fabric routes by `dst_id`. So node `n`'s NMU must use `src_id = n`,
  or responses eject at the wrong node. The bidirectional harness sets each NMU's `src_id`
  to its node coordinate.
- Node coordinates and the generated address ranges keep all `dst_id` in mesh; the router's
  out-of-mesh and `vc_id`-range aborts are not triggered in normal operation.
- Reset follows the existing tb_top `rst_ni` discipline; the router models construction-as-reset.

## 9. Scope boundary

In scope: bidirectional `tb_top` over the real `RouterChannel`; `RouterChannelShellAdapter` +
DPI; per-master/per-slave harness generalization; wb2axip-subset coordinate-variant generator;
scoreboard-only validation. Out of scope: wb2axip protocol-checker re-integration (deferred,
checker-first to be restored later); >2 nodes / NxM mesh; full 37-pattern augmentation;
`ChannelModel` co-sim removal (`tb_genamba*` still use it).
