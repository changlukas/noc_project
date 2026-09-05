# 2-Channel vs 3-Channel Latency Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a deterministic end-to-end comparison of shared REQ/RSP mapping against a separate DAT mapping, then show Control completion latency in the concise performance report.

**Architecture:** Keep the existing Verilator top and C++ NI/Router models. Add a runtime NI channel mode with native behavior as the default; the two comparison modes encode Data-memory W/R payloads as 64-bit while preserving the Data AXI class used to select Data memory. Add one directed test mode that starts a single-beat Control transfer and a 256-beat Data burst together with random stalls disabled.

**Tech Stack:** C++17, SystemVerilog, DPI-C, Verilator, Python 3, GoogleTest, pytest.

**Spec:** `docs/superpowers/specs/2026-08-28-channel-count-latency-design.md`

## Global Constraints

- Do not modify Router routing or arbitration.
- `Native` mode must preserve the current 64-bit Control and 512-bit Data behavior.
- Comparison modes use 64-bit Data payloads; physical REQ/RSP/DAT signal widths remain unchanged.
- `2-channel` means Data-memory requests use REQ and responses use RSP; DAT is idle.
- `3-channel` means Data-memory requests and responses use DAT.
- Control is `AxLEN=0`; Data is `AxLEN=255`, `AxSIZE=3`.
- No random endpoint stalls, load sweep, warm-up, fixed measurement window, seed sweep, or averaging.
- Do not add dependencies or change generated parameter defaults.

---

### Task 1: Add normalized NI channel modes

**Files:**
- Create: `ref_model/c_model/include/ni/channel_mode.hpp`
- Modify: `ref_model/c_model/include/nmu/packetize.hpp`
- Modify: `ref_model/c_model/include/nmu/depacketize.hpp`
- Modify: `ref_model/c_model/include/nmu/nmu.hpp`
- Modify: `ref_model/c_model/include/nmu/nmu_standalone.hpp`
- Modify: `ref_model/c_model/include/nsu/packetize.hpp`
- Modify: `ref_model/c_model/include/nsu/depacketize.hpp`
- Modify: `ref_model/c_model/include/nsu/nsu.hpp`
- Modify: `ref_model/c_model/include/nsu/nsu_standalone.hpp`
- Test: `ref_model/c_model/tests/nmu/test_nmu_dat_face.cpp`
- Test: `ref_model/c_model/tests/nmu/test_depacketize.cpp`
- Test: `ref_model/c_model/tests/nsu/test_nsu_packetize.cpp`
- Test: `ref_model/c_model/tests/nsu/test_nsu.cpp`

**Interfaces:**
- Produces: `ni::cmodel::ni::ChannelMode { Native, TwoChannel64, ThreeChannel64 }`.
- Produces: `Nmu::set_channel_mode(ChannelMode)` and `Nsu::set_channel_mode(ChannelMode)` before traffic begins.
- Preserves: existing constructors and `Native` behavior.

- [ ] **Step 1: Write failing NMU steering and payload tests**

Add cases proving:

```cpp
// TwoChannel64: DataW/DataAr leave through REQ; DAT stays empty.
// ThreeChannel64: DataW/DataAr leave through DAT; REQ carries only Control.
// Both modes: one Data W beat encodes exactly 64 WDATA bits and 8 WSTRB bits.
```

Use `axi_ch == DataW/DataAr` to retain the Data-memory class. Assert the 64-bit W data round-trips without reading the native 512-bit `data_w` offsets.

- [ ] **Step 2: Run the focused tests and confirm failure**

```bash
cmake --build build/cmodel --target test_nmu_dat_face test_depacketize test_nsu_packetize test_nsu
ctest --test-dir build/cmodel -R 'test_nmu_dat_face|test_depacketize|test_nsu_packetize|test_nsu' --output-on-failure
```

Expected: new channel-mode tests fail because the mode and normalized encode/decode paths do not exist.

- [ ] **Step 3: Implement the minimum mode-dependent paths**

Define:

```cpp
enum class ChannelMode : uint8_t {
    Native = 0,
    TwoChannel64 = 2,
    ThreeChannel64 = 3,
};
```

Implement these rules without changing native branches:

```text
TwoChannel64:   Data AW/W/AR -> REQ, Data B/R -> RSP
ThreeChannel64: Data AW/W/AR -> DAT, Data B/R -> DAT
```

For `TwoChannel64` and `ThreeChannel64`, encode/decode DataW with the `narrow_w` payload layout and DataR with the `narrow_r` payload layout while retaining `axi_ch=DataW/DataR`. Allow Data-class ingress on the selected physical channel. Add DataAR to the DAT request arbiter and DataB to the DAT response arbiter only for `ThreeChannel64`.

- [ ] **Step 4: Run focused and integration tests**

```bash
cmake --build build/cmodel --target test_nmu_dat_face test_depacketize test_nsu_packetize test_nsu test_request_response_loopback
ctest --test-dir build/cmodel -R 'test_nmu_dat_face|test_depacketize|test_nsu_packetize|test_nsu|test_request_response_loopback' --output-on-failure
```

Expected: all selected tests pass; existing native-format assertions remain unchanged.

- [ ] **Step 5: Commit**

```bash
git add ref_model/c_model/include/ni/channel_mode.hpp ref_model/c_model/include/nmu ref_model/c_model/include/nsu ref_model/c_model/tests/nmu ref_model/c_model/tests/nsu
git commit -m "feat(cmodel): add normalized channel mappings"
```

---

### Task 2: Expose channel mode through DPI and Verilator

**Files:**
- Modify: `ref_model/c_model/include/wrap/nmu_wrap.hpp`
- Modify: `ref_model/c_model/include/wrap/nsu_wrap.hpp`
- Modify: `ref_model/dpi/cmodel_dpi.cpp`
- Modify: `sim/tb/noc_tb_top.sv`
- Modify: `sim/tests/model_egress/fake_nmu_dpi.cpp`
- Test: `ref_model/c_model/tests/wrap/test_cmodel_dpi.cpp`

**Interfaces:**
- Produces: `cmodel_nmu_set_channel_mode(ctx, mode)`.
- Produces: `cmodel_nsu_set_channel_mode(ctx, mode)`.
- Consumes: `+channel_mode=0|2|3`; omitted means `Native`.

- [ ] **Step 1: Write failing DPI tests**

Add lifecycle and validation cases:

```cpp
EXPECT_NO_FATAL_FAILURE(cmodel_nmu_set_channel_mode(nmu, 2));
EXPECT_NO_FATAL_FAILURE(cmodel_nsu_set_channel_mode(nsu, 3));
cmodel_nmu_set_channel_mode(nmu, 1);  // must set DPI error
```

Also assert that changing mode after the first tick is rejected, matching a hardware configuration register that must be programmed before enable.

- [ ] **Step 2: Run the test and confirm failure**

```bash
cmake --build build/cmodel --target test_cmodel_dpi
ctest --test-dir build/cmodel -R test_cmodel_dpi --output-on-failure
```

Expected: compile or link failure for the missing DPI setters.

- [ ] **Step 3: Implement setters and plusarg propagation**

Parse `channel_mode` once in `noc_tb_top.sv`, validate it is `0`, `2`, or `3`, and call both setters for every NMU/NSU handle after creation and before reset release. Keep mode `0` as the default so normal simulations do not change.

- [ ] **Step 4: Run DPI tests**

```bash
cmake --build build/cmodel --target test_cmodel_dpi
ctest --test-dir build/cmodel -R test_cmodel_dpi --output-on-failure
```

Expected: PASS, including invalid-mode and configure-after-tick cases.

- [ ] **Step 5: Commit**

```bash
git add ref_model/c_model/include/wrap ref_model/dpi/cmodel_dpi.cpp ref_model/c_model/tests/wrap/test_cmodel_dpi.cpp sim/tb/noc_tb_top.sv sim/tests/model_egress/fake_nmu_dpi.cpp
git commit -m "feat(sim): select channel mapping at runtime"
```

---

### Task 3: Generate and synchronize the deterministic traffic

**Files:**
- Modify: `sim/tools/gen_test_patterns.py`
- Modify: `sim/tools/test_gen_test_patterns_filemaster.py`
- Modify: `sim/tb/test/user_node_endpoint.sv`
- Modify: `sim/tools/gen_tb_top.py`
- Test: `sim/tools/test_gen_tb_top.py`

**Interfaces:**
- Consumes: `--pattern channel_compare --channel-case write|read`.
- Produces: node 0 Control files, node 1 Data files, and empty files for other initiators.
- Consumes: `+injection_mode=3` and `+channel_case=write|read`.
- Produces log line: `[ChannelCompare] case=<read|write> node=<n> start=<cycle> complete=<cycle> latency=<cycles>`.

- [ ] **Step 1: Write failing generator tests**

For `mesh_4x4`, assert:

```text
node0: node 3 Control-memory address, AxLEN=0, AxSIZE=3
node1: node 3 Data-memory address, AxLEN=255, AxSIZE=3
node2..node15: empty write/read files
```

For the read case, include preload writes for both addresses before the measured reads. Reject `channel_compare` on a topology that does not contain nodes 0, 1, and 3 or lacks either SAM range.

- [ ] **Step 2: Run Python tests and confirm failure**

```bash
python3 -m pytest sim/tools/test_gen_test_patterns_filemaster.py sim/tools/test_gen_tb_top.py -q
```

Expected: failure because `channel_compare`, `channel_case`, and the barrier ports do not exist.

- [ ] **Step 3: Implement mode 3 and the common start barrier**

Extend the generated top and endpoint with comparison-only signals:

```systemverilog
logic compare_ready_o;
logic compare_start_i;
logic compare_done_o;
```

Behavior:

```text
write case: load files -> ready -> common start -> AW/W -> B completion
read case:  preload AW/W -> B completion -> ready -> common start -> AR -> R completion
idle node:  ready=1 and done=1 without issuing traffic
```

At the common start, node 0 and node 1 begin their AXI operations on the same cycle. Latch node 0 completion on B for write or RLAST for read. Disable the random master backpressure and memory delay in this mode. The generated top must apply the non-vacuous check only to nodes 0 and 1 when `injection_mode=3`.

- [ ] **Step 4: Add exact beat-count checks**

Count node 1 AXI W handshakes in the write case and node 1 AXI R handshakes in the read case. Fatal unless the count is exactly 256. Unit tests from Task 1 prove the corresponding NI path emits/accepts one normalized Data flit per data beat.

- [ ] **Step 5: Run Python tests**

```bash
python3 -m pytest sim/tools/test_gen_test_patterns_filemaster.py sim/tools/test_gen_tb_top.py -q
```

Expected: PASS for both cases, address classes, lengths, idle nodes, and generated barrier wiring.

- [ ] **Step 6: Commit**

```bash
git add sim/tools/gen_test_patterns.py sim/tools/test_gen_test_patterns_filemaster.py sim/tb/test/user_node_endpoint.sv sim/tools/gen_tb_top.py sim/tools/test_gen_tb_top.py
git commit -m "test(sim): add deterministic channel contention traffic"
```

---

### Task 4: Add run targets and machine-readable results

**Files:**
- Modify: `sim/verilator/Makefile`
- Modify: `sim/tools/emit_result_csv.py`
- Modify: `sim/tools/test_emit_result_csv.py`

**Interfaces:**
- Consumes: `CHANNEL_MODE=2|3` and `CHANNEL_CASE=write|read`.
- Produces run tags: `channel_compare_2ch_write`, `channel_compare_2ch_read`, `channel_compare_3ch_write`, `channel_compare_3ch_read`.
- Produces CSV fields: `channel_mapping`, `channel_case`, `control_start_cycle`, `control_completion_cycle`, `control_completion_latency`.

- [ ] **Step 1: Write failing parser tests**

Use a log fixture containing:

```text
[ChannelCompare] case=write node=0 start=12 complete=301 latency=289
```

Assert exact integer CSV values and reject missing, duplicate, negative, or inconsistent `complete - start` records.

- [ ] **Step 2: Run the parser tests and confirm failure**

```bash
python3 -m pytest sim/tools/test_emit_result_csv.py -q
```

Expected: failure because the comparison fields are absent.

- [ ] **Step 3: Implement Makefile validation and CSV parsing**

Add a dedicated `channel-compare-gen` and `channel-compare-run` path that reuses the normal Verilator binary and passes `injection_mode=3`. Reject other `CHANNEL_MODE` or `CHANNEL_CASE` values before simulation. Require one valid node 0 marker and the normal scoreboard/PASS checks.

- [ ] **Step 4: Run parser tests**

```bash
python3 -m pytest sim/tools/test_emit_result_csv.py -q
```

Expected: PASS for valid and invalid log fixtures.

- [ ] **Step 5: Commit**

```bash
git add sim/verilator/Makefile sim/tools/emit_result_csv.py sim/tools/test_emit_result_csv.py
git commit -m "feat(perf): record channel comparison latency"
```

---

### Task 5: Render the concise report and run acceptance

**Files:**
- Modify: `sim/tools/perf_report.py`
- Modify: `sim/tools/test_perf_report.py`
- Generate: `perf_report.md`
- Update: `docs/noc-performance-parameters.md`
- Update: `docs/trade-off.md`

**Interfaces:**
- Consumes the four `channel_compare_*` result directories.
- Produces a two-row `2-channel vs 3-channel` table with direct labels and integer cycle values.

- [ ] **Step 1: Write failing report tests**

Assert the report contains:

```markdown
| Case | 2-channel latency | 3-channel latency | Difference |
| Control write with 256-beat Data write | ... |
| Control read with 256-beat Data read | ... |
```

Also assert it does not contain `s2zl005_data`, internal run tags, offered-load columns, seeds, or bandwidth/link-utilization columns in this section.

- [ ] **Step 2: Run report tests and confirm failure**

```bash
python3 -m pytest sim/tools/test_perf_report.py -q
```

Expected: failure because the deterministic comparison section is absent.

- [ ] **Step 3: Implement the concise report changes**

Render human-readable zero-load labels, place per-stage latency beside zero-load results, keep all available native-width traffic patterns, and keep max link utilization in the appendix. The comparison section reports only Control completion latency and the cycle difference. State once that it is a deterministic event and includes the implemented flow-control/buffering differences.

- [ ] **Step 4: Run focused tests, build, and the four simulations**

```bash
python3 -m pytest sim/tools/test_gen_test_patterns_filemaster.py sim/tools/test_gen_tb_top.py sim/tools/test_emit_result_csv.py sim/tools/test_perf_report.py -q
cmake --build build/cmodel
ctest --test-dir build/cmodel --output-on-failure
make -C sim/verilator channel-compare-run CONFIG=mesh_4x4 CHANNEL_MODE=2 CHANNEL_CASE=write
make -C sim/verilator channel-compare-run CONFIG=mesh_4x4 CHANNEL_MODE=3 CHANNEL_CASE=write
make -C sim/verilator channel-compare-run CONFIG=mesh_4x4 CHANNEL_MODE=2 CHANNEL_CASE=read
make -C sim/verilator channel-compare-run CONFIG=mesh_4x4 CHANNEL_MODE=3 CHANNEL_CASE=read
python3 sim/tools/perf_report.py sim/verilator/output --out perf_report.md
```

Expected: all tests pass; all four runs complete with exactly 256 Data beats; `perf_report.md` contains the two comparison rows and no internal probe tags.

- [ ] **Step 5: Record the architecture and document references**

Update `docs/trade-off.md` with the implemented mapping and its flow-control/buffering limitation. Update `docs/noc-performance-parameters.md` with the exact single-beat/256-beat experiment definition. List every Markdown file referencing `ChannelMode`, the 64-bit normalization, or the comparison table and resolve mismatches before commit.

- [ ] **Step 6: Commit**

```bash
git add sim/tools/perf_report.py sim/tools/test_perf_report.py perf_report.md docs/noc-performance-parameters.md docs/trade-off.md
git commit -m "docs(perf): report two-channel versus three-channel latency"
```

After all tasks pass, fold the campaign result into `docs/backlog.md`, remove the completed channel-comparison stages from `IMPLEMENTATION_PLAN.md`, and retain the standing verification section.
