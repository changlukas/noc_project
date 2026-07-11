# DUT depth parameters — single bilingual source via specgen

Date: 2026-07-11
Status: Draft (brainstormed with user, FlooNoC floogen survey, Codex correctness review of this B2
rewrite applied — SOUND-WITH-FIXES, all fixes folded; pending final user review)

## Goal

Every DUT microarchitecture depth (NMU/NSU reorder-buffer, per-AXI-ID order-list, AXI-channel FIFO,
depacketize FIFO, MetaBuffer, VC-arbiter staging) has exactly one home: `specgen/source/constants.yaml`,
the language-neutral parameter source already generating `ni_params.h` (C++) and `ni_params_pkg.sv` (SV).
Retire the scattered defaults (`wrap_defaults.hpp`, `port_params.yaml`, the two C++ YAML readers, the
Python-baked `= 32` in `gen_tb_top.py`, and the per-`NmuConfig`/`NsuConfig` literals).

## Motivation

The same depth is defined in up to four places (C++ `NmuConfig` default, Python codegen bake, Makefile
`?= 32`, SV plusarg), so changing one requires hunting across languages. The scatter has already drifted:

| depth | co-sim (`wrap_defaults`) | unit test (`port_params.yaml`) |
|---|---|---|
| AXI-channel queue / depacketize FIFO | `kAxiQueueDepth = 16` | `32` |

The two homes disagree (16 vs 32) with nothing forcing them consistent — the exact failure a single
source prevents.

## Reference

Two precedents, one external, one in-project:
- **FlooNoC** keeps every NI microarch depth in ONE source — the RTL `ChimneyDefaultCfg` localparam
  (`hw/floo_pkg.sv:342-355`: `MaxTxns`, `MaxUniqueIds`, `MaxTxnsPerId`, `BRoBType/Size`, `RRoBType/Size`,
  FIFO cuts). It is never duplicated into the floogen YAML.
- **This project already solves the bilingual case** for shared params: `specgen/source/constants.yaml`
  is a language-neutral source with per-param metadata (`type/units/default/min/max/sv_symbol/cpp_symbol`)
  that emits both `ni_params.h` and `ni_params_pkg.sv`. It already carries depth params
  (`ROUTER_VC_DEPTH`, `SLAVE_VC_BUFFER_DEPTH`, `ROUTER_OUTPUT_FIFO_DEPTH`). Adding NMU/NSU depths is the
  same mechanism, so C++ model, unit tests, and the SV co-sim tb all read one generated source.

## Parameter taxonomy (shared vs independent)

Shared params are already single-sourced in `constants.yaml` and left untouched. Only the per-unit
depths move. **NMU and NSU keep separate symbols even at equal defaults** (deliberate NMU/NSU
independence), but **within a unit the 5 AXI channels share one queue-depth symbol** and one
depacketize-depth symbol — no shipped path ever sizes the channels differently, and FlooNoC's
`ChimneyDefaultCfg` is likewise a single depth, not per-channel (ponytail + Codex both flagged 5
per-channel symbols as dead once only one is wired). 11 symbols total.

| param | consumer | classification | new symbol (cpp / sv) | default |
|---|---|---|---|---|
| `num_vc`, VC depth, mesh dims, AXI widths | NMU+NSU+router | SHARED (untouched) | existing `NOC_*` / `AXI_*` | — |
| RoB B / R depth | NMU | NMU-only | `NMU_ROB_B_DEPTH` / `NMU_ROB_R_DEPTH` (+`_DFLT`) | 32 |
| max_txns_per_id | NMU | NMU-only | `NMU_MAX_TXNS_PER_ID` | 32 |
| AXI-channel queue depth (all 5) | NMU | one per unit | `NMU_QUEUE_DEPTH` | 16 |
| depacketize FIFO (B+R) | NMU | one per unit | `NMU_DEPKT_Q_DEPTH` | 16 |
| wormhole / vc-arbiter staging | NMU | NMU-only | `NMU_ARBITER_FIFO_DEPTH` | 4 |
| AXI-channel queue depth (all 5) | NSU | one per unit | `NSU_QUEUE_DEPTH` | 16 |
| depacketize FIFO (AW+W+AR) | NSU | one per unit | `NSU_DEPKT_Q_DEPTH` | 16 |
| MetaBuffer max_outstanding | NSU | NSU-only | `NSU_META_BUFFER_MAX_OUTSTANDING` | 32 |
| MetaBuffer max_unique_ids | NSU | NSU-only | `NSU_META_BUFFER_MAX_UNIQUE_IDS` | 1 |
| wormhole / vc-arbiter staging | NSU | NSU-only | `NSU_ARBITER_FIFO_DEPTH` | 4 |

Every value here is a configurable default, not a fixed constant — the number is just the elaboration
default (plusarg still overrides for sweeps). Queue + depacketize default to **16**, which happens to
keep the co-sim path bit-identical (co-sim already used 16 via `wrap_defaults`); unit tests, which read
32 from the retired YAML, move to 16 (depth-insensitive — most read `fx.params.*`, not a literal 32).

`kChannelModelDepth` (64) is a **test-fixture** depth (ChannelModel stub), not DUT — it stays a
test-only constant, out of specgen.

## Decisions

| # | decision | rationale |
|---|---|---|
| **A** — depths → `specgen/source/constants.yaml` new `nmu:` / `nsu:` domains | Same mechanism as the existing shared depths; one source, generated to both languages, so no bake / no YAML reader / no C++-header parse. |
| **B** — separate `NMU_*` / `NSU_*` symbols even at equal defaults | Honours the design's independent NMU/NSU evolution; a future asymmetry is a value edit, not a refactor. |
| **C** — queue + depacketize default = 16 | Preserves validated co-sim numbers (user decision). |
| **D** — SV co-sim initialises its plusarg locals from the generated `ni_params_pkg` symbol; plusarg stays override-only | B2 makes depths SV-visible (`ni_params_pkg.sv` is already in the tb sources and the tb already references `*_DFLT`, Codex-confirmed). The tb keeps a mutable local (`$value$plusargs` writes into it) but initialises it from `NMU_ROB_B_DEPTH_DFLT` instead of a literal `32` — not a bare package reference (that would break the plusarg override). Applies to the **NSU MetaBuffer** bakes too (`max_unique_ids`, `max_outstanding`), not only RoB. |
| **E** — retire `wrap_defaults.hpp`, `port_params.yaml`, `load_nmu_port_params`, `load_nsu_port_params` | Their values move to the generated header; the readers have nothing left to read. |
| **F** — DV-tier cleanup deferred | `--memory-size` auto-derive + `test_aperture` relocation are stimulus/DV knobs, orthogonal to DUT depths; a separate follow-up round keeps this round coherently "all DUT depths, one source". |

## Mechanism

**INPUT** — `constants.yaml` gains `nmu:` and `nsu:` domains, each param in the existing schema shape
(`type/units/description/default/min/max/sv_symbol/cpp_symbol`).

**COMPUTE** — regeneration:
- `tools/elaborate/cpp_params.py` + `sv_params.py`: add an emit group per new domain (`nmu`, `nsu`),
  mirroring the `noc` group (both currently iterate only `axi`/`noc`/`derived` — Codex-confirmed, so
  this is a code change, not data-only). Add the domains to schema validation
  (`ni_spec/handshake_schema.py` top-level keys + `invariants.py`) so the loader accepts them.
- Regenerate `generated/cpp/ni_params.h` + `generated/sv/ni_params_pkg.sv`; update the golden files the
  codegen tests compare against (`tests/golden/ni_params.h.golden`, `tests/golden/ni_params_pkg.sv.golden`),
  not the test bodies.

**OUTPUT** — consumer rewiring (Codex-audited; the full live consumer set):
- `NmuConfig` / `NsuConfig` field initialisers reference `ni::NMU_*` / `ni::NSU_*` (drop the bare `= 32`).
- `nmu_wrap.hpp` / `nsu_wrap.hpp`: default the create args from the generated symbols; delete the
  `#include "wrap/wrap_defaults.hpp"`.
- **`src/dpi/cmodel_dpi.cpp`** (`:383`, `:399`, `:522`): passes `kAxiQueueDepth` + `kRobBDepth/RDepth/
  MaxTxnsPerId` directly into NMU/NSU `init` — rewire to the generated constants.
- **`router_wrap.hpp`** (`:49`): remove the now-dangling `#include "wrap/wrap_defaults.hpp"` (it only
  pulled `kChannelModelDepth`; audit that no symbol is actually used, then drop the include).
- `nmu/rob.hpp` ctor defaults reference `ni::NMU_ROB_*`.
- Unit tests that built `NmuConfig`/`NsuConfig` from `load_*_port_params` construct from the generated
  constants instead; delete the loader calls. Includes `tests/wrap/test_nmu_wrap.cpp` (uses
  `wrap_defaults.hpp` / `kAxiQueueDepth`).
- `gen_tb_top.py`: initialise the RoB **and NSU MetaBuffer** SV plusarg locals from `ni_params_pkg::
  NMU_*_DFLT` / `NSU_*_DFLT`; remove the `= 32` / `= 1` literals (`:555`, `:556`, `:570`, `:571`, `:582`,
  `:588`). Keep the plusarg override forwarding conditional (Codex MAJOR-2, earlier review) and add
  `constants.yaml` + regenerated `ni_params_pkg.sv` as a tb-generation prerequisite (Codex MAJOR-1).
- **ChannelModel test fixture**: `port_params.yaml` also holds the `channel_model:` block, read by
  `load_channel_model_params` (`tests/common/channel_model_params.hpp`) for integration tests. Since
  ChannelModel is a test stub (not DUT), retire that reader too: move `req_depth`/`rsp_depth` (and
  `kChannelModelDepth`) into test-fixture constants, so `port_params.yaml` is deleted with no YAML config
  file left.
- **`nmu/port_params.hpp` / `nsu/port_params.hpp` are NOT deleted** — they also define the
  `PortParams` struct still used by `nmu.hpp:137`, `nsu.hpp:54`, `axi_slave_port.hpp`, `axi_master_port.hpp`
  (Codex). Strip each to the struct only (member initialisers = `ni::NMU_*` / `ni::NSU_*`), delete the
  `load_*_port_params` function and the `<yaml-cpp>` include.
- Delete `port_params.yaml` and `wrap_defaults.hpp` (their values now live in the generated header /
  the ChannelModel test fixture).

## Non-goals

- **DV-tier deferred**: `--memory-size` auto-derive and `test_aperture` relocation (a separate round).
- **No memory model, no aperture right-sizing**: `tile_size` stays parameterized; the tile is a
  placeholder for a future heterogeneous subsystem.
- **No sizing the DV slave**: pulp `axi_rand_slave` has no memory-size knob and policy forbids modifying
  ported IP; the NI aperture remains the only bound.
- **ChannelModel depth stays a test constant**: it is fixture infrastructure, not DUT.

## Success criteria

- Each migrated depth appears in exactly one place — `constants.yaml` — and nowhere else as a literal
  default.
- `ni_params.h` and `ni_params_pkg.sv` both carry the new `NMU_*` / `NSU_*` symbols; specgen pytest
  (incl. codegen golden) green.
- ctest green (model + unit tests consume the generated constants).
- Co-sim directed + constrained_random green on `tb_mesh_4x4_vc1`; a plusarg override still swaps a
  depth; co-sim numbers unchanged (queue depth held at 16).
- `wrap_defaults.hpp` and `port_params.yaml` deleted; no dangling references.
