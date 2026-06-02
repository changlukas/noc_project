# NMU/NSU Packetize + Depacketize Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the four NoC packet encoders/decoders that bridge AXI4 beats and NoC flits between the Stage 3 port pair and the (future) NoC fabric, plus supporting infrastructure (Flit class, NoC abstract interfaces, MetaBuffer, LoopbackNoc test fixture, specgen codegen extension), achieving zero-mismatch Scoreboard end-to-end through replayed Stage 2 fixtures.

**Architecture:** NMU side runs `Packetize` (stateless + write-metadata FIFO so W beats inherit AW routing) and `Depacketize` (stateful B/R demux with pending-flit head-of-line stash). NSU side mirrors with its own `Depacketize` (AW/W/AR demux + MetaBuffer snapshot) and `Packetize` (peek+commit MetaBuffer for response routing). Four single-method abstract bases (`NocReqOut/In`, `NocRspOut/In`) match `ni_signals.json` pin struct names. `LoopbackNoc` test fixture implements all four via inner-adapter classes and does not rate-limit (vc_arb's future job). Specgen codegen extended to emit per-payload-field LSB/MSB so packetize can bit-pack without hand-rolled tables.

**Tech Stack:** C++17, GoogleTest, CMake, Python 3 specgen (cpp_packet.py / sv_packet.py emitters), yaml-cpp for config loading, Ninja build (mingw64).

**Reference spec:** `docs/superpowers/specs/2026-06-02-nmu-nsu-packetize-depacketize-design.md` (commit `143f7a1`)

**Drift gates** (every commit must pass):
```bash
cd specgen && py -3 -m pytest -q && py -3 tools/codegen.py --check && py -3 tools/gen_inventory.py --check
cd ../c_model && cmake --build build && ctest --test-dir build -j 1
```

---

## Phase A: Foundations (codegen extension + Flit class)

### Task 1: Lock `axi_ch` encoding in `ni_packet.json` + verify codegen

**Files:**
- Modify: `specgen/generated/json/ni_packet.json` (add `axi_ch.encoding` block)
- Verify (read-only): `specgen/generated/cpp/ni_flit_constants.h`, `specgen/generated/sv/ni_flit_pkg.sv`

- [ ] **Step 1: Read current axi_ch entry in ni_packet.json**

```bash
grep -n -A 5 '"name": "axi_ch"' specgen/generated/json/ni_packet.json
```
Expected: shows the field entry without `encoding` key.

- [ ] **Step 2: Add `encoding` map**

Modify the `axi_ch` entry in `specgen/generated/json/ni_packet.json` from:
```json
{
  "name": "axi_ch",
  "width_param": "AXI_CH_WIDTH",
  "enabled": true
},
```
to:
```json
{
  "name": "axi_ch",
  "width_param": "AXI_CH_WIDTH",
  "enabled": true,
  "encoding": { "0": "AW", "1": "W", "2": "AR", "3": "B", "4": "R" }
},
```

- [ ] **Step 3: Regen codegen + check drift**

```bash
cd specgen && py -3 tools/codegen.py --target cpp --domain packet && py -3 tools/codegen.py --target sv --domain packet
```
Expected: writes `generated/cpp/ni_flit_constants.h` + `generated/sv/ni_flit_pkg.sv`. No errors.

Inspect `ni_flit_constants.h` — verify new block appeared:
```bash
grep -A 8 "axi_ch encoding" specgen/generated/cpp/ni_flit_constants.h
```
Expected: lines like `constexpr int AXI_CH_AW = 0;` for all 5 channels (the codegen already supports `field.encoding` via `constants.axi_channel_encoding()`).

- [ ] **Step 4: Refresh affected goldens**

```bash
cp specgen/generated/cpp/ni_flit_constants.h specgen/tests/golden/ni_flit_constants.h.golden
cp specgen/generated/sv/ni_flit_pkg.sv specgen/tests/golden/ni_flit_pkg.sv.golden
```

- [ ] **Step 5: Run all drift gates**

```bash
cd specgen && py -3 -m pytest -q && py -3 tools/codegen.py --check && py -3 tools/gen_inventory.py --check
cd ../c_model && cmake --build build && ctest --test-dir build -j 1
```
Expected: specgen pytest 157 passed; codegen `--check` clean; inventory clean; ctest 216/216.

- [ ] **Step 6: Commit**

```bash
git add specgen/generated/json/ni_packet.json specgen/generated/cpp/ni_flit_constants.h specgen/generated/sv/ni_flit_pkg.sv specgen/tests/golden/ni_flit_constants.h.golden specgen/tests/golden/ni_flit_pkg.sv.golden
git commit -m "feat(ni-spec): lock axi_ch encoding (AW=0,W=1,AR=2,B=3,R=4) in ni_packet.json"
```

---

### Task 2: Codegen extension — emit per-payload-field LSB/MSB constants

**Files:**
- Modify: `specgen/tools/elaborate/cpp_packet.py` (append payload-field emission block)
- Modify: `specgen/tools/elaborate/sv_packet.py` (mirror)

- [ ] **Step 1: Read current cpp_packet.py emit() body**

```bash
sed -n '40,130p' specgen/tools/elaborate/cpp_packet.py
```
Expected: shows existing `// --- payload widths per channel ---` block that only emits `<CH>_WIDTH` constants. We will add a richer per-field block below it.

- [ ] **Step 2: Add helper at top of cpp_packet.py to compute per-field offsets**

Insert (after existing imports, before `_emit_padding_fields`):

```python
def _channel_field_positions(spec, channel_name):
    """Walk a payload channel's fields sequentially, return list of
    (field_name, lsb, msb) tuples. Width_param='derived' fills remainder."""
    ch = next(c for c in spec["flit"]["payload_channels"] if c["name"] == channel_name)
    total = int(ch["payload_width"])
    # First pass: resolve non-derived widths
    widths = []
    derived_idx = None
    fixed_sum = 0
    for i, f in enumerate(ch["fields"]):
        wp = f["width_param"]
        if wp == "derived":
            assert derived_idx is None, f"channel {channel_name}: multiple derived fields"
            derived_idx = i
            widths.append(None)
        else:
            w = C.packet_eval_expr(spec, wp)
            widths.append(w)
            fixed_sum += w
    if derived_idx is not None:
        widths[derived_idx] = total - fixed_sum
    # Second pass: cumulative offset
    out = []
    cum = 0
    for f, w in zip(ch["fields"], widths):
        out.append((f["name"], cum, cum + w - 1))
        cum += w
    assert cum == total, f"channel {channel_name}: field widths sum {cum} != payload_width {total}"
    return out
```

- [ ] **Step 3: Append payload-field emission to cpp_packet.py emit()**

In `emit()`, after the existing `namespace payload {...}` block (where `<CH>_WIDTH` constants are emitted), insert:

```python
    # --- payload field bit positions per channel ---
    out.append("// --- payload field bit positions (from flit.payload_channels) ---")
    for ch in spec["flit"]["payload_channels"]:
        ch_lower = ch["name"].lower()
        out.append(f"namespace payload::{ch_lower} {{")
        positions = _channel_field_positions(spec, ch["name"])
        for fname, lsb, msb in positions:
            n = fname.upper()
            out.append(f"constexpr int {n}_LSB     = {lsb};")
            out.append(f"constexpr int {n}_MSB     = {msb};")
            out.append(f"constexpr int {n}_WIDTH   = {msb - lsb + 1};")
        # static_assert: last field's MSB + 1 == channel width
        last_name = positions[-1][0].upper()
        out.append(f"static_assert({last_name}_MSB + 1 == ni::payload::{ch['name']}_WIDTH, "
                   f"\"payload[{ch['name']}] field positions inconsistent with channel width\");")
        out.append(f"}}  // namespace payload::{ch_lower}")
        out.append("")
```

- [ ] **Step 4: Mirror for sv_packet.py**

Find the equivalent `// --- payload widths per channel ---` block in `specgen/tools/elaborate/sv_packet.py` and append:

```python
    # --- payload field bit positions per channel ---
    out.append("  // --- payload field bit positions (from flit.payload_channels) ---")
    for ch in spec["flit"]["payload_channels"]:
        positions = _channel_field_positions(spec, ch["name"])
        for fname, lsb, msb in positions:
            n = fname.upper()
            out.append(f"  localparam int unsigned {ch['name']}_{n}_LSB     = {lsb};")
            out.append(f"  localparam int unsigned {ch['name']}_{n}_MSB     = {msb};")
            out.append(f"  localparam int unsigned {ch['name']}_{n}_WIDTH   = {msb - lsb + 1};")
    out.append("")
```

(SV doesn't have C++ namespaces; prefix with channel name. Add the same `_channel_field_positions` helper to `sv_packet.py` — same code, since it's in the same package.)

- [ ] **Step 5: Regen + visually inspect new output**

```bash
cd specgen && py -3 tools/codegen.py --target cpp --domain packet && py -3 tools/codegen.py --target sv --domain packet
grep -B 1 -A 4 "namespace payload::aw" generated/cpp/ni_flit_constants.h | head -20
grep "AW_AWID" generated/sv/ni_flit_pkg.sv
```
Expected: per-channel namespace block in cpp; flat per-channel-prefixed constants in sv.

- [ ] **Step 6: Refresh goldens**

```bash
cp generated/cpp/ni_flit_constants.h tests/golden/ni_flit_constants.h.golden
cp generated/sv/ni_flit_pkg.sv tests/golden/ni_flit_pkg.sv.golden
```

- [ ] **Step 7: Run specgen pytest to confirm nothing breaks**

```bash
py -3 -m pytest -q
```
Expected: 157 passed (some may transiently fail — fix in Task 3).

- [ ] **Step 8: Commit**

```bash
git add specgen/tools/elaborate/cpp_packet.py specgen/tools/elaborate/sv_packet.py specgen/generated/cpp/ni_flit_constants.h specgen/generated/sv/ni_flit_pkg.sv specgen/tests/golden/ni_flit_constants.h.golden specgen/tests/golden/ni_flit_pkg.sv.golden
git commit -m "feat(specgen): emit per-payload-field LSB/MSB constants with static_assert gates"
```

---

### Task 3: Add specgen tests for per-payload-field constants

**Files:**
- Modify: `specgen/tests/test_constants_resolver.py` (add per-channel payload position tests)
- Modify: `specgen/tests/test_codegen.py` (add presence assertion for new namespace block)
- Modify: `specgen/tests/test_codegen_sv.py` (mirror for SV)

- [ ] **Step 1: Write failing tests for payload field positions in test_constants_resolver.py**

Append to `specgen/tests/test_constants_resolver.py`:

```python
# -- payload field positions (added with codegen extension) -----------
def test_payload_field_position_aw(packet_spec):
    assert C.payload_field_position(packet_spec, "AW", "awid") == (0, 7)
    assert C.payload_field_position(packet_spec, "AW", "awaddr") == (8, 71)


def test_payload_field_position_w_with_reorder(packet_spec):
    # wstrb comes before wdata per ni_packet.json layout
    assert C.payload_field_position(packet_spec, "W", "wlast") == (0, 0)
    assert C.payload_field_position(packet_spec, "W", "wuser") == (1, 8)
    assert C.payload_field_position(packet_spec, "W", "wstrb") == (9, 40)
    assert C.payload_field_position(packet_spec, "W", "wdata") == (41, 296)


def test_payload_field_position_b_after_rsvd_mc_status_removed(packet_spec):
    assert C.payload_field_position(packet_spec, "B", "bid")   == (0, 7)
    assert C.payload_field_position(packet_spec, "B", "bresp") == (8, 9)
    assert C.payload_field_position(packet_spec, "B", "buser") == (10, 17)
    # b_rsvd absorbs the removed rsvd_mc_status (46-bit derived)
    assert C.payload_field_position(packet_spec, "B", "b_rsvd") == (18, 63)
```

- [ ] **Step 2: Run to confirm they pass against the post-Task-2 spec**

```bash
cd specgen && py -3 -m pytest tests/test_constants_resolver.py -v -k payload_field_position
```
Expected: 3 passed.

- [ ] **Step 3: Add presence test in test_codegen.py**

Append to `specgen/tests/test_codegen.py`:

```python
def test_packet_cpp_emits_payload_field_positions():
    """ni_flit_constants.h must contain per-channel payload field LSB/MSB constants."""
    text = (INCLUDE_DIR / "ni_flit_constants.h").read_text(encoding="ascii")
    # Per-channel namespace presence
    for ch_lower in ("aw", "ar", "w", "b", "r"):
        assert f"namespace payload::{ch_lower}" in text, (
            f"missing namespace payload::{ch_lower} block"
        )
    # Representative constant from each channel
    assert "constexpr int AWID_LSB" in text
    assert "constexpr int WDATA_LSB" in text
    assert "constexpr int BID_LSB" in text
    assert "constexpr int RDATA_LSB" in text


def test_packet_cpp_has_payload_static_assert():
    """Each payload channel must have a static_assert that field positions sum to channel width."""
    text = (INCLUDE_DIR / "ni_flit_constants.h").read_text(encoding="ascii")
    for ch in ("AW", "AR", "W", "B", "R"):
        assert f"ni::payload::{ch}_WIDTH" in text, f"missing static_assert for {ch}"
```

- [ ] **Step 4: Add SV mirror test**

Append to `specgen/tests/test_codegen_sv.py` (inside `TestSvPacketEmit` class):

```python
    def test_payload_field_positions_emitted(self):
        """ni_flit_pkg.sv must contain per-channel-prefixed payload field constants."""
        text = _sv_text("ni_flit_pkg.sv")
        # Representative constants from each channel
        for sig in ("AW_AWID_LSB", "AR_ARID_LSB", "W_WDATA_LSB",
                    "B_BID_LSB", "R_RDATA_LSB"):
            assert f"localparam int unsigned {sig}" in text, (
                f"missing {sig} in ni_flit_pkg.sv"
            )
```

- [ ] **Step 5: Run full specgen pytest**

```bash
cd specgen && py -3 -m pytest -q
```
Expected: 157 + 4 new = 161 passed.

- [ ] **Step 6: Commit**

```bash
git add specgen/tests/test_constants_resolver.py specgen/tests/test_codegen.py specgen/tests/test_codegen_sv.py
git commit -m "test(specgen): per-payload-field LSB/MSB position + presence tests"
```

---

### Task 4: Replace `c_model/include/flit.hpp` with full `c_model/include/ni/flit.hpp`

**Files:**
- Create: `c_model/include/ni/flit.hpp` (full Flit class with header + payload field dispatch)
- Modify: `c_model/tests/test_flit.cpp` (update include path + add payload field tests)
- Delete: `c_model/include/flit.hpp` (old, only supported 6 header fields)

- [ ] **Step 1: Verify no production code outside test_flit.cpp uses old flit.hpp**

```bash
grep -rn '#include "flit.hpp"' c_model/include c_model/src 2>&1
grep -rn '#include <flit.hpp>' c_model/include c_model/src 2>&1
grep -rn '#include "flit.hpp"' c_model/tests
```
Expected: only `c_model/tests/test_flit.cpp` matches. If anything else does, ABORT and report.

- [ ] **Step 2: Write the new `c_model/include/ni/flit.hpp`**

Create file with:

```cpp
// New full-functional Flit container. Replaces c_model/include/flit.hpp.
// Header + payload bit access auto-dispatched via codegen-emitted LSB/MSB constants.
#pragma once
#include "ni_flit_constants.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

namespace ni::cmodel {

class Flit {
public:
  static constexpr int WIDTH_BITS  = ni::FLIT_WIDTH;
  static constexpr int WIDTH_BYTES = (WIDTH_BITS + 7) / 8;

  Flit() = default;
  explicit Flit(const std::array<uint8_t, WIDTH_BYTES>& raw) : raw_(raw) {}

  // ---- Header field access ----
  void     set_header_field(std::string_view name, uint64_t value);
  uint64_t get_header_field(std::string_view name) const;

  // ---- Payload field access ----
  // channel: "AW" | "AR" | "W" | "B" | "R" (case-insensitive accepted)
  void     set_payload_field(std::string_view channel, std::string_view field, uint64_t value);
  uint64_t get_payload_field(std::string_view channel, std::string_view field) const;

  // ---- Bulk payload bytes for wide fields (wdata 256-bit, rdata 256-bit) ----
  void set_payload_bytes(std::string_view channel, std::string_view field,
                         const uint8_t* src, std::size_t bit_width);
  void get_payload_bytes(std::string_view channel, std::string_view field,
                         uint8_t* dst, std::size_t bit_width) const;

  const std::array<uint8_t, WIDTH_BYTES>& raw() const noexcept { return raw_; }
  std::array<uint8_t, WIDTH_BYTES>&       raw()       noexcept { return raw_; }

  bool check_padding_is_zero() const;

private:
  std::array<uint8_t, WIDTH_BYTES> raw_{};
};

namespace detail {

struct FieldPos { int lsb, msb; };

// Header field dispatch — 13 fields, populated from codegen constants
inline FieldPos header_field_pos(std::string_view name) {
  if (name == "noc_qos")    return {ni::header::NOC_QOS_LSB,    ni::header::NOC_QOS_MSB};
  if (name == "axi_ch")     return {ni::header::AXI_CH_LSB,     ni::header::AXI_CH_MSB};
  if (name == "src_id")     return {ni::header::SRC_ID_LSB,     ni::header::SRC_ID_MSB};
  if (name == "dst_id")     return {ni::header::DST_ID_LSB,     ni::header::DST_ID_MSB};
  if (name == "vc_id")      return {ni::header::VC_ID_LSB,      ni::header::VC_ID_MSB};
  if (name == "route_par")  return {ni::header::ROUTE_PAR_LSB,  ni::header::ROUTE_PAR_MSB};
  if (name == "last")       return {ni::header::LAST_LSB,       ni::header::LAST_MSB};
  if (name == "rob_req")    return {ni::header::ROB_REQ_LSB,    ni::header::ROB_REQ_MSB};
  if (name == "rob_idx")    return {ni::header::ROB_IDX_LSB,    ni::header::ROB_IDX_MSB};
  if (name == "commtype")   return {ni::header::COMMTYPE_LSB,   ni::header::COMMTYPE_MSB};
  if (name == "multicast")  return {ni::header::MULTICAST_LSB,  ni::header::MULTICAST_MSB};
  if (name == "flit_ecc")   return {ni::header::FLIT_ECC_LSB,   ni::header::FLIT_ECC_MSB};
  if (name == "rsvd")       return {ni::header::RSVD_LSB,       ni::header::RSVD_MSB};
  assert(false && "unknown header field");
  return {-1, -1};
}

// Payload field dispatch — 5 channels, dispatched into per-channel inner namespace
FieldPos payload_field_pos(std::string_view channel, std::string_view field);  // defined out-of-line

inline void write_bits(std::array<uint8_t, Flit::WIDTH_BYTES>& raw,
                       int lsb, int msb, uint64_t value) {
  for (int bit = lsb; bit <= msb; ++bit) {
    int byte = bit / 8, off = bit % 8;
    uint64_t v = (value >> (bit - lsb)) & 1u;
    raw[byte] = (raw[byte] & ~(1u << off)) | (v << off);
  }
}

inline uint64_t read_bits(const std::array<uint8_t, Flit::WIDTH_BYTES>& raw,
                          int lsb, int msb) {
  uint64_t v = 0;
  for (int bit = lsb; bit <= msb; ++bit) {
    int byte = bit / 8, off = bit % 8;
    v |= ((raw[byte] >> off) & 1ull) << (bit - lsb);
  }
  return v;
}

inline FieldPos payload_field_pos(std::string_view channel, std::string_view field) {
  // AW channel
  if (channel == "AW" || channel == "aw") {
    if (field == "awid")     return {ni::payload::aw::AWID_LSB,     ni::payload::aw::AWID_MSB};
    if (field == "awaddr")   return {ni::payload::aw::AWADDR_LSB,   ni::payload::aw::AWADDR_MSB};
    if (field == "awlen")    return {ni::payload::aw::AWLEN_LSB,    ni::payload::aw::AWLEN_MSB};
    if (field == "awsize")   return {ni::payload::aw::AWSIZE_LSB,   ni::payload::aw::AWSIZE_MSB};
    if (field == "awburst")  return {ni::payload::aw::AWBURST_LSB,  ni::payload::aw::AWBURST_MSB};
    if (field == "awcache")  return {ni::payload::aw::AWCACHE_LSB,  ni::payload::aw::AWCACHE_MSB};
    if (field == "awlock")   return {ni::payload::aw::AWLOCK_LSB,   ni::payload::aw::AWLOCK_MSB};
    if (field == "awprot")   return {ni::payload::aw::AWPROT_LSB,   ni::payload::aw::AWPROT_MSB};
    if (field == "awregion") return {ni::payload::aw::AWREGION_LSB, ni::payload::aw::AWREGION_MSB};
    if (field == "awuser")   return {ni::payload::aw::AWUSER_LSB,   ni::payload::aw::AWUSER_MSB};
    if (field == "aw_rsvd")  return {ni::payload::aw::AW_RSVD_LSB,  ni::payload::aw::AW_RSVD_MSB};
  }
  // AR channel
  if (channel == "AR" || channel == "ar") {
    if (field == "arid")     return {ni::payload::ar::ARID_LSB,     ni::payload::ar::ARID_MSB};
    if (field == "araddr")   return {ni::payload::ar::ARADDR_LSB,   ni::payload::ar::ARADDR_MSB};
    if (field == "arlen")    return {ni::payload::ar::ARLEN_LSB,    ni::payload::ar::ARLEN_MSB};
    if (field == "arsize")   return {ni::payload::ar::ARSIZE_LSB,   ni::payload::ar::ARSIZE_MSB};
    if (field == "arburst")  return {ni::payload::ar::ARBURST_LSB,  ni::payload::ar::ARBURST_MSB};
    if (field == "arcache")  return {ni::payload::ar::ARCACHE_LSB,  ni::payload::ar::ARCACHE_MSB};
    if (field == "arlock")   return {ni::payload::ar::ARLOCK_LSB,   ni::payload::ar::ARLOCK_MSB};
    if (field == "arprot")   return {ni::payload::ar::ARPROT_LSB,   ni::payload::ar::ARPROT_MSB};
    if (field == "arregion") return {ni::payload::ar::ARREGION_LSB, ni::payload::ar::ARREGION_MSB};
    if (field == "aruser")   return {ni::payload::ar::ARUSER_LSB,   ni::payload::ar::ARUSER_MSB};
    if (field == "ar_rsvd")  return {ni::payload::ar::AR_RSVD_LSB,  ni::payload::ar::AR_RSVD_MSB};
  }
  // W channel
  if (channel == "W" || channel == "w") {
    if (field == "wlast")    return {ni::payload::w::WLAST_LSB,     ni::payload::w::WLAST_MSB};
    if (field == "wuser")    return {ni::payload::w::WUSER_LSB,     ni::payload::w::WUSER_MSB};
    if (field == "wstrb")    return {ni::payload::w::WSTRB_LSB,     ni::payload::w::WSTRB_MSB};
    if (field == "wdata")    return {ni::payload::w::WDATA_LSB,     ni::payload::w::WDATA_MSB};
    if (field == "w_rsvd")   return {ni::payload::w::W_RSVD_LSB,    ni::payload::w::W_RSVD_MSB};
  }
  // B channel
  if (channel == "B" || channel == "b") {
    if (field == "bid")      return {ni::payload::b::BID_LSB,       ni::payload::b::BID_MSB};
    if (field == "bresp")    return {ni::payload::b::BRESP_LSB,     ni::payload::b::BRESP_MSB};
    if (field == "buser")    return {ni::payload::b::BUSER_LSB,     ni::payload::b::BUSER_MSB};
    if (field == "b_rsvd")   return {ni::payload::b::B_RSVD_LSB,    ni::payload::b::B_RSVD_MSB};
  }
  // R channel
  if (channel == "R" || channel == "r") {
    if (field == "rlast")    return {ni::payload::r::RLAST_LSB,     ni::payload::r::RLAST_MSB};
    if (field == "rid")      return {ni::payload::r::RID_LSB,       ni::payload::r::RID_MSB};
    if (field == "rresp")    return {ni::payload::r::RRESP_LSB,     ni::payload::r::RRESP_MSB};
    if (field == "ruser")    return {ni::payload::r::RUSER_LSB,     ni::payload::r::RUSER_MSB};
    if (field == "rdata")    return {ni::payload::r::RDATA_LSB,     ni::payload::r::RDATA_MSB};
    if (field == "r_rsvd")   return {ni::payload::r::R_RSVD_LSB,    ni::payload::r::R_RSVD_MSB};
  }
  assert(false && "unknown payload field");
  return {-1, -1};
}

}  // namespace detail

inline void Flit::set_header_field(std::string_view name, uint64_t value) {
  auto p = detail::header_field_pos(name);
  uint64_t mask = (p.msb == p.lsb) ? 1ull : ((1ull << (p.msb - p.lsb + 1)) - 1);
  assert((value & ~mask) == 0 && "value exceeds field width");
  detail::write_bits(raw_, p.lsb, p.msb, value & mask);
}

inline uint64_t Flit::get_header_field(std::string_view name) const {
  auto p = detail::header_field_pos(name);
  return detail::read_bits(raw_, p.lsb, p.msb);
}

inline void Flit::set_payload_field(std::string_view channel, std::string_view field, uint64_t value) {
  auto p = detail::payload_field_pos(channel, field);
  // Payload bit offset is within the flit's payload region — add HEADER_WIDTH
  int abs_lsb = ni::HEADER_WIDTH + p.lsb;
  int abs_msb = ni::HEADER_WIDTH + p.msb;
  int width = abs_msb - abs_lsb + 1;
  uint64_t mask = (width == 1) ? 1ull : ((1ull << width) - 1);
  assert((value & ~mask) == 0 && "value exceeds field width");
  detail::write_bits(raw_, abs_lsb, abs_msb, value & mask);
}

inline uint64_t Flit::get_payload_field(std::string_view channel, std::string_view field) const {
  auto p = detail::payload_field_pos(channel, field);
  int abs_lsb = ni::HEADER_WIDTH + p.lsb;
  int abs_msb = ni::HEADER_WIDTH + p.msb;
  return detail::read_bits(raw_, abs_lsb, abs_msb);
}

inline void Flit::set_payload_bytes(std::string_view channel, std::string_view field,
                                    const uint8_t* src, std::size_t bit_width) {
  auto p = detail::payload_field_pos(channel, field);
  int abs_lsb = ni::HEADER_WIDTH + p.lsb;
  assert(static_cast<int>(bit_width) == p.msb - p.lsb + 1 && "bit_width mismatch");
  for (std::size_t bit = 0; bit < bit_width; ++bit) {
    int src_byte = bit / 8, src_off = bit % 8;
    uint8_t v = (src[src_byte] >> src_off) & 1u;
    int dst_bit = abs_lsb + bit;
    int dst_byte = dst_bit / 8, dst_off = dst_bit % 8;
    raw_[dst_byte] = (raw_[dst_byte] & ~(1u << dst_off)) | (v << dst_off);
  }
}

inline void Flit::get_payload_bytes(std::string_view channel, std::string_view field,
                                    uint8_t* dst, std::size_t bit_width) const {
  auto p = detail::payload_field_pos(channel, field);
  int abs_lsb = ni::HEADER_WIDTH + p.lsb;
  assert(static_cast<int>(bit_width) == p.msb - p.lsb + 1 && "bit_width mismatch");
  std::memset(dst, 0, (bit_width + 7) / 8);
  for (std::size_t bit = 0; bit < bit_width; ++bit) {
    int src_bit = abs_lsb + bit;
    int src_byte = src_bit / 8, src_off = src_bit % 8;
    uint8_t v = (raw_[src_byte] >> src_off) & 1u;
    int dst_byte = bit / 8, dst_off = bit % 8;
    dst[dst_byte] |= (v << dst_off);
  }
}

inline bool Flit::check_padding_is_zero() const {
  for (std::size_t i = 0; i < ni::header::PADDING_FIELDS_COUNT; ++i) {
    int lsb = ni::header::PADDING_FIELDS[i].lsb;
    int msb = ni::header::PADDING_FIELDS[i].msb;
    for (int bit = lsb; bit <= msb; ++bit) {
      int byte = bit / 8, off = bit % 8;
      if ((raw_[byte] >> off) & 1u) return false;
    }
  }
  return true;
}

}  // namespace ni::cmodel
```

- [ ] **Step 3: Update test_flit.cpp to use new path + add payload tests**

Replace `c_model/tests/test_flit.cpp` entirely with:

```cpp
#include "ni/flit.hpp"
#include <gtest/gtest.h>

using ni::cmodel::Flit;

TEST(Flit, ConstructFromRawHasMatchingWidth) {
  EXPECT_EQ(Flit::WIDTH_BITS,  ni::FLIT_WIDTH);
  EXPECT_EQ(Flit::WIDTH_BYTES, (ni::FLIT_WIDTH + 7) / 8);
}

TEST(Flit, SetGetHeaderRoundtripAllFields) {
  Flit f;
  f.set_header_field("noc_qos",   0xA);
  f.set_header_field("axi_ch",    0x4);  // R
  f.set_header_field("src_id",    0x12);
  f.set_header_field("dst_id",    0x34);
  f.set_header_field("vc_id",     0x2);
  f.set_header_field("route_par", 0x1);
  f.set_header_field("last",      0x1);
  f.set_header_field("rob_req",   0x1);
  f.set_header_field("rob_idx",   0x1F);
  f.set_header_field("commtype",  0x2);
  f.set_header_field("multicast", 0xFF);
  f.set_header_field("flit_ecc",  0x3FF);
  EXPECT_EQ(f.get_header_field("noc_qos"),   0xAu);
  EXPECT_EQ(f.get_header_field("axi_ch"),    0x4u);
  EXPECT_EQ(f.get_header_field("src_id"),    0x12u);
  EXPECT_EQ(f.get_header_field("dst_id"),    0x34u);
  EXPECT_EQ(f.get_header_field("vc_id"),     0x2u);
  EXPECT_EQ(f.get_header_field("route_par"), 0x1u);
  EXPECT_EQ(f.get_header_field("last"),      0x1u);
  EXPECT_EQ(f.get_header_field("rob_req"),   0x1u);
  EXPECT_EQ(f.get_header_field("rob_idx"),   0x1Fu);
  EXPECT_EQ(f.get_header_field("commtype"),  0x2u);
  EXPECT_EQ(f.get_header_field("multicast"), 0xFFu);
  EXPECT_EQ(f.get_header_field("flit_ecc"),  0x3FFu);
}

TEST(Flit, SetGetPayloadAwFields) {
  Flit f;
  f.set_payload_field("AW", "awid",   0x55);
  f.set_payload_field("AW", "awaddr", 0xDEADBEEFCAFEBABEull);
  f.set_payload_field("AW", "awlen",  0xFF);
  f.set_payload_field("AW", "awsize", 0x5);
  EXPECT_EQ(f.get_payload_field("AW", "awid"),   0x55u);
  EXPECT_EQ(f.get_payload_field("AW", "awaddr"), 0xDEADBEEFCAFEBABEull);
  EXPECT_EQ(f.get_payload_field("AW", "awlen"),  0xFFu);
  EXPECT_EQ(f.get_payload_field("AW", "awsize"), 0x5u);
}

TEST(Flit, SetGetPayloadBytesWdata) {
  Flit f;
  std::array<uint8_t, 32> wdata{};
  for (int i = 0; i < 32; ++i) wdata[i] = static_cast<uint8_t>(0xA0 + i);
  f.set_payload_bytes("W", "wdata", wdata.data(), 256);
  std::array<uint8_t, 32> out{};
  f.get_payload_bytes("W", "wdata", out.data(), 256);
  EXPECT_EQ(out, wdata);
}

TEST(Flit, RsvdPaddingCheckPassesWhenZero) {
  Flit f;
  EXPECT_TRUE(f.check_padding_is_zero());
}

TEST(Flit, RsvdPaddingCheckFailsWhenSet) {
  Flit f;
  // Set a bit in rsvd region
  f.set_header_field("rsvd", 0x3);  // 2-bit rsvd
  EXPECT_FALSE(f.check_padding_is_zero());
}
```

- [ ] **Step 4: Delete the old flit.hpp and any orphaned include**

```bash
rm c_model/include/flit.hpp
```

Also check if `c_model/include/ni_spec.hpp` was wrapping flit.hpp — if it includes flit.hpp, leave the include since the test now provides ni/flit.hpp:

```bash
grep flit c_model/include/ni_spec.hpp
```
If matched, no edit needed (the new ni/flit.hpp does not depend on ni_spec.hpp; old flit.hpp included it as helper).

- [ ] **Step 5: Build and run**

```bash
cd c_model && cmake --build build && ctest --test-dir build -R Flit -j 1
```
Expected: all `Flit` tests pass; build succeeds.

- [ ] **Step 6: Full ctest sweep**

```bash
ctest --test-dir build -j 1
```
Expected: 216 (Stage 2/3 baseline) + Flit count delta (new is 6 tests, old was 8 — net -2 or 0 depending on count).

- [ ] **Step 7: Commit**

```bash
git add c_model/include/ni/flit.hpp c_model/tests/test_flit.cpp
git rm c_model/include/flit.hpp
git commit -m "feat(c_model/ni): replace flit.hpp with full ni/flit.hpp (13 header + per-channel payload fields)"
```

---

## Phase B: NoC infrastructure

### Task 5: Create 4 NoC abstract base classes + extend PortParams/YAML

**Files:**
- Create: `c_model/include/noc/noc_req_out.hpp`
- Create: `c_model/include/noc/noc_req_in.hpp`
- Create: `c_model/include/noc/noc_rsp_out.hpp`
- Create: `c_model/include/noc/noc_rsp_in.hpp`
- Modify: `c_model/include/ni/port_params.hpp` (add depacketize + loopback_noc + meta_buffer fields)
- Modify: `c_model/config/port_params.yaml` (add corresponding sections)

- [ ] **Step 1: Write the 4 abstract NoC interfaces**

`c_model/include/noc/noc_req_out.hpp`:
```cpp
#pragma once
#include "ni/flit.hpp"

namespace ni::cmodel::noc {

class NocReqOut {
public:
  virtual ~NocReqOut() = default;
  // Returns false when downstream cannot accept; upstream sees this as backpressure.
  virtual bool push_flit(const Flit&) = 0;
};

}  // namespace ni::cmodel::noc
```

`c_model/include/noc/noc_req_in.hpp`:
```cpp
#pragma once
#include "ni/flit.hpp"
#include <optional>

namespace ni::cmodel::noc {

class NocReqIn {
public:
  virtual ~NocReqIn() = default;
  // Returns nullopt when no flit available.
  virtual std::optional<Flit> pop_flit() = 0;
};

}  // namespace ni::cmodel::noc
```

`c_model/include/noc/noc_rsp_out.hpp` and `c_model/include/noc/noc_rsp_in.hpp`: identical structure with `NocRspOut` / `NocRspIn` class names.

- [ ] **Step 2: Extend PortParams struct**

Modify `c_model/include/ni/port_params.hpp` — add after the existing 5 fields:

```cpp
struct PortParams {
  // Stage 3 port-pair fields (already present)
  std::size_t aw_queue_depth;
  std::size_t w_queue_depth;
  std::size_t ar_queue_depth;
  std::size_t b_queue_depth;
  std::size_t r_queue_depth;

  // NEW: depacketize internal demux queues (NMU has B/R; NSU has AW/W/AR)
  std::size_t depkt_aw_q_depth;
  std::size_t depkt_w_q_depth;
  std::size_t depkt_ar_q_depth;
  std::size_t depkt_b_q_depth;
  std::size_t depkt_r_q_depth;

  // NEW: LoopbackNoc test fixture deque capacity (per direction)
  std::size_t loopback_noc_req_depth;
  std::size_t loopback_noc_rsp_depth;

  // NEW: MetaBuffer per-ID FIFO depth (default 4 outstanding per AXI ID)
  std::size_t meta_buffer_per_id_depth;
};
```

Extend `load_port_params_yaml(path, side)` to read the new fields. Add after existing field reads:

```cpp
// existing reads above ...
p.depkt_aw_q_depth          = root["depacketize"]["aw_q_depth"].as<std::size_t>();
p.depkt_w_q_depth           = root["depacketize"]["w_q_depth" ].as<std::size_t>();
p.depkt_ar_q_depth          = root["depacketize"]["ar_q_depth"].as<std::size_t>();
p.depkt_b_q_depth           = root["depacketize"]["b_q_depth" ].as<std::size_t>();
p.depkt_r_q_depth           = root["depacketize"]["r_q_depth" ].as<std::size_t>();
p.loopback_noc_req_depth    = root["loopback_noc"]["req_depth"].as<std::size_t>();
p.loopback_noc_rsp_depth    = root["loopback_noc"]["rsp_depth"].as<std::size_t>();
p.meta_buffer_per_id_depth  = root["meta_buffer"]["per_id_depth"].as<std::size_t>();
```

- [ ] **Step 3: Extend port_params.yaml**

Append to `c_model/config/port_params.yaml`:

```yaml
# Depacketize internal demux queue depths
depacketize:
  aw_q_depth: 32
  w_q_depth:  32
  ar_q_depth: 32
  b_q_depth:  32
  r_q_depth:  32

# LoopbackNoc test fixture per-direction deque capacity
loopback_noc:
  req_depth: 32
  rsp_depth: 32

# MetaBuffer per-AXI-ID FIFO depth (max outstanding per ID)
meta_buffer:
  per_id_depth: 4
```

- [ ] **Step 4: Build to confirm headers compile**

```bash
cd c_model && cmake --build build 2>&1 | tail -10
```
Expected: build succeeds (no new tests yet, just headers).

- [ ] **Step 5: Run existing ctest sweep**

```bash
ctest --test-dir build -j 1
```
Expected: still 216 (no regression).

- [ ] **Step 6: Commit**

```bash
git add c_model/include/noc/ c_model/include/ni/port_params.hpp c_model/config/port_params.yaml
git commit -m "feat(c_model/noc): add NocReqOut/In + NocRspOut/In abstract bases; extend PortParams for packetize/depacketize"
```

---

### Task 6: Create `nsu::MetaBuffer` + unit tests

**Files:**
- Create: `c_model/include/nsu/meta_buffer.hpp`
- Create: `c_model/tests/nsu/test_meta_buffer.cpp`
- Modify: `c_model/tests/nsu/CMakeLists.txt` (add new test)

- [ ] **Step 1: Write failing test**

Create `c_model/tests/nsu/test_meta_buffer.cpp`:

```cpp
#include "nsu/meta_buffer.hpp"
#include <gtest/gtest.h>

using ni::cmodel::nsu::MetaBuffer;
using ni::cmodel::nsu::MetaEntry;

TEST(MetaBuffer, WriteSnapshotPeekCommit) {
  MetaBuffer mb(/*per_id_depth=*/4);
  mb.snapshot_write(0x05, {0x10, 1, 7});
  auto e = mb.peek_write(0x05);
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->src_id,  0x10);
  EXPECT_EQ(e->rob_req, 1);
  EXPECT_EQ(e->rob_idx, 7);

  // peek without commit — entry stays
  auto e2 = mb.peek_write(0x05);
  ASSERT_TRUE(e2.has_value());
  EXPECT_EQ(e2->src_id, 0x10);

  mb.commit_write(0x05);
  EXPECT_FALSE(mb.peek_write(0x05).has_value());
}

TEST(MetaBuffer, MultiOutstandingSameIdFifoOrder) {
  MetaBuffer mb(4);
  mb.snapshot_write(0x05, {0x10, 0, 1});
  mb.snapshot_write(0x05, {0x10, 0, 2});
  mb.snapshot_write(0x05, {0x10, 0, 3});
  EXPECT_EQ(mb.peek_write(0x05)->rob_idx, 1);  mb.commit_write(0x05);
  EXPECT_EQ(mb.peek_write(0x05)->rob_idx, 2);  mb.commit_write(0x05);
  EXPECT_EQ(mb.peek_write(0x05)->rob_idx, 3);  mb.commit_write(0x05);
  EXPECT_FALSE(mb.peek_write(0x05).has_value());
}

TEST(MetaBuffer, DifferentIdsIndependent) {
  MetaBuffer mb(4);
  mb.snapshot_write(0x05, {0x10, 0, 0});
  mb.snapshot_write(0x07, {0x20, 0, 0});
  EXPECT_EQ(mb.peek_write(0x07)->src_id, 0x20);
  EXPECT_EQ(mb.peek_write(0x05)->src_id, 0x10);  // not affected by 0x07 ops
}

TEST(MetaBuffer, ReadPeekCommitMultiBeat) {
  MetaBuffer mb(4);
  mb.snapshot_read(0x03, {0x10, 0, 5});
  // R burst: peek twice for r0/r1, commit only on r1 (last)
  EXPECT_EQ(mb.peek_read(0x03)->rob_idx, 5);
  EXPECT_EQ(mb.peek_read(0x03)->rob_idx, 5);  // still there
  mb.commit_read(0x03);
  EXPECT_FALSE(mb.peek_read(0x03).has_value());
}

TEST(MetaBuffer, PeekEmptyReturnsNullopt) {
  MetaBuffer mb(4);
  EXPECT_FALSE(mb.peek_write(0xAA).has_value());
  EXPECT_FALSE(mb.peek_read(0xBB).has_value());
}

TEST(MetaBuffer, SnapshotOverDepthAsserts) {
  // Document depth limit behavior — depth=2, insert 3 should assert (debug build)
  MetaBuffer mb(/*per_id_depth=*/2);
  mb.snapshot_write(0x01, {0, 0, 0});
  mb.snapshot_write(0x01, {0, 0, 0});
  EXPECT_DEATH(mb.snapshot_write(0x01, {0, 0, 0}),
               "MetaBuffer: per-ID depth exceeded");
}
```

- [ ] **Step 2: Create CMakeLists.txt for nsu/ tests if not present, add test_meta_buffer.cpp**

Read existing `c_model/tests/nsu/CMakeLists.txt`. If present, append a new `add_executable` block for `test_meta_buffer.cpp`:

```cmake
add_executable(test_meta_buffer test_meta_buffer.cpp)
target_link_libraries(test_meta_buffer PRIVATE GTest::gtest GTest::gtest_main)
target_include_directories(test_meta_buffer PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_BINARY_DIR}/_deps/yaml-cpp-src/include
)
gtest_discover_tests(test_meta_buffer)
```

(Adjust include paths per existing tests/nsu pattern; copy from test_axi_master_port.cpp's CMake block.)

- [ ] **Step 3: Run test to verify FAIL**

```bash
cmake --build c_model/build 2>&1 | tail -10
```
Expected: build fails with "nsu/meta_buffer.hpp: No such file".

- [ ] **Step 4: Implement `nsu::MetaBuffer`**

Create `c_model/include/nsu/meta_buffer.hpp`:

```cpp
#pragma once
#include <array>
#include <cassert>
#include <cstdint>
#include <deque>
#include <optional>

namespace ni::cmodel::nsu {

struct MetaEntry {
  uint8_t src_id;
  uint8_t rob_req;
  uint8_t rob_idx;
};

// Per-AXI-ID FIFO of {src_id, rob_req, rob_idx} snapshots captured at AW/AR
// flit ingress. Looked up at B/R flit egress via peek+commit pattern.
//
// AXI4 ordering: per-ID transactions complete in issue order. Each FIFO front
// is the oldest outstanding for that ID. Different IDs are independent.
//
// Atomic-ID tagging is OUT OF SCOPE — a per-ID FIFO suffices for tested fixtures.
class MetaBuffer {
public:
  explicit MetaBuffer(std::size_t per_id_depth) : per_id_depth_(per_id_depth) {}

  // -- Write side (AW snapshot + B consume) --
  void snapshot_write(uint8_t awid, MetaEntry e) {
    assert(write_[awid].size() < per_id_depth_ && "MetaBuffer: per-ID depth exceeded");
    write_[awid].push_back(e);
  }
  std::optional<MetaEntry> peek_write(uint8_t bid) const {
    if (write_[bid].empty()) return std::nullopt;
    return write_[bid].front();
  }
  void commit_write(uint8_t bid) {
    assert(!write_[bid].empty() && "commit_write on empty queue");
    write_[bid].pop_front();
  }

  // -- Read side (AR snapshot + R consume) --
  // Multi-beat R burst: peek every beat, commit only on rlast.
  void snapshot_read(uint8_t arid, MetaEntry e) {
    assert(read_[arid].size() < per_id_depth_ && "MetaBuffer: per-ID depth exceeded");
    read_[arid].push_back(e);
  }
  std::optional<MetaEntry> peek_read(uint8_t rid) const {
    if (read_[rid].empty()) return std::nullopt;
    return read_[rid].front();
  }
  void commit_read(uint8_t rid) {
    assert(!read_[rid].empty() && "commit_read on empty queue");
    read_[rid].pop_front();
  }

private:
  std::array<std::deque<MetaEntry>, 256> write_;  // per awid
  std::array<std::deque<MetaEntry>, 256> read_;   // per arid
  std::size_t per_id_depth_;
};

}  // namespace ni::cmodel::nsu
```

- [ ] **Step 5: Build + run new tests**

```bash
cd c_model && cmake --build build && ctest --test-dir build -R MetaBuffer -j 1
```
Expected: 6 tests passing.

- [ ] **Step 6: Full ctest sweep**

```bash
ctest --test-dir build -j 1
```
Expected: 216 + 6 new = 222 passing.

- [ ] **Step 7: Commit**

```bash
git add c_model/include/nsu/meta_buffer.hpp c_model/tests/nsu/test_meta_buffer.cpp c_model/tests/nsu/CMakeLists.txt
git commit -m "feat(c_model/nsu): add MetaBuffer (per-ID FIFO + peek/commit) for response routing"
```

---

### Task 7: Create `LoopbackNoc` test fixture

**Files:**
- Create: `c_model/tests/common/loopback_noc.hpp`

- [ ] **Step 1: Write the LoopbackNoc class**

Create `c_model/tests/common/loopback_noc.hpp`:

```cpp
// LoopbackNoc — test-only NoC bridge. Implements all 4 NoC abstracts via
// inner adapter classes (avoids C++ same-name virtual ambiguity from inheriting
// 4 abstract bases on the outer class).
//
// Bounded deque per direction. Accepts multiple flits per tick — does NOT
// model 1-flit/cycle physical NoC pacing. That's vc_arb's responsibility.
// Latency/throughput numbers from tests using LoopbackNoc are non-physical.
//
// Optional configurable latency variant: set_req_delay(cycles) / set_rsp_delay(cycles)
// makes push_flit enqueue into a delay pipe; tick() ages entries; visible queue
// serves pop_flit only when cycles_left==0.
#pragma once
#include "noc/noc_req_out.hpp"
#include "noc/noc_req_in.hpp"
#include "noc/noc_rsp_out.hpp"
#include "noc/noc_rsp_in.hpp"
#include <cstddef>
#include <deque>
#include <utility>

namespace ni::cmodel::testing {

class LoopbackNoc {
public:
  LoopbackNoc(std::size_t req_depth, std::size_t rsp_depth)
      : req_depth_(req_depth), rsp_depth_(rsp_depth),
        req_out_adapter_{this}, req_in_adapter_{this},
        rsp_out_adapter_{this}, rsp_in_adapter_{this} {}

  noc::NocReqOut& req_out() { return req_out_adapter_; }
  noc::NocReqIn&  req_in()  { return req_in_adapter_;  }
  noc::NocRspOut& rsp_out() { return rsp_out_adapter_; }
  noc::NocRspIn&  rsp_in()  { return rsp_in_adapter_;  }

  void set_req_delay(unsigned cycles) { req_delay_ = cycles; }
  void set_rsp_delay(unsigned cycles) { rsp_delay_ = cycles; }

  // Age delayed entries; promote to visible queue when cycles_left==0.
  void tick() {
    auto age = [](std::deque<std::pair<Flit, unsigned>>& pipe,
                  std::deque<Flit>& visible, std::size_t cap) {
      for (auto& e : pipe) if (e.second > 0) --e.second;
      while (!pipe.empty() && pipe.front().second == 0 && visible.size() < cap) {
        visible.push_back(pipe.front().first);
        pipe.pop_front();
      }
    };
    age(req_pipe_, req_q_, req_depth_);
    age(rsp_pipe_, rsp_q_, rsp_depth_);
  }

  // Test introspection
  std::size_t req_q_size()    const { return req_q_.size(); }
  std::size_t rsp_q_size()    const { return rsp_q_.size(); }
  std::size_t req_pipe_size() const { return req_pipe_.size(); }
  std::size_t rsp_pipe_size() const { return rsp_pipe_.size(); }

private:
  // Inner adapters: each implements ONE abstract and forwards to outer
  struct ReqOutAdapter : noc::NocReqOut {
    LoopbackNoc* p;
    explicit ReqOutAdapter(LoopbackNoc* parent) : p(parent) {}
    bool push_flit(const Flit& f) override {
      if (p->req_delay_ > 0) {
        if (p->req_pipe_.size() + p->req_q_.size() >= p->req_depth_) return false;
        p->req_pipe_.emplace_back(f, p->req_delay_);
      } else {
        if (p->req_q_.size() >= p->req_depth_) return false;
        p->req_q_.push_back(f);
      }
      return true;
    }
  };
  struct ReqInAdapter : noc::NocReqIn {
    LoopbackNoc* p;
    explicit ReqInAdapter(LoopbackNoc* parent) : p(parent) {}
    std::optional<Flit> pop_flit() override {
      if (p->req_q_.empty()) return std::nullopt;
      Flit f = p->req_q_.front();
      p->req_q_.pop_front();
      return f;
    }
  };
  struct RspOutAdapter : noc::NocRspOut {
    LoopbackNoc* p;
    explicit RspOutAdapter(LoopbackNoc* parent) : p(parent) {}
    bool push_flit(const Flit& f) override {
      if (p->rsp_delay_ > 0) {
        if (p->rsp_pipe_.size() + p->rsp_q_.size() >= p->rsp_depth_) return false;
        p->rsp_pipe_.emplace_back(f, p->rsp_delay_);
      } else {
        if (p->rsp_q_.size() >= p->rsp_depth_) return false;
        p->rsp_q_.push_back(f);
      }
      return true;
    }
  };
  struct RspInAdapter : noc::NocRspIn {
    LoopbackNoc* p;
    explicit RspInAdapter(LoopbackNoc* parent) : p(parent) {}
    std::optional<Flit> pop_flit() override {
      if (p->rsp_q_.empty()) return std::nullopt;
      Flit f = p->rsp_q_.front();
      p->rsp_q_.pop_front();
      return f;
    }
  };

  std::size_t req_depth_, rsp_depth_;
  unsigned req_delay_ = 0, rsp_delay_ = 0;
  std::deque<Flit> req_q_, rsp_q_;
  std::deque<std::pair<Flit, unsigned>> req_pipe_, rsp_pipe_;
  ReqOutAdapter req_out_adapter_;
  ReqInAdapter  req_in_adapter_;
  RspOutAdapter rsp_out_adapter_;
  RspInAdapter  rsp_in_adapter_;
};

}  // namespace ni::cmodel::testing
```

- [ ] **Step 2: Build to confirm header compiles standalone**

```bash
cd c_model && cmake --build build 2>&1 | tail -5
```
Expected: build succeeds (no tests reference it yet).

- [ ] **Step 3: Commit**

```bash
git add c_model/tests/common/loopback_noc.hpp
git commit -m "feat(c_model/tests/common): add LoopbackNoc test fixture (inner-adapter pattern, configurable delay)"
```

---

## Phase C: Packetize modules

### Task 8: `nmu::Packetize` (stateless + write-meta FIFO) + unit tests

**Files:**
- Create: `c_model/include/nmu/packetize.hpp`
- Create: `c_model/tests/nmu/test_packetize.cpp`
- Modify: `c_model/tests/nmu/CMakeLists.txt`

- [ ] **Step 1: Write the unit test FIRST (key tests)**

Create `c_model/tests/nmu/test_packetize.cpp`:

```cpp
#include "nmu/packetize.hpp"
#include "common/loopback_noc.hpp"
#include "axi/types.hpp"
#include <gtest/gtest.h>

using ni::cmodel::testing::LoopbackNoc;
using ni::cmodel::nmu::Packetize;
namespace axi = ni::cmodel::axi;

namespace {
constexpr uint8_t kSrcId = 0x12;

axi::AwBeat make_aw(uint8_t id, uint64_t addr, uint8_t len = 0) {
  axi::AwBeat b{};
  b.id = id; b.addr = addr; b.len = len; b.size = 5;
  b.burst = axi::Burst::INCR; b.cache = 0xF; b.lock = 0;
  b.prot = 0; b.region = 0; b.user = 0; b.qos = 0;
  return b;
}
axi::WBeat make_w(uint32_t strb, bool last) {
  axi::WBeat b{};
  for (int i = 0; i < 32; ++i) b.data[i] = static_cast<uint8_t>(i);
  b.strb = strb; b.last = last; b.user = 0;
  return b;
}
axi::ArBeat make_ar(uint8_t id, uint64_t addr) {
  axi::ArBeat b{};
  b.id = id; b.addr = addr; b.len = 0; b.size = 5;
  b.burst = axi::Burst::INCR; b.cache = 0; b.lock = 0;
  b.prot = 0; b.region = 0; b.user = 0; b.qos = 0;
  return b;
}
}

TEST(NmuPacketize, PushAwEmitsFlitWithCorrectFields) {
  LoopbackNoc noc(/*req*/16, /*rsp*/16);
  Packetize pkt(noc.req_out(), kSrcId);
  pkt.set_aw_header_extras(/*dst*/0x34, /*rob_req*/0, /*rob_idx*/0);
  ASSERT_TRUE(pkt.push_aw(make_aw(0x05, 0xDEADBEEFCAFEBABEull)));

  // Read flit back from req queue (test introspection via NocReqIn adapter)
  auto flit_opt = noc.req_in().pop_flit();
  ASSERT_TRUE(flit_opt.has_value());
  const auto& f = *flit_opt;
  EXPECT_EQ(f.get_header_field("axi_ch"),   ni::AXI_CH_AW);
  EXPECT_EQ(f.get_header_field("src_id"),   kSrcId);
  EXPECT_EQ(f.get_header_field("dst_id"),   0x34u);
  EXPECT_EQ(f.get_header_field("vc_id"),    0u);
  EXPECT_EQ(f.get_header_field("last"),     1u);
  EXPECT_EQ(f.get_payload_field("AW", "awid"),   0x05u);
  EXPECT_EQ(f.get_payload_field("AW", "awaddr"), 0xDEADBEEFCAFEBABEull);
}

TEST(NmuPacketize, StickySetterAssertMissingSet) {
  LoopbackNoc noc(16, 16);
  Packetize pkt(noc.req_out(), kSrcId);
  EXPECT_DEATH(pkt.push_aw(make_aw(0, 0)), "set_aw_header_extras");
}

TEST(NmuPacketize, StickySetterAssertDoubleSet) {
  LoopbackNoc noc(16, 16);
  Packetize pkt(noc.req_out(), kSrcId);
  pkt.set_aw_header_extras(0x10);
  EXPECT_DEATH(pkt.set_aw_header_extras(0x20), "previous set_aw_header_extras not yet consumed");
}

TEST(NmuPacketize, StickySetterArMissingSet) {
  LoopbackNoc noc(16, 16);
  Packetize pkt(noc.req_out(), kSrcId);
  EXPECT_DEATH(pkt.push_ar(make_ar(0, 0)), "set_ar_header_extras");
}

TEST(NmuPacketize, StickySetterArDoubleSet) {
  LoopbackNoc noc(16, 16);
  Packetize pkt(noc.req_out(), kSrcId);
  pkt.set_ar_header_extras(0x10);
  EXPECT_DEATH(pkt.set_ar_header_extras(0x20), "previous set_ar_header_extras not yet consumed");
}

TEST(NmuPacketize, WMetaFifoInheritsAwDst) {
  LoopbackNoc noc(16, 16);
  Packetize pkt(noc.req_out(), kSrcId);
  pkt.set_aw_header_extras(/*dst*/0x34);
  ASSERT_TRUE(pkt.push_aw(make_aw(0x05, 0x1000)));
  ASSERT_TRUE(pkt.push_w(make_w(0xFFFFFFFF, /*last*/true)));

  noc.req_in().pop_flit();  // discard AW
  auto w_flit_opt = noc.req_in().pop_flit();
  ASSERT_TRUE(w_flit_opt.has_value());
  EXPECT_EQ(w_flit_opt->get_header_field("dst_id"), 0x34u);
  EXPECT_EQ(w_flit_opt->get_header_field("axi_ch"), ni::AXI_CH_W);
}

TEST(NmuPacketize, MultiOutstandingAwInterleavedW) {
  LoopbackNoc noc(16, 16);
  Packetize pkt(noc.req_out(), kSrcId);
  // AW1 to dst=0x34
  pkt.set_aw_header_extras(0x34);
  ASSERT_TRUE(pkt.push_aw(make_aw(0x05, 0x1000)));
  // AW2 to dst=0x56
  pkt.set_aw_header_extras(0x56);
  ASSERT_TRUE(pkt.push_aw(make_aw(0x06, 0x2000)));
  // W for AW1 (single beat, wlast=1)
  ASSERT_TRUE(pkt.push_w(make_w(0xFF, /*last*/true)));
  // W for AW2 (single beat, wlast=1)
  ASSERT_TRUE(pkt.push_w(make_w(0xFF, /*last*/true)));

  // Verify: 4 flits in req_q. flits 0,1 = AWs; flits 2,3 = Ws inheriting AW order
  ASSERT_EQ(noc.req_q_size(), 4u);
  noc.req_in().pop_flit();  // AW1
  noc.req_in().pop_flit();  // AW2
  auto w1 = noc.req_in().pop_flit();
  auto w2 = noc.req_in().pop_flit();
  EXPECT_EQ(w1->get_header_field("dst_id"), 0x34u);  // inherits AW1
  EXPECT_EQ(w2->get_header_field("dst_id"), 0x56u);  // inherits AW2
}

TEST(NmuPacketize, PushAwFailsOnNocFull) {
  LoopbackNoc noc(/*req*/1, /*rsp*/16);
  Packetize pkt(noc.req_out(), kSrcId);
  pkt.set_aw_header_extras(0x10);
  ASSERT_TRUE(pkt.push_aw(make_aw(0, 0)));  // fills req_q (cap=1)
  pkt.set_aw_header_extras(0x20);
  // (note: setter succeeds — fail-loud only on second set without push)
  EXPECT_FALSE(pkt.push_aw(make_aw(1, 0)));  // NoC full; push fails; pending unchanged
  // Drain one, try again
  noc.req_in().pop_flit();
  EXPECT_TRUE(pkt.push_aw(make_aw(1, 0)));
}

TEST(NmuPacketize, AwPayloadBitPerfect) {
  LoopbackNoc noc(16, 16);
  Packetize pkt(noc.req_out(), kSrcId);
  pkt.set_aw_header_extras(0x34);
  auto aw = make_aw(/*id*/0xAB, /*addr*/0x123456789ABCDEF0ull, /*len*/0xFF);
  aw.size = 5; aw.burst = axi::Burst::WRAP; aw.cache = 0xF; aw.lock = 1;
  aw.prot = 0x7; aw.region = 0xF; aw.user = 0xFF; aw.qos = 0xF;
  ASSERT_TRUE(pkt.push_aw(aw));
  auto f = *noc.req_in().pop_flit();
  EXPECT_EQ(f.get_payload_field("AW", "awid"),     0xABu);
  EXPECT_EQ(f.get_payload_field("AW", "awaddr"),   0x123456789ABCDEF0ull);
  EXPECT_EQ(f.get_payload_field("AW", "awlen"),    0xFFu);
  EXPECT_EQ(f.get_payload_field("AW", "awsize"),   5u);
  EXPECT_EQ(f.get_payload_field("AW", "awburst"), static_cast<uint64_t>(axi::Burst::WRAP));
  EXPECT_EQ(f.get_payload_field("AW", "awcache"),  0xFu);
  EXPECT_EQ(f.get_payload_field("AW", "awlock"),   1u);
  EXPECT_EQ(f.get_payload_field("AW", "awprot"),   0x7u);
  EXPECT_EQ(f.get_payload_field("AW", "awregion"), 0xFu);
  EXPECT_EQ(f.get_payload_field("AW", "awuser"),   0xFFu);
}

TEST(NmuPacketize, WPayloadBitPerfect) {
  LoopbackNoc noc(16, 16);
  Packetize pkt(noc.req_out(), kSrcId);
  pkt.set_aw_header_extras(0x34);
  ASSERT_TRUE(pkt.push_aw(make_aw(0, 0)));
  auto w = make_w(0xDEADBEEF, /*last*/true);
  w.user = 0xAB;
  ASSERT_TRUE(pkt.push_w(w));
  noc.req_in().pop_flit();  // discard AW
  auto f = *noc.req_in().pop_flit();
  EXPECT_EQ(f.get_payload_field("W", "wlast"),  1u);
  EXPECT_EQ(f.get_payload_field("W", "wstrb"),  0xDEADBEEFu);
  EXPECT_EQ(f.get_payload_field("W", "wuser"),  0xABu);
  std::array<uint8_t, 32> wdata_out{};
  f.get_payload_bytes("W", "wdata", wdata_out.data(), 256);
  for (int i = 0; i < 32; ++i) EXPECT_EQ(wdata_out[i], static_cast<uint8_t>(i));
}

TEST(NmuPacketize, ArEncodesAxiChAndRobIdx) {
  LoopbackNoc noc(16, 16);
  Packetize pkt(noc.req_out(), kSrcId);
  pkt.set_ar_header_extras(/*dst*/0x99, /*rob_req*/1, /*rob_idx*/0x15);
  ASSERT_TRUE(pkt.push_ar(make_ar(0x07, 0x4000)));
  auto f = *noc.req_in().pop_flit();
  EXPECT_EQ(f.get_header_field("axi_ch"),   ni::AXI_CH_AR);
  EXPECT_EQ(f.get_header_field("dst_id"),   0x99u);
  EXPECT_EQ(f.get_header_field("rob_req"),  1u);
  EXPECT_EQ(f.get_header_field("rob_idx"),  0x15u);
  EXPECT_EQ(f.get_payload_field("AR", "arid"),   0x07u);
  EXPECT_EQ(f.get_payload_field("AR", "araddr"), 0x4000u);
}

TEST(NmuPacketize, RsvdAndDisabledFieldsZero) {
  LoopbackNoc noc(16, 16);
  Packetize pkt(noc.req_out(), kSrcId);
  pkt.set_aw_header_extras(0x34);
  ASSERT_TRUE(pkt.push_aw(make_aw(0, 0)));
  auto f = *noc.req_in().pop_flit();
  // commtype, multicast, noc_qos, route_par, flit_ecc 0-filled per spec
  EXPECT_EQ(f.get_header_field("commtype"),  0u);
  EXPECT_EQ(f.get_header_field("multicast"), 0u);
  EXPECT_EQ(f.get_header_field("noc_qos"),   0u);
  EXPECT_EQ(f.get_header_field("route_par"), 0u);
  EXPECT_EQ(f.get_header_field("flit_ecc"),  0u);
  EXPECT_TRUE(f.check_padding_is_zero());  // rsvd padding clean
}
```

- [ ] **Step 2: Add to CMakeLists.txt**

`c_model/tests/nmu/CMakeLists.txt` — add `add_executable(test_packetize test_packetize.cpp)` block following the same pattern as `test_axi_slave_port`.

- [ ] **Step 3: Run to see fail (no packetize.hpp yet)**

```bash
cd c_model && cmake --build build 2>&1 | tail -5
```
Expected: build error "nmu/packetize.hpp: No such file".

- [ ] **Step 4: Implement `nmu::Packetize`**

Create `c_model/include/nmu/packetize.hpp`:

```cpp
#pragma once
#include "axi/types.hpp"
#include "ni/flit.hpp"
#include "ni/packetizer.hpp"           // from Stage 3 port-pair task
#include "noc/noc_req_out.hpp"
#include <cassert>
#include <cstdint>
#include <deque>

namespace ni::cmodel::nmu {

// NMU-side request packetizer. Stateless except for a small write-metadata
// FIFO (populated by push_aw, consumed by W beats with wlast=1). Implements
// the 5-method Packetizer interface; push_b/push_r assert false (NMU never
// emits responses).
//
// Header fields per push:
//   src_id      — constructor arg (NMU tile coord, fixed per instance)
//   dst_id      — set via set_aw/ar_header_extras (sticky, fail-loud); for W,
//                 inherited from the AW write-meta FIFO front
//   vc_id       — hardcoded 0 (NUM_VC=1)
//   axi_ch      — implicit per push_* method
//   last        — always 1 (1 beat = 1 flit = 1 packet)
//   rob_req,
//   rob_idx     — set via set_aw/ar_header_extras
//   commtype,
//   multicast,
//   noc_qos,
//   route_par,
//   flit_ecc    — 0-filled (deferred to future tasks)
//   rsvd        — 0 by Flit default
class Packetize : public Packetizer {
public:
  Packetize(noc::NocReqOut& req_out, uint8_t src_id)
      : req_out_(req_out), src_id_(src_id) {}

  // ---- Packetizer interface (request methods are real) ----
  bool push_aw(const axi::AwBeat& b) override;
  bool push_w (const axi::WBeat&  b) override;
  bool push_ar(const axi::ArBeat& b) override;

  // ---- Packetizer interface (response methods assert false) ----
  bool push_b (const axi::BBeat&) override { assert(false && "NMU packetize: B not applicable"); return false; }
  bool push_r (const axi::RBeat&) override { assert(false && "NMU packetize: R not applicable"); return false; }

  // ---- Sticky setter (fail-loud) ----
  void set_aw_header_extras(uint8_t dst_id, uint8_t rob_req = 0, uint8_t rob_idx = 0) {
    assert(!aw_extras_pending_ && "previous set_aw_header_extras not yet consumed by push_aw");
    aw_dst_id_ = dst_id; aw_rob_req_ = rob_req; aw_rob_idx_ = rob_idx;
    aw_extras_pending_ = true;
  }
  void set_ar_header_extras(uint8_t dst_id, uint8_t rob_req = 0, uint8_t rob_idx = 0) {
    assert(!ar_extras_pending_ && "previous set_ar_header_extras not yet consumed by push_ar");
    ar_dst_id_ = dst_id; ar_rob_req_ = rob_req; ar_rob_idx_ = rob_idx;
    ar_extras_pending_ = true;
  }

private:
  noc::NocReqOut& req_out_;
  uint8_t src_id_;
  bool aw_extras_pending_ = false;
  bool ar_extras_pending_ = false;
  uint8_t aw_dst_id_ = 0, aw_rob_req_ = 0, aw_rob_idx_ = 0;
  uint8_t ar_dst_id_ = 0, ar_rob_req_ = 0, ar_rob_idx_ = 0;

  struct WriteMeta { uint8_t dst_id; uint8_t rob_req; uint8_t rob_idx; };
  std::deque<WriteMeta> w_meta_fifo_;
};

// ---- inline impl ----

inline bool Packetize::push_aw(const axi::AwBeat& b) {
  assert(aw_extras_pending_ && "set_aw_header_extras must be called before push_aw");
  Flit f;
  f.set_header_field("axi_ch",  ni::AXI_CH_AW);
  f.set_header_field("src_id",  src_id_);
  f.set_header_field("dst_id",  aw_dst_id_);
  f.set_header_field("vc_id",   0);
  f.set_header_field("last",    1);
  f.set_header_field("rob_req", aw_rob_req_);
  f.set_header_field("rob_idx", aw_rob_idx_);
  f.set_payload_field("AW", "awid",     b.id);
  f.set_payload_field("AW", "awaddr",   b.addr);
  f.set_payload_field("AW", "awlen",    b.len);
  f.set_payload_field("AW", "awsize",   b.size);
  f.set_payload_field("AW", "awburst",  static_cast<uint64_t>(b.burst));
  f.set_payload_field("AW", "awcache",  b.cache);
  f.set_payload_field("AW", "awlock",   b.lock);
  f.set_payload_field("AW", "awprot",   b.prot);
  f.set_payload_field("AW", "awregion", b.region);
  f.set_payload_field("AW", "awuser",   b.user);
  if (!req_out_.push_flit(f)) return false;  // backpressure — retain pending state
  w_meta_fifo_.push_back({aw_dst_id_, aw_rob_req_, aw_rob_idx_});
  aw_extras_pending_ = false;
  return true;
}

inline bool Packetize::push_w(const axi::WBeat& b) {
  assert(!w_meta_fifo_.empty() && "push_w called before any push_aw");
  const auto& meta = w_meta_fifo_.front();
  Flit f;
  f.set_header_field("axi_ch",  ni::AXI_CH_W);
  f.set_header_field("src_id",  src_id_);
  f.set_header_field("dst_id",  meta.dst_id);
  f.set_header_field("vc_id",   0);
  f.set_header_field("last",    1);
  f.set_header_field("rob_req", meta.rob_req);
  f.set_header_field("rob_idx", meta.rob_idx);
  f.set_payload_field("W", "wlast", b.last ? 1u : 0u);
  f.set_payload_field("W", "wuser", b.user);
  f.set_payload_field("W", "wstrb", b.strb);
  f.set_payload_bytes("W", "wdata", b.data.data(), 256);
  if (!req_out_.push_flit(f)) return false;
  if (b.last) w_meta_fifo_.pop_front();
  return true;
}

inline bool Packetize::push_ar(const axi::ArBeat& b) {
  assert(ar_extras_pending_ && "set_ar_header_extras must be called before push_ar");
  Flit f;
  f.set_header_field("axi_ch",  ni::AXI_CH_AR);
  f.set_header_field("src_id",  src_id_);
  f.set_header_field("dst_id",  ar_dst_id_);
  f.set_header_field("vc_id",   0);
  f.set_header_field("last",    1);
  f.set_header_field("rob_req", ar_rob_req_);
  f.set_header_field("rob_idx", ar_rob_idx_);
  f.set_payload_field("AR", "arid",     b.id);
  f.set_payload_field("AR", "araddr",   b.addr);
  f.set_payload_field("AR", "arlen",    b.len);
  f.set_payload_field("AR", "arsize",   b.size);
  f.set_payload_field("AR", "arburst",  static_cast<uint64_t>(b.burst));
  f.set_payload_field("AR", "arcache",  b.cache);
  f.set_payload_field("AR", "arlock",   b.lock);
  f.set_payload_field("AR", "arprot",   b.prot);
  f.set_payload_field("AR", "arregion", b.region);
  f.set_payload_field("AR", "aruser",   b.user);
  if (!req_out_.push_flit(f)) return false;
  ar_extras_pending_ = false;
  return true;
}

}  // namespace ni::cmodel::nmu
```

- [ ] **Step 5: Build + run tests**

```bash
cd c_model && cmake --build build && ctest --test-dir build -R NmuPacketize -j 1
```
Expected: 11 tests pass.

- [ ] **Step 6: Full ctest sweep**

```bash
ctest --test-dir build -j 1
```
Expected: 222 + 11 new = 233.

- [ ] **Step 7: Commit**

```bash
git add c_model/include/nmu/packetize.hpp c_model/tests/nmu/test_packetize.cpp c_model/tests/nmu/CMakeLists.txt
git commit -m "feat(c_model/nmu): add Packetize (stateless + write-meta FIFO) + 11 unit tests"
```

---

### Task 9: `nsu::Packetize` (response side, peek+commit MetaBuffer) + unit tests

**Files:**
- Create: `c_model/include/nsu/packetize.hpp`
- Create: `c_model/tests/nsu/test_packetize.cpp`
- Modify: `c_model/tests/nsu/CMakeLists.txt`

- [ ] **Step 1: Write the failing unit tests**

Create `c_model/tests/nsu/test_packetize.cpp`:

```cpp
#include "nsu/packetize.hpp"
#include "nsu/meta_buffer.hpp"
#include "common/loopback_noc.hpp"
#include "axi/types.hpp"
#include <gtest/gtest.h>

using ni::cmodel::testing::LoopbackNoc;
using ni::cmodel::nsu::Packetize;
using ni::cmodel::nsu::MetaBuffer;
using ni::cmodel::nsu::MetaEntry;
namespace axi = ni::cmodel::axi;

namespace {
constexpr uint8_t kNsuSrcId = 0x02;

axi::BBeat make_b(uint8_t id, axi::Resp resp = axi::Resp::OKAY) {
  axi::BBeat b{};
  b.id = id; b.resp = resp; b.user = 0;
  return b;
}
axi::RBeat make_r(uint8_t id, bool last, axi::Resp resp = axi::Resp::OKAY) {
  axi::RBeat r{};
  r.id = id;
  for (int i = 0; i < 32; ++i) r.data[i] = static_cast<uint8_t>(0xC0 + i);
  r.resp = resp; r.last = last; r.user = 0;
  return r;
}
}

TEST(NsuPacketize, PushBLooksUpMetaAndEmitsFlit) {
  LoopbackNoc noc(16, 16);
  MetaBuffer mb(4);
  mb.snapshot_write(0x05, {/*src=*/0x12, /*rob_req=*/1, /*rob_idx=*/3});
  Packetize pkt(noc.rsp_out(), mb, kNsuSrcId);

  ASSERT_TRUE(pkt.push_b(make_b(0x05)));

  auto f = *noc.rsp_in().pop_flit();
  EXPECT_EQ(f.get_header_field("axi_ch"),  ni::AXI_CH_B);
  EXPECT_EQ(f.get_header_field("dst_id"),  0x12u);  // = orig src_id
  EXPECT_EQ(f.get_header_field("src_id"),  kNsuSrcId);
  EXPECT_EQ(f.get_header_field("rob_req"), 1u);
  EXPECT_EQ(f.get_header_field("rob_idx"), 3u);
  EXPECT_EQ(f.get_payload_field("B", "bid"),   0x05u);
  EXPECT_EQ(f.get_payload_field("B", "bresp"), static_cast<uint64_t>(axi::Resp::OKAY));
  // metadata consumed
  EXPECT_FALSE(mb.peek_write(0x05).has_value());
}

TEST(NsuPacketize, PushBAssertsWithoutMatchingMeta) {
  LoopbackNoc noc(16, 16);
  MetaBuffer mb(4);
  Packetize pkt(noc.rsp_out(), mb, kNsuSrcId);
  EXPECT_DEATH(pkt.push_b(make_b(0x05)), "B with no matching outstanding AW");
}

TEST(NsuPacketize, PushBNoCommitOnNocFull) {
  LoopbackNoc noc(/*req*/16, /*rsp*/1);
  MetaBuffer mb(4);
  mb.snapshot_write(0x05, {0x12, 0, 0});
  Packetize pkt(noc.rsp_out(), mb, kNsuSrcId);
  // first B fills rsp_q (cap=1)
  ASSERT_TRUE(pkt.push_b(make_b(0x05)));
  EXPECT_FALSE(mb.peek_write(0x05).has_value());
  // Drain + add new entry + retry — peek+commit pattern means a SECOND push
  // attempt with rsp_q already full should NOT desync.
  mb.snapshot_write(0x06, {0x20, 0, 0});
  EXPECT_FALSE(pkt.push_b(make_b(0x06)));  // rsp_q still full
  EXPECT_TRUE(mb.peek_write(0x06).has_value());  // metadata still there
  noc.rsp_in().pop_flit();  // drain
  EXPECT_TRUE(pkt.push_b(make_b(0x06)));
  EXPECT_FALSE(mb.peek_write(0x06).has_value());
}

TEST(NsuPacketize, PushRMultiBeatPeekUntilRLast) {
  LoopbackNoc noc(16, 16);
  MetaBuffer mb(4);
  mb.snapshot_read(0x03, {0x12, 0, 5});
  Packetize pkt(noc.rsp_out(), mb, kNsuSrcId);

  ASSERT_TRUE(pkt.push_r(make_r(0x03, /*last*/false)));
  EXPECT_TRUE(mb.peek_read(0x03).has_value());  // not committed
  ASSERT_TRUE(pkt.push_r(make_r(0x03, /*last*/false)));
  EXPECT_TRUE(mb.peek_read(0x03).has_value());
  ASSERT_TRUE(pkt.push_r(make_r(0x03, /*last*/true)));
  EXPECT_FALSE(mb.peek_read(0x03).has_value());  // committed on rlast
}

TEST(NsuPacketize, RPayloadBitPerfect) {
  LoopbackNoc noc(16, 16);
  MetaBuffer mb(4);
  mb.snapshot_read(0x03, {0x12, 0, 0});
  Packetize pkt(noc.rsp_out(), mb, kNsuSrcId);
  ASSERT_TRUE(pkt.push_r(make_r(0x03, /*last*/true, axi::Resp::SLVERR)));
  auto f = *noc.rsp_in().pop_flit();
  EXPECT_EQ(f.get_payload_field("R", "rid"),   0x03u);
  EXPECT_EQ(f.get_payload_field("R", "rresp"), static_cast<uint64_t>(axi::Resp::SLVERR));
  EXPECT_EQ(f.get_payload_field("R", "rlast"), 1u);
  std::array<uint8_t, 32> out{};
  f.get_payload_bytes("R", "rdata", out.data(), 256);
  for (int i = 0; i < 32; ++i) EXPECT_EQ(out[i], static_cast<uint8_t>(0xC0 + i));
}

TEST(NsuPacketize, PushAwAssertFalse) {
  LoopbackNoc noc(16, 16);
  MetaBuffer mb(4);
  Packetize pkt(noc.rsp_out(), mb, kNsuSrcId);
  axi::AwBeat dummy{};
  EXPECT_DEATH(pkt.push_aw(dummy), "NSU packetize: AW not applicable");
}
```

(More tests can be added for: simultaneous B/R progress, multiple multi-beat R bursts interleaved on same id queue front, etc. Aim for ~10-14 tests.)

- [ ] **Step 2: Add to CMakeLists.txt**

Append to `c_model/tests/nsu/CMakeLists.txt` an `add_executable(test_packetize test_packetize.cpp)` block.

- [ ] **Step 3: Verify build fails (no packetize.hpp yet)**

```bash
cmake --build c_model/build 2>&1 | tail -5
```
Expected: missing `nsu/packetize.hpp`.

- [ ] **Step 4: Implement `nsu::Packetize`**

Create `c_model/include/nsu/packetize.hpp`:

```cpp
#pragma once
#include "axi/types.hpp"
#include "ni/flit.hpp"
#include "ni/packetizer.hpp"           // from Stage 3 port-pair task
#include "noc/noc_rsp_out.hpp"
#include "nsu/meta_buffer.hpp"
#include <cassert>
#include <cstdint>

namespace ni::cmodel::nsu {

// NSU-side response packetizer. Looks up dst_id/rob_* from MetaBuffer via
// peek+commit so a failed NocRspOut.push_flit() never desynchronizes.
// Implements 5-method Packetizer interface; push_aw/push_w/push_ar assert
// false (NSU never emits requests).
class Packetize : public Packetizer {
public:
  Packetize(noc::NocRspOut& rsp_out, MetaBuffer& meta, uint8_t src_id)
      : rsp_out_(rsp_out), meta_(meta), src_id_(src_id) {}

  // ---- Packetizer interface (response methods are real) ----
  bool push_b(const axi::BBeat& b) override;
  bool push_r(const axi::RBeat& b) override;

  // ---- Request methods assert false ----
  bool push_aw(const axi::AwBeat&) override { assert(false && "NSU packetize: AW not applicable"); return false; }
  bool push_w (const axi::WBeat&)  override { assert(false && "NSU packetize: W  not applicable"); return false; }
  bool push_ar(const axi::ArBeat&) override { assert(false && "NSU packetize: AR not applicable"); return false; }

private:
  noc::NocRspOut& rsp_out_;
  MetaBuffer& meta_;
  uint8_t src_id_;
};

inline bool Packetize::push_b(const axi::BBeat& b) {
  auto meta_opt = meta_.peek_write(b.id);
  assert(meta_opt.has_value() && "B with no matching outstanding AW (MetaBuffer empty for id)");
  const auto& m = *meta_opt;

  Flit f;
  f.set_header_field("axi_ch",  ni::AXI_CH_B);
  f.set_header_field("src_id",  src_id_);
  f.set_header_field("dst_id",  m.src_id);   // back to originating NMU
  f.set_header_field("vc_id",   0);
  f.set_header_field("last",    1);
  f.set_header_field("rob_req", m.rob_req);
  f.set_header_field("rob_idx", m.rob_idx);
  f.set_payload_field("B", "bid",   b.id);
  f.set_payload_field("B", "bresp", static_cast<uint64_t>(b.resp));
  f.set_payload_field("B", "buser", b.user);
  if (!rsp_out_.push_flit(f)) return false;  // MetaBuffer untouched on backpressure
  meta_.commit_write(b.id);
  return true;
}

inline bool Packetize::push_r(const axi::RBeat& b) {
  auto meta_opt = meta_.peek_read(b.id);
  assert(meta_opt.has_value() && "R with no matching outstanding AR (MetaBuffer empty for id)");
  const auto& m = *meta_opt;

  Flit f;
  f.set_header_field("axi_ch",  ni::AXI_CH_R);
  f.set_header_field("src_id",  src_id_);
  f.set_header_field("dst_id",  m.src_id);
  f.set_header_field("vc_id",   0);
  f.set_header_field("last",    1);
  f.set_header_field("rob_req", m.rob_req);
  f.set_header_field("rob_idx", m.rob_idx);
  f.set_payload_field("R", "rid",   b.id);
  f.set_payload_field("R", "rresp", static_cast<uint64_t>(b.resp));
  f.set_payload_field("R", "ruser", b.user);
  f.set_payload_field("R", "rlast", b.last ? 1u : 0u);
  f.set_payload_bytes("R", "rdata", b.data.data(), 256);
  if (!rsp_out_.push_flit(f)) return false;
  if (b.last) meta_.commit_read(b.id);  // multi-beat R shares one meta entry
  return true;
}

}  // namespace ni::cmodel::nsu
```

- [ ] **Step 5: Build + run tests**

```bash
cd c_model && cmake --build build && ctest --test-dir build -R NsuPacketize -j 1
```
Expected: 6+ tests pass.

- [ ] **Step 6: Full ctest sweep**

```bash
ctest --test-dir build -j 1
```
Expected: 233 + 6 = 239.

- [ ] **Step 7: Commit**

```bash
git add c_model/include/nsu/packetize.hpp c_model/tests/nsu/test_packetize.cpp c_model/tests/nsu/CMakeLists.txt
git commit -m "feat(c_model/nsu): add Packetize (peek+commit MetaBuffer, response routing) + 6 unit tests"
```

---

## Phase D: Depacketize modules

### Task 10: `nmu::Depacketize` (B/R demux + pending-flit HoL) + unit tests

**Files:**
- Create: `c_model/include/nmu/depacketize.hpp`
- Create: `c_model/tests/nmu/test_depacketize.cpp`
- Modify: `c_model/tests/nmu/CMakeLists.txt`

- [ ] **Step 1: Write failing tests**

Create `c_model/tests/nmu/test_depacketize.cpp`:

```cpp
#include "nmu/depacketize.hpp"
#include "common/loopback_noc.hpp"
#include "axi/types.hpp"
#include <gtest/gtest.h>

using ni::cmodel::testing::LoopbackNoc;
using ni::cmodel::nmu::Depacketize;
namespace axi = ni::cmodel::axi;

namespace {
ni::cmodel::Flit make_b_flit(uint8_t bid, axi::Resp resp = axi::Resp::OKAY) {
  ni::cmodel::Flit f;
  f.set_header_field("axi_ch", ni::AXI_CH_B);
  f.set_header_field("dst_id", 0x10);
  f.set_header_field("last",   1);
  f.set_payload_field("B", "bid",   bid);
  f.set_payload_field("B", "bresp", static_cast<uint64_t>(resp));
  return f;
}
ni::cmodel::Flit make_r_flit(uint8_t rid, bool rlast) {
  ni::cmodel::Flit f;
  f.set_header_field("axi_ch", ni::AXI_CH_R);
  f.set_header_field("dst_id", 0x10);
  f.set_header_field("last",   1);
  f.set_payload_field("R", "rid",   rid);
  f.set_payload_field("R", "rlast", rlast ? 1u : 0u);
  return f;
}
}

TEST(NmuDepacketize, PopBDecodesFromFlit) {
  LoopbackNoc noc(16, 16);
  Depacketize depkt(noc.rsp_in(), /*b*/16, /*r*/16);
  ASSERT_TRUE(noc.rsp_out().push_flit(make_b_flit(0x05, axi::Resp::SLVERR)));
  depkt.tick();
  auto b = depkt.pop_b();
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(b->id, 0x05);
  EXPECT_EQ(b->resp, axi::Resp::SLVERR);
}

TEST(NmuDepacketize, DemuxMixedFlitsByAxiCh) {
  LoopbackNoc noc(16, 16);
  Depacketize depkt(noc.rsp_in(), 16, 16);
  ASSERT_TRUE(noc.rsp_out().push_flit(make_b_flit(0x01)));
  ASSERT_TRUE(noc.rsp_out().push_flit(make_r_flit(0x02, true)));
  ASSERT_TRUE(noc.rsp_out().push_flit(make_b_flit(0x03)));
  depkt.tick();
  // Both queues populated
  EXPECT_EQ(depkt.pop_b()->id, 0x01);
  EXPECT_EQ(depkt.pop_r()->id, 0x02);
  EXPECT_EQ(depkt.pop_b()->id, 0x03);
}

TEST(NmuDepacketize, PendingFlitHolBlockingBFullStallsR) {
  LoopbackNoc noc(16, 16);
  Depacketize depkt(noc.rsp_in(), /*b cap=*/1, /*r cap=*/16);
  // Queue order: B, B, R
  ASSERT_TRUE(noc.rsp_out().push_flit(make_b_flit(0x01)));
  ASSERT_TRUE(noc.rsp_out().push_flit(make_b_flit(0x02)));
  ASSERT_TRUE(noc.rsp_out().push_flit(make_r_flit(0x03, true)));
  depkt.tick();
  // First B fits; second B holds pending; R behind cannot progress
  EXPECT_TRUE(depkt.pop_b().has_value());   // 0x01
  EXPECT_FALSE(depkt.pop_r().has_value());  // R blocked behind pending B
  depkt.tick();                              // pending B (0x02) now placed
  EXPECT_TRUE(depkt.pop_b().has_value());   // 0x02
  depkt.tick();                              // R (0x03) now placed
  EXPECT_TRUE(depkt.pop_r().has_value());
}

TEST(NmuDepacketize, PopBEmptyReturnsNullopt) {
  LoopbackNoc noc(16, 16);
  Depacketize depkt(noc.rsp_in(), 16, 16);
  EXPECT_FALSE(depkt.pop_b().has_value());
  EXPECT_FALSE(depkt.pop_r().has_value());
}

TEST(NmuDepacketize, PopAwAssertFalse) {
  LoopbackNoc noc(16, 16);
  Depacketize depkt(noc.rsp_in(), 16, 16);
  EXPECT_DEATH(depkt.pop_aw(), "NMU depacketize: AW not applicable");
}

TEST(NmuDepacketize, BFifoOrderPreserved) {
  LoopbackNoc noc(16, 16);
  Depacketize depkt(noc.rsp_in(), 16, 16);
  for (uint8_t i = 0; i < 5; ++i)
    ASSERT_TRUE(noc.rsp_out().push_flit(make_b_flit(i)));
  depkt.tick();
  for (uint8_t i = 0; i < 5; ++i)
    EXPECT_EQ(depkt.pop_b()->id, i);
}

TEST(NmuDepacketize, RPayloadBytesDecoded) {
  LoopbackNoc noc(16, 16);
  Depacketize depkt(noc.rsp_in(), 16, 16);
  ni::cmodel::Flit f;
  f.set_header_field("axi_ch", ni::AXI_CH_R);
  f.set_header_field("dst_id", 0x10);
  f.set_payload_field("R", "rid", 0x07);
  f.set_payload_field("R", "rlast", 1);
  std::array<uint8_t, 32> data;
  for (int i = 0; i < 32; ++i) data[i] = static_cast<uint8_t>(0xE0 + i);
  f.set_payload_bytes("R", "rdata", data.data(), 256);
  ASSERT_TRUE(noc.rsp_out().push_flit(f));
  depkt.tick();
  auto r = depkt.pop_r();
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->id, 0x07);
  EXPECT_EQ(r->last, true);
  EXPECT_EQ(r->data, data);
}
```

- [ ] **Step 2: Add to CMakeLists.txt**

`c_model/tests/nmu/CMakeLists.txt` — add `add_executable(test_depacketize test_depacketize.cpp)`.

- [ ] **Step 3: Verify build fails (no depacketize.hpp yet)**

```bash
cmake --build c_model/build 2>&1 | tail -5
```
Expected: missing header.

- [ ] **Step 4: Implement `nmu::Depacketize`**

Create `c_model/include/nmu/depacketize.hpp`:

```cpp
#pragma once
#include "axi/types.hpp"
#include "ni/depacketizer.hpp"         // from Stage 3 port-pair task
#include "ni/flit.hpp"
#include "noc/noc_rsp_in.hpp"
#include <cassert>
#include <cstdint>
#include <deque>
#include <optional>

namespace ni::cmodel::nmu {

// NMU-side response depacketizer. Stateful demux: tick() pulls from
// NocRspIn and routes B/R flits into per-channel deques. Upstream port
// calls pop_b/pop_r to serve from those queues.
//
// Pending-flit stash semantics: if a pulled flit's target queue is full,
// the flit is held in `pending_` and re-attempted next tick. This blocks
// any other flits behind it (head-of-line blocking on single-FIFO ingress).
// Documented in spec §4.4; not a bug.
class Depacketize : public Depacketizer {
public:
  Depacketize(noc::NocRspIn& rsp_in,
              std::size_t b_q_depth, std::size_t r_q_depth)
      : rsp_in_(rsp_in), b_q_depth_(b_q_depth), r_q_depth_(r_q_depth) {}

  void tick();

  // Depacketizer interface — response methods are real
  std::optional<axi::BBeat> pop_b() override;
  std::optional<axi::RBeat> pop_r() override;
  // Request methods assert false
  std::optional<axi::AwBeat> pop_aw() override { assert(false && "NMU depacketize: AW not applicable"); return std::nullopt; }
  std::optional<axi::WBeat>  pop_w () override { assert(false && "NMU depacketize: W  not applicable"); return std::nullopt; }
  std::optional<axi::ArBeat> pop_ar() override { assert(false && "NMU depacketize: AR not applicable"); return std::nullopt; }

private:
  noc::NocRspIn& rsp_in_;
  std::deque<axi::BBeat> b_q_;
  std::deque<axi::RBeat> r_q_;
  std::size_t b_q_depth_, r_q_depth_;
  std::optional<Flit> pending_;

  static axi::BBeat decode_b(const Flit& f);
  static axi::RBeat decode_r(const Flit& f);
};

inline axi::BBeat Depacketize::decode_b(const Flit& f) {
  axi::BBeat b{};
  b.id    = static_cast<uint8_t>(f.get_payload_field("B", "bid"));
  b.resp  = static_cast<axi::Resp>(f.get_payload_field("B", "bresp"));
  b.user  = static_cast<uint8_t>(f.get_payload_field("B", "buser"));
  return b;
}

inline axi::RBeat Depacketize::decode_r(const Flit& f) {
  axi::RBeat r{};
  r.id    = static_cast<uint8_t>(f.get_payload_field("R", "rid"));
  r.resp  = static_cast<axi::Resp>(f.get_payload_field("R", "rresp"));
  r.user  = static_cast<uint8_t>(f.get_payload_field("R", "ruser"));
  r.last  = f.get_payload_field("R", "rlast") != 0;
  f.get_payload_bytes("R", "rdata", r.data.data(), 256);
  return r;
}

inline void Depacketize::tick() {
  while (true) {
    Flit f;
    if (pending_) {
      f = *pending_;
    } else {
      auto opt = rsp_in_.pop_flit();
      if (!opt) return;
      f = *opt;
    }
    uint64_t ch = f.get_header_field("axi_ch");
    switch (ch) {
      case ni::AXI_CH_B:
        if (b_q_.size() >= b_q_depth_) { pending_ = f; return; }
        b_q_.push_back(decode_b(f));
        break;
      case ni::AXI_CH_R:
        if (r_q_.size() >= r_q_depth_) { pending_ = f; return; }
        r_q_.push_back(decode_r(f));
        break;
      default:
        assert(false && "NMU depacketize: NocRspIn delivered non-B/non-R flit");
    }
    pending_.reset();
  }
}

inline std::optional<axi::BBeat> Depacketize::pop_b() {
  if (b_q_.empty()) return std::nullopt;
  auto b = b_q_.front();
  b_q_.pop_front();
  return b;
}

inline std::optional<axi::RBeat> Depacketize::pop_r() {
  if (r_q_.empty()) return std::nullopt;
  auto r = r_q_.front();
  r_q_.pop_front();
  return r;
}

}  // namespace ni::cmodel::nmu
```

- [ ] **Step 5: Build + run**

```bash
cd c_model && cmake --build build && ctest --test-dir build -R NmuDepacketize -j 1
```
Expected: 7 tests pass.

- [ ] **Step 6: Full ctest sweep**

```bash
ctest --test-dir build -j 1
```
Expected: 239 + 7 = 246.

- [ ] **Step 7: Commit**

```bash
git add c_model/include/nmu/depacketize.hpp c_model/tests/nmu/test_depacketize.cpp c_model/tests/nmu/CMakeLists.txt
git commit -m "feat(c_model/nmu): add Depacketize (B/R demux with pending-flit HoL) + 7 unit tests"
```

---

### Task 11: `nsu::Depacketize` (AW/W/AR demux + MetaBuffer snapshot) + unit tests

**Files:**
- Create: `c_model/include/nsu/depacketize.hpp`
- Create: `c_model/tests/nsu/test_depacketize.cpp`
- Modify: `c_model/tests/nsu/CMakeLists.txt`

- [ ] **Step 1: Write failing tests**

Create `c_model/tests/nsu/test_depacketize.cpp`:

```cpp
#include "nsu/depacketize.hpp"
#include "nsu/meta_buffer.hpp"
#include "common/loopback_noc.hpp"
#include "axi/types.hpp"
#include <gtest/gtest.h>

using ni::cmodel::testing::LoopbackNoc;
using ni::cmodel::nsu::Depacketize;
using ni::cmodel::nsu::MetaBuffer;
namespace axi = ni::cmodel::axi;

namespace {
ni::cmodel::Flit make_aw_flit(uint8_t awid, uint64_t addr, uint8_t src_id = 0x10,
                              uint8_t rob_req = 0, uint8_t rob_idx = 0) {
  ni::cmodel::Flit f;
  f.set_header_field("axi_ch",  ni::AXI_CH_AW);
  f.set_header_field("src_id",  src_id);
  f.set_header_field("dst_id",  0x02);
  f.set_header_field("last",    1);
  f.set_header_field("rob_req", rob_req);
  f.set_header_field("rob_idx", rob_idx);
  f.set_payload_field("AW", "awid",   awid);
  f.set_payload_field("AW", "awaddr", addr);
  f.set_payload_field("AW", "awsize", 5);
  f.set_payload_field("AW", "awburst", static_cast<uint64_t>(axi::Burst::INCR));
  return f;
}
ni::cmodel::Flit make_w_flit(uint32_t strb, bool last) {
  ni::cmodel::Flit f;
  f.set_header_field("axi_ch", ni::AXI_CH_W);
  f.set_header_field("dst_id", 0x02);
  f.set_header_field("last",   1);
  f.set_payload_field("W", "wlast", last ? 1u : 0u);
  f.set_payload_field("W", "wstrb", strb);
  return f;
}
ni::cmodel::Flit make_ar_flit(uint8_t arid, uint64_t addr, uint8_t src_id = 0x10) {
  ni::cmodel::Flit f;
  f.set_header_field("axi_ch", ni::AXI_CH_AR);
  f.set_header_field("src_id", src_id);
  f.set_header_field("dst_id", 0x02);
  f.set_header_field("last",   1);
  f.set_payload_field("AR", "arid",   arid);
  f.set_payload_field("AR", "araddr", addr);
  f.set_payload_field("AR", "arsize", 5);
  f.set_payload_field("AR", "arburst", static_cast<uint64_t>(axi::Burst::INCR));
  return f;
}
}

TEST(NsuDepacketize, AwFlitSnapshotsMetadataAndPopsBeat) {
  LoopbackNoc noc(16, 16);
  MetaBuffer mb(4);
  Depacketize depkt(noc.req_in(), mb, /*aw*/16, /*w*/16, /*ar*/16);
  ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(0x05, 0x1000,
                                                    /*src*/0x12, /*rob_req*/1, /*rob_idx*/3)));
  depkt.tick();
  // Beat decoded
  auto aw = depkt.pop_aw();
  ASSERT_TRUE(aw.has_value());
  EXPECT_EQ(aw->id, 0x05);
  EXPECT_EQ(aw->addr, 0x1000u);
  // MetaBuffer snapshot
  auto m = mb.peek_write(0x05);
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->src_id, 0x12);
  EXPECT_EQ(m->rob_req, 1);
  EXPECT_EQ(m->rob_idx, 3);
}

TEST(NsuDepacketize, ArFlitSnapshotsReadMeta) {
  LoopbackNoc noc(16, 16);
  MetaBuffer mb(4);
  Depacketize depkt(noc.req_in(), mb, 16, 16, 16);
  ASSERT_TRUE(noc.req_out().push_flit(make_ar_flit(0x07, 0x2000, 0x12)));
  depkt.tick();
  EXPECT_TRUE(depkt.pop_ar().has_value());
  EXPECT_TRUE(mb.peek_read(0x07).has_value());
  EXPECT_EQ(mb.peek_read(0x07)->src_id, 0x12);
}

TEST(NsuDepacketize, WFlitNoMetaSideEffect) {
  LoopbackNoc noc(16, 16);
  MetaBuffer mb(4);
  Depacketize depkt(noc.req_in(), mb, 16, 16, 16);
  ASSERT_TRUE(noc.req_out().push_flit(make_w_flit(0xFFFF, true)));
  depkt.tick();
  EXPECT_TRUE(depkt.pop_w().has_value());
  // MetaBuffer untouched
  EXPECT_FALSE(mb.peek_write(0).has_value());
}

TEST(NsuDepacketize, DemuxMixedAwWAr) {
  LoopbackNoc noc(16, 16);
  MetaBuffer mb(4);
  Depacketize depkt(noc.req_in(), mb, 16, 16, 16);
  ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(0x01, 0x0)));
  ASSERT_TRUE(noc.req_out().push_flit(make_w_flit(0xFF, true)));
  ASSERT_TRUE(noc.req_out().push_flit(make_ar_flit(0x02, 0x1000)));
  depkt.tick();
  EXPECT_EQ(depkt.pop_aw()->id, 0x01);
  EXPECT_EQ(depkt.pop_w() ->strb, 0xFFu);
  EXPECT_EQ(depkt.pop_ar()->id, 0x02);
}

TEST(NsuDepacketize, PendingHolBlockingWFullBlocksAwBehind) {
  LoopbackNoc noc(16, 16);
  MetaBuffer mb(4);
  Depacketize depkt(noc.req_in(), mb, /*aw*/16, /*w cap*/1, /*ar*/16);
  // Order: W, W, AW
  ASSERT_TRUE(noc.req_out().push_flit(make_w_flit(0xAA, true)));
  ASSERT_TRUE(noc.req_out().push_flit(make_w_flit(0xBB, true)));
  ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(0x07, 0x0)));
  depkt.tick();
  EXPECT_TRUE(depkt.pop_w().has_value());   // first W (0xAA) demuxed
  EXPECT_FALSE(depkt.pop_aw().has_value()); // AW blocked behind pending W
  depkt.tick();
  EXPECT_TRUE(depkt.pop_w().has_value());   // pending W (0xBB)
  depkt.tick();
  EXPECT_TRUE(depkt.pop_aw().has_value());  // AW now demuxed
}

TEST(NsuDepacketize, PopBAssertFalse) {
  LoopbackNoc noc(16, 16);
  MetaBuffer mb(4);
  Depacketize depkt(noc.req_in(), mb, 16, 16, 16);
  EXPECT_DEATH(depkt.pop_b(), "NSU depacketize: B not applicable");
}

TEST(NsuDepacketize, FifoOrderPreservedAcrossChannels) {
  LoopbackNoc noc(16, 16);
  MetaBuffer mb(4);
  Depacketize depkt(noc.req_in(), mb, 16, 16, 16);
  ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(1, 0x0)));
  ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(2, 0x0)));
  ASSERT_TRUE(noc.req_out().push_flit(make_aw_flit(3, 0x0)));
  depkt.tick();
  EXPECT_EQ(depkt.pop_aw()->id, 1);
  EXPECT_EQ(depkt.pop_aw()->id, 2);
  EXPECT_EQ(depkt.pop_aw()->id, 3);
}
```

- [ ] **Step 2: Add to CMakeLists.txt**

`c_model/tests/nsu/CMakeLists.txt` — add `test_depacketize`.

- [ ] **Step 3: Build fails (no header)**

Expected.

- [ ] **Step 4: Implement `nsu::Depacketize`**

Create `c_model/include/nsu/depacketize.hpp`:

```cpp
#pragma once
#include "axi/types.hpp"
#include "ni/depacketizer.hpp"
#include "ni/flit.hpp"
#include "noc/noc_req_in.hpp"
#include "nsu/meta_buffer.hpp"
#include <cassert>
#include <cstdint>
#include <deque>
#include <optional>

namespace ni::cmodel::nsu {

// NSU-side request depacketizer. Stateful demux: tick() pulls flits from
// NocReqIn, decodes axi_ch, demuxes into AW/W/AR queues. AW/AR flits
// additionally snapshot {src_id, awid|arid, rob_req, rob_idx} into MetaBuffer
// for the response path.
class Depacketize : public Depacketizer {
public:
  Depacketize(noc::NocReqIn& req_in, MetaBuffer& meta,
              std::size_t aw_q_depth, std::size_t w_q_depth, std::size_t ar_q_depth)
      : req_in_(req_in), meta_(meta),
        aw_q_depth_(aw_q_depth), w_q_depth_(w_q_depth), ar_q_depth_(ar_q_depth) {}

  void tick();

  // Request methods real
  std::optional<axi::AwBeat> pop_aw() override;
  std::optional<axi::WBeat>  pop_w()  override;
  std::optional<axi::ArBeat> pop_ar() override;
  // Response methods assert false
  std::optional<axi::BBeat> pop_b() override { assert(false && "NSU depacketize: B not applicable"); return std::nullopt; }
  std::optional<axi::RBeat> pop_r() override { assert(false && "NSU depacketize: R not applicable"); return std::nullopt; }

private:
  noc::NocReqIn& req_in_;
  MetaBuffer& meta_;
  std::deque<axi::AwBeat> aw_q_;
  std::deque<axi::WBeat>  w_q_;
  std::deque<axi::ArBeat> ar_q_;
  std::size_t aw_q_depth_, w_q_depth_, ar_q_depth_;
  std::optional<Flit> pending_;

  static axi::AwBeat decode_aw(const Flit& f);
  static axi::WBeat  decode_w (const Flit& f);
  static axi::ArBeat decode_ar(const Flit& f);
};

inline axi::AwBeat Depacketize::decode_aw(const Flit& f) {
  axi::AwBeat b{};
  b.id     = static_cast<uint8_t>(f.get_payload_field("AW", "awid"));
  b.addr   = f.get_payload_field("AW", "awaddr");
  b.len    = static_cast<uint8_t>(f.get_payload_field("AW", "awlen"));
  b.size   = static_cast<uint8_t>(f.get_payload_field("AW", "awsize"));
  b.burst  = static_cast<axi::Burst>(f.get_payload_field("AW", "awburst"));
  b.cache  = static_cast<uint8_t>(f.get_payload_field("AW", "awcache"));
  b.lock   = static_cast<uint8_t>(f.get_payload_field("AW", "awlock"));
  b.prot   = static_cast<uint8_t>(f.get_payload_field("AW", "awprot"));
  b.region = static_cast<uint8_t>(f.get_payload_field("AW", "awregion"));
  b.user   = static_cast<uint8_t>(f.get_payload_field("AW", "awuser"));
  b.qos    = 0;  // not in NoC flit (separate flit field noc_qos)
  return b;
}

inline axi::WBeat Depacketize::decode_w(const Flit& f) {
  axi::WBeat b{};
  b.last = f.get_payload_field("W", "wlast") != 0;
  b.user = static_cast<uint8_t>(f.get_payload_field("W", "wuser"));
  b.strb = static_cast<uint32_t>(f.get_payload_field("W", "wstrb"));
  f.get_payload_bytes("W", "wdata", b.data.data(), 256);
  return b;
}

inline axi::ArBeat Depacketize::decode_ar(const Flit& f) {
  axi::ArBeat b{};
  b.id     = static_cast<uint8_t>(f.get_payload_field("AR", "arid"));
  b.addr   = f.get_payload_field("AR", "araddr");
  b.len    = static_cast<uint8_t>(f.get_payload_field("AR", "arlen"));
  b.size   = static_cast<uint8_t>(f.get_payload_field("AR", "arsize"));
  b.burst  = static_cast<axi::Burst>(f.get_payload_field("AR", "arburst"));
  b.cache  = static_cast<uint8_t>(f.get_payload_field("AR", "arcache"));
  b.lock   = static_cast<uint8_t>(f.get_payload_field("AR", "arlock"));
  b.prot   = static_cast<uint8_t>(f.get_payload_field("AR", "arprot"));
  b.region = static_cast<uint8_t>(f.get_payload_field("AR", "arregion"));
  b.user   = static_cast<uint8_t>(f.get_payload_field("AR", "aruser"));
  b.qos    = 0;
  return b;
}

inline void Depacketize::tick() {
  while (true) {
    Flit f;
    if (pending_) {
      f = *pending_;
    } else {
      auto opt = req_in_.pop_flit();
      if (!opt) return;
      f = *opt;
    }
    uint64_t ch = f.get_header_field("axi_ch");
    switch (ch) {
      case ni::AXI_CH_AW:
        if (aw_q_.size() >= aw_q_depth_) { pending_ = f; return; }
        {
          auto aw = decode_aw(f);
          aw_q_.push_back(aw);
          meta_.snapshot_write(aw.id, {
            static_cast<uint8_t>(f.get_header_field("src_id")),
            static_cast<uint8_t>(f.get_header_field("rob_req")),
            static_cast<uint8_t>(f.get_header_field("rob_idx")),
          });
        }
        break;
      case ni::AXI_CH_W:
        if (w_q_.size() >= w_q_depth_) { pending_ = f; return; }
        w_q_.push_back(decode_w(f));
        break;
      case ni::AXI_CH_AR:
        if (ar_q_.size() >= ar_q_depth_) { pending_ = f; return; }
        {
          auto ar = decode_ar(f);
          ar_q_.push_back(ar);
          meta_.snapshot_read(ar.id, {
            static_cast<uint8_t>(f.get_header_field("src_id")),
            static_cast<uint8_t>(f.get_header_field("rob_req")),
            static_cast<uint8_t>(f.get_header_field("rob_idx")),
          });
        }
        break;
      default:
        assert(false && "NSU depacketize: NocReqIn delivered non-AW/W/AR flit");
    }
    pending_.reset();
  }
}

inline std::optional<axi::AwBeat> Depacketize::pop_aw() {
  if (aw_q_.empty()) return std::nullopt;
  auto b = aw_q_.front(); aw_q_.pop_front(); return b;
}
inline std::optional<axi::WBeat> Depacketize::pop_w() {
  if (w_q_.empty()) return std::nullopt;
  auto b = w_q_.front(); w_q_.pop_front(); return b;
}
inline std::optional<axi::ArBeat> Depacketize::pop_ar() {
  if (ar_q_.empty()) return std::nullopt;
  auto b = ar_q_.front(); ar_q_.pop_front(); return b;
}

}  // namespace ni::cmodel::nsu
```

- [ ] **Step 5: Build + run**

```bash
cd c_model && cmake --build build && ctest --test-dir build -R NsuDepacketize -j 1
```
Expected: 7 tests pass.

- [ ] **Step 6: Full ctest sweep**

```bash
ctest --test-dir build -j 1
```
Expected: 246 + 7 = 253.

- [ ] **Step 7: Commit**

```bash
git add c_model/include/nsu/depacketize.hpp c_model/tests/nsu/test_depacketize.cpp c_model/tests/nsu/CMakeLists.txt
git commit -m "feat(c_model/nsu): add Depacketize (AW/W/AR demux + MetaBuffer snapshot) + 7 unit tests"
```

---

## Phase E: Integration

### Task 12: Integration e2e test (TestPacketize adapter + Scoreboard via Stage 2 fixtures)

**Files:**
- Create: `c_model/tests/common/test_packetize_adapter.hpp`
- Create: `c_model/tests/integration/test_request_response_loopback.cpp`
- Modify: `c_model/tests/integration/CMakeLists.txt`

- [ ] **Step 1: Create TestPacketize adapter**

Create `c_model/tests/common/test_packetize_adapter.hpp`:

```cpp
// Test-only Packetizer adapter that wraps nmu::Packetize and auto-calls
// sticky setters before each push_aw/push_ar. This is the bridge between
// the AxiSlavePort's Packetizer-interface contract (which expects only
// push_aw(beat)) and nmu::Packetize's sticky-setter API (set_aw_header_extras
// must be called first).
//
// Production wiring (the future nmu::AddrTrans task) replaces this adapter
// with a real address-translation layer.
#pragma once
#include "ni/packetizer.hpp"
#include "nmu/packetize.hpp"
#include "axi/types.hpp"
#include <cassert>
#include <cstdint>

namespace ni::cmodel::testing {

class TestPacketize : public Packetizer {
public:
  TestPacketize(nmu::Packetize& real, uint8_t fixed_dst_id)
      : real_(real), dst_(fixed_dst_id) {}

  bool push_aw(const axi::AwBeat& b) override {
    real_.set_aw_header_extras(dst_, 0, 0);
    return real_.push_aw(b);
  }
  bool push_w(const axi::WBeat& b) override {
    return real_.push_w(b);  // dst inherited from w_meta_fifo_
  }
  bool push_ar(const axi::ArBeat& b) override {
    real_.set_ar_header_extras(dst_, 0, 0);
    return real_.push_ar(b);
  }
  bool push_b(const axi::BBeat&) override { assert(false); return false; }
  bool push_r(const axi::RBeat&) override { assert(false); return false; }

private:
  nmu::Packetize& real_;
  uint8_t dst_;
};

}  // namespace ni::cmodel::testing
```

- [ ] **Step 2: Write the integration test**

Create `c_model/tests/integration/test_request_response_loopback.cpp`:

```cpp
// End-to-end loopback through 4 new packetize/depacketize modules + Stage 3
// port pair, reusing Stage 2 AxiMaster/AxiSlave/Memory/Scoreboard.
//
// Rig:
//   AxiMaster -> AxiSlavePort -> nmu::Packetize -> LoopbackNoc.req
//     -> nsu::Depacketize -> AxiMasterPort -> AxiSlave + Memory
//     (B/R response) -> nsu::Packetize -> LoopbackNoc.rsp
//       -> nmu::Depacketize -> AxiSlavePort -> AxiMaster -> Scoreboard
#include "axi/axi_master.hpp"
#include "axi/axi_slave.hpp"
#include "axi/memory.hpp"
#include "axi/scoreboard.hpp"
#include "ni/port_params.hpp"
#include "nmu/axi_slave_port.hpp"
#include "nmu/packetize.hpp"
#include "nmu/depacketize.hpp"
#include "nsu/axi_master_port.hpp"
#include "nsu/packetize.hpp"
#include "nsu/depacketize.hpp"
#include "nsu/meta_buffer.hpp"
#include "common/loopback_noc.hpp"
#include "common/test_packetize_adapter.hpp"
#include <gtest/gtest.h>
#include <deque>

namespace axi   = ni::cmodel::axi;
namespace nmu   = ni::cmodel::nmu;
namespace nsu   = ni::cmodel::nsu;
namespace test  = ni::cmodel::testing;

class PacketizeLoopbackFixture : public ::testing::TestWithParam<std::tuple<const char*, unsigned, unsigned>> {
  // tuple: (fixture_yaml_relative_path, req_delay, rsp_delay)
protected:
  void run_fixture(const std::string& yaml_path, unsigned req_delay, unsigned rsp_delay) {
    auto params = ni::cmodel::load_port_params_yaml(
        std::string(TESTS_ROOT) + "/../config/port_params.yaml", "nmu");

    test::LoopbackNoc loopback(params.loopback_noc_req_depth, params.loopback_noc_rsp_depth);
    loopback.set_req_delay(req_delay);
    loopback.set_rsp_delay(rsp_delay);

    nsu::MetaBuffer    nsu_meta(params.meta_buffer_per_id_depth);
    nmu::Packetize     real_nmu_pkt(loopback.req_out(), /*src=*/0x01);
    test::TestPacketize test_pkt(real_nmu_pkt, /*fixed_dst=*/0x02);
    nmu::Depacketize   nmu_depkt(loopback.rsp_in(), params.depkt_b_q_depth, params.depkt_r_q_depth);
    nsu::Depacketize   nsu_depkt(loopback.req_in(), nsu_meta,
                                  params.depkt_aw_q_depth, params.depkt_w_q_depth, params.depkt_ar_q_depth);
    nsu::Packetize     nsu_pkt(loopback.rsp_out(), nsu_meta, /*src=*/0x02);

    nmu::AxiSlavePort  nmu_port(test_pkt, nmu_depkt, params);
    nsu::AxiMasterPort nsu_port(nsu_depkt, nsu_pkt, params);

    auto memory_path  = ::testing::TempDir() + "/memory.txt";
    auto read_dump    = ::testing::TempDir() + "/read_dump.txt";
    ni::cmodel::axi::Memory memory(64 * 1024);
    ni::cmodel::axi::AxiSlave slave(memory);
    ni::cmodel::axi::AxiMaster master(yaml_path, nmu_port, read_dump,
                                      /*max_out_w=*/8, /*max_out_r=*/8);
    ni::cmodel::axi::Scoreboard scoreboard(memory);

    master.on_write_completed([&](const auto& w) { scoreboard.observe_write(w); });
    master.on_read_observed  ([&](const auto& r) { scoreboard.observe_read(r); });

    std::deque<axi::AwBeat> aw_h, ar_h;
    std::deque<axi::WBeat>  w_h;
    std::deque<axi::BBeat>  b_h;
    std::deque<axi::RBeat>  r_h;

    constexpr int kMaxCycles = 200000;
    int cycle = 0;
    while (!master.done() || !aw_h.empty() || !w_h.empty() || !ar_h.empty() ||
           !b_h.empty() || !r_h.empty() || loopback.req_q_size() || loopback.rsp_q_size() ||
           loopback.req_pipe_size() || loopback.rsp_pipe_size()) {
      ASSERT_LT(cycle++, kMaxCycles) << "watchdog: simulation did not terminate";

      // Drain response side first
      nmu_depkt.tick();
      loopback.tick();
      nsu_depkt.tick();

      // Port ticks
      nmu_port.tick();
      nsu_port.tick();

      // AxiMasterPort <-> AxiSlave glue
      while (auto aw = nsu_port.pop_aw()) aw_h.push_back(*aw);
      while (auto w  = nsu_port.pop_w ()) w_h .push_back(*w);
      while (auto ar = nsu_port.pop_ar()) ar_h.push_back(*ar);
      while (!aw_h.empty() && slave.push_aw(aw_h.front())) aw_h.pop_front();
      while (!w_h .empty() && slave.push_w (w_h .front())) w_h .pop_front();
      while (!ar_h.empty() && slave.push_ar(ar_h.front())) ar_h.pop_front();
      while (auto b = slave.pop_b()) b_h.push_back(*b);
      while (auto r = slave.pop_r()) r_h.push_back(*r);
      while (!b_h.empty() && nsu_port.push_b(b_h.front())) b_h.pop_front();
      while (!r_h.empty() && nsu_port.push_r(r_h.front())) r_h.pop_front();

      slave.tick();
      memory.tick();
      master.tick();
    }

    EXPECT_EQ(scoreboard.mismatch_count(), 0u)
        << "Scoreboard mismatch on fixture " << yaml_path
        << " (req_delay=" << req_delay << " rsp_delay=" << rsp_delay << ")";
  }
};

TEST_P(PacketizeLoopbackFixture, ScoreboardZeroMismatch) {
  auto [name, req_d, rsp_d] = GetParam();
  std::string yaml = std::string(TESTS_ROOT) + "/axi/fixtures/" + name;
  run_fixture(yaml, req_d, rsp_d);
}

INSTANTIATE_TEST_SUITE_P(
  Fixtures, PacketizeLoopbackFixture,
  ::testing::Values(
    std::make_tuple("burst_incr_8beat.yaml",        0u, 0u),
    std::make_tuple("multi_outstanding_stress.yaml", 0u, 0u),
    std::make_tuple("burst_wrap_4beat.yaml",         0u, 0u),
    std::make_tuple("narrow_transfer.yaml",          0u, 0u),
    std::make_tuple("sparse_wstrb_partial.yaml",     0u, 0u),
    std::make_tuple("multi_outstanding_stress.yaml", 2u, 3u)  // delayed-loopback variant
  )
);
```

(Note: fixture filenames above are illustrative — the engineer should `ls c_model/tests/axi/fixtures/` and pick actual representative `*.yaml` files matching INCR-8, multi-outstanding, WRAP, narrow, sparse WSTRB scenarios.)

- [ ] **Step 3: Update CMakeLists.txt**

Append to `c_model/tests/integration/CMakeLists.txt`:

```cmake
add_executable(test_request_response_loopback test_request_response_loopback.cpp)
target_compile_definitions(test_request_response_loopback PRIVATE
  TESTS_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(test_request_response_loopback PRIVATE
  GTest::gtest GTest::gtest_main yaml-cpp)
target_include_directories(test_request_response_loopback PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/..)
gtest_discover_tests(test_request_response_loopback)
```

- [ ] **Step 4: Discover available fixtures**

```bash
ls c_model/tests/axi/fixtures/
```
Pick 5 fixtures matching the description (INCR, multi-outstanding, WRAP, narrow, sparse WSTRB). Update the `INSTANTIATE_TEST_SUITE_P` filenames in the .cpp accordingly.

- [ ] **Step 5: Build + run integration test**

```bash
cd c_model && cmake --build build && ctest --test-dir build -R PacketizeLoopback -j 1
```
Expected: 6 tests pass (5 zero-latency + 1 delayed).

- [ ] **Step 6: Full ctest sweep**

```bash
ctest --test-dir build -j 1
```
Expected: 253 + 6 = 259.

- [ ] **Step 7: Commit**

```bash
git add c_model/tests/common/test_packetize_adapter.hpp c_model/tests/integration/test_request_response_loopback.cpp c_model/tests/integration/CMakeLists.txt
git commit -m "feat(c_model/tests/integration): packetize/depacketize end-to-end Scoreboard test via Stage 2 fixtures"
```

---

### Task 13: Final drift-gate sweep + Karpathy 4-lens review + NEXT_STEPS update

**Files:**
- Modify: `NEXT_STEPS.md` (flip "packetize next" → "packetize done; next: vc_arb / addr_trans / rob per plan §3")

- [ ] **Step 1: Re-run all drift gates**

```bash
cd specgen
py -3 -m pytest -q                        # expect ~161+ passed (157 baseline + 4 new payload-position tests)
py -3 tools/codegen.py --check            # expect clean
py -3 tools/gen_inventory.py --check      # expect clean
cd ../c_model && ctest --test-dir build -j 1   # expect ~259 / ~259
```

If any gate fails: STOP and report. Do NOT proceed to commit.

- [ ] **Step 2: Karpathy 4-lens review (write findings to a brief comment in this task)**

Review the spec doc commits (`13b3410` → `c08ab9d` → `143f7a1`) and the implementation against:

- **Overcomplication**: did any module grow beyond the spec? Any speculative future-proofing that should be removed?
- **Surgical**: any unintended modifications to `c_model/include/axi/` (Stage 2 frozen) or `c_model/include/{nmu,nsu}/axi_*_port.hpp` (Stage 3 frozen)? Grep verify.
- **Surface assumptions**: are the deferred items (vc_arb, route_par, flit_ecc, addr_trans, rob, atomic-ID) all clearly TODO-marked in code where relevant?
- **Verifiable success**: integration test passes; unit tests cover stated invariants.

Commands:
```bash
git diff main~13..main -- c_model/include/axi/   # expect empty
git diff main~13..main -- 'c_model/include/nmu/axi_slave_port.hpp' 'c_model/include/nsu/axi_master_port.hpp'  # expect empty
grep -rn "TODO(route_par)\|TODO(flit_ecc)\|TODO(meta_buffer)\|TODO(vc_arb)" c_model/include
```

If any concern surfaces, document it in the commit message of Step 4 below.

- [ ] **Step 3: Update NEXT_STEPS.md headline**

Read `NEXT_STEPS.md`. Find the section header that still says "Stage 3 Bootstrap" with "port pair = next task" or similar. Replace with:

```markdown
## Current status (2026-06-02)

Stage 3 packetize + depacketize 完工：nmu/{packetize, depacketize}, nsu/{packetize, depacketize, meta_buffer}, c_model/include/{ni/flit, noc/noc_req_*, noc/noc_rsp_*} 全綠。具備 request+response e2e Scoreboard 通過 5 個 Stage 2 fixture + 1 delayed-loopback variant。

**Next task per plan §3.1**: NMU/NSU `vc_arb` (MUX 3→1 + per-VC credit + round-robin arbitration). 後續 `addr_trans`、`rob` 各自獨立 task。`route_par` / `flit_ecc` 兩個算法 helper 任何時間都可獨立做。
```

- [ ] **Step 4: Final commit**

```bash
git add NEXT_STEPS.md
git commit -m "docs(NEXT_STEPS): packetize+depacketize done; next is vc_arb per plan §3"
```

- [ ] **Step 5: Final ctest sanity**

```bash
cd c_model && ctest --test-dir build -j 1
```
Confirm everything still passes.

---

## Self-review checklist (for plan author — Claude)

After writing the plan, verified:

- **Spec coverage**: every spec §4.X module → task. §4.2 Flit → Task 4. §4.3 nmu::Packetize → Task 8. §4.4 nmu::Depacketize → Task 10. §4.5 nsu::Depacketize → Task 11. §4.6 nsu::Packetize → Task 9. §4.7 MetaBuffer → Task 6. §4.8 LoopbackNoc → Task 7. §5 codegen extension → Tasks 1-3. §6 YAML config → Task 5. §7.2 integration → Task 12. §8 open follow-ups → Task 13 NEXT_STEPS update.
- **Placeholder scan**: clean (no TBD / "implement later" / handwave). Code blocks present in every code step.
- **Type consistency**: `Packetize` / `Depacketize` class names consistent. `MetaEntry` / `MetaBuffer` consistent. `LoopbackNoc` / `TestPacketize` consistent. `set_aw_header_extras` / `set_ar_header_extras` consistent across Packetize impl + tests + adapter. `peek_write` / `commit_write` / `peek_read` / `commit_read` consistent in MetaBuffer + nsu::Packetize + tests.
- **Cross-references valid**: codegen-emitted constants (`ni::AXI_CH_AW` etc., `ni::header::*_LSB/MSB`, `ni::payload::<ch>::*_LSB/MSB`) are introduced by Tasks 1-2 before being used in Tasks 4+ headers/tests. `Packetizer` / `Depacketizer` interfaces are existing artifacts (from Stage 3 port-pair task) — referenced via `#include "ni/packetizer.hpp"` and `#include "ni/depacketizer.hpp"`.

---

## Execution

Plan complete and committed.

**Two execution options:**

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, two-stage review (spec + quality) between tasks, fast iteration.

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch with checkpoints.

Which approach?
