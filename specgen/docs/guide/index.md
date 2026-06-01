# Spec-as-code User Guide

A toolchain that generates C++ headers and SystemVerilog packages from machine-readable NI spec JSON files. Update the JSON SSoT, run codegen, downstream code and RTL pick up the change without manual transcription.

## What it does

- Reads JSON SSoT from `generated/`. `ni_function_blocks.json` is kept as a feature inventory and cross-domain consistency check, not as codegen input.
- Elaborates C++ headers into `include/` for the C-model.
- Elaborates SystemVerilog packages into `rtl_pkg/` for co-sim.
- Detects drift between committed artifacts and current SSoT (`--check`).
- Smoke-lints SV output with verilator (`--lint-sv`).

## Read order

1. [Installing](installing.md) — Python plus optional `g++` / `verilator`.
2. [Quickstart](quickstart.md) — elaborate one header, compile the sample, run it.
3. [Architecture](architecture.md) — how a JSON SSoT becomes a `.h` or `.sv`.
4. [Commands](commands.md) — every `codegen.py` flag and what it does.
5. [Artifacts](artifacts.md) — what gets generated, where, when.
6. [Using constants](using-constants.md) — consume the output from C++ or Python.
7. [Troubleshooting](troubleshooting.md) — common errors and fixes.

## Links

- Design rationale: [`docs/plans/2026-05-26-spec-as-code-unified-design.md`](../plans/2026-05-26-spec-as-code-unified-design.md)
- Codegen entry point: `tools/codegen.py`
- Public Python API: `ni_spec/constants.py`
