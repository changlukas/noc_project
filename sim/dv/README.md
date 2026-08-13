# sim/dv — imported DV IP (do not edit; one flagged local modification below)

Take files with `git show <tag>:<path>` against the upstream clone, never `cp` from its working
tree. A clone sits at whatever revision it was last left on: copying `axi_sim_mem.sv` out of one
parked at v0.39.10 into the v0.39.7 directory here mixed two releases, and the two versions of
that file differ. The directory name is the contract; the fetch has to name the same tag.

| package | upstream | rev | files | license | modified |
|---------|----------|-----|-------|---------|----------|
| axi-0.39.7 | github.com/pulp-platform/axi | v0.39.7 | src/{axi_pkg,axi_intf,axi_test}.sv src/{axi_xbar,axi_xbar_unmuxed,axi_mux,axi_demux,axi_demux_simple,axi_err_slv,axi_atop_filter,axi_multicut,axi_cut,axi_id_prepend,axi_id_remap,axi_delayer,axi_sim_mem,axi_rw_join}.sv include/axi/{typedef,assign}.svh | Solderpad 0.51 | - |
| common_verification-0.2.5 | github.com/pulp-platform/common_verification | v0.2.5 | src/rand_id_queue.sv | Solderpad 0.51 | - |
| common_cells-1.37.0 | github.com/pulp-platform/common_cells | v1.37.0 | src/{cf_math_pkg,addr_decode,addr_decode_dync,lzc,rr_arb_tree,fifo_v3,counter,delta_counter,spill_register,spill_register_flushable,stream_register,stream_delay,lfsr_16bit}.sv src/{stream_fork,stream_fifo,stream_fifo_optimal_wrap,passthrough_stream_fifo,fall_through_register}.sv include/common_cells/{registers,assertions}.svh | Solderpad 0.51 | - |
| idma-0.6.5 | github.com/pulp-platform/iDMA | v0.6.5 | src/idma_pkg.sv src/idma_{backend,legalizer,transport_layer}_rw_axi.sv src/idma_{axi_read,axi_write,dataflow_element,error_handler,channel_coupler,legalizer_page_splitter}.sv include/idma/{typedef,guard}.svh | Solderpad 0.51 | - |
| floonoc-test | github.com/pulp-platform/FlooNoC | 14c253c996fcdc78b793fe28ac18964769b768df | axi_bw_monitor.sv | Solderpad 0.51 (LICENSE-SHL) | yes — 2-line $display latency-N addition in axi_bw_monitor.sv (consumed by sim/tools/emit_result_csv.py) |

No transitive `.svh` includes beyond the six listed (`axi_intf.sv` pulls in `axi/typedef.svh`; the axi
crossbar files pull in `axi/assign.svh` and `common_cells/registers.svh`; the iDMA files pull in
`idma/guard.svh`, `common_cells/{registers,assertions}.svh` and `axi/typedef.svh`; `include/axi/port.svh`
is not referenced by any copied file and was not imported).

The axi crossbar subset is the elaboration closure of `axi_xbar`, plus the tile memory behind it:
`axi_delayer` (timing) in front of `axi_sim_mem` (storage), which together replace the MAPPED
`axi_rand_slave` the two targets used before. `axi_delayer` pulls in `stream_delay`, which pulls in
`lfsr_16bit`. Everything is taken at the version already vendored (axi v0.39.7, common_cells
v1.37.0) so no two files come from different releases — at v0.39.7 `axi_demux_id_counters` is a second
module inside `axi_demux_simple.sv` rather than its own file, which a master-branch copy would miss.
`axi_test.sv` references `rand_id_queue_pkg`, which is why common_verification is vendored: any
compile or lint that globs `axi-0.39.7/src/*.sv` has to carry `common_verification-0.2.5/src/` too.

iDMA publishes two tags per release and they are not interchangeable. **`v0.6.5` is the one pinned**:
it commits the generated RTL under `target/rtl/`, so `git show v0.6.5:target/rtl/idma_backend_rw_axi.sv`
works. `v0.6.5-src` holds only a `.gitignore` there and would make iDMA's generator, and therefore
bender, a build dependency of this repo.

The iDMA subset is the elaboration closure of `idma_backend_rw_axi` at `ErrorCap =
NO_ERROR_HANDLING`, `RAWCouplingAvail = 1` and `HardwareLegalizer = 1`, plus `axi_rw_join` to fold its
two manager ports into one, which is how FlooNoC uses it. `idma_error_handler.sv` is taken although
the default `ErrorCap` elides it, so raising `ErrorCap` does not become a second vendoring round. The
five new common_cells files and `axi_rw_join.sv` are pulled in by this closure and are taken at the
tags already vendored here.

The files column records where each file sits under `sim/dv/<package>/`, not where it came from: two
packages' upstream paths differ from the `src/` + `include/` layout used here. To reproduce those two
fetches: iDMA's `idma_pkg.sv` is upstream `src/`, its three `*_rw_axi.sv` are `target/rtl/`, its other
six `.sv` are `src/backend/`, and both its headers are `src/include/idma/`; `axi_bw_monitor.sv` is
upstream `hw/test/`.

Every package directory also carries the upstream `LICENSE` its licence column names; the files
column lists everything else in it.
