# c_model — Spec Validation Harness (First Round)

This is NOT a complete C model. It's a first-round bootstrap that:

1. Exercises codegen output by writing two c_model classes (Flit, RegisterFile)
   that reference only codegen-elaborated symbols
2. Surfaces spec sufficiency and codegen gaps via `SUFFICIENCY_FINDINGS.md`
3. Establishes stable boundaries for future cycle-accurate behavior (Stage 2)

See `docs/superpowers/specs/2026-05-27-c-model-bootstrap-design.md`.

## Build

cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure

The build target depends on `codegen_check` — drift fails the build.
