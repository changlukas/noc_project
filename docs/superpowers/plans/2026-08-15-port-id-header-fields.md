# Round 2: the two port_id header fields

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `dst_port_id` and `src_port_id` (2 b each) to the flit header and carry them from the
NMU that issues a request to the NSU that serves it and back, with every port `00` and every
behaviour unchanged.

**Architecture:** The header grows 44 b to 48 b, so every network's flit width grows by 4 b.
The NMU stamps `src_port_id` from its own config and `dst_port_id` from the SAM entry it resolved.
The NSU keeps the requester's port in its MetaBuffer and stamps it back onto the response.
Both halves assert that an arriving flit names their own port, so the round trip is checked on
every transaction rather than by one unit test.

**Tech Stack:** C++17, specgen codegen (JSON -> C++ + SystemVerilog), CMake/GoogleTest, Verilator co-sim.

**Spec:** `docs/superpowers/specs/2026-08-15-peripheral-addressing-design.md`

**Branch:** `feat/port-id-header-fields`, off `main`.

## Global Constraints

- `HEADER_TOTAL_WIDTH` is 48. `dst_port_id` at bits 44-45, `src_port_id` at 46-47.
  No existing header field's LSB moves.
- Every port value is `00` in every shipped topology. No behaviour changes.
- Never hand-edit anything under `specgen/generated/cpp/` or `specgen/generated/sv/`
  or `specgen/tests/golden/`. Regenerate with `specgen/tools/codegen.py`.
- `codegen.py --check` must exit 0 at the end of every task.
- Run `clang-format -i` on every `.hpp` / `.cpp` touched.
- Commit message format `type(scope): description`, English.
- Build and test on WSL, from `/mnt/e/05_NoC/noc_project`, with `BUILD_ROOT=$HOME/noc_build`.
  Never `py -3`; use `PYTHON3=python3`.
- Every co-sim invocation opens with the pre-clean from `docs/backlog.md`:
  `rm -f sim/filelist_*.f sim/tb/test/tb_top_*.sv sim/tb/soc/tb_top_dma_*.sv; rm -rf $BUILD_ROOT/verilator/obj_dir_*`
- New asserts need a fault-injection proof that they fire.
- Each of the four PUBLIC constructors that gains a `port_id` takes it as a TRAILING parameter
  defaulted to 0 — `nmu::Packetize`, `nmu::Depacketize`, `nsu::Packetize`, `nsu::Depacketize`.
  One rule for all four, and no existing call site changes. `Nmu` / `Nsu` pass `cfg.port_id`
  explicitly. The rule does not bind private helpers: `nsu::Packetize::build_b_flit` /
  `build_r_flit` are private statics with two callers each, and take `port_id` beside the
  `src_id` it belongs with.

## Rulings made while writing this plan

**`SamEntry::port` comes forward into round 2.** The spec lists it under Decision 5 without naming
a round, and round 3 is where a non-zero value first appears. It lands here because `dst_port_id`
otherwise has no source and the NMU would stamp a literal 0, which is a placeholder rather than a
value. The field is a defaulted `uint8_t` and the YAML loader needs no change: every endpoint in
every shipped topology sits on its router's LOCAL port today, `sim/topologies/mesh_2x2_vc1_periph.yaml`
included, because peripherals there are still coordinate-addressed and do not move on-grid until
round 3. Cost if wrong: a three-line struct member arrives one round early.

Spec line 248 says "the header fields do not touch the SAM". That sentence sits inside the
collective-eligibility acceptance section and means the eligibility declaration is unaffected,
which stays true — a defaulted member changes no `collective_coords` result. `SamEntry::space`,
the other half of Decision 5, stays in round 3: it is the space-indexed re-keying of the whole
table, not an additive field.

**`route_compute` stays in round 3.** The spec's Decision 1 says its `return LOCAL` becomes the
port the field names. That is peripheral delivery, not header plumbing: it changes nothing while
every port is 0, it has no round-2 acceptance criterion beyond a unit test, and round 2's stated
purpose is to separate a width failure from a peripheral failure. Cost if wrong: round 3 carries
one more change.

---

### Task 1: Widen the header

The whole round rests on this. Nothing else can compile until the generated constants carry the
two fields.

**Files:**
- Modify: `specgen/generated/json/ni_packet.json`
- Modify: `specgen/source/constants.yaml:113-139`
- Regenerate: `specgen/generated/cpp/`, `specgen/generated/sv/`, `specgen/tests/golden/`

**Interfaces:**
- Produces: `ni::header::DST_PORT_ID_LSB/MSB/WIDTH`, `ni::header::SRC_PORT_ID_LSB/MSB/WIDTH`,
  `ni::width::HEADER_TOTAL_WIDTH == 48`, `ni::HEADER_WIDTH == 48`,
  `ni::NOC_REQ_FLIT_WIDTH == 136`, `ni::NOC_RSP_FLIT_WIDTH == 126`, `ni::NOC_DAT_FLIT_WIDTH == 633`,
  and the SV twins in `ni_flit_pkg.sv` / `ni_params_pkg.sv`.

- [ ] **Step 1: Add the two width params**

In `specgen/generated/json/ni_packet.json`, in `flit.field_widths`, after `"COLLECTIVE_MASK_WIDTH": 8`:

```json
  "DST_PORT_ID_WIDTH": 2,
  "SRC_PORT_ID_WIDTH": 2,
```

and change `"HEADER_TOTAL_WIDTH": 44` to `"HEADER_TOTAL_WIDTH": 48`.

- [ ] **Step 2: Add the two header fields**

Fields are packed LSB-first in list order, so appending puts them above every existing field and
moves nothing. At the END of `flit.header_fields`, after the `collective_mask` entry:

```json
  {
   "name": "dst_port_id",
   "width_param": "DST_PORT_ID_WIDTH",
   "enabled": true
  },
  {
   "name": "src_port_id",
   "width_param": "SRC_PORT_ID_WIDTH",
   "enabled": true
  }
```

- [ ] **Step 3: Bump the three network flit widths**

In `specgen/source/constants.yaml`, three entries. `invariants.py` cross-checks each against
`HEADER_WIDTH + max(payload_width)` for that network, so a missed one fails codegen, not a test.

| key | `default` | `description` |
|---|---|---|
| `REQ_FLIT_WIDTH` (`:113`) | 132 -> 136 | `HEADER_WIDTH(44)` -> `HEADER_WIDTH(48)`, `= 132` -> `= 136` |
| `RSP_FLIT_WIDTH` (`:122`) | 122 -> 126 | `HEADER_WIDTH(44)` -> `HEADER_WIDTH(48)`, `= 122` -> `= 126` |
| `DAT_FLIT_WIDTH` (`:131`) | 629 -> 633 | `HEADER_WIDTH(44)` -> `HEADER_WIDTH(48)`, `= 629` -> `= 633` |

- [ ] **Step 4: Regenerate everything**

```bash
python3 specgen/tools/codegen.py --target cpp --domain packet
python3 specgen/tools/codegen.py --target cpp --domain params
python3 specgen/tools/codegen.py --target sv  --domain packet
python3 specgen/tools/codegen.py --target sv  --domain params
for v in 1 2 4 8; do python3 specgen/tools/codegen.py --target sv --domain noc_types --num-vc $v; done
```

Then refresh the goldens. They are byte-identical copies of the generated files, so copy, do
not edit. The packet goldens are checked by `specgen/tests/test_byte_identical_golden.py`; the
params goldens by `test_codegen.py:296` and `test_codegen_sv.py:313`:

```bash
cp specgen/generated/cpp/ni_flit_constants.h specgen/tests/golden/ni_flit_constants.h.golden
cp specgen/generated/cpp/ni_params.h         specgen/tests/golden/ni_params.h.golden
cp specgen/generated/sv/ni_flit_pkg.sv       specgen/tests/golden/ni_flit_pkg.sv.golden
cp specgen/generated/sv/ni_params_pkg.sv     specgen/tests/golden/ni_params_pkg.sv.golden
```

- [ ] **Step 5: Verify**

```bash
python3 specgen/tools/codegen.py --check          # expect exit 0
python3 -m pytest specgen/tests -q                # expect all pass
grep -n "DST_PORT_ID_LSB\|SRC_PORT_ID_LSB\|HEADER_WIDTH " specgen/generated/cpp/ni_flit_constants.h
```

Expected from the grep: `DST_PORT_ID_LSB = 44`, `SRC_PORT_ID_LSB = 46`, `HEADER_WIDTH = 48`.

If `--check` reports drift in a file the commands above did not regenerate, regenerate that
domain too rather than editing the file.

- [ ] **Step 6: Commit**

```bash
git add specgen/
git commit -m "feat(specgen): add dst_port_id and src_port_id header fields"
```

---

### Task 2: Give every NI half a port_id

`src_port_id` needs a source. This wires one config value from the generated testbench down to
`NmuConfig` / `NsuConfig`. Nothing reads it yet.

**Files:**
- Modify: `ref_model/c_model/include/nmu/nmu.hpp:152` (`NmuConfig`)
- Modify: `ref_model/c_model/include/nsu/nsu.hpp:62` (`NsuConfig`)
- Modify: `ref_model/c_model/include/wrap/nmu_wrap.hpp:75-88`
- Modify: `ref_model/c_model/include/wrap/nsu_wrap.hpp:72-80`
- Modify: `ref_model/dpi/cmodel_dpi.h:155-159,219-221`
- Modify: `ref_model/dpi/cmodel_dpi.cpp:459,465,644`
- Modify: `sim/tools/gen_tb_top.py:1255-1266,1318-1323`

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `NmuConfig::port_id`, `NsuConfig::port_id` (both `uint8_t`, default 0);
  `cmodel_nmu_create_ex(..., int port_id, const char* config_path)` and
  `cmodel_nsu_create(..., int port_id, const char* config_path)` — `port_id` goes immediately
  before `config_path` in both.

- [ ] **Step 1: Add the config fields**

In `NmuConfig`, after `uint8_t src_id = 0;`:

```cpp
    // Which endpoint at this coordinate. 0 is the router's LOCAL port, the
    // tile. Non-zero names a boundary-port peripheral (round 3). Stamped into
    // every request this NI issues as src_port_id, and checked against the
    // dst_port_id of every response that comes back.
    uint8_t port_id = 0;
```

In `NsuConfig`, after `uint8_t src_id = 0;`, the same field with the responder's wording:

```cpp
    // Which endpoint at this coordinate. 0 is the router's LOCAL port, the
    // tile. Non-zero names a boundary-port peripheral (round 3). Checked
    // against the dst_port_id of every request that arrives, and stamped into
    // every response as src_port_id.
    uint8_t port_id = 0;
```

- [ ] **Step 2: Pass it through the wraps**

`NmuWrap::init` — add `uint8_t port_id = 0` to the parameter list immediately after
`uint8_t src_id = 0`, and `cfg.port_id = port_id;` immediately after `cfg.src_id = src_id;`.

Both wraps are a config trust boundary, so both reject the reserved encoding here, where the value
enters. Spec Decision 1 assigns `11` to reserved; `00`, `01` and `10` are the legal set:

```cpp
        if (port_id > 2) {
            throw std::invalid_argument(
                "NmuWrap::init: port_id 3 is the reserved encoding (0 = LOCAL, 1 = x face, "
                "2 = y face)");
        }
```

`NsuWrap::init` — the same two edits, same positions.

- [ ] **Step 3: Extend the DPI create calls**

In `ref_model/dpi/cmodel_dpi.h`:

```c
unsigned long long cmodel_nmu_create_ex(const char* name, int src_id, int dat_num_vc,
                                        int rob_enabled, int b_rob_depth, int r_rob_depth,
                                        int max_txns_per_id, int port_id, const char* config_path);
```

```c
unsigned long long cmodel_nsu_create(const char* name, int src_id, int dat_num_vc,
                                     int max_unique_ids, int max_outstanding, int port_id,
                                     const char* config_path);
```

`cmodel_nmu_create` (the non-`_ex` entry) keeps its signature and passes `port_id = 0`.

In `ref_model/dpi/cmodel_dpi.cpp`, mirror the three signatures and forward `port_id` into the
matching `init` argument. Do not reorder any other argument.

- [ ] **Step 4: Emit it from the generator**

In `sim/tools/gen_tb_top.py`, the `import "DPI-C"` declaration for `cmodel_nmu_create_ex` gains
a line before `input string config_path`:

```python
    w('                                                                 input int port_id,')
```

The same line goes into the `cmodel_nsu_create` declaration, before its `input string config_path`.

At the two call sites (`:1318`, `:1322`), pass `0` in the new position. Every endpoint in every
shipped topology is a tile:

```python
        w(f'        nmu_ctx[{i}] = cmodel_nmu_create_ex("nmu_{i}", {c}, DAT_NUM_VC, '
          f'READ_ROB_ENABLED, b_rob_depth, r_rob_depth, max_txns_per_id, 0, '
          f'sam_config_path);  '
          f'// src_id = {"node" if i < n else "peripheral"}{i} coord {c}, port 0 = LOCAL')
```

```python
        w(f'        nsu_ctx[{i}] = cmodel_nsu_create("nsu_{i}", {c}, DAT_NUM_VC, max_unique_ids, '
          f'max_outstanding, 0, sam_config_path);')
```

- [ ] **Step 5: Build and test**

```bash
make build-cmodel && ctest --test-dir $HOME/noc_build/cmodel --output-on-failure
python3 -m pytest sim/tools -q
```

Expect ctest fully green (this task changes no behaviour) and pytest green. If a pytest golden in
`sim/tools/` pins the generated `tb_top` text, update the expected string to match the new
argument — it is a generator-output fixture, not a behavioural assertion.

- [ ] **Step 6: Commit**

```bash
git add ref_model/ sim/tools/gen_tb_top.py
git commit -m "feat(ni): carry a per-endpoint port_id from the testbench to NmuConfig and NsuConfig"
```

---

### Task 3: The NMU stamps and checks

**Files:**
- Modify: `ref_model/c_model/include/nmu/addr_trans.hpp:16-31` (`Translated`, `SamEntry`) and `translate()`
- Modify: `ref_model/c_model/include/nmu/packetize.hpp:56-69,80-91,196-204,238-245,282-290` and the `WMeta` struct
- Modify: `ref_model/c_model/include/nmu/staged_beats.hpp:10-30` (`AdmittedAw`, `AdmittedAr`)
- Modify: `ref_model/c_model/include/nmu/rob.hpp:428-431,518-520,564`
- Modify: `ref_model/c_model/include/nmu/depacketize.hpp:29-31` and `drain_ingress_`
- Modify: `ref_model/c_model/include/nmu/nmu.hpp:75-91,99-107` (`NmuReqS1Bridge`) and the `Packetize` / `Depacketize` construction
- Test: `ref_model/c_model/tests/nmu/test_packetize.cpp`, `ref_model/c_model/tests/nmu/test_depacketize.cpp`, `ref_model/c_model/tests/nmu/test_rob.cpp`

**The production path does not go through `Packetize::push_aw`.** It goes
`AxiSlavePort -> nmu::Rob -> NmuReqS1Bridge -> Packetize::push_aw_with_meta`, and the metadata is
rebuilt member-by-member three times along it: `Rob` builds an `AwHeaderMeta` brace, the bridge
unpacks it into an `AdmittedAw` and repacks it into a fresh `AwHeaderMeta`. Every one of those is
positional — C++17 has no designated initializers, which is why `staged_beats.hpp:8` documents the
field set as mirroring `AwHeaderMeta`. A member not named at all three points is silently dropped.
Miss this and every production flit carries `dst_port_id = 0` whatever the SAM says, and round 2
cannot detect it because every port IS 0. It surfaces in round 3 as "peripherals unreachable",
which is the exact failure this round exists to rule out.

**Interfaces:**
- Consumes: `ni::header::DST_PORT_ID_*` / `SRC_PORT_ID_*` (Task 1); `NmuConfig::port_id` (Task 2).
- Produces: `SamEntry::port`, `Translated::port` (both `uint8_t`, default 0);
  `AwHeaderMeta::dst_port` (`uint8_t`, default 0, appended last).

- [ ] **Step 1: Write the failing tests**

Add to `ref_model/c_model/tests/nmu/test_packetize.cpp`, after
`NmuPacketize.PushAwEmitsFlitWithCorrectFields`. `SamTable`'s only entry-level constructor is
`explicit SamTable(std::vector<SamEntry>)` — there is no `add()`:

```cpp
TEST(NmuPacketize, StampsItsOwnPortAndTheSamEntrysPortIntoTheHeader) {
    // A SAM entry whose port is 1 is what a peripheral destination will look
    // like in round 3. No shipped topology declares one yet, so the value is
    // the fixture's.
    addr_trans::SamTable sam{
        {{/*base=*/0x0, /*size=*/0x1000, /*dst_id=*/0x11, axi::AxiClass::Data, /*port=*/1}}};
    ReqCapture aw_cap, w_cap, ar_cap;
    Packetize pkt(aw_cap, w_cap, ar_cap, aw_cap, w_cap, kSrcId, std::move(sam), /*port_id=*/2);
    ASSERT_TRUE(pkt.push_aw(make_aw(0x05, 0x40)));

    auto flit_opt = aw_cap.pop();
    ASSERT_TRUE(flit_opt.has_value());
    EXPECT_EQ(flit_opt->get_header_field("dst_port_id"), 1u);
    EXPECT_EQ(flit_opt->get_header_field("src_port_id"), 2u);
}
```

Add to `ref_model/c_model/tests/nmu/test_depacketize.cpp`. Copy the fixture wiring from the
neighbouring `TEST(NmuDepacketize, ...)` cases at `:127` and `:155`, which already build a
`ChannelModel`, push a B flit and tick:

```cpp
TEST(NmuDepacketize, AcceptsAResponseAddressedToItsOwnPort) {
    ChannelModel channel;
    nmu::Depacketize depkt(channel.rsp_in(), /*b_q_depth=*/16, /*r_q_depth=*/16,
                           router::null_rsp_in(), /*port_id=*/1);
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_NarrowB);
    f.set_header_field("src_id", 0x10);
    f.set_header_field("dst_id", 0x01);
    f.set_header_field("dst_port_id", 1);
    channel.rsp_out().push_flit(f);
    depkt.tick();
    EXPECT_TRUE(depkt.pop_b().has_value());
}

TEST(NmuDepacketizeDeath, RejectsAResponseAddressedToAnotherPort) {
    ChannelModel channel;
    nmu::Depacketize depkt(channel.rsp_in(), /*b_q_depth=*/16, /*r_q_depth=*/16,
                           router::null_rsp_in(), /*port_id=*/1);
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_NarrowB);
    f.set_header_field("src_id", 0x10);
    f.set_header_field("dst_id", 0x01);
    f.set_header_field("dst_port_id", 2);
    channel.rsp_out().push_flit(f);
    EXPECT_DEATH(depkt.tick(), "dst_port_id");
}
```

The death test is the fault-injection proof the standing rules require. Match the existing
`*Death` tests in this directory for the exact death-test macro and fixture spelling.

Add to `ref_model/c_model/tests/nmu/test_rob.cpp`. This is the one that catches a break anywhere
along the three-hop metadata rebuild, which the `Packetize`-only test above cannot see:

```cpp
TEST(NmuRob, CarriesTheSamEntrysPortThroughToTheFlit) {
    // The production path is Rob -> NmuReqS1Bridge -> Packetize, and each hop
    // rebuilds AwHeaderMeta positionally. A dropped member shows up here and
    // nowhere else while every shipped port is 0.
    addr_trans::SamTable sam{
        {{/*base=*/0x0, /*size=*/0x1000, /*dst_id=*/0x11, axi::AxiClass::Data, /*port=*/1}}};
    // ... build the Rob + Packetize fixture the way the neighbouring tests do,
    //     passing this sam instead of the file's default ...
    ASSERT_TRUE(rob.push_aw(make_aw(0x05, 0x40)));
    // ... tick until the AW flit reaches the capture ...
    EXPECT_EQ(aw_flit->get_header_field("dst_port_id"), 1u);
}
```

Add the AR twin in the same shape. Both RoB modes reach `push_ar_with_meta`, through
`rob.hpp:518-520` when Enabled and `rob.hpp:564` when Disabled, so run this test in whichever mode
the surrounding fixtures use and add a second case for the other.

- [ ] **Step 2: Run them and watch them fail**

```bash
ctest --test-dir $HOME/noc_build/cmodel -R "NmuPacketize|NmuDepacketize" --output-on-failure
```

Expect compile errors: no `port` member, no fifth `Depacketize` ctor argument.

- [ ] **Step 3: Carry the port through the SAM**

`Translated` gains, after `cls`:

```cpp
    // Which endpoint at dst_id. 0 is the tile on the router's LOCAL port;
    // non-zero names a boundary-port peripheral (round 3). Every entry a
    // shipped topology declares today is a tile, so this is 0 throughout.
    uint8_t port = 0;
```

`SamEntry` gains the same field with the same default, after its `cls`. `SamTable::translate`
copies `entry->port` into the `Translated` it returns. No loader change: the YAML has no
peripheral block yet, so every entry takes the default.

- [ ] **Step 4: Stamp the two fields on every request flit**

`AwHeaderMeta` gains, appended LAST (the struct is initialised positionally at several call sites):

```cpp
    uint8_t dst_port = 0;  // from addr_trans; which endpoint at dst_id receives
```

`Packetize`'s constructor gains a trailing `uint8_t port_id = 0` after the `SamTable sam`
parameter, stored as `port_id_`. Trailing and defaulted, so the existing call sites in
`test_packetize.cpp:245` and `test_rob.cpp:1643` keep compiling.

The two direct-path calls (`push_aw:107`, `push_ar:120`) pass `t.port` as the new trailing
member: `{t.dst_id, t.local_addr, 0, 0, t.cls, axi::COLLECTIVE_OP_UNICAST, 0, t.port}`.

`WMeta` gains `uint8_t dst_port = 0;` and `push_aw_with_meta` latches `meta.dst_port` into it,
beside the `dst_id` it already latches, so W beats follow their AW.

All three flit builders gain two lines beside their existing `set_header_field("dst_id", ...)`:

```cpp
    f.set_header_field("dst_port_id", meta.dst_port);
    f.set_header_field("src_port_id", port_id_);
```

For the W builder the source is the `WMeta` front, matching how it reads `dst_id`.

- [ ] **Step 5: Carry it down the production path**

`AdmittedAw` and `AdmittedAr` in `staged_beats.hpp` each gain the same member, appended LAST:

```cpp
    uint8_t dst_port = 0;  // mirrors AwHeaderMeta::dst_port
```

`NmuReqS1Bridge` names it at both conversions. `push_aw_with_meta` (`nmu.hpp:77-78`) appends
`meta.dst_port` to its `s1_aw_.accept({...})` brace; `push_ar_with_meta` (`:87-88`) appends it to
`s1_ar_.accept({...})`. In `tick()` (`:99-107`), the AW re-pack appends `e.dst_port` and the AR
re-pack appends `axi::COLLECTIVE_OP_UNICAST, 0, e.dst_port` — the AR brace stops short at `cls`
today, and `dst_port` sits behind the two collective members.

The three `Rob` sites: rather than spelling the collective members out at the AR sites just to
reach a trailing one, hoist the meta into a named local and assign. This is the same shape
`nsu::Depacketize::pop_aw` already uses for its collective fields:

```cpp
    AwHeaderMeta meta{t.dst_id, t.local_addr, 0, 0, t.cls};
    meta.dst_port = t.port;
    if (!next_pkt_.push_ar_with_meta(b, meta)) {
        return false;  // downstream backpressure: no state mutation
    }
```

Apply that at `rob.hpp:564` (Disabled AR) and `:518-520` (Enabled AR, whose `ordering_req` /
`ordering_tag` are the `needs_rob` expressions, not 0). At `:428-431` (AW) the brace already runs
to `collective_mask`, so `t.port` appends directly as the next member.

- [ ] **Step 6: Check the returning port**

`nmu::Depacketize`'s constructor gains a trailing defaulted argument, so no existing call site
changes:

```cpp
    Depacketize(router::NocRspIn& rsp_in, std::size_t b_q_depth, std::size_t r_q_depth,
                router::NocRspIn& dat_rsp_in = router::null_rsp_in(), uint8_t port_id = 0)
        : rsp_in_(rsp_in), dat_rsp_in_(dat_rsp_in), b_q_depth_(b_q_depth), r_q_depth_(r_q_depth),
          port_id_(port_id) {}
```

In `drain_ingress_`, where a pulled flit is first inspected, before it is decoded:

```cpp
    // The NSU echoes the requester's src_port_id back as dst_port_id. A
    // mismatch means the response reached the wrong endpoint at this
    // coordinate, which no amount of downstream decoding can detect.
    if (f.get_header_field("dst_port_id") != port_id_) {
        assert(false && "nmu::Depacketize: response dst_port_id names another endpoint");
        std::abort();
    }
```

`Nmu`'s constructor passes `cfg.port_id` into both `Packetize` and `Depacketize`.

- [ ] **Step 7: Run the tests**

```bash
ctest --test-dir $HOME/noc_build/cmodel -R "NmuPacketize|NmuDepacketize|NmuRob" --output-on-failure
ctest --test-dir $HOME/noc_build/cmodel --output-on-failure
```

Expect the new tests to pass and the full suite to stay green. Every response the NSU builds
still leaves `dst_port_id` at 0 until Task 4, and every NMU has `port_id` 0, so the new check
passes trivially — that is the correct round-2 behaviour.

- [ ] **Step 8: Commit**

```bash
clang-format -i ref_model/c_model/include/nmu/*.hpp ref_model/c_model/tests/nmu/test_packetize.cpp ref_model/c_model/tests/nmu/test_depacketize.cpp
git add ref_model/c_model
git commit -m "feat(nmu): stamp dst_port_id and src_port_id, check the port on returning responses"
```

---

### Task 4: The NSU echoes and checks

**Files:**
- Modify: `ref_model/c_model/include/nsu/meta_buffer.hpp:20-43` (`MetaEntry`)
- Modify: `ref_model/c_model/include/nsu/depacketize.hpp:50-56,383-395,424-433`
- Modify: `ref_model/c_model/include/nsu/packetize.hpp:39-41,93-97,120-124`
- Modify: `ref_model/c_model/include/nsu/nsu.hpp` (the `Packetize` and `Depacketize` construction)
- Test: `ref_model/c_model/tests/nsu/test_nsu.cpp`, `ref_model/c_model/tests/nsu/test_nsu_depacketize.cpp`

**Interfaces:**
- Consumes: `NsuConfig::port_id` (Task 2); the header fields (Task 1).
- Produces: `MetaEntry::src_port` (`uint8_t`, default 0, appended last).

- [ ] **Step 1: Write the failing tests**

Add to `ref_model/c_model/tests/nsu/test_nsu.cpp`. This is
`NsuTopLevel.WriteRoundTripDecodesReqFlitsAndProducesBRspFlit` (`:57`) with one extra header
field on each request flit and two extra expectations on the response — copy that test whole and
edit, rather than writing a new fixture:

```cpp
TEST(NsuTopLevel, EchoesTheRequestersPortBackOntoTheBResponse) {
    // Same drive as WriteRoundTripDecodesReqFlitsAndProducesBRspFlit, except
    // the requester sits on port 1 at its own coordinate. The B must name that
    // port, so it reaches the requester and not the tile beside it.
    // ... cfg / NsuStandalone / aw_flit / w_flit exactly as in that test ...
    aw_flit.set_header_field("src_port_id", 1);
    w_flit.set_header_field("src_port_id", 1);
    // ... same drain loop, same B push, same b_flit wait loop ...
    EXPECT_EQ(b_flit->get_header_field("dst_port_id"), 1u)
        << "B should be addressed back to the port that issued the AW";
    EXPECT_EQ(b_flit->get_header_field("src_port_id"), 0u)
        << "this NSU is the tile on the router's LOCAL port";
}
```

Add to `ref_model/c_model/tests/nsu/test_nsu_depacketize.cpp`. `make_aw_flit` is the file's own
helper at `:15`; the `Depacketize` construction copies
`NsuDepacketize.AwFlitSnapshotsMetadataAndPopsBeat` (`:54`):

```cpp
TEST(NsuDepacketize, RecordsTheRequestersPortInTheMetaEntry) {
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ axi::AXI_ID_SPACE);
    auto f = make_aw_flit(0x05, 0x1000, /*src_id=*/0x12);
    f.set_header_field("src_port_id", 1);
    ASSERT_TRUE(noc.req_out().push_flit(f));
    depkt.tick();
    ASSERT_TRUE(depkt.pop_aw().has_value());
    EXPECT_EQ(mb.peek_write(0x05)->src_port, 1u);
}

TEST(NsuDepacketizeDeath, RejectsARequestAddressedToAnotherPort) {
    ChannelModel noc(16, 16);
    MetaBuffer mb(4);
    // port_id is the seventh constructor argument, after space_coords.
    Depacketize depkt(noc.req_in(), mb, /*max_unique_ids*/ axi::AXI_ID_SPACE,
                      router::null_req_in(), /*src_id=*/0x02, /*space_coords=*/{},
                      /*port_id=*/0);
    auto f = make_aw_flit(0x05, 0x1000, /*src_id=*/0x12);
    f.set_header_field("dst_port_id", 1);
    ASSERT_TRUE(noc.req_out().push_flit(f));
    EXPECT_DEATH(depkt.tick(), "dst_port_id");
}
```

`MetaBuffer::peek_write(uint8_t bid)` returns `std::optional<MetaEntry>` (`meta_buffer.hpp:93`)
and the entry is allocated inside `pop_aw`, not at `tick` — hence the ordering above.
`max_unique_ids = AXI_ID_SPACE` makes `remap_downstream_id` the identity, so `0x05` is the key.

- [ ] **Step 2: Run them and watch them fail**

```bash
ctest --test-dir $HOME/noc_build/cmodel -R "Nsu" --output-on-failure
```

Expect a compile error on the missing ctor argument and a failure on the echoed value being 0.

- [ ] **Step 3: Carry the requester's port in the MetaBuffer**

`MetaEntry` gains, appended LAST — the file's own comment at `:38-40` says why, and
`Depacketize::pop_ar` initialises the struct positionally:

```cpp
    // Which endpoint at src_id issued this. Echoed back as the response's
    // dst_port_id so it reaches the requester and not the tile beside it.
    uint8_t src_port = 0;
```

`nsu::Depacketize`'s constructor gains a trailing `uint8_t port_id = 0`, after the existing
`space_coords` parameter, stored as `port_id_`. Trailing and defaulted, so no existing call site
changes.

`pop_aw` (`:384`) sets `e.src_port = f.get_header_field("src_port_id");` beside the two
collective assignments it already makes after the aggregate init.

`pop_ar` (`:429`) appends the same value as the last positional member of its `MetaEntry`
initialiser.

- [ ] **Step 4: Check the arriving port**

In `nsu::Depacketize`, where an arriving request flit is first inspected — the same place the
existing `axi_ch` validation runs:

```cpp
    // dst_port_id names which endpoint at this coordinate the request is for.
    // The router delivers by coordinate, so a wrong value lands here silently.
    if (f.get_header_field("dst_port_id") != port_id_) {
        assert(false && "nsu::Depacketize: request dst_port_id names another endpoint");
        std::abort();
    }
```

- [ ] **Step 5: Stamp the response flits**

`nsu::Packetize`'s constructor gains a trailing `uint8_t port_id = 0` after `src_id`, stored as
`port_id_`. `build_b_flit` and `build_r_flit` are static, so they gain a `uint8_t port_id`
parameter after their existing `src_id`, and both callers in `tick()` pass `port_id_`.

Both builders gain two lines beside their `set_header_field("dst_id", m.src_id)`:

```cpp
    f.set_header_field("dst_port_id", m.src_port);
    f.set_header_field("src_port_id", port_id);
```

`Nsu`'s constructor passes `cfg.port_id` into both `Packetize` and `Depacketize`.

- [ ] **Step 6: Run the tests**

```bash
ctest --test-dir $HOME/noc_build/cmodel --output-on-failure
```

Expect the whole suite green. The NMU's check from Task 3 now reads a value the NSU actually
wrote, and both are 0 in every fixture that does not set them.

- [ ] **Step 7: Commit**

```bash
clang-format -i ref_model/c_model/include/nsu/*.hpp ref_model/c_model/tests/nsu/test_nsu.cpp ref_model/c_model/tests/nsu/test_nsu_depacketize.cpp
git add ref_model/c_model
git commit -m "feat(nsu): echo the requester's port onto responses and check the port on arriving requests"
```

---

### Task 5: Co-sim gate

The header width reaches every link in the fabric. This is the task that proves it.

**Files:** none. This task runs the simulator and fixes whatever it finds.

- [ ] **Step 1: Pre-clean and run the Tier 2 set**

```bash
rm -f sim/filelist_*.f sim/tb/test/tb_top_*.sv sim/tb/soc/tb_top_dma_*.sv
rm -rf $BUILD_ROOT/verilator/obj_dir_*
make -C sim TB=mesh_2x2_vc1 PATTERN=neighbor
make -C sim TB=mesh_4x4_vc1 PATTERN=neighbor
make -C sim TB=mesh_2x2_vc1 PATTERN=beat_exact
```

`beat_exact` is in the set because the flit width changed, which is exactly the DPI word-packing
property it checks. 136, 126 and 633 bits still pack into 5, 4 and 20 `svBitVecVal` words, the
same counts as before, so this should pass unchanged — confirm it rather than assume it.

- [ ] **Step 2: Run the multi-VC and collective cases**

```bash
make -C sim TB=mesh_4x4_vc4 PATTERN=transpose
make -C sim TB=mesh_4x4_vc1 PATTERN=multicast MCAST_SHAPE=row
make -C sim TB=mesh_4x4_vc1 PATTERN=multicast MCAST_SHAPE=col
```

The collective runs matter here: a B flit's `dst_port_id` travels through the RSP router's join,
which recomputes the expected-input set from `dst_id` plus the mask.

- [ ] **Step 3: Run the DMA flavour**

```bash
make -C sim TB=mesh_2x2_vc1 DMA=1
```

The DMA flavour is a flag over an existing topology, not a topology of its own — there is no
`mesh_2x2_vc1_dma.yaml`, and `TB=mesh_2x2_vc1_dma` dies in `build_config.mk`'s `--print-num-vc`
guard. Likewise the multicast variable is `MCAST_SHAPE` (`sim/verilator/Makefile:180`), which
defaults to `row`: `MULTICAST_SHAPE=col` is silently ignored and re-runs row under a row tag.

- [ ] **Step 4: Record the results**

Write the pass/fail line for each run into the task report. A failure here is a real finding: the
round changed only widths and zero-valued fields, so anything red is a width-propagation bug, not
a peripheral bug. That separation is the whole reason this round exists.

---

### Task 6: Docs

The three flit widths and the 44 b header appear across seven documents and three source files.
Every one is now wrong.

**Files:**
- Modify: `docs/noc-target-spec.md` (13 mentions), `docs/nmu-spec.md` (11), `docs/nsu-spec.md` (9),
  `docs/router-spec.md` (9), `docs/noc-performance-parameters.md` (3),
  `docs/verification-environment.md` (2), `docs/noc_high_perf_targets.md` (1)
- Modify: `ref_model/top/router_wrap.sv:34`, `ref_model/dpi/cmodel_dpi.h:69,75,144,205`,
  `ref_model/dpi/dpi_marshal.hpp:4,32` — comments only, but they state the widths as fact

- [ ] **Step 1: Find every mention**

The search covers `ref_model/` as well as `docs/`: seven of the stale mentions are in source
comments, not documentation.

```bash
grep -rn "629\|132 b\|122 b\|132/122\|44-bit header\|44 b header\|\[131:44\]\|\[121:44\]\|\[628:44\]\|\[136:44\]\|\[126:44\]\|NOC_REQ_FLIT_WIDTH\|NOC_RSP_FLIT_WIDTH\|NOC_DAT_FLIT_WIDTH" docs/*.md ref_model/
```

`docs/router-spec.md:83-85` needs reading before editing. It already says
`REQ [136:44], RSP [126:44]`, which are wrong TODAY — they should read `[131:44]` and `[121:44]`.
The substitution table below turns them into `[135:48]` and `[125:48]`, which fixes both the
pre-existing error and this round's change in one edit. Do not let the `136` already sitting in
that line collide with the `132 -> 136` row.

- [ ] **Step 2: Apply the substitutions**

| old | new |
|---|---|
| 44-bit header / 44 b header | 48-bit header / 48 b header |
| REQ 132 b / `132` | REQ 136 b / `136` |
| RSP 122 b / `122` | RSP 126 b / `126` |
| DAT 629 b / `629` | DAT 633 b / `633` |
| `[131:44]` and the stale `[136:44]` in router-spec | `[135:48]` |
| `[121:44]` and the stale `[126:44]` in router-spec | `[125:48]` |
| `[628:44]` | `[632:48]` |
| `629'h0` | `633'h0` |
| `132/122/629 b` (source comments) | `136/126/633 b` |

Read each hit in context before editing. A `132` that is not a flit width must not change; the
grep is a candidate list, not a substitution script.

- [ ] **Step 3: Add the two fields to the header field tables**

`docs/nmu-spec.md`, `docs/nsu-spec.md` and `docs/router-spec.md` each carry a header-field table.
Add two rows at the bottom of each, matching the surrounding column style:

| field | width | description |
|---|---|---|
| `dst_port_id` | 2 | Which endpoint at `dst_id` receives. 0 is the tile on the router's LOCAL port. |
| `src_port_id` | 2 | Which endpoint at `src_id` issued. The response is addressed back to it. |

`docs/noc-target-spec.md` §6 carries the same table for the spec-level flit; add the same two rows
there.

- [ ] **Step 4: Verify nothing was missed**

```bash
grep -rn "44 b header\|44-bit header\|\[131:44\]\|\[121:44\]\|\[628:44\]\|\[136:44\]\|\[126:44\]\|629\|132/122" docs/*.md ref_model/
```

Expect no output.

- [ ] **Step 5: Commit**

```bash
git add docs/ ref_model/
git commit -m "docs: header is 48 b and flits are 136/126/633 b after the port_id fields"
```

---

## Round acceptance

- `codegen.py --check` exits 0
- `python3 -m pytest specgen/tests sim/tools -q` green
- full `ctest` green, and specifically `SamYaml.CoordRangesDerivedFromTheBlockStride` — the
  spec (lines 246-248) assigns round 2 an unchanged re-run of round 1's collective-eligibility
  assertion, and a silent loss of eligibility is the failure mode it exists to catch
- every co-sim run in Task 5 green
- `grep -rn "dst_port_id" ref_model/ | wc -l` shows the field is both written and read on each side
