# NI layer cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Delete `c_model/include/ni/` subdirectory. Replace 5/7-method ISP-violating base interfaces with 4 narrow REQ/RSP × producer/consumer interfaces. Split 13-field `PortParams` into per-side structs plus a test-fixture struct. Move hand-maintained flit dispatch into codegen-emitted `FieldDescriptor` arrays.

**Architecture:** Five atomic buildable commits matching the spec sequencing. (1) Codegen emit `FieldDescriptor` arrays + golden refresh, no consumer change. (2) Mechanical `flit.hpp` move + hand-dispatch rewrite + 65 `#include` rewrites. (3) Atomic bundle: split `PortParams` + de-duplicate `NmuConfig`/`NsuConfig` + regroup YAML + cosim adapter cleanup. (4) Atomic bundle: 4 narrow interfaces + concrete-class re-base + `AxiSlavePort`/`AxiMasterPort` ctor change + Loopback split + `DelayedLoopback` migration + delete `ni/`. (5) Asset fix: `packet_format_overview.jpg` title typo (manual image edit).

**Tech Stack:** C++17 + CMake + GoogleTest; Python 3 codegen via `specgen/tools/codegen.py` + `specgen/tools/elaborate/cpp_packet.py`; YAML config via yaml-cpp.

**Spec:** `docs/superpowers/specs/2026-06-09-ni-layer-cleanup-design.md` (committed `f823ce3`).

**Shell conventions:** Bash commands run under Git Bash on Windows; Python via `py -3`; build via `cmake --build . --parallel` (generator-neutral — repo uses Ninja per `c_model/build/CMakeCache.txt`).

---

## File Structure

### Created

- `c_model/include/flit.hpp` — moved from `c_model/include/ni/flit.hpp`; hand dispatch replaced by generic loop over codegen descriptors
- `c_model/include/request_io.hpp` — `RequestPacketizer` + `RequestDepacketizer` narrow interfaces (3 methods each)
- `c_model/include/response_io.hpp` — `ResponseMeta` + `ResponsePacketizer` (2 methods) + `ResponseDepacketizer` (2 pure-virtual + 2 virtual w/ default)
- `c_model/include/nmu/port_params.hpp` — `nmu::PortParams` (7 fields) + `load_nmu_port_params(path)`
- `c_model/include/nsu/port_params.hpp` — `nsu::PortParams` (9 fields) + `load_nsu_port_params(path)`
- `c_model/tests/common/channel_model_params.hpp` — `ni::cmodel::testing::ChannelModelParams` (2 fields) + `load_channel_model_params(path)`
- `c_model/tests/common/loopback_request_io.hpp` — `RequestChannelSet` (3 deques) + `LoopbackRequestPacketizer` + `LoopbackRequestDepacketizer`
- `c_model/tests/common/loopback_response_io.hpp` — `ResponseChannelSet` (2 deques) + `LoopbackResponsePacketizer` + `LoopbackResponseDepacketizer` + aggregate `LoopbackChannelSet`

### Modified

- `specgen/tools/elaborate/cpp_packet.py` — add `FieldDescriptor` array emitter
- `specgen/generated/cpp/ni_flit_constants.h` — regenerated with descriptor arrays
- `specgen/tests/golden/ni_flit_constants.h.golden` — golden refresh
- `c_model/config/port_params.yaml` — regrouped to `nmu:` / `nsu:` / `channel_model:` with `queues:` sub-block
- `c_model/include/nmu/nmu.hpp` — `NmuConfig` de-dup; `Nmu::Nmu` ctor sources `Depacketize` depths from `cfg.port_params.*`
- `c_model/include/nsu/nsu.hpp` — `NsuConfig` de-dup; `Nsu::Nsu` ctor sources `Depacketize` + `MetaBuffer` depths from `cfg.port_params.*`
- `c_model/include/nmu/axi_slave_port.hpp` — ctor signature `(RequestPacketizer&, ResponseDepacketizer&, nmu::PortParams)`
- `c_model/include/nsu/axi_master_port.hpp` — ctor signature `(RequestDepacketizer&, ResponsePacketizer&, nsu::PortParams)`
- `c_model/include/nmu/packetize.hpp` — `: public RequestPacketizer`, delete `push_b/r` stubs
- `c_model/include/nmu/depacketize.hpp` — `: public ResponseDepacketizer`, delete `pop_aw/w/ar` stubs
- `c_model/include/nsu/packetize.hpp` — `: public ResponsePacketizer`, delete `push_aw/w/ar` stubs
- `c_model/include/nsu/depacketize.hpp` — `: public RequestDepacketizer`, delete `pop_b/r` stubs
- `c_model/include/cosim/nmu_shell_adapter.hpp` — remove `channel_model_*_depth` assignment at line 61-62; switch to `nmu::PortParams`
- `c_model/include/cosim/nsu_shell_adapter.hpp` — remove `channel_model_*_depth` assignment; switch to `nsu::PortParams`
- `c_model/tests/integration/test_port_pair_loopback.cpp` — `DelayedLoopback` (line 91) migrated to narrow base interfaces
- All 65 sites including `ni/flit.hpp` (verify pre-commit: `grep -rln '"ni/flit.hpp"' c_model/ | wc -l` returns 65)
- `docs/image/packet_format_overview.jpg` — title text 48 → 56 (manual image edit, Task 5)

### Deleted

- `c_model/include/ni/flit.hpp`
- `c_model/include/ni/packetizer.hpp`
- `c_model/include/ni/depacketizer.hpp`
- `c_model/include/ni/port_params.hpp`
- `c_model/include/ni/wrong_side.hpp`
- `c_model/include/ni/` (empty directory, removed)
- `c_model/tests/common/loopback_packetizer.hpp`
- `c_model/tests/common/loopback_depacketizer.hpp`

---

## Task 1: Codegen emit `FieldDescriptor` arrays

**Files:**
- Modify: `specgen/tools/elaborate/cpp_packet.py`
- Regen: `specgen/generated/cpp/ni_flit_constants.h`
- Refresh: `specgen/tests/golden/ni_flit_constants.h.golden`
- Test: `specgen/tests/test_field_descriptor_arrays.py` (new)

- [ ] **Step 1: Write the failing test**

Create `specgen/tests/test_field_descriptor_arrays.py`:

```python
"""Verify cpp_packet.py emits FieldDescriptor arrays alongside LSB/MSB constants."""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
GENERATED = REPO / "specgen/generated/cpp/ni_flit_constants.h"


def test_field_descriptor_struct_present():
    text = GENERATED.read_text()
    assert "struct FieldDescriptor" in text
    assert "std::string_view name" in text
    assert "int lsb" in text
    assert "int msb" in text


def test_header_fields_array_skips_disabled():
    text = GENERATED.read_text()
    # Non-greedy match the array body; stops at `};` not at the first `}`.
    m = re.search(r"constexpr FieldDescriptor HEADER_FIELDS\[\]\s*=\s*\{(.*?)\};",
                  text, re.DOTALL)
    assert m, "HEADER_FIELDS[] array not emitted"
    body = m.group(1)
    # All 12 enabled header fields present.
    for name in ["noc_qos", "axi_ch", "src_id", "dst_id", "vc_id", "route_par",
                 "last", "rob_req", "rob_idx", "commtype", "multicast", "flit_ecc"]:
        assert f'"{name}"' in body, f"missing {name} in HEADER_FIELDS"
    # rsvd is enabled=false in spec, must be skipped.
    assert '"rsvd"' not in body, "rsvd should be skipped (enabled=false)"


def test_payload_field_arrays_per_channel():
    text = GENERATED.read_text()
    for channel in ["AW", "AR", "W", "B", "R"]:
        assert f"constexpr FieldDescriptor {channel}_PAYLOAD_FIELDS[]" in text, \
            f"missing {channel}_PAYLOAD_FIELDS[] array"


def test_string_view_header_included():
    text = GENERATED.read_text()
    assert "#include <string_view>" in text
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd specgen && py -3 -m pytest tests/test_field_descriptor_arrays.py -v
```

Expected: 4 failures — `struct FieldDescriptor`, `HEADER_FIELDS[]`, `*_PAYLOAD_FIELDS[]`, and `<string_view>` are not yet emitted.

- [ ] **Step 3: Extend `cpp_packet.py` to emit FieldDescriptor + arrays**

Read `specgen/tools/elaborate/cpp_packet.py` to see the existing emit style: `emit()` builds a `list[str] out` and appends lines via `out.append(...)`, then returns `"\n".join(out)`. Constants live in nested namespaces — `ni::header::NOC_QOS_LSB`, `ni::payload::aw::AWID_LSB` etc. (see `specgen/generated/cpp/ni_flit_constants.h:24, :96` for the actual hierarchy). There is an existing helper `C.payload_field_position(channel, field)` that returns `None` when a field has width 0; reuse it instead of inventing a width resolver.

At the top of the generated file, ensure `#include <string_view>` is in the existing `#include` block.

After all existing `ni::header::*` and `ni::payload::*::*` namespace blocks are emitted (so all `*_LSB` / `*_MSB` symbols are visible), but **before** the closing `}` of the outer `ni::` namespace, append the descriptor arrays. Match the existing append style:

```python
def _emit_field_descriptor_struct(out):
    out.append("struct FieldDescriptor {")
    out.append("    std::string_view name;")
    out.append("    int lsb;")
    out.append("    int msb;")
    out.append("};")
    out.append("")


def _emit_header_fields_array(out, header_fields):
    out.append("constexpr FieldDescriptor HEADER_FIELDS[] = {")
    for f in header_fields:
        if not f.get("enabled", True):
            continue  # skip rsvd
        name = f["name"]
        upper = name.upper()
        out.append(f'    {{ "{name}", header::{upper}_LSB, header::{upper}_MSB }},')
    out.append("};")
    out.append("")


def _emit_payload_fields_arrays(out, payload_channels):
    for ch in payload_channels:
        ch_lower = ch["name"].lower()       # nested ns is lowercase (aw, ar, w, b, r)
        ch_upper = ch["name"]                # array name uses uppercase (AW_PAYLOAD_FIELDS)
        out.append(f"constexpr FieldDescriptor {ch_upper}_PAYLOAD_FIELDS[] = {{")
        for f in ch["fields"]:
            # Reuse existing helper; None signals width==0 / not emitted.
            pos = C.payload_field_position(ch["name"], f["name"])
            if pos is None:
                continue
            const = f['name'].upper()  # AWID, AWADDR, ... constants live in payload::aw::
            out.append(f'    {{ "{f["name"]}", payload::{ch_lower}::{const}_LSB, '
                       f'payload::{ch_lower}::{const}_MSB }},')
        out.append("};")
        out.append("")
```

Wire the three helpers into the existing `emit()` flow at the position described above. Pass `header_fields` from `spec["flit"]["header_fields"]` and `payload_channels` from `spec["flit"]["payload_channels"]` — same data structure the existing constants emit already consumes.

If the actual constant naming in `ni_flit_constants.h` differs from `header::*_LSB` / `payload::<ch>::*_LSB`, adjust the f-strings to match what the file currently emits before this change. Sanity-check by `grep -n 'NOC_QOS_LSB\|AWID_LSB' specgen/generated/cpp/ni_flit_constants.h`.

- [ ] **Step 4: Regenerate header + golden**

```bash
py -3 specgen/tools/codegen.py --target cpp --domain packet --out specgen/generated/cpp/
cp specgen/generated/cpp/ni_flit_constants.h specgen/tests/golden/ni_flit_constants.h.golden
```

- [ ] **Step 5: Run test to verify it passes**

```bash
cd specgen && py -3 -m pytest tests/test_field_descriptor_arrays.py -v
py -3 -m pytest tests/test_byte_identical_golden.py -v
```

Expected: 4 + N PASS. Byte-identical gate confirms regenerated header matches golden.

- [ ] **Step 6: Verify no c_model consumer change required yet**

```bash
cd c_model/build && cmake --build . -- -j
ctest --output-on-failure
```

Expected: full c_model build + tests still pass. Generated arrays are unused but cause no warning (constexpr globals).

- [ ] **Step 7: Commit**

```bash
git add specgen/tools/elaborate/cpp_packet.py \
        specgen/generated/cpp/ni_flit_constants.h \
        specgen/tests/golden/ni_flit_constants.h.golden \
        specgen/tests/test_field_descriptor_arrays.py
git commit -m "feat(specgen): emit FieldDescriptor arrays for flit dispatch

Add HEADER_FIELDS[] (skips enabled=false) and {AW,AR,W,B,R}_PAYLOAD_FIELDS[]
arrays to ni_flit_constants.h. Move codegen ownership of name -> {LSB, MSB}
mapping out of hand-maintained ni/flit.hpp dispatch. No consumer change yet.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 2: Move `flit.hpp` to top-level + generic-loop dispatch

**Files:**
- Move: `c_model/include/ni/flit.hpp` → `c_model/include/flit.hpp`
- Modify: 65 sites with `#include "ni/flit.hpp"` (rewrite to `#include "flit.hpp"`)
- Test: `c_model/tests/common/test_flit.cpp` (existing — extend with rsvd abort test)

- [ ] **Step 1: Write the failing test (rsvd abort behavior change)**

Append to `c_model/tests/common/test_flit.cpp`:

```cpp
TEST(FlitDispatch, RsvdHeaderFieldQueryAborts) {
    // Spec marks rsvd as enabled=false (ni_packet.json header_fields).
    // After this refactor, codegen-emitted HEADER_FIELDS[] skips rsvd, so
    // the generic dispatch loop falls through and aborts.
    EXPECT_DEATH(
        { ni::cmodel::detail::header_field_pos("rsvd"); },
        "header_field_pos: name not found"
    );
}

TEST(FlitDispatch, EnabledHeaderFieldsStillResolve) {
    // Sanity: enabled fields still resolve via generic loop.
    auto pos = ni::cmodel::detail::header_field_pos("dst_id");
    EXPECT_EQ(pos.lsb, ni::header::DST_ID_LSB);
    EXPECT_EQ(pos.msb, ni::header::DST_ID_MSB);
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd c_model/build
cmake --build . --target test_flit --parallel    # rebuild — ctest alone runs stale binary
ctest -R FlitDispatch --output-on-failure
```

Expected: `RsvdHeaderFieldQueryAborts` fails (hand-dispatch currently returns `{RSVD_LSB, RSVD_MSB}` instead of aborting); `EnabledHeaderFieldsStillResolve` passes.

- [ ] **Step 3: Move file**

```bash
git mv c_model/include/ni/flit.hpp c_model/include/flit.hpp
```

- [ ] **Step 4: Rewrite all 65 includes**

```bash
# Verify count first.
grep -rln '"ni/flit.hpp"' c_model/ | wc -l   # expected: 65

# Rewrite. Use Python to handle Windows line endings safely.
py -3 -c "
import pathlib, re
for p in pathlib.Path('c_model').rglob('*'):
    if p.is_file() and p.suffix in {'.hpp', '.cpp', '.h'}:
        s = p.read_text(encoding='utf-8')
        new = s.replace('#include \"ni/flit.hpp\"', '#include \"flit.hpp\"')
        if new != s:
            p.write_text(new, encoding='utf-8')
"

# Verify nothing remains.
grep -rln '"ni/flit.hpp"' c_model/   # expected: empty
```

- [ ] **Step 5: Replace hand-maintained dispatch with generic loop**

Edit `c_model/include/flit.hpp`. Confirm the include block already has `<optional>`, `<cstdio>`, `<cassert>`, `<cstdlib>`, `<algorithm>` (add any missing). Replace the existing `header_field_pos()` body (currently 13 hand-listed `if (name == ...)`) and `payload_field_pos()` body (currently 5 channel-keyed sub-tables) with:

```cpp
namespace detail {

struct FieldPos {
    int lsb, msb;
};

inline FieldPos header_field_pos(std::string_view name) {
    for (const auto& f : ni::HEADER_FIELDS) {
        if (f.name == name) return {f.lsb, f.msb};
    }
    std::fprintf(stderr,
                 "ni::cmodel::detail::header_field_pos: name '%.*s' not found "
                 "in codegen HEADER_FIELDS[] — check ni_packet.json or regen.\n",
                 static_cast<int>(name.size()), name.data());
    assert(false && "header_field_pos: name not found");
    std::abort();
}

// Case-insensitive equality (preserves existing case-normalization contract:
// callers pass "AW" or "aw" interchangeably — current flit.hpp:100 behaviour).
inline bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::toupper(ca) != std::toupper(cb)) return false;
    }
    return true;
}

inline FieldPos payload_field_pos(std::string_view channel, std::string_view field) {
    auto find_in = [&](const auto& arr) -> std::optional<FieldPos> {
        for (const auto& f : arr) {
            if (f.name == field) return FieldPos{f.lsb, f.msb};
        }
        return std::nullopt;
    };
    std::optional<FieldPos> hit;
    if      (ieq(channel, "AW")) hit = find_in(ni::AW_PAYLOAD_FIELDS);
    else if (ieq(channel, "AR")) hit = find_in(ni::AR_PAYLOAD_FIELDS);
    else if (ieq(channel, "W"))  hit = find_in(ni::W_PAYLOAD_FIELDS);
    else if (ieq(channel, "B"))  hit = find_in(ni::B_PAYLOAD_FIELDS);
    else if (ieq(channel, "R"))  hit = find_in(ni::R_PAYLOAD_FIELDS);
    if (hit) return *hit;
    std::fprintf(stderr,
                 "payload_field_pos: %.*s.%.*s not found.\n",
                 static_cast<int>(channel.size()), channel.data(),
                 static_cast<int>(field.size()),   field.data());
    assert(false && "payload_field_pos: field not found");
    std::abort();
}

}  // namespace detail
```

Add `#include <cctype>` for `std::toupper` if not already present.

- [ ] **Step 6: Run all tests**

```bash
cd c_model/build && cmake --build . --parallel && ctest --output-on-failure
```

Expected: all green, including the new `RsvdHeaderFieldQueryAborts` and existing case-insensitive payload field tests (regression check that `ieq()` preserves `"AW"`/`"aw"` parity).

- [ ] **Step 7: Commit**

```bash
git add c_model/
git commit -m "refactor(c_model): move flit.hpp to top-level + generic dispatch

flit is a NI-block-shared packet container per ni_packet.json metadata
(NI = NMU + NSU umbrella). Top-level matches register_file.hpp precedent.
Replace 13-entry hand-listed if-chain with one generic loop over codegen
HEADER_FIELDS[] / *_PAYLOAD_FIELDS[]. rsvd query now aborts (enabled=false
in spec) — contract narrowing covered by new test. 65 include path rewrites.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 3: Atomic — split `PortParams` + de-dup `NmuConfig`/`NsuConfig` + YAML regroup

**Files:**
- Create: `c_model/include/nmu/port_params.hpp`, `c_model/include/nsu/port_params.hpp`, `c_model/tests/common/channel_model_params.hpp`
- Modify: `c_model/config/port_params.yaml`, `c_model/include/nmu/nmu.hpp`, `c_model/include/nsu/nsu.hpp`, `c_model/include/nmu/axi_slave_port.hpp` (params type only), `c_model/include/nsu/axi_master_port.hpp` (params type only), `c_model/include/cosim/nmu_shell_adapter.hpp`, `c_model/include/cosim/nsu_shell_adapter.hpp`, all tests using `PortParams`
- Delete: `c_model/include/ni/port_params.hpp`
- Test: extend integration test with asymmetric values

- [ ] **Step 1: Write failing tests (asymmetric sizing + de-dup + loader fail-loud)**

Append to `c_model/tests/integration/test_port_pair_loopback.cpp`:

```cpp
TEST(PortParamsSplit, AsymmetricNmuNsuAwQueueSaturationIndependent) {
    // Verify NMU saturates at its own aw_queue_depth, NSU at its own.
    // Pre-refactor (single shared PortParams) both used identical values.
    ni::cmodel::nmu::PortParams nmu_pp{
        /*aw=*/ 2, /*w=*/ 32, /*ar=*/ 32, /*b=*/ 32, /*r=*/ 32,
        /*depkt_b=*/ 32, /*depkt_r=*/ 32};
    ni::cmodel::nsu::PortParams nsu_pp{
        /*aw=*/ 64, /*w=*/ 32, /*ar=*/ 32, /*b=*/ 32, /*r=*/ 32,
        /*depkt_aw=*/ 64, /*depkt_w=*/ 64, /*depkt_ar=*/ 64,
        /*meta_buffer_per_id=*/ 4};

    // Use the existing port-pair loopback fixture pattern from this file
    // (LoopbackChannelSet + NMU AxiSlavePort + NSU AxiMasterPort). Bridge:
    // 5 push_aw to NMU, expect the 3rd to fail (NMU AW queue cap = 2).
    // Pump tick(); drain to NSU. NSU AW queue (cap = 64) absorbs without backpressure.
    ni::cmodel::testing::LoopbackChannelSet ch;
    ni::cmodel::testing::LoopbackRequestPacketizer  req_pkt(ch.request);
    ni::cmodel::testing::LoopbackResponseDepacketizer rsp_depkt(ch.response);
    ni::cmodel::nmu::AxiSlavePort nmu_port(req_pkt, rsp_depkt, nmu_pp);

    axi::AwBeat aw{};
    EXPECT_TRUE(nmu_port.push_aw(aw));
    EXPECT_TRUE(nmu_port.push_aw(aw));
    EXPECT_FALSE(nmu_port.push_aw(aw));  // NMU side full at depth 2
}

TEST(PortParamsSplit, NmuConfigDepacketizeDepthRoutesFromPortParams) {
    // Verify NmuConfig de-dup: setting cfg.port_params.depkt_b_q_depth
    // alone (no standalone shadow field) drives the actual Depacketize
    // capacity. Distinctive small value -> saturation observable.
    ni::cmodel::nmu::PortParams pp{
        /*aw=*/ 32, /*w=*/ 32, /*ar=*/ 32, /*b=*/ 32, /*r=*/ 32,
        /*depkt_b=*/ 3, /*depkt_r=*/ 32};   // 3 = distinctive

    ni::cmodel::nmu::NmuConfig cfg{};
    cfg.port_params = pp;
    cfg.src_id = 0;
    // Re-use existing helper that constructs a fully-wired Nmu around NocReqOut/NocRspIn
    // stubs (see test_nmu.cpp make_loopback_nmu()). Push 4 B-flits into the NMU's
    // Depacketize input; the 4th must be rejected.
    auto nmu = ni::cmodel::testing::make_loopback_nmu(cfg);
    Flit b_flit; b_flit.set_header_field("axi_ch", ni::AXI_CH_B);
    EXPECT_TRUE(nmu.depacketize().push_flit(b_flit));
    EXPECT_TRUE(nmu.depacketize().push_flit(b_flit));
    EXPECT_TRUE(nmu.depacketize().push_flit(b_flit));
    EXPECT_FALSE(nmu.depacketize().push_flit(b_flit));  // 4th rejected at depth 3
}

TEST(PortParamsSplit, LoaderMissingNmuBlockThrows) {
    auto p = std::filesystem::temp_directory_path() / "bad_nmu.yaml";
    std::ofstream(p) << "nsu: {}\nchannel_model: {}\n";  // nmu missing
    EXPECT_THROW(ni::cmodel::nmu::load_nmu_port_params(p.string()),
                 std::runtime_error);
}

TEST(PortParamsSplit, LoaderMissingNmuQueueKeyThrows) {
    auto p = std::filesystem::temp_directory_path() / "bad_nmu_key.yaml";
    std::ofstream(p) <<
        "nmu:\n  queues:\n    aw_queue_depth: 32\n"
        "    # w_queue_depth intentionally missing\n"
        "    ar_queue_depth: 32\n    b_queue_depth: 32\n    r_queue_depth: 32\n"
        "  depacketize: { b_q_depth: 32, r_q_depth: 32 }\n";
    EXPECT_ANY_THROW(ni::cmodel::nmu::load_nmu_port_params(p.string()));
}
```

(Helper `make_loopback_nmu(cfg)` lives in `c_model/tests/nmu/test_nmu.cpp` test-fixture utilities — if not exposed, either promote it to `c_model/tests/common/nmu_fixture.hpp` as part of this commit, or inline the equivalent NocReqOut/NocRspIn stub setup.)

- [ ] **Step 2: Run failing test**

```bash
cd c_model/build && ctest -R PortParamsSplit --output-on-failure
```

Expected: compile errors — `nmu::PortParams` / `nsu::PortParams` don't exist yet.

- [ ] **Step 3: Create `nmu/port_params.hpp`**

```cpp
#pragma once
// NMU-side port-pair parameters. Source: c_model/config/port_params.yaml
// `nmu:` top-level block. Fields that NSU does not consume live in
// nsu/port_params.hpp; shared 5 AXI-channel depths are duplicated by
// design (independent NMU/NSU evolution, no spec-mandated symmetry).
#include <yaml-cpp/yaml.h>
#include <cstddef>
#include <stdexcept>
#include <string>

namespace ni::cmodel::nmu {

struct PortParams {
    // 5 AXI-channel slave-port internal FIFO depths.
    std::size_t aw_queue_depth;
    std::size_t w_queue_depth;
    std::size_t ar_queue_depth;
    std::size_t b_queue_depth;
    std::size_t r_queue_depth;
    // NMU Depacketize internal demux FIFO depths (NMU consumes B/R).
    std::size_t depkt_b_q_depth;
    std::size_t depkt_r_q_depth;
};

inline PortParams load_nmu_port_params(const std::string& path) {
    auto root = YAML::LoadFile(path);
    auto nmu = root["nmu"];
    if (!nmu) throw std::runtime_error("port_params.yaml: missing 'nmu:' block");
    auto q = nmu["queues"];
    if (!q) throw std::runtime_error("port_params.yaml: missing 'nmu.queues:' block");
    auto d = nmu["depacketize"];
    if (!d) throw std::runtime_error("port_params.yaml: missing 'nmu.depacketize:' block");
    PortParams p{};
    p.aw_queue_depth = q["aw_queue_depth"].as<std::size_t>();
    p.w_queue_depth  = q["w_queue_depth"].as<std::size_t>();
    p.ar_queue_depth = q["ar_queue_depth"].as<std::size_t>();
    p.b_queue_depth  = q["b_queue_depth"].as<std::size_t>();
    p.r_queue_depth  = q["r_queue_depth"].as<std::size_t>();
    p.depkt_b_q_depth = d["b_q_depth"].as<std::size_t>();
    p.depkt_r_q_depth = d["r_q_depth"].as<std::size_t>();
    return p;
}

}  // namespace ni::cmodel::nmu
```

- [ ] **Step 4: Create `nsu/port_params.hpp`**

```cpp
#pragma once
#include <yaml-cpp/yaml.h>
#include <cstddef>
#include <stdexcept>
#include <string>

namespace ni::cmodel::nsu {

struct PortParams {
    // 5 AXI-channel master-port internal FIFO depths.
    std::size_t aw_queue_depth;
    std::size_t w_queue_depth;
    std::size_t ar_queue_depth;
    std::size_t b_queue_depth;
    std::size_t r_queue_depth;
    // NSU Depacketize internal demux FIFO depths (NSU consumes AW/W/AR).
    std::size_t depkt_aw_q_depth;
    std::size_t depkt_w_q_depth;
    std::size_t depkt_ar_q_depth;
    // NSU MetaBuffer per-AXI-ID outstanding-request depth.
    std::size_t meta_buffer_per_id_depth;
};

inline PortParams load_nsu_port_params(const std::string& path) {
    auto root = YAML::LoadFile(path);
    auto nsu = root["nsu"];
    if (!nsu) throw std::runtime_error("port_params.yaml: missing 'nsu:' block");
    auto q = nsu["queues"];
    if (!q) throw std::runtime_error("port_params.yaml: missing 'nsu.queues:' block");
    auto d = nsu["depacketize"];
    if (!d) throw std::runtime_error("port_params.yaml: missing 'nsu.depacketize:' block");
    auto m = nsu["meta_buffer"];
    if (!m) throw std::runtime_error("port_params.yaml: missing 'nsu.meta_buffer:' block");
    PortParams p{};
    p.aw_queue_depth = q["aw_queue_depth"].as<std::size_t>();
    p.w_queue_depth  = q["w_queue_depth"].as<std::size_t>();
    p.ar_queue_depth = q["ar_queue_depth"].as<std::size_t>();
    p.b_queue_depth  = q["b_queue_depth"].as<std::size_t>();
    p.r_queue_depth  = q["r_queue_depth"].as<std::size_t>();
    p.depkt_aw_q_depth = d["aw_q_depth"].as<std::size_t>();
    p.depkt_w_q_depth  = d["w_q_depth"].as<std::size_t>();
    p.depkt_ar_q_depth = d["ar_q_depth"].as<std::size_t>();
    p.meta_buffer_per_id_depth = m["per_id_depth"].as<std::size_t>();
    return p;
}

}  // namespace ni::cmodel::nsu
```

- [ ] **Step 5: Create `tests/common/channel_model_params.hpp`**

```cpp
#pragma once
// ChannelModel test-fixture per-direction in-flight flit deque capacity.
// Not a production parameter — production cosim ChannelModelShellAdapter
// uses kPoCChannelModelDepth from cosim/poc_defaults.hpp directly.
#include <yaml-cpp/yaml.h>
#include <cstddef>
#include <stdexcept>
#include <string>

namespace ni::cmodel::testing {

struct ChannelModelParams {
    std::size_t req_depth;
    std::size_t rsp_depth;
};

inline ChannelModelParams load_channel_model_params(const std::string& path) {
    auto root = YAML::LoadFile(path);
    auto cm = root["channel_model"];
    if (!cm) throw std::runtime_error("port_params.yaml: missing 'channel_model:' block");
    ChannelModelParams p{};
    p.req_depth = cm["req_depth"].as<std::size_t>();
    p.rsp_depth = cm["rsp_depth"].as<std::size_t>();
    return p;
}

}  // namespace ni::cmodel::testing
```

- [ ] **Step 6: Rewrite `c_model/config/port_params.yaml`**

```yaml
# NMU/NSU port-pair parameters and test-fixture sizings.
# Each loader (load_nmu_port_params / load_nsu_port_params /
# load_channel_model_params) parses its own top-level block.

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

- [ ] **Step 7: De-duplicate `NmuConfig` / `NsuConfig`**

Edit `c_model/include/nmu/nmu.hpp`:

- In `NmuConfig`, swap `ni::cmodel::PortParams port_params;` → `ni::cmodel::nmu::PortParams port_params;`
- Delete any standalone fields shadowing `depkt_b_q_depth` / `depkt_r_q_depth` (current nmu.hpp lines 55, 128 region per Codex audit)
- In `Nmu::Nmu` ctor body, change `Depacketize` construction to read depths from `cfg.port_params.depkt_b_q_depth` and `cfg.port_params.depkt_r_q_depth` instead of the deleted shadow fields

Edit `c_model/include/nsu/nsu.hpp`:

- Same swap for `nsu::PortParams`
- Delete standalone fields shadowing `depkt_aw_q_depth` / `depkt_w_q_depth` / `depkt_ar_q_depth` / `meta_buffer_per_id_depth`
- In `Nsu::Nsu` ctor body, route `Depacketize` and `MetaBuffer` construction from `cfg.port_params.*`

- [ ] **Step 8: Update port classes to typed `PortParams`**

In `c_model/include/nmu/axi_slave_port.hpp`, change ctor parameter type from `PortParams` (currently `ni::cmodel::PortParams`) to `nmu::PortParams`. Member `params_` retypes accordingly. Body unchanged — same 5 `queue_depth` field names.

In `c_model/include/nsu/axi_master_port.hpp`, same change to `nsu::PortParams`.

Drop `#include "ni/port_params.hpp"`; add `#include "nmu/port_params.hpp"` (or `nsu/`).

- [ ] **Step 9: Clean cosim shell adapters**

In `c_model/include/cosim/nmu_shell_adapter.hpp`, lines 61-62 currently:
```cpp
cfg.port_params.channel_model_req_depth = kPoCChannelModelDepth;
cfg.port_params.channel_model_rsp_depth = kPoCChannelModelDepth;
```
**Delete these two lines.** Channel model depth is no longer a `PortParams` field — `ChannelModelShellAdapter` already uses `kPoCChannelModelDepth` directly per `cosim/channel_model_shell_adapter.hpp:31`.

Switch `cfg.port_params` type to `ni::cmodel::nmu::PortParams`; fill remaining 7 fields from `kPoCDefaults` or a synthetic literal.

Same for `c_model/include/cosim/nsu_shell_adapter.hpp` (channel_model assignment at the analogous line; switch to `nsu::PortParams`).

- [ ] **Step 10: Migrate all remaining callers**

```bash
# Find all sites.
grep -rln 'PortParams\|port_params\|load_port_params' c_model/ \
    | grep -v 'build/' | grep -v '\.yaml$' | grep -v 'CMakeLists\.txt'
```

Replace each:
- `ni::cmodel::PortParams` → `ni::cmodel::nmu::PortParams` or `nsu::PortParams` depending on the consumer's side
- `load_port_params_yaml(path, "nmu")` → `load_nmu_port_params(path)`
- `load_port_params_yaml(path, "nsu")` → `load_nsu_port_params(path)`
- Test fixtures touching `channel_model_*_depth`: switch to `load_channel_model_params(path)` returning `testing::ChannelModelParams`
- Any test setting `meta_buffer_per_id_depth` on a `PortParams` literal: move to `nsu::PortParams`
- `#include "ni/port_params.hpp"` → appropriate `nmu/port_params.hpp` / `nsu/port_params.hpp` / `tests/common/channel_model_params.hpp`

- [ ] **Step 11: Delete `ni/port_params.hpp`**

```bash
git rm c_model/include/ni/port_params.hpp
```

- [ ] **Step 12: Build + run all tests**

```bash
cd c_model/build && cmake --build . -- -j && ctest --output-on-failure
```

Expected: full green including new `PortParamsSplit.*` tests.

- [ ] **Step 13: Commit**

```bash
git add c_model/ && git commit -m "refactor(c_model): split PortParams + de-dup NmuConfig/NsuConfig

Split 13-field ni::cmodel::PortParams into nmu::PortParams (7), nsu::PortParams
(9), and testing::ChannelModelParams (2). Each side now declares only the
fields it consumes; previous unused-fields-loaded-but-ignored ISP violation
gone. Regroup port_params.yaml into nmu:/nsu:/channel_model: blocks with
queues: sub-block. De-duplicate NmuConfig/NsuConfig: delete standalone
depacketize/meta_buffer fields, route Depacketize/MetaBuffer construction
from cfg.port_params.*. Remove channel_model_*_depth assignments in cosim
adapters (ChannelModelShellAdapter sources kPoCChannelModelDepth directly).

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 4: Atomic — 4 narrow interfaces + Loopback split + delete `ni/`

**Files:**
- Create: `c_model/include/request_io.hpp`, `c_model/include/response_io.hpp`, `c_model/tests/common/loopback_request_io.hpp`, `c_model/tests/common/loopback_response_io.hpp`
- Modify: `c_model/include/nmu/{packetize,depacketize}.hpp`, `c_model/include/nsu/{packetize,depacketize}.hpp`, `c_model/include/nmu/axi_slave_port.hpp`, `c_model/include/nsu/axi_master_port.hpp`, `c_model/tests/integration/test_port_pair_loopback.cpp` (DelayedLoopback line 91), all test fixtures touching Loopback or the old Packetizer/Depacketizer base
- Delete: `c_model/include/ni/{packetizer,depacketizer,wrong_side}.hpp`, `c_model/include/ni/` (now empty dir), `c_model/tests/common/loopback_{packetizer,depacketizer}.hpp`

- [ ] **Step 1: Write failing tests (narrow interface dispatch + default meta)**

Create `c_model/tests/common/test_narrow_io.cpp`:

```cpp
#include "common/loopback_request_io.hpp"
#include "common/loopback_response_io.hpp"
#include <gtest/gtest.h>

TEST(NarrowInterface, RequestPacketizerAcceptsRequestBeats) {
    ni::cmodel::testing::RequestChannelSet req;
    ni::cmodel::testing::LoopbackRequestPacketizer p(req);
    axi::AwBeat beat{};
    EXPECT_TRUE(p.push_aw(beat));
    EXPECT_EQ(req.aw.size(), 1u);
}

TEST(NarrowInterface, ResponseDepacketizerDefaultMetaForwards) {
    ni::cmodel::testing::ResponseChannelSet rsp;
    rsp.b_capacity = 4;
    ni::cmodel::testing::LoopbackResponseDepacketizer d(rsp);
    axi::BBeat beat{};
    beat.id = 7;
    rsp.b.push_back(beat);
    auto out = d.pop_b_with_meta();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->first.id, 7);
    EXPECT_EQ(out->second.rob_idx, 0);   // default forward stamps {0, 0}
    EXPECT_EQ(out->second.rob_req, 0);
}
```

The narrow-interface deletion gate is enforced **by the build**: after this task, no file may `#include "ni/packetizer.hpp"` or reference `ni::cmodel::Packetizer`. Compile-time check via a sanity grep in step 11 below (`grep -rln 'ni::cmodel::Packetizer' c_model/` returns empty).

- [ ] **Step 2: Run failing test**

Add `test_narrow_io.cpp` to `c_model/tests/common/CMakeLists.txt` (mirror existing test target). Then:

```bash
cd c_model/build && cmake --build . --target test_narrow_io --parallel
```

Expected: compile error — `loopback_request_io.hpp` / `loopback_response_io.hpp` don't exist yet, includes fail.

Task 4 is an atomic refactor: steps 3 through 10 leave the build broken in intermediate states (expected — concrete classes don't compile until ports are re-based, etc.). The single passing checkpoint is **Step 11** (full build + ctest + sanity grep). Commit at Step 12.

- [ ] **Step 3: Create `request_io.hpp`**

```cpp
#pragma once
// REQ-network narrow interfaces. Producer (RequestPacketizer) implemented
// by nmu::Packetize; consumer (RequestDepacketizer) implemented by
// nsu::Depacketize. Pass AXI beats only — not flits. Lives at top-level
// because these are NMU/NSU boundary contracts, not noc/ port skin.
#include "axi/types.hpp"
#include <optional>

namespace ni::cmodel {

class RequestPacketizer {
  public:
    virtual ~RequestPacketizer() = default;
    virtual bool push_aw(const axi::AwBeat& b) = 0;
    virtual bool push_w(const axi::WBeat& b) = 0;
    virtual bool push_ar(const axi::ArBeat& b) = 0;
};

class RequestDepacketizer {
  public:
    virtual ~RequestDepacketizer() = default;
    virtual std::optional<axi::AwBeat> pop_aw() = 0;
    virtual std::optional<axi::WBeat>  pop_w()  = 0;
    virtual std::optional<axi::ArBeat> pop_ar() = 0;
};

}  // namespace ni::cmodel
```

- [ ] **Step 4: Create `response_io.hpp`**

```cpp
#pragma once
// RSP-network narrow interfaces. ResponseMeta co-located here because
// only ResponseDepacketizer surfaces it.
#include "axi/types.hpp"
#include <cstdint>
#include <optional>
#include <utility>

namespace ni::cmodel {

struct ResponseMeta {
    uint8_t rob_idx;
    uint8_t rob_req;
};

class ResponsePacketizer {
  public:
    virtual ~ResponsePacketizer() = default;
    virtual bool push_b(const axi::BBeat& b) = 0;
    virtual bool push_r(const axi::RBeat& b) = 0;
};

class ResponseDepacketizer {
  public:
    virtual ~ResponseDepacketizer() = default;
    virtual std::optional<axi::BBeat> pop_b() = 0;
    virtual std::optional<axi::RBeat> pop_r() = 0;

    // Default impl forwards to pop_b()/pop_r() and stamps zero metadata.
    // Disabled-mode fixtures and non-Rob-aware concrete classes inherit
    // this behaviour without overriding.
    virtual std::optional<std::pair<axi::BBeat, ResponseMeta>> pop_b_with_meta() {
        auto b = pop_b();
        if (!b) return std::nullopt;
        return std::make_pair(*b, ResponseMeta{0, 0});
    }
    virtual std::optional<std::pair<axi::RBeat, ResponseMeta>> pop_r_with_meta() {
        auto r = pop_r();
        if (!r) return std::nullopt;
        return std::make_pair(*r, ResponseMeta{0, 0});
    }
};

}  // namespace ni::cmodel
```

- [ ] **Step 5: Re-base concrete classes**

In `c_model/include/nmu/packetize.hpp`:
- Replace `#include "ni/packetizer.hpp"` with `#include "request_io.hpp"`
- Drop `#include "ni/wrong_side.hpp"`
- `class Packetize : public Packetizer` → `class Packetize : public RequestPacketizer`
- Delete the 2 `push_b` / `push_r` stub overrides that call `wrong_side_()`

In `c_model/include/nmu/depacketize.hpp`:
- Replace `#include "ni/depacketizer.hpp"` with `#include "response_io.hpp"`
- `class Depacketize : public Depacketizer` → `class Depacketize : public ResponseDepacketizer`
- Delete the 3 `pop_aw` / `pop_w` / `pop_ar` stub overrides
- `ResponseMeta` already moved to `response_io.hpp`; remove any local definition

In `c_model/include/nsu/packetize.hpp`:
- Replace `#include "ni/packetizer.hpp"` with `#include "response_io.hpp"`
- Drop `#include "ni/wrong_side.hpp"`
- `class Packetize : public Packetizer` → `class Packetize : public ResponsePacketizer`
- Delete the 3 `push_aw` / `push_w` / `push_ar` stub overrides

In `c_model/include/nsu/depacketize.hpp`:
- Replace `#include "ni/depacketizer.hpp"` with `#include "request_io.hpp"`
- `class Depacketize : public Depacketizer` → `class Depacketize : public RequestDepacketizer`
- Delete the 2 `pop_b` / `pop_r` stub overrides

- [ ] **Step 6: Update port ctor signatures**

In `c_model/include/nmu/axi_slave_port.hpp`:
```cpp
// Old:  AxiSlavePort(Packetizer& pkt, Depacketizer& depkt, ni::cmodel::PortParams);
// New:
AxiSlavePort(RequestPacketizer& pkt, ResponseDepacketizer& depkt, nmu::PortParams params)
    : pkt_(pkt), depkt_(depkt), params_(params) {}
```
Update member types `pkt_` / `depkt_` accordingly. Drop `#include "ni/packetizer.hpp"` / `"ni/depacketizer.hpp"`; add `#include "request_io.hpp"` + `#include "response_io.hpp"`.

In `c_model/include/nsu/axi_master_port.hpp`:
```cpp
AxiMasterPort(RequestDepacketizer& depkt, ResponsePacketizer& pkt, nsu::PortParams params)
    : depkt_(depkt), pkt_(pkt), params_(params) {}
```

- [ ] **Step 7: Create `loopback_request_io.hpp`**

```cpp
#pragma once
// REQ-side loopback test stubs. NMU drives push_aw/w/ar into the channel
// set; NSU's test pops pop_aw/w/ar from the same set. Zero-latency
// in-process short-circuit of the NoC fabric.
#include "axi/types.hpp"
#include "request_io.hpp"
#include <cstddef>
#include <deque>
#include <optional>

namespace ni::cmodel::testing {

struct RequestChannelSet {
    std::size_t aw_capacity = 32;
    std::size_t w_capacity  = 32;
    std::size_t ar_capacity = 32;
    std::deque<axi::AwBeat> aw;
    std::deque<axi::WBeat>  w;
    std::deque<axi::ArBeat> ar;
};

class LoopbackRequestPacketizer : public RequestPacketizer {
  public:
    explicit LoopbackRequestPacketizer(RequestChannelSet& ch) : ch_(ch) {}
    bool push_aw(const axi::AwBeat& b) override {
        if (ch_.aw.size() >= ch_.aw_capacity) return false;
        ch_.aw.push_back(b); return true;
    }
    bool push_w(const axi::WBeat& b) override {
        if (ch_.w.size() >= ch_.w_capacity) return false;
        ch_.w.push_back(b); return true;
    }
    bool push_ar(const axi::ArBeat& b) override {
        if (ch_.ar.size() >= ch_.ar_capacity) return false;
        ch_.ar.push_back(b); return true;
    }
  private:
    RequestChannelSet& ch_;
};

class LoopbackRequestDepacketizer : public RequestDepacketizer {
  public:
    explicit LoopbackRequestDepacketizer(RequestChannelSet& ch) : ch_(ch) {}
    std::optional<axi::AwBeat> pop_aw() override {
        if (ch_.aw.empty()) return std::nullopt;
        auto v = ch_.aw.front(); ch_.aw.pop_front(); return v;
    }
    std::optional<axi::WBeat> pop_w() override {
        if (ch_.w.empty()) return std::nullopt;
        auto v = ch_.w.front(); ch_.w.pop_front(); return v;
    }
    std::optional<axi::ArBeat> pop_ar() override {
        if (ch_.ar.empty()) return std::nullopt;
        auto v = ch_.ar.front(); ch_.ar.pop_front(); return v;
    }
  private:
    RequestChannelSet& ch_;
};

}  // namespace ni::cmodel::testing
```

- [ ] **Step 8: Create `loopback_response_io.hpp`**

```cpp
#pragma once
#include "axi/types.hpp"
#include "common/loopback_request_io.hpp"
#include "response_io.hpp"
#include <cstddef>
#include <deque>
#include <optional>

namespace ni::cmodel::testing {

struct ResponseChannelSet {
    std::size_t b_capacity = 32;
    std::size_t r_capacity = 32;
    std::deque<axi::BBeat> b;
    std::deque<axi::RBeat> r;
};

// Aggregate for integration loopback wiring both planes together.
struct LoopbackChannelSet {
    RequestChannelSet  request;
    ResponseChannelSet response;
};

class LoopbackResponsePacketizer : public ResponsePacketizer {
  public:
    explicit LoopbackResponsePacketizer(ResponseChannelSet& ch) : ch_(ch) {}
    bool push_b(const axi::BBeat& b) override {
        if (ch_.b.size() >= ch_.b_capacity) return false;
        ch_.b.push_back(b); return true;
    }
    bool push_r(const axi::RBeat& b) override {
        if (ch_.r.size() >= ch_.r_capacity) return false;
        ch_.r.push_back(b); return true;
    }
  private:
    ResponseChannelSet& ch_;
};

class LoopbackResponseDepacketizer : public ResponseDepacketizer {
  public:
    explicit LoopbackResponseDepacketizer(ResponseChannelSet& ch) : ch_(ch) {}
    std::optional<axi::BBeat> pop_b() override {
        if (ch_.b.empty()) return std::nullopt;
        auto v = ch_.b.front(); ch_.b.pop_front(); return v;
    }
    std::optional<axi::RBeat> pop_r() override {
        if (ch_.r.empty()) return std::nullopt;
        auto v = ch_.r.front(); ch_.r.pop_front(); return v;
    }
    // pop_b_with_meta / pop_r_with_meta inherit default forwarding from
    // ResponseDepacketizer base (stamps ResponseMeta{0, 0}).
};

}  // namespace ni::cmodel::testing
```

- [ ] **Step 9: Migrate `DelayedLoopback` and call sites**

In `c_model/tests/integration/test_port_pair_loopback.cpp` around line 91, `DelayedLoopback` currently inherits the old 5/7-method bases. Split it into a request-side wrapper inheriting `RequestPacketizer` / `RequestDepacketizer` and a response-side wrapper inheriting `ResponsePacketizer` / `ResponseDepacketizer`. Pattern follows the new Loopback stubs (delay buffer + delegate to underlying channel set).

Find all other Loopback users:
```bash
grep -rln 'LoopbackPacketizer\|LoopbackDepacketizer\|LoopbackChannelSet' c_model/tests/
```
Each test fixture using the old monolithic `LoopbackPacketizer` rewrites to construct two narrow stubs over a shared `LoopbackChannelSet` aggregate. NMU unit tests usually need only `LoopbackRequestPacketizer + LoopbackResponseDepacketizer`; NSU unit tests the symmetric pair.

- [ ] **Step 10: Delete `ni/` and old loopback files**

```bash
git rm c_model/include/ni/packetizer.hpp \
       c_model/include/ni/depacketizer.hpp \
       c_model/include/ni/wrong_side.hpp \
       c_model/tests/common/loopback_packetizer.hpp \
       c_model/tests/common/loopback_depacketizer.hpp

# Empty ni/ dir is automatically removed by `git rm` (git does not track
# directories). Verify:
test ! -d c_model/include/ni && echo "ni/ removed" || echo "ni/ NOT empty"
```

- [ ] **Step 11: Build + run all tests + sanity grep**

```bash
cd c_model/build && cmake --build . --parallel && ctest --output-on-failure
# Confirm no stale references to old 5-method base:
grep -rln 'ni::cmodel::Packetizer\b\|ni::cmodel::Depacketizer\b\|wrong_side_' c_model/   # expected: empty
grep -rln '"ni/' c_model/   # expected: empty (entire ni/ subdir gone)
```

Expected: full green including `NarrowInterface.*`, the spec's asymmetric-PortParams integration test (from Task 3), and all existing fixtures using migrated Loopback. Both grep returns empty.

- [ ] **Step 12: Commit**

```bash
git add c_model/ && git commit -m "refactor(c_model): replace 5/7-method base with 4 narrow REQ/RSP interfaces

Spec REQ/RSP networks (ni_packet.json payload_channels[].network) are
independent planes; producer + consumer of each are 4 narrow roles.
Replace ni::cmodel::Packetizer (5 methods, 3 wrong_side stubs per concrete
class) + Depacketizer (7 methods, 3 stubs) with RequestPacketizer,
RequestDepacketizer, ResponsePacketizer, ResponseDepacketizer. Loopback
test fixture splits into RequestChannelSet + ResponseChannelSet + 4 narrow
stubs; integration aggregate via LoopbackChannelSet. AxiSlavePort and
AxiMasterPort ctors take narrow refs. DelayedLoopback migrated. Delete
c_model/include/ni/ (now empty) + loopback_{packetizer,depacketizer}.hpp.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 5: Fix `packet_format_overview.jpg` title typo

**Files:**
- Modify: `docs/image/packet_format_overview.jpg`

The image asset has no SVG / pptx source in the repo. The bit ranges in the same figure are 56-consistent (e.g. `rsvd [55:54]`); only the figure title "Header Layout (48 bits)" is stale.

- [ ] **Step 1: Edit the image**

Open `docs/image/packet_format_overview.jpg` in any image editor (Paint.NET, GIMP, Photoshop, Affinity Photo). Change the visible title text `Header Layout (48 bits)` to `Header Layout (56 bits)`. Match font / size / color of surrounding text (the figure already uses the same value `56` semantically — bit range `[55:54]` confirms total = 56). Export back to JPEG, same path, equivalent quality (>= 85).

- [ ] **Step 2: Visual verify**

Open the saved JPEG. Confirm:
- Title reads "Header Layout (56 bits)"
- All other figure elements (`qos`, `axi_ch`, `src_id`, ..., `rsvd` cells and bit-range rows) unchanged
- File still loads in standard image viewers

- [ ] **Step 3: Commit**

```bash
git add docs/image/packet_format_overview.jpg
git commit -m "docs(image): fix packet_format_overview header total 48 -> 56

Title was stale; bit ranges in the same figure ([55:54] for rsvd) already
imply total = 56, matching ni_packet.json HEADER_TOTAL_WIDTH and header.jpg.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Self-Review

**Spec coverage check.** Walk through `docs/superpowers/specs/2026-06-09-ni-layer-cleanup-design.md` section-by-section:

| Spec section | Task |
|---|---|
| File disposition (flit.hpp move) | Task 2 |
| File disposition (port_params split) | Task 3 |
| File disposition (narrow interface replace) | Task 4 |
| File disposition (delete wrong_side.hpp) | Task 4 step 10 |
| Narrow interface section | Task 4 step 3-4 |
| `pop_b/r_with_meta` default impl | Task 4 step 4 |
| AxiSlavePort/AxiMasterPort ctor change | Task 4 step 6 |
| PortParams split (3 structs) | Task 3 step 3-5 |
| NmuConfig / NsuConfig de-duplication | Task 3 step 7 |
| YAML schema regroup | Task 3 step 6 |
| Loopback channel set (Request/Response/aggregate) | Task 4 step 7-8 |
| Codegen emit strategy + `<string_view>` + golden | Task 1 |
| Behavior change on `"rsvd"` | Task 2 step 1 |
| Commit sequencing | Tasks 1-5 = commits 1-5 |
| Test: schema fail-loud | Embedded in Task 3 loader code (`throw std::runtime_error`) — covered by upstream caller try/catch in existing test pattern; add explicit fail-loud test if any caller skips the throw path |
| Test: asymmetric NMU/NSU | Task 3 step 1 |
| Test: codegen golden | Task 1 step 5 (`test_byte_identical_golden.py`) |
| Test: `"rsvd"` contract narrowing | Task 2 step 1 |
| Title typo fix | Task 5 |

All spec items covered.

**Placeholder scan.** No "TBD" / "TODO" / "similar to" — each step has actual code or actual command.

**Type consistency.** `RequestPacketizer`, `RequestDepacketizer`, `ResponsePacketizer`, `ResponseDepacketizer` referenced consistently across Tasks 4 + tests. `nmu::PortParams` / `nsu::PortParams` / `testing::ChannelModelParams` consistent across Tasks 3 + tests. `FieldDescriptor` / `HEADER_FIELDS[]` / `*_PAYLOAD_FIELDS[]` consistent across Tasks 1 + 2. `RequestChannelSet` / `ResponseChannelSet` / `LoopbackChannelSet` consistent across Task 4. `load_nmu_port_params` / `load_nsu_port_params` / `load_channel_model_params` consistent.

---

## Out-of-scope (backlog, do not include in this plan)

- `ni::cmodel::testing::` namespace retag to avoid GoogleTest `::testing::` confusion (Codex D2 round)
- `packetizer` term → `BeatSink/Source` rename (Codex D3 round)
- `packet_format_overview.jpg` bit-range source-of-truth audit (Task 5 fixes title only)

Update memory `[[pending-delete-ni-directory]]` to **complete** after Task 4 commit merges.
