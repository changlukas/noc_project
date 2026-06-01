# c_model — Pure AXI Subsystem (Stage 2 complete)

Tick-driven C++ behavior model implementing AXI4 master + slave + memory + scoreboard, used as:
- Golden reference for RTL verification (NMU/NSU/Router co-sim, Stage 5)
- Cross-pairing endpoint for cocotbext-axi / SystemVerilog VIP

Phase A + B + C complete (182/182 sequential ctest):
- A: INCR aligned size=5 baseline
- B: sparse WSTRB, unaligned start, narrow transfer, WRAP/FIXED burst, 4KB cross auto-split, per-ID FIFO, runtime protocol validation
- C: exclusive access (AxLOCK + EXOKAY) with slave-side monitor

See `NEXT_STEPS.md` for status + known limitations + roadmap.
See `../../docs/noc_cmodel_rtl_plan.md` for Stage 2/3/4/5 plan.

## Build

```
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure -j 1
```

The build target depends on `codegen_check` — codegen drift fails the build.
Elaborated headers come from `../specgen/generated/cpp/`.
