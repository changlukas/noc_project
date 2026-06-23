# NI layer cleanup — delete c_model/include/ni/

**Date:** 2026-06-09

Delete the entire `c_model/include/ni/` sub-directory. Spec recognizes no "NI layer" — `meta.block = "Network Interface (NI: NMU + NSU)"` is an umbrella, not a sub-layer. 5 files there are c_model engineering convenience + ISP-violation residue:

- `packetizer.hpp` + `depacketizer.hpp` — 5/7-method abstract base; each NMU/NSU concrete class implements only one half, other half routed through `wrong_side_()` abort
- `port_params.hpp` — 13-field struct mixing NMU-only / NSU-only / test-fixture fields; comment self-confesses unused fields per side
- `wrong_side.hpp` — pure ISP-symptom runtime fallback
- `flit.hpp` — production code, survives migration; hand-maintained dispatch table replaced by codegen-emitted descriptor arrays

## Scope

**In:**

- Move + redistribute 5 ni/ files (3 deleted, 2 redistributed)
- Split base classes 5/7-method → 4 narrow interfaces per REQ/RSP × producer/consumer plane
- Split PortParams 13-field → `nmu::PortParams` (7) + `nsu::PortParams` (9) + `testing::ChannelModelParams` (2)
- YAML schema regroup → top-level `nmu:` / `nsu:` / `channel_model:` blocks + `queues:` sub-block
- Codegen emit `FieldDescriptor` arrays for header/payload field lookup (move hand-maintained dispatch into codegen)
- Update all `#include "ni/flit.hpp"` sites (flit.hpp path change; `grep -rln 'ni/flit.hpp' c_model/`) + ctor signature changes across nmu/nsu/test/cosim
- De-duplicate `NmuConfig` / `NsuConfig` (nmu.hpp / nsu.hpp): remove standalone depacketizer / meta_buffer depth fields, drive Depacketize / MetaBuffer construction from `cfg.port_params.*`
- Fix `docs/image/packet_format_overview.jpg` title typo (48b → 56b)

**Out:**

- `ni::cmodel::testing::` namespace retag (Codex flagged GoogleTest `::testing::` confusion; separate task)
- `packetizer` term → `BeatSink/Source` rename (Codex flagged concrete vs interface naming proximity; not needed here)
- Any RTL touch (refactor is c_model + codegen + test only)
- New CSR / spec changes
- Bit-range audit of `packet_format_overview.jpg` (this task only fixes the title typo, not bit ranges)

## Ground truth

- `specgen/generated/json/ni_packet.json` `meta.block = "Network Interface (NI: NMU + NSU)"` — NI is umbrella, no sub-layer
- `specgen/generated/json/ni_packet.json` `payload_channels[].network = "REQ" | "RSP"` — REQ + RSP are independent network planes
- `docs/image/nmu.jpg` + `docs/image/nsu.jpg` — per-unit Packetizing / De-packetizing stages; NMU has Read Re-Ordering, NSU has Meta Buffer (asymmetric)
- `docs/image/header.jpg` + `ni_packet.json HEADER_TOTAL_WIDTH=56` — 56-bit header authoritative; `packet_format_overview.jpg` title says 48 (stale typo, bit ranges in the same figure are 56-consistent)
- `c_model/include/ni/port_params.hpp:33-34` — self-confessed ISP comment ("Kept as a single struct to match the Packetizer/Depacketizer abstract base shape")
- `c_model/include/ni/flit.hpp:55-75` + `:98-163` — hand-maintained `name → {LSB, MSB}` dispatch tables targeted for codegen migration
- `c_model/config/port_params.yaml` comment "unused fields on each side are loaded but ignored" — explicit ISP-by-config admission

## Design

### File disposition

| Old | New | Action |
|---|---|---|
| `ni/flit.hpp` | `c_model/include/flit.hpp` | Move + replace hand-maintained dispatch with generic loop over codegen `FieldDescriptor` arrays |
| `ni/port_params.hpp` | `nmu/port_params.hpp` + `nsu/port_params.hpp` + `tests/common/channel_model_params.hpp` | Split |
| `ni/packetizer.hpp` | `c_model/include/request_io.hpp` + `c_model/include/response_io.hpp` | Replace with 4 narrow interfaces |
| `ni/depacketizer.hpp` | same | |
| `ni/wrong_side.hpp` | — | Delete (root-fixed by narrow interfaces) |

### Narrow interface (replaces 5/7-method base)

| Interface | Methods | Implementer | Loopback stub |
|---|---|---|---|
| `RequestPacketizer` | `push_aw/w/ar` (3) | `nmu::Packetize` | `LoopbackRequestPacketizer` |
| `RequestDepacketizer` | `pop_aw/w/ar` (3) | `nsu::Depacketize` | `LoopbackRequestDepacketizer` |
| `ResponsePacketizer` | `push_b/r` (2) | `nsu::Packetize` | `LoopbackResponsePacketizer` |
| `ResponseDepacketizer` | `pop_b/r` (2 pure-virtual) + `pop_b/r_with_meta` (2 virtual w/ default) | `nmu::Depacketize` | `LoopbackResponseDepacketizer` |

`pop_b_with_meta` / `pop_r_with_meta` default impl preserved from current `ni/depacketizer.hpp:45-54` — forwards to `pop_b()` / `pop_r()` and stamps `ResponseMeta{0, 0}` so Disabled-mode callers and non-Rob fixtures don't need to override:

```cpp
virtual std::optional<std::pair<axi::BBeat, ResponseMeta>> pop_b_with_meta() {
    auto b = pop_b();
    if (!b) return std::nullopt;
    return std::make_pair(*b, ResponseMeta{0, 0});
}
```

Namespace `ni::cmodel` (NMU/NSU boundary contract, not subsystem-scoped). `ResponseMeta` co-locates with `ResponseDepacketizer` in `response_io.hpp`. Interface files live at top-level — they pass AXI beats, not flits, so do not belong in `noc/` (which owns `NocReqIn/Out` + arbitration).

Ctor signature changes:

```cpp
nmu::AxiSlavePort(RequestPacketizer&, ResponseDepacketizer&, nmu::PortParams);
nsu::AxiMasterPort(RequestDepacketizer&, ResponsePacketizer&, nsu::PortParams);
```

### PortParams split

| Struct | Fields | Loader |
|---|---|---|
| `nmu::PortParams` | 5 queue_depth + `depkt_b_q_depth` + `depkt_r_q_depth` (7) | `load_nmu_port_params(path)` |
| `nsu::PortParams` | 5 queue_depth + `depkt_aw/w/ar_q_depth` + `meta_buffer_per_id_depth` (9) | `load_nsu_port_params(path)` |
| `ni::cmodel::testing::ChannelModelParams` | `req_depth` + `rsp_depth` (2) | `load_channel_model_params(path)` |

5 `queue_depth` fields duplicated across NMU/NSU structs — Codex confirmed: spec does not mandate symmetry, size pressure tests may want asymmetry, only saves 5 lines via composition at cost of `params.queues.aw_queue_depth` indirection layer. Re-evaluate when a 3rd production consumer appears.

#### NmuConfig / NsuConfig de-duplication

`c_model/include/nmu/nmu.hpp` `NmuConfig` and `c_model/include/nsu/nsu.hpp` `NsuConfig` currently carry standalone fields for depacketizer queue depths / MetaBuffer depth that **shadow** the corresponding `PortParams` fields — production `Nmu::Nmu` / `Nsu::Nsu` constructs `Depacketize` and `MetaBuffer` from these standalone fields, **not** from `cfg.port_params.*`. After the split, the YAML-loaded `nmu::PortParams::depkt_*_q_depth` and `nsu::PortParams::meta_buffer_per_id_depth` would have no effect unless the duplication is removed.

Action (part of commit #3 below): delete the standalone fields from `NmuConfig` / `NsuConfig`; rewrite `Nmu::Nmu` / `Nsu::Nsu` to drive Depacketize and MetaBuffer constructors from `cfg.port_params.*`. Single source of truth: YAML → typed `PortParams` → both `AxiSlavePort/AxiMasterPort` and `Depacketize/MetaBuffer`.

### YAML schema

```yaml
nmu:
  queues:
    aw_queue_depth: 32
    w_queue_depth:  32
    ar_queue_depth: 32
    b_queue_depth:  32
    r_queue_depth:  32
  depacketize:
    b_q_depth: 32
    r_q_depth: 32
nsu:
  queues:
    aw_queue_depth: 32
    w_queue_depth:  32
    ar_queue_depth: 32
    b_queue_depth:  32
    r_queue_depth:  32
  depacketize:
    aw_q_depth: 32
    w_q_depth:  32
    ar_q_depth: 32
  meta_buffer:
    per_id_depth: 4
channel_model:
  req_depth: 32
  rsp_depth: 32
```

Each loader parses only its own top-level block. `queues:` sub-block isolates 5 AXI-channel depths from future port-level config additions (backpressure policy, etc.).

### Loopback channel set

```cpp
struct RequestChannelSet {
    std::size_t aw_capacity = 32, w_capacity = 32, ar_capacity = 32;
    std::deque<axi::AwBeat> aw;
    std::deque<axi::WBeat>  w;
    std::deque<axi::ArBeat> ar;
};
struct ResponseChannelSet {
    std::size_t b_capacity = 32, r_capacity = 32;
    std::deque<axi::BBeat> b;
    std::deque<axi::RBeat> r;
};
struct LoopbackChannelSet {
    RequestChannelSet  request;
    ResponseChannelSet response;
};
```

Narrow stubs depend only on the sub-set they need (NMU unit test instantiates `RequestPacketizer` stub + `ResponseDepacketizer` stub backed by `RequestChannelSet` + `ResponseChannelSet`). Integration loopback wires the aggregate `LoopbackChannelSet`.

### Codegen emit strategy

Extend `specgen/tools/elaborate/cpp_packet.py` to emit `constexpr FieldDescriptor` arrays into `ni_flit_constants.h` alongside existing LSB/MSB constants:

```cpp
struct FieldDescriptor { std::string_view name; int lsb; int msb; };

constexpr FieldDescriptor HEADER_FIELDS[] = {
    {"noc_qos", NOC_QOS_LSB, NOC_QOS_MSB},
    {"axi_ch",  AXI_CH_LSB,  AXI_CH_MSB},
    // ... only fields with enabled = true; rsvd skipped
};

constexpr FieldDescriptor AW_PAYLOAD_FIELDS[] = { ... };
constexpr FieldDescriptor AR_PAYLOAD_FIELDS[] = { ... };
constexpr FieldDescriptor W_PAYLOAD_FIELDS[]  = { ... };
constexpr FieldDescriptor B_PAYLOAD_FIELDS[]  = { ... };
constexpr FieldDescriptor R_PAYLOAD_FIELDS[]  = { ... };
```

`flit.hpp` replaces 13-entry hand-listed `if (name == "X") return {X_LSB, X_MSB};` with one generic loop over `HEADER_FIELDS[]`. Payload dispatch routes per `channel` string to the matching array.

`_ENABLED` policy:
- Header: emitter skips fields with `enabled: false` (current spec: `rsvd`)
- Payload: emitter skips fields with `width == 0` (spec has no per-payload `_ENABLED` flag)

`FieldDescriptor` uses `std::string_view` → generated `ni_flit_constants.h` adds `#include <string_view>`.

Codegen emits data only — `assert`, `abort`, case normalization stay hand-written in `flit.hpp`. SV emitter (`sv_packet.py`) is not modified; RTL accesses fields via packed-struct slicing, not name-keyed dispatch.

**Behaviour change**: current `flit.hpp:68` still returns a `{LSB, MSB}` for `"rsvd"` even though `RSVD_ENABLED = false`. After this refactor, `header_field_pos("rsvd")` aborts (name not in `HEADER_FIELDS[]`). Production has no caller of `"rsvd"` today (dormant code per `[[pending-delete-ni-directory]]` memory), but this is a contract narrowing — covered by a dedicated test (see Test additions).

**Golden update**: `specgen/tests/golden/ni_flit_constants.h.golden` regenerates as part of commit #1; existing byte-identical gate at `specgen/tests/test_byte_identical_golden.py:57` covers drift detection.

### Commit sequencing

| # | Commit | Buildable? |
|---|---|---|
| 1 | Extend codegen `cpp_packet.py` to emit `FieldDescriptor` arrays; regen `ni_flit_constants.h` + update golden; no consumer change yet | ✓ |
| 2 | Move `ni/flit.hpp` → `c_model/include/flit.hpp`; rewrite hand dispatch → generic loop over `FieldDescriptor`; rewrite all `#include "ni/flit.hpp"` sites (`grep -rln 'ni/flit.hpp' c_model/`) | ✓ |
| 3 | Atomic: add `nmu/port_params.hpp` + `nsu/port_params.hpp` + `tests/common/channel_model_params.hpp` + new YAML schema; de-duplicate `NmuConfig` / `NsuConfig`; migrate all `port_params`-touching code (production + tests + cosim adapters incl. removing `channel_model_*_depth` assignments in `nmu_shell_adapter.hpp:51` and `nsu_shell_adapter.hpp:56`); delete `ni/port_params.hpp` | ✓ |
| 4 | Atomic: add `request_io.hpp` + `response_io.hpp`; switch 4 concrete classes + `AxiSlavePort` + `AxiMasterPort` to narrow interfaces; refactor Loopback into `RequestChannelSet` + `ResponseChannelSet` + sub-set stubs in `loopback_request_io.hpp` + `loopback_response_io.hpp`; migrate all callers (incl. `DelayedLoopback` at `test_port_pair_loopback.cpp:91`); delete `ni/packetizer.hpp` + `ni/depacketizer.hpp` + `ni/wrong_side.hpp` + `loopback_packetizer.hpp` + `loopback_depacketizer.hpp` | ✓ |
| 5 | Fix `docs/image/packet_format_overview.jpg` title 48 → 56 | ✓ |

Atomic-bundle rationale for #3 and #4: splitting Loopback stub migration from base-interface deletion (orig. #3+#5) leaves a window where Loopback inherits a deleted base — unbuildable. Splitting `NmuConfig` / `NsuConfig` de-dup from `PortParams` split leaves YAML loader values inert. Each commit compiles + passes all tests (CLAUDE.md quality gate).

## Test additions

- Schema fail-loud tests for each loader: missing top-level block, missing key inside block
- Integration test loading **asymmetric** queue values (`nmu.queues.aw_queue_depth=8`, `nsu.queues.aw_queue_depth=64`) to verify struct split allows independent NMU/NSU sizing
- Integration test loading **asymmetric** depacketize / meta_buffer values to verify `NmuConfig` / `NsuConfig` de-duplication routes YAML values through to `Depacketize` / `MetaBuffer` constructors
- Codegen golden test: regen `ni_flit_constants.h.golden` + existing byte-identical gate (`specgen/tests/test_byte_identical_golden.py:57`) catches drift in `HEADER_FIELDS[]` and 5 per-channel `*_PAYLOAD_FIELDS[]`
- Contract narrowing test: `header_field_pos("rsvd")` aborts after refactor (previously returned `{RSVD_LSB, RSVD_MSB}` despite `RSVD_ENABLED = false`)

## Out-of-scope (backlog)

- `ni::cmodel::testing::` namespace retag (avoid GoogleTest `::testing::` confusion; touches `loopback_*`, `per_channel_capture`, `scenario`, `test_logger`)
- `packetizer` → `BeatSink/Source` term rename (interface vs concrete `Packetize` proximity)
- `packet_format_overview.jpg` bit-range source-of-truth audit (this task fixes title only)
