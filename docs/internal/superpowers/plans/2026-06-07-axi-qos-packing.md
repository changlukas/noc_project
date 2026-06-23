# AXI QoS Packing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `awqos` / `arqos` to the AW/AR flit payload so the AXI QoS bits propagate end-to-end through NMU → NoC → NSU instead of being dropped at packetize and forced to 0 at depacketize.

**Architecture:** One spec-source edit drives codegen regen across `ni_packet.json` / `ni_flit_constants.h` / `ni_flit_pkg.sv`. Two `set_payload_field` calls in NMU packetize and two `get_payload_field` calls in NSU depacketize wire the new bits. Scenario YAML grows a `qos` key. New scenario CAT `QOS` is registered. Three atomic commits, each `make check` clean.

**Tech Stack:** C++17, CMake 3.20+, GoogleTest, Verilator 5.036, Python 3.9+ (PyYAML), specgen codegen toolchain.

**Anchor:** `docs/image/{header,aw_ar_format,b_format,w_format,r_format}.jpg` are ground truth — `FLIT_WIDTH=408`, `HEADER_WIDTH=56`, `NOC_QOS_WIDTH=4`. Spec markdown text that disagrees is stale (see `memory/feedback_image_spec_ground_truth.md`).

**Spec doc:** `docs/superpowers/specs/2026-06-07-axi-qos-packing-design.md` (committed at `7550c21`).

---

## Task 1 — Spec source edit + codegen regen + golden update

**Files:**
- Modify: `spec/ni/doc/packet_format.md` (§1.2 Group 3 and §3.1 AW table)
- Generated (regen, do not hand-edit): `specgen/generated/json/ni_packet.json`, `specgen/generated/cpp/ni_flit_constants.h`, `specgen/generated/sv/ni_flit_pkg.sv`
- Update (goldens): `specgen/tests/golden/ni_flit_constants.h.golden`, `specgen/tests/golden/ni_flit_pkg.sv.golden`, `specgen/tests/golden/ni_packet.json.golden` (if exists)

- [ ] **Step 1.1: Add `AXI_QOS_WIDTH` row to §1.2 Group 3 in `packet_format.md`**

  Locate the Group 3 AXI Payload Sub-Fields table (search for `AXI_ADDR_WIDTH | 64`). Insert a new row immediately after `AXI_REGION_WIDTH`:

  ```markdown
  | `AXI_QOS_WIDTH` | 4 | AXI QoS signal width (per AXI4 IHI 0022 §A8.1.1; master-set priority hint, 4 bits) |
  ```

- [ ] **Step 1.2: Add `awqos` row to §3.1 AW table in `packet_format.md`**

  Locate the §3.1 AW/AR Channel Payload table (the row block starting `| awid | AXI_ID_WIDTH | [7:0] |`). Insert `awqos` between `awregion` and `awuser`, and shift `awuser` / `aw_rsvd` ranges:

  ```markdown
  | awregion | `AXI_REGION_WIDTH` | [96:93] | Memory region identifier |
  | awqos | `AXI_QOS_WIDTH` | [100:97] | QoS priority (per AXI4 IHI 0022 §A8.1.1) |
  | awuser | `AXI_USER_WIDTH` | [108:101] | AXI user signal |
  | aw_rsvd | derived (3) | [111:109] | Reserved (alignment) |
  ```

  Update the section heading line if it states a numeric width: change `AW_PAYLOAD_WIDTH / AR_PAYLOAD_WIDTH = 108 bits` to `= 112 bits`.

- [ ] **Step 1.3: Regenerate the C++ packet constants**

  Run: `py -3 specgen/tools/codegen.py --target cpp --domain packet`
  Expected: `specgen/generated/cpp/ni_flit_constants.h` now contains `AWQOS_LSB = 97`, `AWQOS_MSB = 100`, `ARQOS_LSB = 97`, `ARQOS_MSB = 100`, `AXI_QOS_WIDTH = 4`, `AW_PAYLOAD_WIDTH = 112`, `AR_PAYLOAD_WIDTH = 112`.

  Verify with: `grep -n "AXI_QOS_WIDTH\|AWQOS\|ARQOS\|AW_PAYLOAD_WIDTH" specgen/generated/cpp/ni_flit_constants.h`

- [ ] **Step 1.4: Regenerate the SV packet package**

  Run: `py -3 specgen/tools/codegen.py --target sv --domain packet`
  Expected: `specgen/generated/sv/ni_flit_pkg.sv` mirrors the C++ constants.

  Verify with: `grep -n "AXI_QOS_WIDTH\|AWQOS\|ARQOS\|AW_PAYLOAD_WIDTH" specgen/generated/sv/ni_flit_pkg.sv`

- [ ] **Step 1.5: Refresh specgen golden test baselines**

  Run: `py -3 -m pytest specgen/tests/test_byte_identical_golden.py -v`
  Expected on first run: FAIL with "actual ≠ golden" because we just changed the generated artifacts.

  Update the goldens. From repo root:

  ```bash
  cp specgen/generated/cpp/ni_flit_constants.h specgen/tests/golden/ni_flit_constants.h.golden
  cp specgen/generated/sv/ni_flit_pkg.sv specgen/tests/golden/ni_flit_pkg.sv.golden
  ```

  If `specgen/tests/golden/ni_packet.json.golden` exists, also:

  ```bash
  cp specgen/generated/json/ni_packet.json specgen/tests/golden/ni_packet.json.golden
  ```

  Re-run: `py -3 -m pytest specgen/tests/test_byte_identical_golden.py -v`
  Expected: PASS.

- [ ] **Step 1.6: Run codegen drift gate**

  Run: `py -3 specgen/tools/codegen.py --check`
  Expected: exit code 0 (no drift between committed JSON and what the generator would emit).

- [ ] **Step 1.7: Run full `make check`**

  Run: `make check PYTHON3="py -3"`
  Expected: all green. Existing tests still pass because the new bit positions are unused; AW/AR payloads simply grew from 108 to 112 bits within the still-352-bit union.

- [ ] **Step 1.8: Commit**

  ```bash
  git add spec/ni/doc/packet_format.md specgen/generated/cpp/ni_flit_constants.h specgen/generated/sv/ni_flit_pkg.sv specgen/generated/json/ni_packet.json specgen/tests/golden/
  git commit -m "spec(packet): add AXI_QOS_WIDTH parameter and awqos/arqos payload fields

  Insert awqos at [100:97] between awregion and awuser in the AW payload
  (arqos symmetric in AR via prefix-swap), per AXI4 IHI 0022H Table A8-1
  ordering. AW_PAYLOAD_WIDTH grows 108 -> 112; PAYLOAD_WIDTH stays 352
  (W/R union max), HEADER_WIDTH stays 56, FLIT_WIDTH stays 408. Regen
  ni_packet.json + ni_flit_constants.h + ni_flit_pkg.sv from the spec
  source via specgen/tools/codegen.py; refresh byte-identical goldens.

  Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
  ```

---

## Task 2 — c_model wiring + QOS scenario CAT registration

**Files:**
- Modify: `c_model/include/nmu/packetize.hpp` (after line 113 for AW, after line 160 for AR)
- Modify: `c_model/include/nsu/depacketize.hpp` (line 73 AW, line 98 AR)
- Modify: `c_model/include/axi/scenario_parser.hpp` (struct around line 40-52; YAML parse around line 171-227; CAT allow-list at line 85-86)
- Modify: `c_model/include/axi/axi_master.hpp` (line 414-434 AW build; line 476-485 AR build)
- Modify: `tools/lint_scenarios.py` (CAT allow-list at line 12-19)
- Modify: `sim/test_patterns/README.md` (CAT mapping table around line 12-23)

- [ ] **Step 2.1: Wire `awqos` in NMU packetize AW path**

  Open `c_model/include/nmu/packetize.hpp` and find the AW `set_payload_field` block ending at line 114 (`set_payload_field("AW", "awuser", b.user);`). Insert a new line directly after `awregion` (line 113):

  ```cpp
  f.set_payload_field("AW", "awqos", b.qos);
  ```

- [ ] **Step 2.2: Wire `arqos` in NMU packetize AR path**

  In the same file, find the AR block ending at line 161 (`set_payload_field("AR", "aruser", b.user);`). Insert directly after `arregion`:

  ```cpp
  f.set_payload_field("AR", "arqos", b.qos);
  ```

- [ ] **Step 2.3: Wire AW qos unpack in NSU depacketize**

  Open `c_model/include/nsu/depacketize.hpp`. Locate line 73 (the AW `decode_aw` body). Replace:

  ```cpp
  b.qos = 0;  // not in NoC flit (separate flit field noc_qos)
  ```

  with:

  ```cpp
  b.qos = static_cast<uint8_t>(f.get_payload_field("AW", "awqos"));
  ```

- [ ] **Step 2.4: Wire AR qos unpack in NSU depacketize**

  In the same file, find the corresponding line in `decode_ar` (line 98). Replace:

  ```cpp
  b.qos = 0;  // not in NoC flit (separate flit field noc_qos)
  ```

  with:

  ```cpp
  b.qos = static_cast<uint8_t>(f.get_payload_field("AR", "arqos"));
  ```

- [ ] **Step 2.5: Add `qos` field to `ScenarioTransaction`**

  Open `c_model/include/axi/scenario_parser.hpp`. Locate the struct definition at line 40. Insert a new field after `lock`:

  ```cpp
  LockType lock = LockType::Normal;  // optional; YAML "normal" or "exclusive"
  uint8_t qos = 0;                   // optional; YAML "qos" (default 0)
  std::size_t scenario_line;
  ```

- [ ] **Step 2.6: Parse `qos` YAML key in `load_scenario`**

  Search the same file for the `lock` YAML parse site (look for `txn["lock"]` or `LockType::Exclusive`). Add a sibling parse for `qos` immediately after, e.g.:

  ```cpp
  if (auto q = txn["qos"]) tx.qos = static_cast<uint8_t>(q.as<unsigned>());
  ```

  Place it in the same per-transaction loop. The YAML field is optional; absent → default 0 (already set by the struct initializer).

- [ ] **Step 2.7: Add `QOS` to scenario CAT allow-list in `scenario_parser.hpp`**

  At `c_model/include/axi/scenario_parser.hpp` line 85 update the regex:

  ```cpp
  R"(^AX4-(BAS|BUR|BND|ORD|EXC|RSP|STR|HSH|INF|QOS)-\d{3}_[a-z0-9_]+$)");
  ```

  At line 86 update the `kCatCategory` map by inserting one entry:

  ```cpp
  {"QOS", "qos"},
  ```

- [ ] **Step 2.8: Plumb `tx.qos` into AwBeat at AxiMaster AW build site**

  Open `c_model/include/axi/axi_master.hpp`. Locate line 414 (`AwBeat aw{};`). After `aw.lock = ...;` (line 423), insert:

  ```cpp
  aw.qos = op.src_txn.qos;
  ```

- [ ] **Step 2.9: Plumb `tx.qos` into ArBeat at AxiMaster AR build site**

  In the same file, locate line 476 (`ArBeat ar{};`). After `ar.lock = ...;` (line 484), insert:

  ```cpp
  ar.qos = op.src_txn.qos;
  ```

- [ ] **Step 2.10: Add `QOS` CAT to `tools/lint_scenarios.py`**

  Open `tools/lint_scenarios.py`. Locate the CAT allow-list (around line 12-19, search for `"BAS"` or `kCatCategory` equivalent). Add `QOS` to the set / list / regex consistent with how the existing CATs are stored.

- [ ] **Step 2.11: Add `QOS` row to `sim/test_patterns/README.md` IHI table**

  Open `sim/test_patterns/README.md`. Locate the IHI 0022H section-to-CAT table (around line 12-23). Insert a new row:

  ```markdown
  | QOS | A8 QoS signaling | awqos / arqos passthrough end-to-end |
  ```

- [ ] **Step 2.12: Build to verify nothing breaks**

  Run: `make build PYTHON3="py -3"`
  Expected: clean build of c_model + Verilator.

- [ ] **Step 2.13: Run full ctest**

  Run: `cd c_model/build && ctest --output-on-failure`
  Expected: all green. New behavior is exercised only when a `qos`-bearing AwBeat is pushed, which existing scenarios don't do; existing assertions don't observe `qos`, so nothing regresses.

- [ ] **Step 2.14: Run `make check`**

  Run: `make check PYTHON3="py -3"`
  Expected: clean.

- [ ] **Step 2.15: Commit**

  ```bash
  git add c_model/include/nmu/packetize.hpp c_model/include/nsu/depacketize.hpp c_model/include/axi/scenario_parser.hpp c_model/include/axi/axi_master.hpp tools/lint_scenarios.py sim/test_patterns/README.md
  git commit -m "feat(ni): pack awqos/arqos through NMU/NSU and register QOS scenario CAT

  NMU packetize.hpp now writes b.qos into the AW/AR payload's new awqos
  bits (Task 1). NSU depacketize.hpp recovers it instead of forcing 0.
  ScenarioTransaction gains a uint8_t qos field that AxiMaster propagates
  into AwBeat/ArBeat at the per-sub-burst push sites. Adds a 'QOS'
  scenario CAT to scenario_parser.hpp regex/category map, tools
  /lint_scenarios.py, and sim/test_patterns/README.md so AX4-QOS-NNN
  scenarios pass lint. Shell adapters already wire qos at nmu_shell_
  adapter:100,126 and nsu_shell_adapter:124,158; no adapter change needed.

  Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
  ```

---

## Task 3 — Unit tests + integration scenario (TDD round-trip coverage)

**Files:**
- Modify: `c_model/tests/nmu/test_packetize.cpp`
- Modify: `c_model/tests/nsu/test_nsu_depacketize.cpp`
- Create: `sim/test_patterns/AX4-QOS-001_awqos_round_trip/scenario.yaml`
- Create: `sim/test_patterns/AX4-QOS-001_awqos_round_trip/data.txt`

- [ ] **Step 3.1: Add AW round-trip test to `test_packetize.cpp`**

  Open `c_model/tests/nmu/test_packetize.cpp`. After the existing `AwPayloadBitPerfect` test (around line 147-175), add:

  ```cpp
  TEST(NmuPacketize, AwqosRoundTrip) {
      LoopbackNoc noc;
      Packetize pkt(noc);
      axi::AwBeat b{};
      b.id = 0x5;
      b.addr = 0x2000;
      b.len = 0;
      b.size = 5;
      b.burst = axi::Burst::INCR;
      b.qos = 0xA;
      ASSERT_TRUE(pkt.push_aw(b));
      pkt.tick();
      auto f = noc.peek_last_req_flit();
      EXPECT_EQ(f.get_payload_field("AW", "awqos"), 0xAu);
  }
  ```

  (Adjust the `LoopbackNoc` / `Packetize` ctor and `peek_last_req_flit()` accessor names to match the surrounding tests' idiom — the existing `AwPayloadBitPerfect` test is the closest template.)

- [ ] **Step 3.2: Add AR round-trip test to `test_packetize.cpp`**

  Add directly below:

  ```cpp
  TEST(NmuPacketize, ArqosRoundTrip) {
      LoopbackNoc noc;
      Packetize pkt(noc);
      axi::ArBeat b{};
      b.id = 0x5;
      b.addr = 0x2000;
      b.len = 0;
      b.size = 5;
      b.burst = axi::Burst::INCR;
      b.qos = 0xA;
      ASSERT_TRUE(pkt.push_ar(b));
      pkt.tick();
      auto f = noc.peek_last_req_flit();
      EXPECT_EQ(f.get_payload_field("AR", "arqos"), 0xAu);
  }
  ```

- [ ] **Step 3.3: Add AW depacketize round-trip to `test_nsu_depacketize.cpp`**

  Open `c_model/tests/nsu/test_nsu_depacketize.cpp`. After the existing `AwFlitSnapshotsMetadataAndPopsBeat` test (around line 52-73), add:

  ```cpp
  TEST(NsuDepacketize, AwqosRecoveredFromFlit) {
      // Construct an AW flit with awqos = 0xA and verify depacketize
      // populates AwBeat.qos accordingly.
      Flit f;
      f.set_header_field("axi_ch", 0u);  // AW channel
      f.set_payload_field("AW", "awid", 0x5);
      f.set_payload_field("AW", "awqos", 0xA);
      Depacketize depkt;
      auto b = depkt.decode_aw(f);
      EXPECT_EQ(b.qos, 0xAu);
  }
  ```

  Adjust to match the surrounding test idiom (the `AwFlitSnapshotsMetadataAndPopsBeat` test is the template).

- [ ] **Step 3.4: Add AR depacketize round-trip**

  Below the above:

  ```cpp
  TEST(NsuDepacketize, ArqosRecoveredFromFlit) {
      Flit f;
      f.set_header_field("axi_ch", 2u);  // AR channel
      f.set_payload_field("AR", "arid", 0x5);
      f.set_payload_field("AR", "arqos", 0xA);
      Depacketize depkt;
      auto b = depkt.decode_ar(f);
      EXPECT_EQ(b.qos, 0xAu);
  }
  ```

- [ ] **Step 3.5: Build and run the new unit tests**

  Run: `cd c_model/build && cmake --build . && ctest -R "Awqos|Arqos" --output-on-failure`
  Expected: all 4 tests PASS (Task 2 wired the data path).

- [ ] **Step 3.6: Create the AX4-QOS-001 scenario directory**

  ```bash
  mkdir -p sim/test_patterns/AX4-QOS-001_awqos_round_trip
  ```

- [ ] **Step 3.7: Write the scenario YAML**

  Create `sim/test_patterns/AX4-QOS-001_awqos_round_trip/scenario.yaml`:

  ```yaml
  schema_version: 1
  metadata:
    name: AX4-QOS-001_awqos_round_trip
    category: qos

  config:
    memory_base: 0x1000
    memory_size: 0x1000
    write_latency: 1
    read_latency: 1
    max_outstanding_write: 1
    max_outstanding_read: 1
  transactions:
    - op: write
      addr: 0x1000
      id: 0x0
      len: 0
      size: 5
      burst: INCR
      qos: 0xA
      data_file: data.txt
  ```

- [ ] **Step 3.8: Write the per-beat data file**

  Create `sim/test_patterns/AX4-QOS-001_awqos_round_trip/data.txt`:

  ```
  00 11 22 33 44 55 66 77 88 99 aa bb cc dd ee ff 10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f
  ```

  (One line of 32 hex bytes = one beat at size=5 (32-byte beat width); covers `(len+1) * (1 << size) = 1 * 32 = 32` bytes required by `c_model/include/axi/axi_master.hpp:251,327-330`.)

- [ ] **Step 3.9: Re-run CMake to pick up the new scenario**

  Run: `make build PYTHON3="py -3"` (this forces CMake reconfigure via `CONFIGURE_DEPENDS` on `sim/test_patterns/AX4-*/scenario.yaml`).

- [ ] **Step 3.10: Run lint to confirm CAT registration**

  Run: `py -3 tools/lint_scenarios.py`
  Expected: PASS (QOS now in allow-list per Task 2).

- [ ] **Step 3.11: Run the integration test against the new scenario**

  Run: `cd c_model/build && ctest -R AX4-QOS-001 --output-on-failure`
  Expected: PASS.

- [ ] **Step 3.12: Run full `make check` end-to-end**

  Run: `make check PYTHON3="py -3"`
  Expected: clean (lint_scenarios + lint_docs + build + full ctest).

- [ ] **Step 3.13: Commit**

  ```bash
  git add c_model/tests/nmu/test_packetize.cpp c_model/tests/nsu/test_nsu_depacketize.cpp sim/test_patterns/AX4-QOS-001_awqos_round_trip/
  git commit -m "test(qos): awqos round-trip coverage at unit and integration layers

  Four unit tests assert payload-field bit position end-to-end at packetize
  (NmuPacketize.{Awqos,Arqos}RoundTrip) and depacketize (NsuDepacketize.
  {Awqos,Arqos}RecoveredFromFlit). One integration scenario AX4-QOS-001
  drives qos=0xA through AxiMaster -> NMU -> LoopbackNoc -> NSU -> AxiSlave
  via the auto-included scenarios glob.

  Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
  ```

---

## Post-implementation gate (Codex acceptance review)

After Task 3 commit, **before pushing**:

- [ ] **Step P.1: Generate full diff vs `origin/main`**

  Run: `git diff origin/main..HEAD > .axi_qos_diff.patch && wc -l .axi_qos_diff.patch`

- [ ] **Step P.2: Run Codex acceptance review**

  Invoke `codex:codex-rescue` agent with: "Read `./.axi_qos_diff.patch`. Verify all 3 commits implement `docs/superpowers/specs/2026-06-07-axi-qos-packing-design.md`. Verify Tasks 1-3 of `docs/superpowers/plans/2026-06-07-axi-qos-packing.md` are complete. Spot-check: (a) `awqos[100:97]` in `ni_flit_constants.h`, (b) `set_payload_field("AW", "awqos", b.qos)` in `packetize.hpp`, (c) `get_payload_field("AW", "awqos")` in `depacketize.hpp`, (d) `tx.qos` plumbed in `axi_master.hpp`, (e) `qos: 0xA` in AX4-QOS-001 scenario.yaml. Report any MUST-FIX before push; otherwise approve."

- [ ] **Step P.3: If Codex flags issues, apply fixes inline + amend most recent commit (or add a fixup). Re-run Codex until clean.**

- [ ] **Step P.4: Clean up the diff file and push**

  ```bash
  rm .axi_qos_diff.patch
  git push origin main
  ```

---

## Self-review checklist

**Spec coverage:** Every section of `2026-06-07-axi-qos-packing-design.md` maps to one or more tasks:
- §3.1 Bit layout → Task 1 steps 1.1-1.4
- §3.2 Parameter addition → Task 1 step 1.1
- §3.3 Codegen impact → Task 1 steps 1.3-1.6
- §3.4 c_model wiring → Task 2 steps 2.1-2.9
- §3.5 Test additions → Task 3 steps 3.1-3.11
- §3.6 CAT registration → Task 2 steps 2.7, 2.10, 2.11
- §4 Commit chain → 3 task = 3 commits
- §6 Success criteria → Task 3 step 3.12 (`make check`) + post-impl Codex gate
- §7 Risks (specgen goldens, existing test_packetize:161-173) → Task 1 step 1.5

**Placeholder scan:** No "TBD" / "TODO" / "implement later" / "add error handling" / vague descriptions remain. Every code block is complete. The `[TBD image regen]` in the spec is explicit out-of-scope, not a plan placeholder.

**Type consistency:** `b.qos` is `uint8_t` throughout (matches `AwBeat::qos` at `axi/types.hpp:85`). `set_payload_field` signature is `(channel, field, value)` with `value` implicitly convertible from `uint8_t` (existing call sites use this idiom). `tx.qos` is `uint8_t` in `ScenarioTransaction` (Step 2.5).
