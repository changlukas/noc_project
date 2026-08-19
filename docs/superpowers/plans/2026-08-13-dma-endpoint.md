# DMA Endpoint Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Put pulp iDMA on the master face of a directory-isolated top, and make `RobMode::Enabled` the NMU default, so a standard AXI manager drives this fabric and the data arrives intact.

**Architecture:** Two staged changes in one round. The default flips first and the six existing co-sim gates re-baseline against it; only then does the DMA arrive, so a red gate after the second step has one new variable. The DMA replaces `axi_file_master` on a new top that reuses the existing generated fabric unchanged — `idma_backend_rw_axi` behind `axi_rw_join` feeds the same tile crossbar the file master feeds today.

**Tech Stack:** SystemVerilog, Verilator 5.048, pulp iDMA v0.6.5, pulp axi v0.39.7, pulp common_cells v1.37.0, Python 3 generators, C++17 behaviour model behind a DPI handle ABI.

**Design:** `docs/superpowers/specs/2026-08-13-dma-endpoint-design.md`. Supporting evidence, already established and not to be re-derived: `.superpowers/sdd/ai-dataflow/survey-idma.md`.

## Global Constraints

- Linux only, from WSL. Never `py -3`; use `python3`.
- **Rsync is load-bearing.** `make test` and every co-sim run from `/mnt/e` test the `~/noc_project` mirror, a separate clone, and will report green against a tree with none of your changes. Rsync first, every time, and say in your report that you did.
- Every co-sim invocation opens with the pre-clean `rm -f sim/filelist_*.f sim/tb/tb_top_*.sv; rm -rf $BUILD_ROOT/verilator/obj_dir_*`.
- One synchronous foreground WSL call per unit of work. Never poll a running WSL job with a second `wsl.exe` call. Expect `E_UNEXPECTED` on the first call after an idle spell and retry once.
- Build command: `BUILD_ROOT=$HOME/noc_build PYTHON3=python3 make test`. Co-sim: `BUILD_ROOT=$HOME/noc_build PYTHON3=python3 make -C sim TB=<topo> PATTERN=<p>`.
- Baselines entering this plan: **ctest 669, pytest 69**, six co-sim gates DIRECTED PASS.
- Vendoring contract, `sim/dv/README.md`: take files with `git show <tag>:<path>` against an upstream clone, **never `cp` from its working tree**. The directory name pins the release. Every package records upstream, revision, exact file list, licence, and any local modification.
- C++ style: snake_case variables and methods, PascalCase types, 4-space indent and continuation indent, 100-column limit, `clang-format -i` on every `.hpp`/`.cpp` touched. SystemVerilog and Python match their own files.
- Commit messages `type(scope): description`, English. Never `--no-verify`.
- `sim/tb/user_node_endpoint.sv` and the six existing `tb_top_*.sv` are not modified by Stage B. Stage B adds files under `sim/tb/dma/` and `sim/dv/idma-0.6.5/`.

## File Structure

| file | responsibility |
|---|---|
| `ref_model/c_model/include/nmu/nmu.hpp:155` | the `NmuConfig` default that ctest inherits |
| `ref_model/c_model/include/wrap/nmu_wrap.hpp:77` | the wrap default |
| `ref_model/dpi/cmodel_dpi.cpp:461` | the plain `cmodel_nmu_create` default |
| `sim/tools/gen_tb_top.py:750` | what the generated tb passes as `rob_enabled` |
| `sim/dv/idma-0.6.5/` | 12 vendored iDMA files |
| `sim/tb/dma/idma_types_pkg.sv` | the AXI and iDMA typedefs and the meta-channel structs, one place |
| `sim/tb/dma/dma_node_endpoint.sv` | iDMA + `axi_rw_join` + the tile crossbar + memories |
| `sim/tb/dma/idma_job_driver.sv` | reads a job file, drives `idma_req_t` |
| `sim/tb/dma/tb_top_dma_<topo>.sv` | generated top for the DMA flavour |
| `sim/tools/gen_dma_jobs.py` | emits job files from the topology's address map |

---

## Stage A: the reorder buffer becomes the default

### Task 1: Flip the default, and make the mode a parameter instead of a filename

`RobMode::Enabled` is what `docs/noc-target-spec.md` §3 describes. `Disabled` is what ships, and it is selected by a **topology name ending `_rob`** — the mode is decided by parsing a string. That is the wrong control for it twice over: the topology name is not where a mode belongs, and a reader cannot tell from the tb what it is running.

It becomes a parameter, reaching the generated top the way `INJECTION_MODE` already does, so toggling it is one Make variable and reading it is one line of the emitted SystemVerilog. `cmodel_nmu_create_ex` already takes `rob_enabled` explicitly and is unchanged; what moves is only how `gen_tb_top.py` decides what to pass.

**Files:**
- Modify: `ref_model/c_model/include/nmu/nmu.hpp:155`
- Modify: `ref_model/c_model/include/wrap/nmu_wrap.hpp:77`
- Modify: `ref_model/dpi/cmodel_dpi.cpp:459-464`
- Modify: `sim/tools/gen_tb_top.py:750` and the `_rob` strip at `:56`
- Modify: `sim/Makefile:23` (the help text naming the suffix)
- Modify: `sim/build_config.mk` — the `$(TOPOLOGY:_rob=)` strip
- Modify: `docs/known-limitations.md` — the row whose "when it bites" says nothing constructs an `Enabled` build
- Test: `ref_model/c_model/tests/nmu/test_nmu.cpp`

**Interfaces:**
- Produces: every NMU constructed without an explicit mode now runs `RobMode::Enabled`. `cmodel_nmu_create_ex`'s `rob_enabled` argument is unchanged and still explicit; only the plain `cmodel_nmu_create` and the struct defaults move.

- [ ] **Step 1: Write the failing test**

Add to `ref_model/c_model/tests/nmu/test_nmu.cpp`, beside the existing test that pins RoBless behaviour explicitly:

```cpp
TEST(NmuTopLevel, ReadReorderBufferIsOnByDefault) {
    // docs/noc-target-spec.md section 3 puts a reorder buffer on the response
    // path. A default-constructed config is what every test and the plain DPI
    // entry point get, so this is the one that decides which path they run.
    nmu::NmuConfig cfg;
    EXPECT_EQ(cfg.read_rob_mode, nmu::RobMode::Enabled);
}
```

- [ ] **Step 2: Run it and watch it fail**

```
wsl -e bash -lc 'cd ~/noc_project && BUILD_ROOT=$HOME/noc_build PYTHON3=python3 make build-cmodel && ctest --test-dir $HOME/noc_build/cmodel -R ReadReorderBufferIsOnByDefault -V 2>&1 | tail -10'
```
Expected: FAIL, the value is `Disabled`.

- [ ] **Step 3: Flip the three C++ defaults**

`nmu.hpp:155`:
```cpp
    RobMode read_rob_mode = RobMode::Enabled;
```

`nmu_wrap.hpp:77`, the `init` default argument:
```cpp
              nmu::RobMode rob_mode = nmu::RobMode::Enabled,
```

`cmodel_dpi.cpp:461`, inside `cmodel_nmu_create`:
```cpp
    return nmu_create_impl(name, src_id, dat_num_vc, ni::cmodel::nmu::RobMode::Enabled,
```

Leave `cmodel_nmu_create_ex` alone. It takes `rob_enabled` explicitly and the generated tb passes it explicitly; that is the path Step 4 changes.

- [ ] **Step 4: Make the mode a parameter**

`gen_tb_top.py:750` currently reads `rob_enabled = requested_name.endswith("_rob")`. The mode becomes an argument to the generator, defaulting to enabled, emitted as a named `localparam` in the tb and passed to `cmodel_nmu_create_ex` from it rather than as a bare literal — so the emitted SystemVerilog says what it is running.

It reaches the generator the way `INJECTION_MODE` does: `sim/Makefile` forwards it when set (`sim/Makefile:65` is the pattern to copy), `sim/build_config.mk` carries it, and the help text at `sim/Makefile:25` gains it beside the other run variables. Default enabled, so a plain `make -C sim TB=... PATTERN=...` runs the mode the spec describes.

Then remove the filename mechanism: the suffix strip at `gen_tb_top.py:56`, `$(TOPOLOGY:_rob=)` in `sim/build_config.mk`, the suffix's mention in `sim/Makefile`'s help, and any topology file whose name carries it, whose callers follow. Grep `_rob` across `sim/` and `docs/` and deal with every hit. `sim-injection-sweep` is the only consumer of the `_rob` topology and it moves to the plain name plus the new variable.

Both halves matter: leaving the filename path in beside the parameter would give the mode two controls that can disagree.

- [ ] **Step 5: Run the full C++ suite**

```
wsl -e bash -lc 'cd ~/noc_project && BUILD_ROOT=$HOME/noc_build PYTHON3=python3 make test 2>&1 | tail -5'
```
Expected: **669, unchanged plus the one new test, so 670.**

21 cases inherit the default and the design establishes that none asserts anything that necessarily changes: they use single ARs, distinct IDs, or completion and readback assertions rather than exact latency or the `Disabled` same-ID interlock. **A test that moves is a finding, not a re-baseline.** If one moves, stop and report it with the assertion and both values rather than adjusting it.

- [ ] **Step 6: Rewrite the known-limitation that this wakes**

`docs/known-limitations.md` carries a row saying the DAT deadlock argument does not cover a bypassed read, whose "when it bites" column says it bites *only* in a `RobMode::Enabled` build and that nothing constructs one. Half of that is now false. Rewrite the column: an `Enabled` build is now every build, and what still keeps the cycle out of reach is that no consumer backpressures R — which Stage B changes. Name Stage B as the thing that closes the second condition.

- [ ] **Step 7: Commit**

```bash
git add ref_model/ sim/ docs/known-limitations.md
git commit -m "feat(nmu): make the read reorder buffer the default"
```

### Task 2: Re-baseline the six gates

The flip moves the occupancy counters the generated top prints, and moves R ordering and latency only where same-ID reads overlap. The scoreboard compares read data against golden writes and is not a latency or occupancy checker, so this is reading new numbers, not re-deriving expectations.

**Files:**
- Modify: `docs/backlog.md` (gitignored; the record of before and after)

**Interfaces:**
- Consumes: Task 1's flip.
- Produces: a recorded before-and-after for the six gates, so Stage B's failures are separable from Stage A's.

- [ ] **Step 1: Capture the six gates**

Each in the foreground, waiting for each, with the pre-clean:

```
BUILD_ROOT=$HOME/noc_build PYTHON3=python3 make -C sim TB=mesh_2x2_vc1_periph PATTERN=neighbor
BUILD_ROOT=$HOME/noc_build PYTHON3=python3 make -C sim TB=mesh_2x2_vc1_periph PATTERN=multicast
BUILD_ROOT=$HOME/noc_build PYTHON3=python3 make -C sim TB=mesh_2x2_vc1 PATTERN=neighbor
BUILD_ROOT=$HOME/noc_build PYTHON3=python3 make -C sim TB=mesh_2x2_vc1 PATTERN=multicast
BUILD_ROOT=$HOME/noc_build PYTHON3=python3 make -C sim TB=mesh_4x4_vc1 PATTERN=neighbor
BUILD_ROOT=$HOME/noc_build PYTHON3=python3 make -C sim TB=mesh_4x4_vc1 PATTERN=multicast
```

Expected: all six DIRECTED PASS, scoreboard clean, non-vacuous.

- [ ] **Step 2: Record what moved**

The `[HWM]` line is filtered out of console output and exists only in `run.log`. For each gate record `read_slot_hwm`, `order_list_hwm`, `write_txns_hwm`, `read_txns_hwm` and the AR admission clause counters, before and after.

Expect the AR clause counters to move most: `Disabled` reads do not count AR admission branches at all, so those go from zero to non-zero. That is the flip working, not a regression.

- [ ] **Step 3: Report, do not fix**

If a gate fails, report the failing output verbatim and stop. A scoreboard mismatch after this flip is a real finding — the design establishes the scoreboard should be unaffected — and it is the whole reason the two changes are staged.

- [ ] **Step 4: Commit the record**

`docs/backlog.md` is gitignored, so there is nothing to commit for it. If Step 2 required a code change, that change is its own commit with its own message.

---

## Stage B: the DMA arrives

### Task 3: Vendor iDMA and prove it elaborates

**Files:**
- Create: `sim/dv/idma-0.6.5/` — 12 files
- Modify: `sim/dv/axi-0.39.7/src/` — add `axi_rw_join.sv`
- Modify: `sim/dv/common_cells-1.37.0/src/` — add 5 files
- Modify: `sim/dv/README.md` — one new row, two amended rows
- Create: `sim/tb/dma/idma_types_pkg.sv`

**Interfaces:**
- Produces: `idma_types_pkg`, exporting `idma_req_t`, `idma_rsp_t`, `axi_req_t`, `axi_resp_t`, `read_meta_channel_t`, `write_meta_channel_t`, and the widths below. Tasks 5 and 6 import it.

- [ ] **Step 1: Clone the three upstreams at their pinned tags**

```bash
git clone --depth 1 --branch v0.6.5  https://github.com/pulp-platform/idma
git clone --depth 1 --branch v1.37.0 https://github.com/pulp-platform/common_cells
git clone --depth 1 --branch v0.39.7 https://github.com/pulp-platform/axi
```

**Pin `v0.6.5`, not `v0.6.5-src`.** iDMA publishes two tags per release; only the first commits the generated RTL under `target/rtl/`, and regenerating needs bender, which this repo has not got.

- [ ] **Step 2: Take the files with `git show`, never `cp`**

From the iDMA clone, into `sim/dv/idma-0.6.5/`:

```
src/idma_pkg.sv
src/include/idma/typedef.svh
src/include/idma/guard.svh
target/rtl/idma_backend_rw_axi.sv
target/rtl/idma_legalizer_rw_axi.sv
target/rtl/idma_transport_layer_rw_axi.sv
src/backend/idma_axi_read.sv
src/backend/idma_axi_write.sv
src/backend/idma_dataflow_element.sv
src/backend/idma_error_handler.sv
src/backend/idma_channel_coupler.sv
src/backend/idma_legalizer_page_splitter.sv
```

That is 12. From the common_cells clone into `sim/dv/common_cells-1.37.0/src/`: `stream_fork.sv`, `stream_fifo_optimal_wrap.sv`, `stream_fifo.sv`, `fall_through_register.sv`, `passthrough_stream_fifo.sv`. From the axi clone into `sim/dv/axi-0.39.7/src/`: `axi_rw_join.sv`.

If elaboration in Step 4 names a module not in this list, add it and say so in your report — the closure was verified by grepping instantiations and includes, but a miss is possible and is worth recording rather than silently patching.

- [ ] **Step 3: Write the type binding**

`sim/tb/dma/idma_types_pkg.sv`. The skeleton is already written and verified: `.superpowers/sdd/ai-dataflow/idma_lint_top.sv` carries the AXI typedefs, the four iDMA typedef macros, the two meta-channel structs and the parameter map, at this project's widths. Lift its type section into a package.

Widths, and where each comes from:

| | value | source |
|---|---|---|
| `DataWidth` | 512 | `ni_params_pkg::AXI_DATA_WIDTH_DFLT` |
| `AddrWidth` | 48 | the spec's AXI address width |
| `AxiIdWidth` | 4 | `ni_params_pkg::AXI_ID_WIDTH_DFLT`, the tile crossbar's slave port |
| `UserWidth` | 58 | the spec's `AWUSER` width |
| `TFLenWidth` | 20 | covers a whole `0x100000` tile window; the bound is 12 to `AddrWidth` |

- [ ] **Step 4: Prove it elaborates**

```
wsl -e bash -lc 'cd ~/noc_project && verilator --lint-only --timing -Wno-fatal -Isim/dv/axi-0.39.7/include -Isim/dv/common_cells-1.37.0/include -Isim/dv/idma-0.6.5 --top-module idma_lint_top sim/dv/axi-0.39.7/src/*.sv sim/dv/common_cells-1.37.0/src/*.sv sim/dv/idma-0.6.5/*.sv .superpowers/sdd/ai-dataflow/idma_lint_top.sv 2>&1 | grep -c "%Error"'
```
Expected: `0`. This was run before vendoring and produced 0 errors with 8 ASCRANGE, 23 WIDTHEXPAND and 5 WIDTHTRUNC warnings; the ASCRANGE ones come from `axi_intf.sv`, which the closure does not use.

- [ ] **Step 5: Record the vendoring**

`sim/dv/README.md` gains a row for `idma-0.6.5` with its upstream, tag, the 12-file list and its licence (Solderpad 0.51). The `axi-0.39.7` and `common_cells-1.37.0` rows gain their new files. Match the table's existing shape.

- [ ] **Step 6: Commit**

```bash
git add sim/dv/ sim/tb/dma/idma_types_pkg.sv
git commit -m "build(dv): vendor pulp iDMA v0.6.5 and its closure"
```

### Task 4: The job emitter

Independently testable with no SystemVerilog. FlooNoC's job file is ten plain-text fields; this adds an eleventh.

**Files:**
- Create: `sim/tools/gen_dma_jobs.py`
- Test: `sim/tools/test_gen_dma_jobs.py`

**Interfaces:**
- Consumes: `address_map.pack(address_map, x_span, y_span)` from `sim/tools/address_map.py`, and `gen_tb_top._route_span(topology_dict)`.
- Produces: `<out>/node<i>/jobs.txt` per endpoint, eleven lines per job, in this order: `length`, `src_addr`, `dst_addr`, `src_protocol`, `dst_protocol`, `max_src_len`, `max_dst_len`, `aw_decoupled`, `rw_decoupled`, `num_errors`, `axi_id`.

- [ ] **Step 1: Write the failing test**

```python
def test_every_job_addresses_a_real_sam_region(tmp_path):
    """A job's src and dst are SAM addresses, so both must land inside a
    region the address map actually declares. FlooNoC's generator has no
    knowledge of this map, which is why the emitter is written here rather
    than ported."""
    out = tmp_path / "jobs"
    g.main(["--topology", "mesh_2x2_vc1", "--out", str(out), "--jobs-per-node", "4"])
    _bases, entries = address_map.pack(_topology("mesh_2x2_vc1")["address_map"], 2, 2)
    windows = [(e["base"], e["base"] + e["size"]) for e in entries]
    for node in range(4):
        for job in _parse_jobs(out / f"node{node}" / "jobs.txt"):
            assert any(lo <= job["src_addr"] < hi for lo, hi in windows)
            assert any(lo <= job["dst_addr"] < hi for lo, hi in windows)


def test_jobs_use_more_than_one_axi_id(tmp_path):
    """FlooNoC leaves idma_job.id at '0, so every transfer uses one ID. This
    NMU keeps a reorder-buffer slot and a meta-buffer bucket per ID, so a
    single-ID stream would report that a DMA runs while touching one of
    eight."""
    out = tmp_path / "jobs"
    g.main(["--topology", "mesh_2x2_vc1", "--out", str(out), "--jobs-per-node", "8"])
    ids = {job["axi_id"] for job in _parse_jobs(out / "node0" / "jobs.txt")}
    assert len(ids) > 1
```

- [ ] **Step 2: Run them and watch them fail**

```
wsl -e bash -lc 'cd ~/noc_project && python3 -m pytest sim/tools/test_gen_dma_jobs.py -q 2>&1 | tail -5'
```
Expected: FAIL, no module `gen_dma_jobs`.

- [ ] **Step 3: Write the emitter**

Derive addresses from the topology through `address_map.pack`, never by restating the base formula — `gen_test_patterns.py` and `address_map.py` are already a bit-identical pair and a third copy of the arithmetic is how they stop being one.

Each node's jobs move a region from its own memory window to a remote node's, so the traffic crosses the fabric on the write path. `axi_id` cycles over the ID space so the per-ID structures are all reached. `max_src_len` and `max_dst_len` take the AXI maximum of 255; `aw_decoupled` and `rw_decoupled` are 0; `num_errors` is 0; both protocols are the AXI enumerator.

- [ ] **Step 4: Run them and watch them pass**

```
wsl -e bash -lc 'cd ~/noc_project && python3 -m pytest sim/tools -q 2>&1 | tail -3'
```
Expected: 71 passed — 69 plus the two new.

- [ ] **Step 5: Commit**

```bash
git add sim/tools/gen_dma_jobs.py sim/tools/test_gen_dma_jobs.py
git commit -m "feat(sim): emit iDMA job files from the topology's address map"
```

### Task 5: The endpoint, the driver and the top

**Files:**
- Create: `sim/tb/dma/dma_node_endpoint.sv`
- Create: `sim/tb/dma/idma_job_driver.sv`
- Modify: `sim/tools/gen_tb_top.py` — a `--dma` flag emitting `sim/tb/dma/tb_top_dma_<topo>.sv`
- Modify: `sim/Makefile`, `sim/build_config.mk` — a `DMA=1` path

**Interfaces:**
- Consumes: `idma_types_pkg` from Task 3, the job file format from Task 4.
- Produces: a top that elaborates. It does not yet check anything; Task 6 adds that.

- [ ] **Step 1: Write the endpoint**

`dma_node_endpoint.sv` is `user_node_endpoint.sv` with the master swapped and the two-phase machinery removed. Keep: the tile crossbar, the per-space memories, the `axi_delayer` in front of each, the link and bandwidth monitors. Drop: `axi_file_master`, the injection-mode `case`, `run_ar_after_b`, the `b_total != file_master.num_writes` epilogue, and the scoreboard — all four are named dependencies on the file master, and the scoreboard's write-then-read golden model does not hold for a DMA.

Add in their place:

```systemverilog
    idma_backend_rw_axi #(
        .DataWidth            ( idma_types_pkg::DataWidth  ),
        .AddrWidth            ( idma_types_pkg::AddrWidth  ),
        .UserWidth            ( idma_types_pkg::UserWidth  ),
        .AxiIdWidth           ( idma_types_pkg::AxiIdWidth ),
        .NumAxInFlight        ( 32'd64 ),   // MaxMstTrans, what the crossbar allows one initiator
        .BufferDepth          ( 32'd3  ),   // iDMA's own recommendation for misaligned transfers
        .TFLenWidth           ( idma_types_pkg::TFLenWidth ),
        .MemSysDepth          ( 32'd0  ),   // no round-trip constant exists here yet
        .RAWCouplingAvail     ( 1'b1   ),
        .MaskInvalidData      ( 1'b1   ),
        .HardwareLegalizer    ( 1'b1   ),
        .RejectZeroTransfers  ( 1'b1   ),
        .ErrorCap             ( idma_pkg::NO_ERROR_HANDLING ),
        .idma_req_t           ( idma_types_pkg::idma_req_t  ),
        .idma_rsp_t           ( idma_types_pkg::idma_rsp_t  ),
        .idma_eh_req_t        ( idma_pkg::idma_eh_req_t ),
        .idma_busy_t          ( idma_pkg::idma_busy_t   ),
        .axi_req_t            ( idma_types_pkg::axi_req_t  ),
        .axi_rsp_t            ( idma_types_pkg::axi_resp_t ),
        .read_meta_channel_t  ( idma_types_pkg::read_meta_channel_t  ),
        .write_meta_channel_t ( idma_types_pkg::write_meta_channel_t )
    ) i_dma (
        .clk_i, .rst_ni,
        .testmode_i      ( 1'b0          ),
        .idma_req_i      ( job_req       ),   // from idma_job_driver
        .req_valid_i     ( job_req_valid ),
        .req_ready_o     ( job_req_ready ),
        .idma_rsp_o      ( job_rsp       ),
        .rsp_valid_o     ( job_rsp_valid ),
        .rsp_ready_i     ( 1'b1          ),
        .idma_eh_req_i   ( '0            ),   // ErrorCap is NO_ERROR_HANDLING
        .eh_req_valid_i  ( 1'b0          ),
        .eh_req_ready_o  (               ),
        .axi_read_req_o  ( dma_read_req  ),
        .axi_read_rsp_i  ( dma_read_rsp  ),
        .axi_write_req_o ( dma_write_req ),
        .axi_write_rsp_i ( dma_write_rsp ),
        .busy_o          ( dma_busy      )
    );

    axi_rw_join #(
        .axi_req_t  ( idma_types_pkg::axi_req_t  ),
        .axi_resp_t ( idma_types_pkg::axi_resp_t )
    ) i_rw_join (
        .clk_i, .rst_ni,
        .slv_read_req_i   ( dma_read_req  ), .slv_read_resp_o  ( dma_read_rsp  ),
        .slv_write_req_i  ( dma_write_req ), .slv_write_resp_o ( dma_write_rsp ),
        .mst_req_o        ( dma_req       ), .mst_resp_i       ( dma_rsp       )
    );
```

`dma_req` / `dma_rsp` go where `axi_file_master`'s port went, into the tile crossbar's slave port.

**Connect the memory write monitors.** `user_node_endpoint.sv:552-553` leaves `mon_w_valid_o` and `mon_w_last_o` unconnected on each `axi_sim_mem_intf`. Bring them out of the endpoint; Task 6's check waits on them.

- [ ] **Step 2: Write the job driver**

`idma_job_driver.sv` opens `<stim_dir>/node<i>/jobs.txt`, reads eleven fields per job with `$fscanf`, fills an `idma_types_pkg::idma_req_t` and drives it with valid/ready. It counts jobs issued and jobs retired, and exposes both; Task 6 uses the counts for completion.

`+stim_dir=` already exists as a plusarg and keeps working, so a run changes stimulus without recompiling. Do not fill the struct from an `initial` block instead: that makes the stimulus a compile-time constant and loses that.

- [ ] **Step 3: Emit the top**

`gen_tb_top.py` gains a `--dma` flag. Under it, it emits `sim/tb/dma/tb_top_dma_<topo>.sv` instead of `sim/tb/tb_top_<topo>.sv`, instantiating `dma_node_endpoint` where it instantiates `user_node_endpoint`. The generated fabric under `ref_model/top/` is emitted unchanged and shared — the endpoint's port list toward the fabric does not move.

**Every shipped topology's `tb_top_*.sv`, `noc_fabric_*.sv` and stimulus must stay bit-identical when `--dma` is absent.** Verify by regenerating both ways and diffing, not by inspection.

- [ ] **Step 4: Elaborate it**

```
wsl -e bash -lc 'cd ~/noc_project && rm -f sim/filelist_*.f sim/tb/tb_top_*.sv && rm -rf $BUILD_ROOT/verilator/obj_dir_* && BUILD_ROOT=$HOME/noc_build PYTHON3=python3 make -C sim TB=mesh_2x2_vc1 DMA=1 ELABORATE_ONLY=1 2>&1 | tail -20'
```
Expected: elaborates. It will not pass a run yet — nothing checks anything and nothing decides when the run ends. That is Task 6.

If `ELABORATE_ONLY` does not exist in `sim/verilator/Makefile`, add it rather than running a full simulation to find out whether it compiles.

- [ ] **Step 5: Commit**

```bash
git add sim/tb/dma/ sim/tools/gen_tb_top.py sim/Makefile sim/build_config.mk
git commit -m "feat(sim): put iDMA on the master face of a separate top"
```

### Task 6: The check, and the gate

**Files:**
- Modify: `sim/tb/dma/tb_top_dma_<topo>.sv` via `gen_tb_top.py` — the compare and the exit logic
- Modify: `sim/Makefile` — the gate's help text

**Interfaces:**
- Consumes: everything above.
- Produces: a green gate.

- [ ] **Step 1: Preload the source regions**

Each node's memory is preloaded with a pattern that is a function of the byte's own address, so a byte landing at the wrong offset is caught rather than matching by luck. Write it through the same hierarchical path the compare reads, from the top's `initial` block.

- [ ] **Step 2: Write the compare**

The path is verified to work under Verilator 5.048 — a probe of this exact shape, two generate levels deep, reading and writing the associative array from a function naming two different iterations, elaborates with zero errors:

```systemverilog
    function automatic int unsigned compare_region(input int unsigned src_ep, input int unsigned src_t,
                                                   input int unsigned dst_ep, input int unsigned dst_t,
                                                   input logic [47:0] src_base,
                                                   input logic [47:0] dst_base,
                                                   input int unsigned n_bytes);
        int unsigned bad = 0;
        for (int unsigned k = 0; k < n_bytes; k++) begin
            logic [7:0] a = g_endpoint[src_ep].u_endpoint.g_tile_mem[src_t].i_mem.i_sim_mem.mem[src_base + k];
            logic [7:0] b = g_endpoint[dst_ep].u_endpoint.g_tile_mem[dst_t].i_mem.i_sim_mem.mem[dst_base + k];
            if (a !== b) begin
                if (bad < 8) $display("[DMA] mismatch ep%0d+%0h = %02h, ep%0d+%0h = %02h",
                                      src_ep, src_base + k, a, dst_ep, dst_base + k, b);
                bad++;
            end
        end
        return bad;
    endfunction
```

- [ ] **Step 3: Decide when to compare**

iDMA completes on the write response, not on the last W beat, and `axi_sim_mem` writes its array on W acceptance and queues B only after the last W, so the data is already there when the DMA reports. The compare still waits on the destination memory's own `mon_w_valid_o && mon_w_last_o`, brought out in Task 5, so it waits on an observed event rather than on that argument.

- [ ] **Step 4: Decide when the run ends**

The run ends when every DMA has retired every job its file holds. A DMA that retires fewer is a failure — this is the DMA-side equivalent of the existing vacuity check, and without it an empty job file would pass.

```systemverilog
        if (jobs_retired[i] != jobs_issued[i])
            $fatal(1, "tb_top_dma: node%0d retired %0d of %0d jobs", i, jobs_retired[i], jobs_issued[i]);
```

- [ ] **Step 5: Run the gate**

```
wsl -e bash -lc 'cd ~/noc_project && rm -f sim/filelist_*.f sim/tb/tb_top_*.sv && rm -rf $BUILD_ROOT/verilator/obj_dir_* && BUILD_ROOT=$HOME/noc_build PYTHON3=python3 make -C sim TB=mesh_2x2_vc1 DMA=1 2>&1 | tail -20'
```
Expected: every job retires and every destination region matches its source.

**If it hangs, that is a result, not a failure to work around.** The design records why: `RobMode::Enabled` is now every build, and iDMA backpressures R when its write path cannot drain, which are exactly the two conditions `docs/known-limitations.md` says keep the DAT deadlock argument's gap out of reach. Report the last flit each side was waiting on and stop. Do not adjust the job sizes, the buffer depth or the outstanding count to make it pass.

- [ ] **Step 6: Prove the check can fail**

A check nobody has watched fail is not yet a check. Corrupt one byte of a destination region after the transfer and before the compare, confirm the run fails and names the address, then revert and confirm it passes.

- [ ] **Step 7: Run every gate**

```
BUILD_ROOT=$HOME/noc_build PYTHON3=python3 make test
python3 -m pytest sim/tools
```
plus the six existing co-sim gates. Expected: ctest 670, pytest 71, six gates DIRECTED PASS with Task 2's re-baselined numbers, and the DMA gate green.

- [ ] **Step 8: Commit**

```bash
git add sim/tools/gen_tb_top.py sim/Makefile
git commit -m "test(sim): check a DMA moved each region intact"
```

---

## Deferred, recorded so the round closes clean

- **Collectives from the DMA.** `idma_req_t.user` reaches `aw_user`, verified, so the path exists. Using it needs the job format to carry an `AWUSER` and the emitter to build a legal collective mask.
- **`MemSysDepth`.** Starts at 0. Settling it means measuring cycles from issue at the joined port to return, across DMA, NI, fabric, NI, crossbar, delayer, memory. It costs throughput, not correctness.
- **`BufferDepth` above 3.** FlooNoC runs 16. At 64 FIFOs per DMA the difference is not free, and whether 3 stalls this fabric is a measurement nobody has taken.
- **Retiring `axi_file_master`.** Both masters exist, in separate tops. Retiring the old one would re-baseline all six gates and lose `beat_exact`, whose hand-written transactions check DPI word packing that a legalizer would take over.
- **AI dataflow patterns.** A traffic generator on top of the job emitter, and the round after this one.
