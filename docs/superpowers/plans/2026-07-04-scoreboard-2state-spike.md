# Scoreboard 2-state Spike Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Empirically determine whether pulp `axi_scoreboard` false-reports under Verilator 2-state on a directed two-phase write→readback (full strobe, `data=f(addr)`), so Stage 3+ of the benchmark rebuild knows whether the scoreboard is usable on WSL/Verilator or must fall back (VCS / self-written comparator).

**Architecture:** A throwaway standalone tb — no NoC, no DPI, no c_model. One `AXI_BUS_DV`: `axi_file_master` drives it (two-phase), `axi_rand_slave(MAPPED=1)` responds as memory, `axi_scoreboard` monitors it. Two hand-authored stimulus cases: (1) read-what-you-wrote (expect clean), (2) read-an-unwritten-address (fault injection, expect the X-collapse to fire — confirms the checker isn't silently passing and pins the root cause).

**Tech Stack:** SystemVerilog, pulp `axi-0.39.7` VIP (`axi_test.sv`), Verilator 5.048 `--binary --timing` on WSL (the project's established constrained-random co-sim environment; the MAPPED rand_slave randomizes, so the z3 solver path is needed → WSL, not Windows).

## Global Constraints

- Spike files are THROWAWAY: keep them in the working tree during the round, `rm` after the verdict is recorded — do NOT commit them (commit-then-delete is pure history noise). Do not wire into the regression build.
- No new dependency: use only `sim/dv/` imported IP.
- Directed stimulus is INCR-only, `atop=0`, full strobe (the scoreboard's supported subset).
- Run on WSL Verilator 5.048 (`VERILATOR`/`PYTHON3` per the project's WSL setup). The rand_slave randomizes → Windows Verilator solver pipe is unavailable.
- Fault-injection first: confirm the checker CAN fire before trusting a clean pass (see Task 2).

## File Structure

- `sim/tb/spike_scoreboard_tb.sv` — standalone spike tb (throwaway).
- `sim/tb/spike.write.txt` / `sim/tb/spike.read_clean.txt` / `sim/tb/spike.read_fault.txt` — hand-authored file_master stimulus (throwaway).
- Build artifacts under `build/spike_obj/` (gitignored via `build/`).

---

### Task 1: Clean-case run — read what you wrote (expect scoreboard clean)

**Files:**
- Create: `sim/tb/spike_scoreboard_tb.sv`
- Create: `sim/tb/spike.write.txt`, `sim/tb/spike.read_clean.txt`

**Interfaces:**
- Consumes: `axi_test::axi_file_master`, `axi_rand_slave`, `axi_scoreboard` from `sim/dv/axi-0.39.7/src/axi_test.sv`; `AXI_BUS_DV` from `axi_intf.sv`.
- Produces: verdict line `SPIKE: run complete` + presence/absence of scoreboard `does not match` / `Unexpected RData` warnings on stdout.

- [ ] **Step 1: Write the spike tb**

Create `sim/tb/spike_scoreboard_tb.sv`:

```systemverilog
// spike_scoreboard_tb.sv — THROWAWAY spike (backlog: scoreboard 2-state).
// Determine whether pulp axi_scoreboard false-reports under Verilator 2-state
// on a directed two-phase write->readback. No NoC/DPI: axi_file_master drives
// one AXI_BUS_DV, axi_rand_slave(MAPPED) responds, axi_scoreboard monitors.
// Delete after the verdict is recorded.
`timescale 1ns/1ps

module spike_scoreboard_tb;
  localparam int  AW = 64;
  localparam int  DW = 64;
  localparam int  IW = 4;
  localparam int  UW = 1;
  localparam time TA = 2ns;
  localparam time TT = 8ns;

  logic clk = 0;
  logic rst_n = 0;
  always #5 clk = ~clk;
  initial begin
    rst_n = 0;
    repeat (5) @(posedge clk);
    rst_n = 1;
  end

  AXI_BUS_DV #(.AXI_ADDR_WIDTH(AW), .AXI_DATA_WIDTH(DW),
               .AXI_ID_WIDTH(IW), .AXI_USER_WIDTH(UW)) axi (clk);

  typedef axi_test::axi_file_master #(.AW(AW), .DW(DW), .IW(IW), .UW(UW),
                                      .TA(TA), .TT(TT)) fm_t;
  typedef axi_test::axi_rand_slave  #(.AW(AW), .DW(DW), .IW(IW), .UW(UW),
                                      .TA(TA), .TT(TT), .MAPPED(1'b1)) rs_t;
  typedef axi_test::axi_scoreboard  #(.IW(IW), .AW(AW), .DW(DW), .UW(UW),
                                      .TT(TT)) sb_t;

  fm_t fm;
  rs_t rs;
  sb_t sb;

  localparam string write_file = "spike.write.txt";
  string read_file = "spike.read_clean.txt";     // +read= swaps clean/fault
  initial void'($value$plusargs("read=%s", read_file));

  // MAPPED memory slave responds on the same bus.
  initial begin
    rs = new(axi);
    rs.reset();
    @(posedge rst_n);
    rs.run();
  end

  // Scoreboard: golden from W, check on R.
  initial begin
    sb = new(axi);
    sb.reset();
    @(posedge rst_n);
    sb.enable_all_checks();
    sb.monitor();
  end

  // Master: two-phase — all writes (+ wait B), barrier, then all reads (+ wait R).
  initial begin
    fm = new(axi);
    fm.load_files(read_file, write_file);
    @(posedge rst_n);
    fork fm.run_aw(); fm.run_w(); fm.wait_b(); join   // phase 1: writes drain
    fork fm.run_ar(); fm.wait_r(); join               // phase 2: reads checked
    #100ns;
    $display("SPIKE: run complete");
    $finish;
  end
endmodule
```

- [ ] **Step 2: Write the write-stimulus file**

Create `sim/tb/spike.write.txt` (parse_write order: id/addr/len/size/burst/lock/cache/prot/qos/region/atop/user, then one W beat `data strb user`). One 8-byte INCR write to `0x1000`, full strobe, `data = addr`:

```
0
0x1000
0
3
1
0
0
0
0
0
0
0
0x1000 0xff 0
```

- [ ] **Step 3: Write the clean read-stimulus file**

Create `sim/tb/spike.read_clean.txt` (parse_read order: 11 fields, NO atop). Read the SAME address `0x1000` that was written:

```
0
0x1000
0
3
1
0
0
0
0
0
0
```

- [ ] **Step 4: Build the spike on WSL Verilator**

Run (in the project's WSL co-sim environment, from repo root):

```bash
verilator --binary --timing --top-module spike_scoreboard_tb \
    -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC \
    -I sim/dv/axi-0.39.7/include \
    sim/dv/common_cells-1.37.0/src/cf_math_pkg.sv \
    sim/dv/common_verification-0.2.5/src/rand_id_queue.sv \
    sim/dv/axi-0.39.7/src/axi_pkg.sv \
    sim/dv/axi-0.39.7/src/axi_intf.sv \
    sim/dv/axi-0.39.7/src/axi_test.sv \
    sim/tb/spike_scoreboard_tb.sv \
    --Mdir build/spike_obj
```

Expected: build succeeds and produces `build/spike_obj/Vspike_scoreboard_tb`. If a package symbol is missing (e.g. `cf_math_pkg`/`rand_id_queue`), add its `.sv` from `sim/dv/` to the file list and rebuild — do NOT stub it.

- [ ] **Step 5: Run the clean case and OBSERVE**

Run (from `sim/tb/` so the relative stimulus filenames resolve):

```bash
cd sim/tb && ../../build/spike_obj/Vspike_scoreboard_tb +read=spike.read_clean.txt \
    2>&1 | tee /tmp/spike_clean.log
```

OBSERVE (record verbatim, do not interpret away):
- Presence/absence of any scoreboard warning containing `does not match` or `Unexpected RData`.
- The `SPIKE: run complete` line (confirms non-vacuous).

**Interpretation:**
- No mismatch warning → **scoreboard is usable on Verilator for the directed axis** (plan premise holds).
- Mismatch warning on this legal readback → the X-collapse hits even clean reads → Stage 3 must patch the don't-care-byte path or fall back (VCS / self-written `f(addr)` comparator).

- [ ] **Step 6: Fault-injection — read an unwritten address, OBSERVE**

Create `sim/tb/spike.read_fault.txt` — read `0x2000`, which the write file never touched (parse_read, 11 fields):

```
0
0x2000
0
3
1
0
0
0
0
0
0
```

Run and observe whether a scoreboard mismatch warning fires:

```bash
cd sim/tb && ../../build/spike_obj/Vspike_scoreboard_tb +read=spike.read_fault.txt \
    2>&1 | tee /tmp/spike_fault.log
```

- Warning fires → checker is live; failure boundary = read of never-written byte (`8'hxx`→`0` collapse). Expected root-cause characterization.
- No warning → the scoreboard isn't checking what we think; fix the monitor/enable wiring before trusting the Step-5 clean result.

---

### Task 2: Record the verdict and decide Stage 3's checker path

**Files:**
- Modify: `docs/superpowers/specs/2026-07-04-checked-traffic-benchmark-design.md` (resolve D6/Stage 1 with the measured outcome)
- Modify: `docs/backlog.md` (one-line pointer to the verdict)

- [ ] **Step 1: Write the verdict into the spec**

From Step 5 (clean) + Step 6 (fault), resolve spec D6 / Stage 1, one of:
- **Clean passes, fault fires** → scoreboard usable on Verilator directed axis. Stage 3 proceeds with `axi_scoreboard` as specified (D6: no patch needed).
- **Clean also warns** → record the exact warning + byte. Decide fallback and update D6 + risk table: (a) patch the scoreboard don't-care path (restore `8'hxx` wildcard under 2-state), (b) directed axis → VCS, or (c) self-written `f(addr)` comparator.

Add a one-line `docs/backlog.md` pointer to the spec verdict.

- [ ] **Step 2: Remove the throwaway spike and commit the verdict**

```bash
rm sim/tb/spike_scoreboard_tb.sv sim/tb/spike.write.txt \
   sim/tb/spike.read_clean.txt sim/tb/spike.read_fault.txt
git add docs/superpowers/specs/2026-07-04-checked-traffic-benchmark-design.md docs/backlog.md
git commit -m "docs: record scoreboard 2-state spike verdict"
```

## Self-Review

- **Spec coverage:** implements spec Stage 1 (scoreboard 2-state spike, directed axis only). Stages 2-5 are out of scope (written after this verdict).
- **Fault injection:** Task 1 Step 6 confirms the checker fires before trusting the clean pass (per project verification discipline).
- **Throwaway discipline:** spike files never committed; Task 2 Step 2 `rm`s them, only the verdict (docs) persists.
- **Decision output:** Task 2 Step 1 branches Stage 3's checker path on the measured result — the whole reason for spike-first.
