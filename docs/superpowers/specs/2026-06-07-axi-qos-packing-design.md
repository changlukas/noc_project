# AXI QoS packing — design spec

**Date:** 2026-06-07
**Topic:** Add `awqos` / `arqos` to AW/AR flit payload; close the spec-vs-impl gap where prior implementation dropped these signals.
**Scope:** packet_format spec + codegen regen + NMU packetize + NSU depacketize + AXI master driver + scenario YAML parser + 1 round-trip test + 1 integration scenario. Single-NI loopback; no router QoS arbitration.

## 1. Problem

`spec/ni/doc/packet_format.md` §2.2.1 / §3.1 normatively states `awqos`/`arqos` are AXI payload fields, packed by NMU and unpacked by NSU. Three layers currently violate that:

- `specgen/generated/json/ni_packet.json` AW/AR field list has no `awqos`/`arqos`.
- `c_model/include/nmu/packetize.hpp:96-115,143-164` packs 11 AW fields and 11 AR fields but never calls `set_payload_field("AW", "awqos", ...)`.
- `c_model/include/nsu/depacketize.hpp:73,98` hardcodes `b.qos = 0` with comment "not in NoC flit".

`c_model/include/axi/types.hpp:85,100` `AwBeat` / `ArBeat` already carry `uint8_t qos`, so the AXI struct layer is correct; only packetize / depacketize / spec source / codegen are short.

`spec/ni/doc/packet_format.md` §2.2.1 also clarifies that `noc_qos` (NoC-layer arbitration field, `NOC_QOS_WIDTH=4` per `docs/image/header.jpg`) and `axi_qos` (AXI4 payload) are different fields at different layers. This design only adds `axi_qos` to the payload; `noc_qos` zero-fill remains deferred.

## 2. Ground truth references

Authoritative anchors (per `feedback-image-spec-ground-truth`):

- `docs/image/header.jpg` — `HEADER_WIDTH = 56`, `NOC_QOS_WIDTH = 4` default, `EN_QOS` option flag, padding "56 − HEADER_WIDTH"
- `docs/image/aw_ar_format.jpg` — 11 AW/AR fields, total 108 bits, `aw_rsvd[107:105]` 3b derived
- `specgen/generated/cpp/ni_flit_constants.h:15` — `FLIT_WIDTH = 408`
- `cosim/c/cmodel_dpi.cpp:30` — `static_assert(::ni::FLIT_WIDTH == 408)`
- AXI4 IHI 0022H §A8.1.1 — `awqos[3:0]` is a 4-bit master-set priority hint, slave behavior unspecified, no protocol-level mutation between master and slave

## 3. Design

### 3.1 Bit layout change

Insert `awqos` at `[100:97]` between `awregion` and `awuser`. AR symmetric (arqos at the same position via spec's "prefix-swap" rule).

| Field | Width Symbol | Default Range | Note |
|---|---|---|---|
| awid | `AXI_ID_WIDTH` | `[7:0]` | unchanged |
| awaddr | `AXI_ADDR_WIDTH` | `[71:8]` | unchanged |
| awlen | `AXI_LEN_WIDTH` | `[79:72]` | unchanged |
| awsize | `AXI_SIZE_WIDTH` | `[82:80]` | unchanged |
| awburst | `AXI_BURST_WIDTH` | `[84:83]` | unchanged |
| awcache | `AXI_CACHE_WIDTH` | `[88:85]` | unchanged |
| awlock | `AXI_LOCK_WIDTH` | `[89]` | unchanged |
| awprot | `AXI_PROT_WIDTH` | `[92:90]` | unchanged |
| awregion | `AXI_REGION_WIDTH` | `[96:93]` | unchanged |
| **awqos** | **`AXI_QOS_WIDTH`** | **`[100:97]`** | **new — matches IHI 0022H Table A8-1 position** |
| awuser | `AXI_USER_WIDTH` | `[108:101]` | shifted +4 |
| aw_rsvd | derived (3) | `[111:109]` | shifted +4 |

`AW_PAYLOAD_WIDTH`: 108 + `AXI_QOS_WIDTH`(4) = 112. `PAYLOAD_WIDTH` stays 352 (W/R union max). `HEADER_WIDTH` stays 56. `FLIT_WIDTH` stays 408. DPI `static_assert(FLIT_WIDTH == 408)` unchanged.

### 3.2 Parameter addition

`spec/ni/doc/packet_format.md` §1.2 Group 3 add row:

| Parameter | Default | Description |
|---|---|---|
| `AXI_QOS_WIDTH` | 4 | AXI QoS signal width (per AXI4 IHI 0022H §A8.1.1) |

Width is parameterized per `feedback-parameterized-design-test-all-values`. Default 4 is the AXI4 fixed value.

### 3.3 Codegen impact

`specgen/ni_spec/generator/packet.py` parses §1.2 Group 3 and §3.1 AW table from the markdown. Inserting one new row in each table makes the generator emit:

- `ni_packet.json`: new entries for `AXI_QOS_WIDTH` in `field_widths`, `awqos` in AW `fields[]`, `arqos` in AR `fields[]`
- `ni_flit_constants.h`: new constants `payload::aw::AWQOS_LSB/MSB`, `payload::ar::ARQOS_LSB/MSB`; per-channel `AW_PAYLOAD_WIDTH = 112`; `width::AXI_QOS_WIDTH = 4`
- `ni_flit_pkg.sv`: SV mirror of the above
- Specgen pytest golden files (`specgen/tests/test_byte_identical_golden.py:57-75` baseline)

All four artifacts regen in one pass via `py -3 specgen/tools/codegen.py --target cpp --domain packet` and `--target sv --domain packet`. Golden test files are updated alongside.

### 3.4 c_model wiring

Three files touched (shell adapters already wire `qos` end-to-end at `nmu_shell_adapter.hpp:100,126` and `nsu_shell_adapter.hpp:124,158`; no shell-adapter change needed):

| File | Change |
|---|---|
| `c_model/include/nmu/packetize.hpp` | Insert `set_payload_field("AW", "awqos", b.qos)` after the awregion line (line 113) and `set_payload_field("AR", "arqos", b.qos)` after the arregion line (line 160). |
| `c_model/include/nsu/depacketize.hpp` | Replace `b.qos = 0;` (line 73, AW) with `b.qos = static_cast<uint8_t>(f.get_payload_field("AW", "awqos"));` Same for AR (line 98). Remove the "not in NoC flit" comment. |
| `c_model/include/nmu/depacketize.hpp` | No change. NMU response-side depacketize handles B/R only; AXI4 has no `bqos` / `rqos`. |
| `c_model/include/axi/scenario_parser.hpp` | (a) Add `uint8_t qos = 0;` to `ScenarioTransaction` struct (around line 40-52). (b) Add `qos` YAML key parse in the txn loader (around lines 171-227): `if (auto v = txn["qos"]) tx.qos = v.as<uint8_t>();`. Default 0 when absent. |
| `c_model/include/axi/axi_master.hpp` | At the `AwBeat` construction site (lines 414-434) assign `aw.qos = src.qos;`. At the `ArBeat` construction site (lines 476-485) assign `ar.qos = src.qos;`. |

### 3.5 Test additions

| Layer | What | Where |
|---|---|---|
| Unit (packetize) | 1 AW round-trip + 1 AR round-trip: `b.qos = 0xA` → packetize → `EXPECT_EQ(f.get_payload_field("AW","awqos"), 0xAu)` | `c_model/tests/nmu/test_packetize.cpp` |
| Unit (depacketize) | 1 AW + 1 AR symmetric reverse | `c_model/tests/nsu/test_nsu_depacketize.cpp` |
| Integration | One scenario through AxiMaster → NMU → LoopbackNoc → NSU → AxiSlave with `qos: 0xA` | `tests/scenarios/AX4-QOS-001_awqos_round_trip/scenario.yaml` |

Test pattern follows existing `test_packetize.cpp:223` idiom (`EXPECT_EQ(f.get_header_field("noc_qos"), 0u)` is the same idiom applied to a payload field).

### 3.6 Scenario category registration

`AX4-QOS-NNN` is a new category code. Three places need updating before the scenario YAML can land:

- `tools/lint_scenarios.py` CAT allow-list
- `c_model/include/axi/scenario_parser.hpp` CAT allow-list
- `tests/scenarios/README.md` IHI 0022H section coverage table (add `QOS — A8 QoS signaling`)

## 4. Commit chain

Three atomic commits per `feedback-codex-review-each-round` (each independently `make check` clean):

1. **`spec(packet): add AXI_QOS_WIDTH parameter and awqos/arqos payload fields`** — `packet_format.md` §1.2 + §3.1 edits, full `codegen.py` regen (`ni_packet.json` + `ni_flit_constants.h` + `ni_flit_pkg.sv` + specgen golden test files). No c_model change.
2. **`feat(ni): pack awqos/arqos through NMU/NSU and CAT-register QOS scenarios`** — `packetize.hpp` + `depacketize.hpp` (both NMU and NSU sides) + `scenario_parser.hpp` + `axi_master.hpp` + `nmu_shell_adapter.hpp` + `nsu_shell_adapter.hpp` + scenario CAT allow-lists (`lint_scenarios.py`, `scenario_parser.hpp`, `tests/scenarios/README.md`).
3. **`test(qos): awqos round-trip unit and integration coverage`** — `test_packetize.cpp` + `test_depacketize.cpp` round-trip cases + `tests/scenarios/AX4-QOS-001_awqos_round_trip/scenario.yaml`.

Each commit passes `make check` (lint_scenarios + lint_docs + build-cmodel + build-verilator + ctest).

## 5. Out of scope

- Updating `docs/image/aw_ar_format.jpg` — image regen is a separate jpeg-tooling task; mark `[TBD image regen]` in spec commit message body.
- QoS Generator (Bypass / Fixed / Limiter / Regulator) — separate session per `project_pending_axi_qos_packing` followup. This design is passthrough only.
- `noc_qos` activation — still deferred; flit-header bit position remains zero-filled.
- NoC router-side QoS arbitration — LoopbackNoc has no QoS-aware arbiter; out-of-scope here.
- Update of `spec/ni/doc/packet_format.md` text-vs-image inconsistencies (FLIT_WIDTH=402, HEADER_WIDTH=50, etc.) — separate `Family A markdown realign` session per audit findings.

## 6. Success criteria

- `make check` clean across all three commits
- `specgen/tools/codegen.py --check` drift gate passes
- AW round-trip test: `b.qos = 0xA` on packetize, `EXPECT_EQ(get_payload_field("AW", "awqos"), 0xAu)`. AR symmetric.
- Integration scenario `AX4-QOS-001`: scenario.yaml `qos: 0xA` propagates through master → NMU → LoopbackNoc → NSU → slave with downstream `axi_awqos_o == 0xA`

## 7. Risks

- **Existing `test_packetize.cpp:161-173` already sets `aw.qos = 0xF`** but does not assert any payload `awqos` field. Codex flagged this in round-1 review. The new round-trip test must assert the actual packed bit position, not just that depacketize recovers the value, to catch any future drift.
- **Specgen golden test files** are byte-identical baselines (`specgen/tests/test_byte_identical_golden.py`). Commit 1 must update goldens or the specgen pytest suite breaks.
- **Scenario tooling allow-list drift** — if `lint_scenarios.py`, `scenario_parser.hpp`, and `tests/scenarios/README.md` are not all updated together, `make check` will fail on the QOS-001 scenario.
