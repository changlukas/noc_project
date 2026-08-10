# sim/dv — imported DV IP (do not edit; one flagged local modification below)

| package | upstream | rev | files | license | modified |
|---------|----------|-----|-------|---------|----------|
| axi-0.39.7 | github.com/pulp-platform/axi | v0.39.7 | src/{axi_pkg,axi_intf,axi_test}.sv src/{axi_xbar,axi_xbar_unmuxed,axi_mux,axi_demux,axi_demux_simple,axi_err_slv,axi_atop_filter,axi_multicut,axi_cut,axi_id_prepend}.sv include/axi/{typedef,assign}.svh | Solderpad 0.51 | - |
| common_verification-0.2.5 | github.com/pulp-platform/common_verification | v0.2.5 | src/rand_id_queue.sv | Solderpad 0.51 | - |
| common_cells-1.37.0 | github.com/pulp-platform/common_cells | v1.37.0 | src/{cf_math_pkg,addr_decode,addr_decode_dync,lzc,rr_arb_tree,fifo_v3,counter,delta_counter,spill_register,spill_register_flushable,stream_register}.sv include/common_cells/{registers,assertions}.svh | Solderpad 0.51 | - |
| floonoc-test | github.com/pulp-platform/FlooNoC | 14c253c996fcdc78b793fe28ac18964769b768df | hw/test/axi_bw_monitor.sv | Solderpad 0.51 (LICENSE-SHL) | yes — 2-line $display latency-N addition in axi_bw_monitor.sv (consumed by sim/tools/emit_result_csv.py) |

No transitive `.svh` includes beyond the four listed (`axi_intf.sv` pulls in `axi/typedef.svh`; the axi
crossbar files pull in `axi/assign.svh` and `common_cells/registers.svh`; `include/axi/port.svh` is not
referenced by any copied file and was not imported).

The axi crossbar subset is the elaboration closure of `axi_xbar`. It builds the
per-tile decode in `user_node_endpoint.sv`: one `axi_xbar_intf` feeding two MAPPED
`axi_rand_slave` memories. Everything is taken at the version already vendored (axi v0.39.7, common_cells
v1.37.0) so no two files come from different releases — at v0.39.7 `axi_demux_id_counters` is a second
module inside `axi_demux_simple.sv` rather than its own file, which a master-branch copy would miss.
