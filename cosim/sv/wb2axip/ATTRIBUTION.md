# OSS Attribution — cosim/sv/wb2axip/

AXI4 protocol checker properties from the ZipCPU/wb2axip project, used as
simulation-runtime observers via Verilator. Two files are modified from
upstream; one file is newly authored for this project.

## Upstream

- Source: https://github.com/ZipCPU/wb2axip
- Commit: 2e8d3bc2d26ddc33d1881022a2a2b9d3f0c16b9b
- License: Apache-2.0

## Files

| This repo                              | Upstream path                       | Status        |
|----------------------------------------|--------------------------------------|---------------|
| `cosim/sv/wb2axip/faxi_master.v`      | `bench/formal/faxi_master.v`         | Modified      |
| `cosim/sv/wb2axip/faxi_slave.v`       | `bench/formal/faxi_slave.v`          | Modified      |
| `cosim/sv/wb2axip/faxi_wstrb.v`       | (none)                               | Newly authored|
| `cosim/sv/wb2axip/sim_wrapper.svh`    | (none)                               | Newly authored|

## Modifications to vendored files

Apache 2.0 §4(b) requires marking modified files. All changes were made in
commit 822a780 to make the formal-only stubs compile under Verilator
simulation mode and in commit 391f13d to fix internal invariant tracking.

### faxi_master.v

**Why**: The upstream file is a formal-tool-only checker. It references
signals that are not declared in the port list (formal tools tolerate this;
Verilator does not). Six concrete patches vs upstream 2e8d3bc:

1. Removed orphan `` `endif `` at end-of-file (no matching `` `ifdef ``).
2. Removed `|| // ...` stub line producing a `|| ||` double-operator syntax
   error (invalid in Verilog-2001 `.v` mode parsed by Verilator).
3. Added missing module-level declarations: `f_past_valid`, `f_reset_length`,
   `F_OPT_INITIAL`, `f_axi_wr_*`, `next_rd_*`, `f_axi_rd_cksize`,
   `val_wr_len`, `wstb_addr`, `wstb_valid`, `this_awsize`. These stubs
   satisfy Verilator's name-resolution pass.
4. Added stub declarations for the `EXCLUSIVE_ACCESS_CHECKER` generate block
   (`i_active_lock`, `i_exlock_*`, `f_axi_ex_*`, `check_this_return`, etc.)
   so Verilator can parse the inactive branch without undeclared-name errors.
5. Added `f_axi_wr_len` capture logic: `always @(posedge i_clk)` block that
   sets `f_axi_wr_len <= i_axi_awlen` on every AW handshake. The upstream
   formal block contains this via `// ...` stub comments; simulation needs
   it explicit so the induction assertion `f_axi_wr_pending <= f_axi_wr_len+1`
   at line 889 receives a correct value.

### faxi_slave.v

Same five patches as `faxi_master.v` above (the two files are symmetric
checker stubs with inverted `SLAVE_ASSUME` / `SLAVE_ASSERT` semantics).

### faxi_wstrb.v (newly authored)

Not present in upstream at commit 2e8d3bc. Created as a permissive simulation
stub to satisfy the `faxi_wstrb` module instantiation inside `faxi_master.v`
and `faxi_slave.v`. The upstream formal version of `faxi_wstrb` is inlined
into the parent modules; this project needs it as a separate module to
satisfy Verilator's structural elaboration.

**Current behavior**: `o_valid` is hardwired to `1'b1` (always valid). Write
strobe alignment checking is effectively disabled. See
`docs/architecture.md` sec. 4 (faxi_wstrb.v -- permissive stub) for the
implication and fix path.

### sim_wrapper.svh (newly authored)

Not a vendored file. Contains only the include-guard and explanatory comments
for the macro shim layer. `SLAVE_ASSUME` and `SLAVE_ASSERT` are defined
inside `faxi_master.v` / `faxi_slave.v` with per-file role semantics; this
file does NOT redefine them to avoid conflicts. Standalone `assume(...)` calls
inside the checker files are mapped to `assert` via the Verilator build flag
`+define+assume=assert` in `cosim/verilator/Makefile`.

