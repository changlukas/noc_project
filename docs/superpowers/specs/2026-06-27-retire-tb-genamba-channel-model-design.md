# Retire tb_genamba + demote ChannelModel to test-only

Status: READY (codex 2-pass reviewed; sequencing + completeness fixes incorporated)
Date: 2026-06-27

## Problem

`c_model/include/wrap/channel_model_wrap.hpp:21` does `#include "common/channel_model.hpp"`
and owns a `testing::ChannelModel`. This is a layering inversion: production `include/`
reverse-depends on a `tests/common/` test fixture.

The inversion exists only to serve the legacy single-node `tb_genamba` / `tb_genamba_tester`
referee co-sim, which uses `ChannelModel` as a NoC stub via the DPI exports
`cmodel_channel_model_{create,set_inputs,tick,get_outputs}`. That testbench is superseded by
the FlooNoC mesh `tb_top_mesh_4x4_vc{1,2,4,8}` fabric (real `router_wrap` / `noc_fabric_*`,
no channel_model). User confirmed tb_genamba can be retired.

`ChannelModel` itself stays: it is the downstream NoC stub for 19 c_model unit tests
(nmu / nsu / integration / common). Its header explicitly documents non-physical
latency/throughput, so it is correct as a test fixture, not a production component.

## Goal / success criteria

- Invariant: no `#include "common/..."` anywhere under `c_model/include/` (grep gate).
- ctest fully green (ChannelModel still serves its 19 unit-test consumers).
- mesh tb_top co-sim green (unaffected; never used channel_model).
- `make genamba` / `run-genamba` targets gone from BOTH Verilator and VCS Makefiles; build
  reports no missing sources; root `Makefile` help no longer advertises removed targets.
- No RTL / no design-behavior change. Pure structural.

## Change units

### (a) Sever the inversion (core)
- Delete `include/wrap/channel_model_wrap.hpp` (the sole production includer of `common/`).
- `sim/c/cmodel_dpi.cpp` + `cmodel_dpi.h`: remove the 4 `cmodel_channel_model_*` exports and
  the `#include "wrap/channel_model_wrap.hpp"`. **RETAIN** the shared flit pack/unpack
  marshalling helpers (`sim/c/cmodel_dpi.cpp:415` region) — they serve router/NMU/NSU, not
  just ChannelModel; delete only the ChannelModel DPI block.
- `sim/c/handle_block.hpp`: remove the `WrapType::ChannelModel` enum value and any
  ChannelModel handle-type validation branch (the enum lives here, not in `cmodel_dpi.cpp`).

### (b) FlitBytes rehome
- New `wrap/flit_bytes.hpp` holding only `FLIT_BYTES`, `FlitBytes`, `FLIT_VEC_WORDS`.
- Delete `wrap/channel_model_wrap_io.hpp` (its `ChannelModelInputs/Outputs` die with the DPI;
  confirmed no continuing consumer needs those two structs).
- Update includers: `nmu_wrap.hpp`, `nsu_wrap.hpp`, `router_wrap_io.hpp`, `nmu_wrap_io.hpp`,
  `nsu_wrap_io.hpp`, `flit_byte_conv.hpp`, plus `sim/c/cmodel_dpi.cpp` (uses `FlitBytes`) and
  `tests/wrap/test_nsu_wrap.cpp` (uses `FlitBytes`).

### (c) Retire testbench (SV / build / C)
- Delete SV: `tb_genamba.sv`, `tb_genamba_tester.sv`, `sv/genamba/`, `genamba_master_bfm.sv`,
  `genamba_init.yaml`.
- Delete C: `verilator/main_genamba.cpp`, `verilator/run_genamba.sh`.
- Verilator `Makefile`: remove `genamba` / `run-genamba` / `genamba-tester` /
  `run-genamba-tester` / `clean-genamba` targets + obj dirs, and the `clean: clean-genamba`
  dependency.
- VCS `Makefile`: remove the parallel genamba build/run flow (`-top tb_genamba`, `genamba` /
  `run-genamba` targets, `GENAMBA_*` / `FSDB_PLUSARG_GENAMBA` / `GENAMBA_SCENARIO*` vars) and
  drop genamba from the `run-all-fsdb` batch.
- `build_config.mk`: remove `GENAMBA_*` source lists (`GENAMBA_SV_SRC`, `GENAMBA_TESTER_SV_SRC`,
  `GENAMBA_INC_DEPS`, `GENAMBA_DEFINES`).
- Root `Makefile`: remove `run-genamba` from the help text.

### (d) Test + doc cleanup
- Delete `tests/wrap/test_channel_model_wrap.cpp`; remove from `tests/wrap/CMakeLists.txt`
  including the now-stale include-path comment at its top.
- Prune the channel-model section of `tests/wrap/test_cmodel_dpi.cpp`.
- `tests/common/channel_model_params.hpp`: update the stale "production ChannelModelWrap"
  comment (ChannelModel is now test-only).
- Doc sync (remove / correct tb_genamba + channel_model-DPI descriptions):
  `docs/architecture.md`, `README.md`, `docs/development.md`, `docs/issue/ARCHITECTURE.md`.
- `CLAUDE.md`: surgical edit only — drop `ChannelModelWrap` from the wrap-layer component list
  (line 11). KEEP the architecture line that calls `ChannelModel` a "test stub" (still true).
- `docs/slides/genamba-role1-port-SLIDES.md`: **untouched** — it is a delivered point-in-time
  milestone report, kept as a historical record, not a living doc.

## Sequencing

Order is `(b) -> (c) -> (a) -> (d)`. Rationale: the genamba SV tops call the
`cmodel_channel_model_*` DPI, so the DPI must NOT be removed while those build targets still
exist — retire the consumers (c) BEFORE severing the DPI (a). Each intermediate state compiles:

1. **(b) FlitBytes rehome** — add `wrap/flit_bytes.hpp`, repoint all `FlitBytes` includers.
   Keep `channel_model_wrap_io.hpp` (still hosts `ChannelModelInputs/Outputs` used by the
   not-yet-removed wrap/DPI).
2. **(c) Retire genamba build flows** — delete genamba SV/C sources + Verilator/VCS Makefile
   targets + `build_config.mk` lists + root help. ChannelModelWrap + DPI still exist but are
   now unreferenced → still compiles.
3. **(a) Sever the inversion** — delete `channel_model_wrap.hpp` + the `cmodel_channel_model_*`
   DPI exports + `handle_block.hpp` enum value. `channel_model_wrap_io.hpp` now has no consumer
   → delete it (its `ChannelModelInputs/Outputs` die here).
4. **(d) Test + doc cleanup** — last; no build-order dependency.

## Out of scope / preserved

- `tests/common/channel_model.hpp` + `channel_model_params.hpp`: kept as test stub (params file
  gets only a stale-comment fix per unit (d)).
- No change to mesh fabric, router, NMU/NSU model behavior.

## Risk

Only structural risk is an incomplete deletion causing a build break. Gated by the grep
invariant + full Verilator/VCS build + ctest + mesh co-sim regression. The one correctness
trap is unit (a): the shared flit marshalling helpers in `cmodel_dpi.cpp` must survive the
ChannelModel-DPI removal — they are not channel-specific.
