# DUT depth parameters — single bilingual source via specgen — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move every NMU/NSU microarchitecture depth into `specgen/source/constants.yaml`, generated to both `ni_params.h` (C++) and `ni_params_pkg.sv` (SV), and retire the scattered homes.

**Architecture:** `constants.yaml` gains `nmu:` / `nsu:` domains (same schema as the existing `noc:` depths). The specgen emitters + schema learn the new domains. Every consumer (model config, wraps, DPI, tests, SV tb) repoints to the generated symbols. `wrap_defaults.hpp` and `port_params.yaml` are deleted; the `PortParams` structs stay (their defaults now reference the generated constants), only their YAML loaders go.

**Tech Stack:** Python 3 (specgen codegen + pytest), C++17 (GoogleTest/ctest), SystemVerilog + Verilator co-sim, GNU Make.

**Spec:** `docs/superpowers/specs/2026-07-11-config-simplification-design.md`

## Global Constraints

- **Behaviour-preserving refactor**: co-sim numbers unchanged (co-sim already uses depth 16 via `wrap_defaults`).
- **11 symbols** (not per-channel): `NMU_{ROB_B_DEPTH,ROB_R_DEPTH,MAX_TXNS_PER_ID,QUEUE_DEPTH,DEPKT_Q_DEPTH,ARBITER_FIFO_DEPTH}` + `NSU_{QUEUE_DEPTH,DEPKT_Q_DEPTH,META_BUFFER_MAX_OUTSTANDING,META_BUFFER_MAX_UNIQUE_IDS,ARBITER_FIFO_DEPTH}`. The 5 AXI channels of a unit share its one `QUEUE_DEPTH`; both depacketize FIFOs share `DEPKT_Q_DEPTH`.
- Values (defaults, all plusarg-overridable): queue/depkt = 16, RoB b/r + max_txns_per_id + meta_outstanding = 32, meta_unique_ids = 1, arbiter = 4.
- Symbol convention: `cpp_symbol` bare (`NMU_QUEUE_DEPTH`); `sv_symbol` same + `_DFLT` (`NMU_QUEUE_DEPTH_DFLT`).
- NMU and NSU keep **separate** symbols even at equal defaults.
- DV-tier (`--memory-size`, `test_aperture`) **out of scope** — don't touch; leave the two uncommitted Makefile edits in the working tree.
- Branch `feat/nmu-rob-floonoc-alignment`. Do not push; stop at working tree. No `--no-verify`.
- Build/test on WSL: `make test` (ctest), `make sim ...`. `PYTHON3=python3`, `BUILD_ROOT=$HOME/noc_build` via `local.mk`.

## File structure

| file | change |
|---|---|
| `specgen/source/constants.yaml` | add `nmu:` + `nsu:` domains (11 params) |
| `specgen/ni_spec/handshake_schema.py:30,71` | whitelist `nmu`/`nsu` |
| `specgen/tools/elaborate/cpp_params.py`, `sv_params.py` | emit nmu/nsu groups |
| `specgen/generated/cpp/ni_params.h`, `generated/sv/ni_params_pkg.sv` | regenerate |
| `specgen/tests/golden/ni_params.h.golden`, `ni_params_pkg.sv.golden` | refresh golden |
| `src/c_model/include/nmu/nmu.hpp`, `nmu/rob.hpp`, `nsu/nsu.hpp` | config defaults → symbols |
| `src/c_model/include/nmu/port_params.hpp`, `nsu/port_params.hpp` | **strip to struct** (defaults → symbols), delete loader + yaml include |
| `src/c_model/include/wrap/{nmu,nsu,router}_wrap.hpp` | create defaults → symbols; drop `wrap_defaults.hpp` include |
| `src/dpi/cmodel_dpi.cpp:383,399,522` | create args → symbols |
| `src/c_model/tests/common/channel_model_params.hpp` | inline fixture constants, drop YAML reader |
| `src/c_model/tests/wrap/test_nmu_wrap.cpp`, `tests/integration/*`, `tests/nmu/test_axi_slave_port.cpp` | construct from symbols, drop loader calls |
| `sim/tools/gen_tb_top.py:555,556,560-563` | init plusarg locals from `ni_params_pkg::*_DFLT` |
| `sim/verilator/Makefile:211-212,224,253-255,132-133` | drop `?=32`, conditional plusargs, YAML prerequisite |
| deleted | `wrap_defaults.hpp`, `port_params.yaml` |

---

### Task 1: Add `nmu:` / `nsu:` domains to the specgen source + schema

**Files:**
- Modify: `specgen/source/constants.yaml`, `specgen/ni_spec/handshake_schema.py:30,71`
- Test: `specgen/tests/test_codegen.py`

**Interfaces:**
- Produces: 11 constant specs under `nmu:` / `nsu:` (each with `default`, `sv_symbol`, `cpp_symbol`), consumed by Tasks 2–7.

- [ ] **Step 1: Write the failing test**

Add to `specgen/tests/test_codegen.py`:
```python
def test_nmu_nsu_domains_load():
    from ni_spec.handshake_schema import load_constants
    from pathlib import Path
    c = load_constants(Path(__file__).resolve().parent.parent / "source" / "constants.yaml")
    assert c["nmu"]["ROB_B_DEPTH"]["default"] == 32
    assert c["nmu"]["QUEUE_DEPTH"]["default"] == 16
    assert c["nmu"]["QUEUE_DEPTH"]["cpp_symbol"] == "NMU_QUEUE_DEPTH"
    assert c["nsu"]["META_BUFFER_MAX_UNIQUE_IDS"]["default"] == 1
    assert c["nsu"]["QUEUE_DEPTH"]["sv_symbol"] == "NSU_QUEUE_DEPTH_DFLT"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd specgen && python3 -m pytest tests/test_codegen.py::test_nmu_nsu_domains_load -q`
Expected: FAIL — schema rejects unknown top-level keys `nmu`/`nsu`.

- [ ] **Step 3: Whitelist the domains**

`handshake_schema.py:30`:
```python
_TOP_LEVEL_KEYS = {"schema_version", "axi", "noc", "nmu", "nsu", "derived"}
```
`handshake_schema.py:71` (plain-param validation loop):
```python
    for domain in ("axi", "noc", "nmu", "nsu"):
```

- [ ] **Step 4: Add the `nmu:` / `nsu:` blocks to `constants.yaml`** (append after `noc:`, before `derived:`)

```yaml
nmu:
  ROB_B_DEPTH:
    type: int
    units: entries
    description: "NMU write-response RoB pool depth. <= 2^ROB_IDX_WIDTH."
    default: 32
    min: 1
    max: 256
    sv_symbol: NMU_ROB_B_DEPTH_DFLT
    cpp_symbol: NMU_ROB_B_DEPTH
  ROB_R_DEPTH:
    type: int
    units: entries
    description: "NMU read-response RoB pool depth. Holds one rdata beat per slot."
    default: 32
    min: 1
    max: 256
    sv_symbol: NMU_ROB_R_DEPTH_DFLT
    cpp_symbol: NMU_ROB_R_DEPTH
  MAX_TXNS_PER_ID:
    type: int
    units: transactions
    description: "NMU per-AXI-ID order-list depth (FlooNoC MaxRoTxnsPerId)."
    default: 32
    min: 1
    max: 256
    sv_symbol: NMU_MAX_TXNS_PER_ID_DFLT
    cpp_symbol: NMU_MAX_TXNS_PER_ID
  QUEUE_DEPTH:
    type: int
    units: beats
    description: "NMU slave-port AXI-channel FIFO depth (shared by AW/W/AR/B/R)."
    default: 16
    min: 1
    max: 1024
    sv_symbol: NMU_QUEUE_DEPTH_DFLT
    cpp_symbol: NMU_QUEUE_DEPTH
  DEPKT_Q_DEPTH:
    type: int
    units: beats
    description: "NMU Depacketize demux FIFO depth (shared by B/R)."
    default: 16
    min: 1
    max: 1024
    sv_symbol: NMU_DEPKT_Q_DEPTH_DFLT
    cpp_symbol: NMU_DEPKT_Q_DEPTH
  ARBITER_FIFO_DEPTH:
    type: int
    units: entries
    description: "NMU wormhole per-input + VC-arbiter pending staging depth."
    default: 4
    min: 1
    max: 64
    sv_symbol: NMU_ARBITER_FIFO_DEPTH_DFLT
    cpp_symbol: NMU_ARBITER_FIFO_DEPTH

nsu:
  QUEUE_DEPTH:
    type: int
    units: beats
    description: "NSU master-port AXI-channel FIFO depth (shared by AW/W/AR/B/R)."
    default: 16
    min: 1
    max: 1024
    sv_symbol: NSU_QUEUE_DEPTH_DFLT
    cpp_symbol: NSU_QUEUE_DEPTH
  DEPKT_Q_DEPTH:
    type: int
    units: beats
    description: "NSU Depacketize demux FIFO depth (shared by AW/W/AR)."
    default: 16
    min: 1
    max: 1024
    sv_symbol: NSU_DEPKT_Q_DEPTH_DFLT
    cpp_symbol: NSU_DEPKT_Q_DEPTH
  META_BUFFER_MAX_OUTSTANDING:
    type: int
    units: transactions
    description: "NSU MetaBuffer shared outstanding pool size, per direction."
    default: 32
    min: 1
    max: 256
    sv_symbol: NSU_META_BUFFER_MAX_OUTSTANDING_DFLT
    cpp_symbol: NSU_META_BUFFER_MAX_OUTSTANDING
  META_BUFFER_MAX_UNIQUE_IDS:
    type: int
    units: count
    description: "Distinct AXI IDs the NSU presents downstream (1 collapses; 256 passes through)."
    default: 1
    allowed: [1, 256]
    sv_symbol: NSU_META_BUFFER_MAX_UNIQUE_IDS_DFLT
    cpp_symbol: NSU_META_BUFFER_MAX_UNIQUE_IDS
  ARBITER_FIFO_DEPTH:
    type: int
    units: entries
    description: "NSU wormhole per-input + VC-arbiter pending staging depth."
    default: 4
    min: 1
    max: 64
    sv_symbol: NSU_ARBITER_FIFO_DEPTH_DFLT
    cpp_symbol: NSU_ARBITER_FIFO_DEPTH
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cd specgen && python3 -m pytest tests/test_codegen.py::test_nmu_nsu_domains_load -q`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add specgen/source/constants.yaml specgen/ni_spec/handshake_schema.py specgen/tests/test_codegen.py
git commit -m "feat(specgen): add nmu/nsu depth domains to constants source"
```

---

### Task 2: Emit the new domains + regenerate the golden headers

**Files:**
- Modify: `specgen/tools/elaborate/cpp_params.py` (plain-value loop `:31`, add groups after `:55`), `sv_params.py` (loop `:33`, add groups after `:57`)
- Regenerate: `specgen/generated/cpp/ni_params.h`, `generated/sv/ni_params_pkg.sv`
- Update: `specgen/tests/golden/ni_params.h.golden`, `ni_params_pkg.sv.golden`

**Interfaces:**
- Produces: `ni::NMU_*` / `ni::NSU_*` (C++) and `ni_params_pkg::NMU_*_DFLT` / `NSU_*_DFLT` (SV).

- [ ] **Step 1: Confirm current codegen green baseline**

Run: `cd specgen && python3 -m pytest tests/test_codegen.py tests/test_codegen_sv.py -q`
Expected: PASS.

- [ ] **Step 2: Add nmu/nsu emit groups — `cpp_params.py`**

Plain-value loop (`:31`) → `for domain in ("axi", "noc", "nmu", "nsu"):`. After the `noc` `_emit_group` (`:51-55`) add:
```python
    _emit_group(
        list(constants.get("nmu", {}).items()),
        "NMU depth defaults",
        lambda _n, s: s["default"],
    )
    _emit_group(
        list(constants.get("nsu", {}).items()),
        "NSU depth defaults",
        lambda _n, s: s["default"],
    )
```

- [ ] **Step 3: Add nmu/nsu emit groups — `sv_params.py`**

Same two edits: loop (`:33`) → `("axi", "noc", "nmu", "nsu")`; two `_emit_group` calls after the `noc` group (`:53-57`) with labels `"NMU depth defaults"` / `"NSU depth defaults"`.

- [ ] **Step 4: Regenerate + refresh golden**

Run (Codex-verified entrypoint — `codegen.py` needs explicit target+domain):
```bash
cd specgen
python3 tools/codegen.py --target cpp --domain params
python3 tools/codegen.py --target sv  --domain params
grep -c "NMU_QUEUE_DEPTH" generated/cpp/ni_params.h generated/sv/ni_params_pkg.sv   # expect nonzero both
cp generated/cpp/ni_params.h  tests/golden/ni_params.h.golden
cp generated/sv/ni_params_pkg.sv tests/golden/ni_params_pkg.sv.golden
```

- [ ] **Step 5: Run codegen suite (emitter == golden)**

Run: `cd specgen && python3 -m pytest tests/test_codegen.py tests/test_codegen_sv.py -q`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add specgen/tools/elaborate specgen/generated specgen/tests/golden
git commit -m "feat(specgen): emit nmu/nsu depth symbols to ni_params.h + ni_params_pkg.sv"
```

---

### Task 3: Repoint C++ model config defaults to the generated symbols

**Files:**
- Modify: `src/c_model/include/nmu/nmu.hpp:133-136`, `nmu/rob.hpp:47-48` (+ add `#include "ni_params.h"`), `nsu/nsu.hpp` (arbiter defaults)

**Interfaces:** Consumes `ni::NMU_*` / `ni::NSU_*`.

- [ ] **Step 1: ctest green baseline**

Run: `make test` — Expected: PASS (record count).

- [ ] **Step 2: Repoint `NmuConfig`** (`nmu.hpp`, add `#include "ni_params.h"` if absent)

```cpp
    std::size_t b_rob_depth = ni::NMU_ROB_B_DEPTH;
    std::size_t r_rob_depth = ni::NMU_ROB_R_DEPTH;
    std::size_t max_txns_per_id = ni::NMU_MAX_TXNS_PER_ID;
```
And the two arbiter defaults → `ni::NMU_ARBITER_FIFO_DEPTH`.

- [ ] **Step 3: Repoint `rob.hpp`** — add its OWN `#include "ni_params.h"` (Codex: standalone inclusion), ctor default args (`:47-48`) → `ni::NMU_ROB_B_DEPTH`, `ni::NMU_ROB_R_DEPTH`, `ni::NMU_MAX_TXNS_PER_ID`.

- [ ] **Step 4: Repoint `NsuConfig`** (`nsu.hpp`) — `wormhole_per_input_depth` / `vc_arbiter_pending_depth` → `ni::NSU_ARBITER_FIFO_DEPTH`.

- [ ] **Step 5: ctest** — Run: `make test` — Expected: PASS, same count.

- [ ] **Step 6: Commit**

```bash
git add src/c_model/include/nmu/nmu.hpp src/c_model/include/nmu/rob.hpp src/c_model/include/nsu/nsu.hpp
git commit -m "refactor(nmu,nsu): source config depth defaults from generated ni_params.h"
```

---

### Task 4: Repoint the wraps + DPI bridge to the generated symbols

**Files:**
- Modify: `src/c_model/include/wrap/nmu_wrap.hpp:51,76-82`, `nsu_wrap.hpp:52,63-70`, `src/dpi/cmodel_dpi.cpp:383,399,522`

**Interfaces:** Consumes `ni::NMU_*` / `ni::NSU_*`. `wrap_defaults.hpp` still exists (deleted in Task 6).

- [ ] **Step 1: Repoint `nmu_wrap` create defaults**

`nmu_wrap.hpp:51` `queue_depth` default `kAxiQueueDepth` → `ni::NMU_QUEUE_DEPTH`; `:53-54` RoB defaults `kRobBDepth/kRobRDepth/kRobMaxTxnsPerId` → `ni::NMU_ROB_B_DEPTH/ROB_R_DEPTH/MAX_TXNS_PER_ID`. The `:76-82` block assigns `queue_depth` to all 5 queue fields; set the two `depkt_*_q_depth` fields to `ni::NMU_DEPKT_Q_DEPTH` and the arbiter fields to `ni::NMU_ARBITER_FIFO_DEPTH`.

- [ ] **Step 2: Repoint `nsu_wrap` create defaults**

`nsu_wrap.hpp:52` `queue_depth` default → `ni::NSU_QUEUE_DEPTH`; `:63-70` block: depkt fields → `ni::NSU_DEPKT_Q_DEPTH`, MetaBuffer → `ni::NSU_META_BUFFER_MAX_OUTSTANDING` / `ni::NSU_META_BUFFER_MAX_UNIQUE_IDS`, arbiter → `ni::NSU_ARBITER_FIFO_DEPTH`.

- [ ] **Step 3: Repoint the DPI bridge**

`cmodel_dpi.cpp:383` `kAxiQueueDepth` → `ni::NMU_QUEUE_DEPTH`; `:522` → `ni::NSU_QUEUE_DEPTH`; `:399` `kRobBDepth, kRobRDepth, kRobMaxTxnsPerId` → `ni::NMU_ROB_B_DEPTH, ni::NMU_ROB_R_DEPTH, ni::NMU_MAX_TXNS_PER_ID`. Add `#include "ni_params.h"` if not transitively present.

- [ ] **Step 4: Build + ctest** — Run: `make test` — Expected: PASS, same count.

- [ ] **Step 5: Commit**

```bash
git add src/c_model/include/wrap/nmu_wrap.hpp src/c_model/include/wrap/nsu_wrap.hpp src/dpi/cmodel_dpi.cpp
git commit -m "refactor(wrap,dpi): source create depth defaults from generated ni_params.h"
```

---

### Task 5: Strip the PortParams headers to the struct; repoint tests

**Files:**
- Modify: `src/c_model/include/nmu/port_params.hpp`, `nsu/port_params.hpp` (strip to struct)
- Modify: `src/c_model/tests/wrap/test_nmu_wrap.cpp:21,26,199`, `tests/integration/test_port_pair_loopback.cpp`, `test_request_response_loopback.cpp`, `tests/nmu/test_axi_slave_port.cpp`

**Interfaces:** Consumes `ni::NMU_*` / `ni::NSU_*`. After this task nothing calls `load_*_port_params`.

- [ ] **Step 1: Strip `nmu/port_params.hpp` to the struct with constant defaults**

Delete `#include <yaml-cpp/yaml.h>` and the whole `load_nmu_port_params` function; add `#include "ni_params.h"`; give the struct member initialisers:
```cpp
struct PortParams {
    std::size_t aw_queue_depth = ni::NMU_QUEUE_DEPTH;
    std::size_t w_queue_depth  = ni::NMU_QUEUE_DEPTH;
    std::size_t ar_queue_depth = ni::NMU_QUEUE_DEPTH;
    std::size_t b_queue_depth  = ni::NMU_QUEUE_DEPTH;
    std::size_t r_queue_depth  = ni::NMU_QUEUE_DEPTH;
    std::size_t depkt_b_q_depth = ni::NMU_DEPKT_Q_DEPTH;
    std::size_t depkt_r_q_depth = ni::NMU_DEPKT_Q_DEPTH;
};
```
Do the same for `nsu/port_params.hpp` (`ni::NSU_QUEUE_DEPTH` / `ni::NSU_DEPKT_Q_DEPTH`, plus the two MetaBuffer fields → `ni::NSU_META_BUFFER_*`), deleting `load_nsu_port_params`.

- [ ] **Step 2: Repoint the tests off the loaders**

In `test_nmu_wrap.cpp` (`:21` include, `:26`/`:199` `kAxiQueueDepth`) drop the `wrap_defaults.hpp` include and use `ni::NMU_QUEUE_DEPTH`. In the positive loader-calling tests, delete the `load_*_port_params(...)` call and default-construct `PortParams{}` (now self-defaulting from constants), adjusting any hardcoded `32` queue expectation to `ni::NMU_QUEUE_DEPTH` / 16.

**Delete the loader-error negative tests** (Codex) — they assert the retired loader throws, so the behaviour they test no longer exists: `LoaderMissingNmuBlockThrows` (`test_port_pair_loopback.cpp:46`), `LoaderMissingNmuQueueKeyThrows` (`:54`), and any NSU counterpart. Find them all first:
```bash
grep -rn "EXPECT_THROW\|EXPECT_ANY_THROW" src/c_model/tests | grep "load_n.u_port_params"
```
Remove each such `TEST(...)` block entirely — do not convert it.

- [ ] **Step 3: Grep no loader callers remain**

Run: `grep -rn "load_nmu_port_params\|load_nsu_port_params" src/c_model`
Expected: no matches.

- [ ] **Step 4: ctest** — Run: `make test` — Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/c_model/include/nmu/port_params.hpp src/c_model/include/nsu/port_params.hpp src/c_model/tests
git commit -m "refactor: strip PortParams to struct with generated defaults, drop YAML loaders"
```

---

### Task 6: Delete the retired files + inline the ChannelModel fixture

**Files:**
- Modify: `src/c_model/tests/common/channel_model_params.hpp`, `src/c_model/include/wrap/router_wrap.hpp:49`, ChannelModel test callers (`test_request_response_loopback.cpp:161,174,191`)
- Delete: `src/c_model/config/port_params.yaml`, `src/c_model/include/wrap/wrap_defaults.hpp`

**Interfaces:** Terminal cleanup.

- [ ] **Step 1: Inline the ChannelModel fixture constants**

In `channel_model_params.hpp` replace `load_channel_model_params(path)` (read `channel_model:` from the YAML) with constexpr `req_depth = 32`, `rsp_depth = 32`, and move `kChannelModelDepth = 64` here from `wrap_defaults.hpp`. Update the callers to use the inlined constants.

- [ ] **Step 2: Remove the stale `wrap_defaults.hpp` include from `router_wrap.hpp:49`**

Grep `router_wrap.hpp` for `kChannelModelDepth`; if used in code (not just the `:86` comment) replace with the ChannelModel fixture constant, then delete the include.

- [ ] **Step 3: Delete the retired files**

```bash
git rm src/c_model/config/port_params.yaml src/c_model/include/wrap/wrap_defaults.hpp
```

- [ ] **Step 4: Grep for dangling references**

Run: `grep -rn "wrap_defaults\|port_params.yaml\|kRobBDepth\|kRobRDepth\|kRobMaxTxnsPerId\|kAxiQueueDepth\|kMetaBuffer\|kArbiterFifoDepth\|kChannelModelDepth\|load_channel_model_params" src/`
Expected: no matches (all repointed).

- [ ] **Step 5: Build + ctest** — Run: `make test` — Expected: PASS, same count.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "refactor: delete port_params.yaml + wrap_defaults.hpp, inline ChannelModel fixture"
```

---

### Task 7: Repoint the SV tb generator + Makefile to the generated package

**Files:**
- Modify: `sim/tools/gen_tb_top.py` (NSU meta locals `:555,556`; RoB locals `:560-563`)
- Modify: `sim/verilator/Makefile` (defaults `:211-212` + `:224`, forwarding `:253-255`, prerequisite `:132-133`)

**Interfaces:** Consumes `ni_params_pkg::NMU_*_DFLT` / `NSU_*_DFLT`. `ni_params_pkg.sv` already in the tb sources (`sim/build_config.mk:87-88`).

- [ ] **Step 1: Init the plusarg locals from the package defaults**

In `gen_tb_top.py`, the generated tb declares mutable locals: NSU MetaBuffer `max_unique_ids = 1` / `max_outstanding = 32` (`:555,556`) and RoB `b_rob_depth/r_rob_depth/max_txns_per_id = 32` (`:560-563`). Change the emitted initialisers:
```python
    w("    int unsigned max_unique_ids  = ni_params_pkg::NSU_META_BUFFER_MAX_UNIQUE_IDS_DFLT;")
    w("    int unsigned max_outstanding = ni_params_pkg::NSU_META_BUFFER_MAX_OUTSTANDING_DFLT;")
    ...
    w("    int unsigned b_rob_depth = ni_params_pkg::NMU_ROB_B_DEPTH_DFLT;")
    w("    int unsigned r_rob_depth = ni_params_pkg::NMU_ROB_R_DEPTH_DFLT;")
    w("    int unsigned max_txns_per_id = ni_params_pkg::NMU_MAX_TXNS_PER_ID_DFLT;")
```
Leave the `$value$plusargs(...)` override lines (which write into these locals) unchanged.

- [ ] **Step 2: Makefile — drop `?=32`, conditional forwarding, YAML prerequisite**

Remove `B_ROB_DEPTH ?= 32` / `R_ROB_DEPTH ?= 32` (`:211-212`) and `MAX_TXNS_PER_ID ?= 32` (`:224`). Make the three plusarg-forwarding lines conditional (`:253-255`): `$(if $(B_ROB_DEPTH),"+b_rob_depth=$(B_ROB_DEPTH)")` etc, so a default run passes no empty plusarg. Add `$(SPECGEN)/generated/sv/ni_params_pkg.sv` as a prerequisite of the `tb_top` generation rule (`:132-133`).

- [ ] **Step 3: Regenerate a tb + confirm the package reference**

Run: `make build-verilator TOPOLOGY=mesh_4x4_vc1_rob`
Expected: build succeeds; `grep NMU_ROB_B_DEPTH_DFLT sim/tb/tb_top_mesh_4x4_vc1_rob.sv` shows the reference.

- [ ] **Step 4: Commit**

```bash
git add sim/tools/gen_tb_top.py sim/verilator/Makefile
git commit -m "refactor(sim): init tb depth locals from ni_params_pkg, drop Makefile ?=32 defaults"
```

---

### Task 8: Co-sim verification — behaviour unchanged + override still works

**Files:** none (verification only).

- [ ] **Step 1: Directed data-integrity run**

Run: `make sim TB=tb_mesh_4x4_vc1 PATTERN=neighbor`
Expected: DIRECTED PASS, scoreboard clean, 16 nodes reporting.

- [ ] **Step 2: Constrained-random run**

Run: `make sim TB=tb_mesh_4x4_vc1 PATTERN=constrained_random`
Expected: CR PASS (reorder_compare zero %Error).

- [ ] **Step 3: Prove the plusarg override still swaps a depth**

Run: `make sim TB=tb_mesh_4x4_vc1_rob PATTERN=neighbor B_ROB_DEPTH=64`
Expected: run passes; log reflects 64.

- [ ] **Step 4: Final grep — one home per depth**

Run: `grep -rn "kRobBDepth\|kRobRDepth\|kRobMaxTxnsPerId\|kAxiQueueDepth\|kMetaBuffer\|kArbiterFifoDepth" src/ ; grep -rn "= 32" src/c_model/include/nmu src/c_model/include/nsu | grep -iE "rob|queue|txns|outstanding"`
Expected: no matches — all reference `ni::*`.

- [ ] **Step 5: Full suite gate**

Run: `make test && cd specgen && python3 -m pytest -q`
Expected: both green. Working tree holds only the intended changes plus the two pre-existing DV-tier Makefile edits (untouched). Do not push.

---

## Self-review

- **Spec coverage:** constants+schema (T1), emit+golden (T2), model config (T3), wraps+DPI (T4), PortParams strip + tests incl `test_nmu_wrap` (T5), ChannelModel + router_wrap + deletions (T6), gen_tb_top + Makefile (T7), co-sim + override (T8). All spec Mechanism/OUTPUT bullets + all Codex-1 (build-safe deletion), Codex-2 (codegen cmd), Codex majors (test_nmu_wrap, wrap wiring collapse, rob.hpp include) + minor line fixes mapped.
- **Placeholder scan:** none.
- **Type consistency:** 11 symbols spelled identically across T1/T3/T4/T5/T7 (`NMU_QUEUE_DEPTH` cpp / `_DFLT` sv); values per spec. `PortParams` struct kept (not deleted) so `nmu.hpp:137` / `nsu.hpp:54` / `axi_slave_port` / `axi_master_port` types resolve.
- **Deferred (not gaps):** `--memory-size` auto-derive, `test_aperture` relocation — separate DV-tier round.
