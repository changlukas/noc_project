# sim/dv — imported DV IP (do not edit; one flagged local modification below)

| package | upstream | rev | files | license | modified |
|---------|----------|-----|-------|---------|----------|
| axi-0.39.7 | github.com/pulp-platform/axi | v0.39.7 | src/axi_pkg.sv src/axi_intf.sv src/axi_test.sv include/axi/typedef.svh include/axi/assign.svh | Solderpad 0.51 | - |
| common_verification-0.2.5 | github.com/pulp-platform/common_verification | v0.2.5 | src/rand_id_queue.sv | Solderpad 0.51 | - |
| common_cells-1.37.0 | github.com/pulp-platform/common_cells | v1.37.0 | src/cf_math_pkg.sv src/addr_decode.sv src/addr_decode_dync.sv | Solderpad 0.51 | - |
| floonoc-test | github.com/pulp-platform/FlooNoC | 14c253c996fcdc78b793fe28ac18964769b768df | hw/test/axi_bw_monitor.sv | Solderpad 0.51 (LICENSE-SHL) | yes — 2-line $display latency-N addition in axi_bw_monitor.sv (consumed by sim/tools/emit_result_csv.py) |
| taxi-d5d38c2 | github.com/fpganinja/taxi | d5d38c2 (2026-08-04) | src/axi/rtl/{taxi_axi_if,taxi_axi_crossbar_1s,taxi_axi_crossbar_1s_wr,taxi_axi_crossbar_1s_rd,taxi_axi_crossbar_wr,taxi_axi_crossbar_rd,taxi_axi_crossbar_addr,taxi_axi_register_wr,taxi_axi_register_rd,taxi_axi_tie_wr,taxi_axi_tie_rd,taxi_axi_ram}.sv src/prim/rtl/{taxi_arbiter,taxi_penc}.sv | CERN-OHL-S-2.0 (taxi_axi_if.sv: MIT) | - |

No transitive `.svh` includes beyond `typedef.svh`/`assign.svh` themselves (`axi_intf.sv` pulls in `axi/typedef.svh`, already listed; `include/axi/port.svh` is not referenced by any copied file and was not imported).

The taxi subset is the closure of `taxi_axi_crossbar_1s.f` plus `taxi_axi_ram.sv`; no file in it uses
`` `include ``, so it needs no incdir. It builds the per-tile decode in `user_node_endpoint.sv`
(one crossbar, config `taxi_axi_ram` + data `axi_rand_slave`).
