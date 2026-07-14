# Architecture inventory (Round 1 preflight, 2026-07-14)

Source of truth for the audit lanes. Built from the tree at 4bae444; docs audited against this, not
the other way around.

## 1. Directory map (top 2 levels)

```
src/
  c_model/{include, tests}
  dpi/          (cmodel_dpi.cpp/.h, handle_block.hpp, dpi_boundary_macros.h)
  sv/           (ni_wrap, nmu_wrap, nsu_wrap, router_wrap, noc_fabric_mesh_*.sv)
sim/
  tb/           (tb_top_*.sv, endpoint, perf monitor, pkg)
  dv/           (vendored: axi-0.39.7, common_cells-1.37.0, common_verification-0.2.5, floonoc-test)
  verilator/    (Makefile, main.cpp, perf_cli_summary.py, output/)
  vcs/          (Makefile)
  tools/        (gen_tb_top.py, gen_filelist.py, gen_test_patterns.py, emit_result_csv.py, plot_injection_sweep.py, ...)
  topologies/   (mesh_1x1_vc1 .. mesh_4x4_vc8 .yaml)
  test_patterns/(directed_* + stim_mesh_* generated dirs)
docs/
  image/, issue/, slides/, internal/, superpowers/   (+ *.md, see §5)
specgen/
  source/, generated/{cpp,json,sv}, ni_spec/, tools/, examples/, docs/, tests/
tools/          (empty)
```

## 2. c_model sub-namespaces and classes

**`axi/`**
| header | class |
|---|---|
| axi_master.hpp | `AxiMasterT` (+ `AxiMasterStandalone`, `WireSlavePort`) |
| axi_slave.hpp | `AxiSlave` |
| memory.hpp | `Memory` (impl. `IMemoryPort`) |
| memory_port.hpp | `IMemoryPort` (+ MemReq/Resp structs) |
| protocol_rules.hpp | (no class — `axi::rules` constants/free functions) |
| scenario_parser.hpp | `Scenario` (+ ScenarioConfig/Transaction structs) |
| scoreboard.hpp | `Scoreboard` |
| types.hpp | AXI beat structs `AwBeat/WBeat/ArBeat/BBeat/RBeat` |

**`ni/`**
| header | class |
|---|---|
| ni_stage.hpp | (no class — `enum class NiPath`) |
| pipeline_stage.hpp | `PipelineStage` |
| vc_pools.hpp | `VcPools` (struct) |
| wormhole_arbiter.hpp | `WormholeArbiter` |

**`nmu/`**
| header | class |
|---|---|
| addr_trans.hpp | `SamTable` (+ `Translated`, `SamEntry`) |
| axi_slave_port.hpp | `AxiSlavePort` |
| depacketize.hpp | `Depacketize` (impl. ResponseDepacketizer) |
| ni_tokens.hpp | (no class — `AdmittedAw/Ar/W` structs) |
| nmu.hpp | `Nmu` (+ `NmuReqS1Bridge`, `NmuConfig`) |
| nmu_standalone.hpp | `NmuStandalone` |
| packetize.hpp | `Packetize` (+ `NmuPacketizeSink`) |
| port_params.hpp | `PortParams` (struct) |
| rob.hpp | `Rob` |
| sam_yaml.hpp | (no class — `load_sam_table()` free function) |
| vc_arbiter.hpp | `VcArbiter` (impl. router::NocReqOut) |

**`nsu/`**
| header | class |
|---|---|
| axi_master_port.hpp | `AxiMasterPort` |
| depacketize.hpp | `Depacketize` (impl. ResponseDepacketizer) |
| meta_buffer.hpp | `MetaBuffer` (+ `MetaEntry`) |
| nsu.hpp | `Nsu` (+ `NsuConfig`) |
| nsu_standalone.hpp | `NsuStandalone` (+ Null NoC stubs) |
| packetize.hpp | `Packetize` (impl. ResponsePacketizer) |
| port_params.hpp | `PortParams` (struct) |
| vc_arbiter.hpp | `VcArbiter` (impl. router::NocRspOut) |

**`router/`**
| header | class |
|---|---|
| req_in.hpp | `NocReqIn` (abstract iface) |
| req_out.hpp | `NocReqOut` (abstract iface) |
| router.hpp | `Router` (+ `RouterConfig`, `RouterLink`, `RouterCreditSink`) |
| router_adapters.hpp | `InjectAdapter`, `EjectAdapter`, `CreditRelay`, `LinkEjectAdapter`, `LinkCreditOut` |
| rsp_in.hpp | `NocRspIn` (abstract iface) |
| rsp_out.hpp | `NocRspOut` (abstract iface) |

**`wrap/`**
| header | class |
|---|---|
| flit_byte_conv.hpp | (no class — `flit_from_bytes`/`flit_to_bytes`) |
| flit_bytes.hpp | (no class — `FLIT_BYTES` constants) |
| nmu_wrap.hpp | `NmuWrap` |
| nmu_wrap_io.hpp | `NmuInputs` / `NmuOutputs` structs |
| nsu_wrap.hpp | `NsuWrap` |
| nsu_wrap_io.hpp | `NsuInputs` / `NsuOutputs` structs |
| perf_collector.hpp | `PerfCollector` |
| router_wrap.hpp | `RouterWrap` |
| router_wrap_io.hpp | `RouterInputs` / `RouterOutputs` structs |

**Top-level (no sub-ns):** `flit.hpp` → `Flit`; `request_io.hpp` → `RequestPacketizer`/`RequestDepacketizer`; `response_io.hpp` → `ResponsePacketizer`/`ResponseDepacketizer`.

## 3. SV modules (project-owned)

| module (file) | role |
|---|---|
| `sim/tb/tb_top_mesh_*.sv` (11 topologies) | generated per-topology tb top: clk/rst/watchdog, `cmodel_*_create`, includes fabric, endpoints + PASS/exit logic |
| `sim/tb/user_node_endpoint.sv` | per-node AXI endpoint: pulp `axi_file_master` + `axi_rand_slave` + in-endpoint scoreboard + bw monitor |
| `sim/tb/link_perf_monitor.sv` | inter-router link throughput/latency perf probe |
| `sim/tb/axi_vip_types_pkg.sv` | package: AXI VIP type defs for the endpoints |
| `src/sv/noc_fabric_mesh_*.sv` | generated fabric: N nodes (NMU + REQ/RSP router_wrap + NSU) joined by N/E/S/W links, DPI ctx handles as ports |
| `src/sv/nmu_wrap.sv` / `nsu_wrap.sv` / `router_wrap.sv` | SV shells around the DPI (set_inputs → tick → get_outputs) |
| `src/sv/ni_wrap.sv` | combined NI (NMU+NSU) wrap shell |
| `specgen/generated/sv/*.sv` | generated packages: flit/param/signal constants + per-VC NoC types |

Vendored under `sim/dv/` (out of audit scope except hand-edited `axi_bw_monitor.sv`): axi-0.39.7,
common_cells-1.37.0, common_verification-0.2.5, floonoc-test.

## 4. Generators

- **specgen/**: JSON in `specgen/generated/json/*.json` is canonical; `specgen/tools/codegen.py` emits
  C++ constants (`generated/cpp/`), SV packages (`generated/sv/`), JSON schemas/index. `--check` is
  the drift gate. Sources: `specgen/source/{constants.yaml, interface_handshake.json, noc_function_blocks.json}`.
- **`sim/tools/gen_tb_top.py`**: from a topology YAML emits `src/sv/noc_fabric_<topo>.sv` and
  `sim/tb/tb_top_<topo>.sv`. Emitted `.sv` are generated artifacts — edit generator/YAML, not output.

## 5. Doc set (`docs/*.md`, non-recursive)

| file | topic |
|---|---|
| architecture.md | system architecture overview |
| 2026-07-02-pulp-axi-vip-node-design.md | pulp AXI VIP test-node design |
| backlog.md | forward-only open work items |
| cosim-log.md | how to read co-sim run logs |
| development.md | workflow, repo conventions, build system |
| nmu-rob-microarchitecture.md | as-built `nmu::Rob` structure-by-structure |
| performance-probe.md | NoC per-transaction latency/throughput probe |
| pg037_axi_perf_mon.md | external AXI perf-monitor product-guide reference |
| verification-environment.md | DUT/verification env per mesh node |

`docs/image/`: 18 JPGs (block/packet-format diagrams — spec ground truth, kept).
`docs/issue/`: `ARCHITECTURE.md` (zh-TW repo structure + refactor decision log).
`docs/slides/`: `genamba-role1-port-SLIDES.md` (historical progress deck).

## Stale baseline claims (seed findings for lane 3)

| doc | claim (quoted, short) | reality (file evidence) |
|---|---|---|
| CLAUDE.md | "no router class in c_model; `ChannelModel` … is the only NoC stub" | Refuted. `src/c_model/include/router/router.hpp:75` defines `class Router` (dir has 6 headers); DPI exposes `cmodel_router_create` (`src/dpi/cmodel_dpi.h:65`). |
| CLAUDE.md | "Destination derivation (XY bit-slice) … via `nmu::addr_trans::xy_route`" | Stale. `xy_route` retired (`wrap/nmu_wrap.hpp:63` comment); routing is SAM lookup via `addr_trans::SamTable` (`nmu/addr_trans.hpp:23`). |
| CLAUDE.md | "Wrap layer: `NmuWrap` / `NsuWrap` / `MasterWrap` / `SlaveWrap`" | Stale. Wraps are `NmuWrap`, `NsuWrap`, `RouterWrap`; AXI master/slave live in SV (`sim/tb/user_node_endpoint.sv`). |
| CLAUDE.md | "`cmodel_<component>_create(name)`" implying master/slave wraps | Stale. Only `cmodel_router_create`, `cmodel_nmu_create`, `cmodel_nsu_create` exist (`src/dpi/cmodel_dpi.h:65,95,138`). |
| CLAUDE.md | "Config: YAML (`c_model/config/`); no JSON" | Stale both counts. No `src/c_model/config/`; topology YAML in `sim/topologies/`; JSON is canonical spec source (`specgen/`). |
| CLAUDE.md | "AXI4 Master → NMU → ChannelModel (test stub) → NSU → AXI4 Slave" | Partially stale. ChannelModel is unit-test-only; co-sim datapath is the router fabric. |
| docs/architecture.md | §2 "The c_model contains no router class." | Refuted (as above). |
| docs/architecture.md | §2 "A router model can replace `ChannelModel` by implementing the four interfaces in `c_model/include/router/`" | Stale framing. `router/` now holds concrete `Router` + 5 adapter classes. |
| docs/architecture.md | §4 "four tick functions: `cmodel_master_tick` … `cmodel_slave_tick`" | Stale. Actual: `cmodel_router_tick`, `cmodel_nmu_tick`, `cmodel_nsu_tick`; no master/slave tick. |
| docs/architecture.md | §4 "(see `sim/c/cmodel_dpi.h`)" | Stale path. Header is `src/dpi/cmodel_dpi.h`. |
| docs/architecture.md | §4 "Four wraps: `NmuWrap`, `NsuWrap`, `MasterWrap`, `SlaveWrap`" | Stale. Actual: `NmuWrap`, `NsuWrap`, `RouterWrap`. |
| docs/architecture.md | §3.1 lists `ChannelModel` as the co-sim NoC hop | Stale for co-sim (router fabric is the hop). |
| docs/architecture.md | §4 tb-top build description omits generation | tb tops + fabric are generated by `sim/tools/gen_tb_top.py`; doc does not mention it. |
