# FlooNoC-style struct refactor + unified `make sim` — Design

Date: 2026-06-25
Status: Draft (revised after Codex + Claude cross-review; pending user review)

## Motivation

The generated fabric/tb attach a NoC link as a SystemVerilog **interface instance** per
edge, flattened by `gen_tb_top.py` into one named instance per node (16 nodes → 32
`noc_intf` in `noc_fabric_*.sv`, 32 `axi4_intf` in `tb_top_*.sv`). FlooNoC's
`tb_floo_axi_mesh` carries the same per-node faces as **packed-struct multidim arrays**
driven by nested `genvar` loops — one declaration, not N. The flattening (not the
interfaces themselves) is what makes our generated `.sv` long and hard to read.

Two cleanups ride on the same `make sim` / pattern surface and are folded in (user chose
one-shot): a single co-sim entry point, and unifying the four traffic patterns onto one
base-driven generation path.

## Scope

| | Item | Files touched |
|--|------|---------------|
| **C** | `noc_intf` + `axi4_intf` SV interfaces → packed-struct typedefs **inside `ni_signals_pkg`**; fabric + tb_top emit `genvar generate` + struct arrays | `specgen/source/interface_handshake.json`, `specgen/tools/elaborate/sv_signals.py`, `specgen/tests/test_codegen_sv.py`, `*_wrap.sv` (nmu/nsu/router/master/slave/ni), `user_node_endpoint.sv`, `gen_tb_top.py`; **delete** `channel_model_wrap.sv` |
| **A** | One `make sim TB=<topology> PATTERN=<pattern>` entry; retire `make bench` + `make sim-regress`; **delete** `run_regress.py` | `Makefile`, `sim/tools/run_benchmark.py` |
| **B** | All four patterns generate from a base scenario (shape from base, dst from pattern) | `sim/tools/gen_test_patterns.py`, `sim/tools/test_gen_test_patterns.py` |

Out of scope: topology address scheme (unchanged), injection-rate saturation sweep.
**Accepted regression** (decision Y): deleting `run_regress.py` drops the curated AX4
bidirectional co-sim sweep; a replacement coverage path is deferred (see §A).

## C — struct refactor

### specgen ownership (corrected)

The SV interface blocks `axi4_intf`/`noc_intf` are **not** generated from `ni_signals.json`.
They come from `specgen/source/interface_handshake.json` (the field set) emitted by
`sv_signals.py::_emit_axi4_intf` / `_emit_noc_intf` plus the hard-coded `_AXI_CHANNEL_SIGNALS`
matrix (`sv_signals.py:28-78`), appended **after `endpackage`** because an SV `interface`
cannot live inside a package (`sv_signals.py:264-265`).

`ni_signals.json` is unchanged — it only feeds `ni_signals.h` (C++) and the in-package
`*_O_RESET` reset-constant localparams (`sv_signals.py` reset section). It plays no part in
the interface bodies.

The refactor:
1. `sv_signals.py` grows a struct-typedef emitter that emits the typedefs **inside the
   package** (a packed struct, unlike an interface, must be in a package to be used as a
   cross-module port type). The `_emit_*_intf` interface emitters are removed.
2. Every wrap/fabric port type becomes `ni_signals_pkg::<struct>` (e.g.
   `ni_signals_pkg::noc_chan_t`). This package-placement + type-qualification change touches
   every NoC/AXI port in the wrap set and the generator.
3. `specgen/tests/test_codegen_sv.py` inverts from asserting `interface axi4_intf`/`noc_intf`
   + modport presence (`:226,231`) to asserting the typedef presence.

### `noc_intf` → struct

Current `noc_intf` (6 signals, one bidirectional bundle, `ni_signals_pkg.sv:141-149`):

| flow | signal | width | driven by |
|------|--------|-------|-----------|
| req fwd (NMU→router) | `req_valid`, `req_flit` | 1, `FLIT_WIDTH` | mosi |
| req credit bwd | `req_credit_return` | `NUM_VC` | miso |
| rsp fwd (router→NMU) | `rsp_valid`, `rsp_flit` | 1, `FLIT_WIDTH` | miso |
| rsp credit bwd | `rsp_credit_return` | `NUM_VC` | mosi |

A packed struct is unidirectional at a port, so one bidirectional `noc_intf` edge becomes
**four directed struct ports**, and each credit struct's direction is **opposite** to its
co-named channel:

```systemverilog
typedef struct packed { logic valid; logic [FLIT_WIDTH-1:0] flit; } noc_chan_t;   // fwd payload
typedef struct packed { logic [NUM_VC-1:0] credit; }                 noc_credit_t; // bwd pulse
```

The `mosi` face (was one modport) becomes: `output noc_chan_t req` + `input noc_credit_t
req_credit` + `input noc_chan_t rsp` + `output noc_credit_t rsp_credit`. The `.mosi`/`.miso`
token at the fabric port (`gen_tb_top.py:250,260`) is replaced by these explicit directions.
This is a materially larger generator rewrite than "stop flattening"; field names are locked
in Stage 1 (see Stages).

### `axi4_intf` → struct

Same treatment: AW/W/AR/B/R into `axi_req_t` (master→slave) and `axi_rsp_t` (slave→master),
mirroring the `modport master`/`slave` split (`ni_signals_pkg.sv:125-133`). Field set is the
union already in `axi4_intf` — no AXI signal added or removed (so the unused `awregion`/
`arregion`, present in the interface but absent from every DPI call, remain as carried-but-
unused struct fields).

### DPI invariant (not a happy accident)

Every wrap hand-unpacks interface members into scalar DPI args **before** each call
(`nmu_wrap.sv:165-199`, `nsu_wrap.sv:194-213`, `router_wrap.sv:182-196`); no interface is
ever passed whole. After the refactor the source expression changes from `noc_mosi_o.rsp_valid`
to `noc_o.rsp.valid` — the DPI prototypes (`cmodel_dpi.h`/`.cpp`) stay byte-identical.

**Invariant (normative): struct members stay individually unpacked at every DPI boundary.**
The spike must NOT "simplify" by passing a whole struct to DPI — that would break the
byte-identical property. `cmodel_dpi.cpp`/`.h` are in the explicit not-touched set.

### fabric / tb_top generate-loop

`gen_tb_top.py` stops flattening. Emit per-node faces as struct arrays
(`noc_chan_t req [N]; noc_credit_t req_credit [N]; ... axi_req_t axi_req [N]; axi_rsp_t
axi_rsp [N];`) and place node instances, inter-router wiring, boundary tie-off assertions,
perf monitors, and endpoints inside `genvar` loops.

**Array-indexing contract** (must preserve current behavior): the live-direction wiring and
boundary tie-off currently emitted per node/direction (`gen_tb_top.py:238,318`) map to
`generate` loops indexed by `[node][direction]`; emit order must match today's `_nodes()`
raster scan (`gen_tb_top.py:107-121`) and the `seen` edge dedup (`:335-343`) so perf labels
stay stable. The LINK faces are already plain `logic [LINK_PORTS]` arrays (NOT `noc_intf`),
so the struct refactor does **not** touch the link perf taps (`gen_tb_top.py:351-362`); only
the NI faces (NMU/NSU↔router) and the tb AXI faces convert. This NI-vs-LINK asymmetry is the
crux of the NoC conversion.

### Invariants preserved

- `gen_test_patterns.ADDR_DST_SHIFT == c_model LOCAL_ADDR_BITS == 32` (addr[39:32]=dst_id,
  addr[31:0]=local; 4 GB per dst tile). Address scheme unchanged; add a cross-language check.
- Beta-tick discipline and sync-reset register behavior in every `*_wrap.sv` unchanged.

## A — unified `make sim`

`make sim TB=<topology> PATTERN=<pattern> [TXN=n SEED=s HOTSPOT=ids BASE=path]`

| param | maps to | default |
|-------|---------|---------|
| `TB` | topology name (`mesh_4x4_vc1/2/4/8`) — node count, VC, `Vtb_top` binary | `mesh_4x4_vc1` |
| `PATTERN` | `neighbor`/`transpose`/`uniform_random`/`hotspot` | (required) |
| `TXN` | `--transactions-per-node` | 1 |
| `SEED` | `--seed` (uniform_random/hotspot) | 0 |
| `HOTSPOT` | `--hotspot` node id(s) | — |
| `BASE` | `--from` base scenario for transaction shape | a fixed canonical base (named in plan) |

`run_benchmark.py` becomes the `make sim` backend and **gains a `--from`/`--base` argument**
(it currently has none, `run_benchmark.py:135-146`). `make bench`, `make sim-regress`, and
`run_regress.py` are deleted.

**Coverage honesty (decision Y):** the deleted `run_regress.py` was the only caller that ran
the curated `test_patterns/AX4-*` bidirectional subset through co-sim (`run_regress.py:73-96`).
A single `make sim PATTERN=neighbor` smoke does **not** subsume that set, and the tb_top PASS
guard (`master_count==N && reads_checked>=N`, `gen_tb_top.py:609-610`) is weaker than the
curated subset (a `uniform_random` self-draw still counts as a read). `make check` runs one
`make sim PATTERN=neighbor TB=mesh_4x4_vc1` as a minimum smoke gate; **restoring the AX4
bidirectional co-sim coverage is deferred** (not claimed as retained).

## B — patterns from base

`base decides transaction shape, pattern decides destination` — orthogonal composition.

**INPUT** base `scenario.yaml` (size/len/burst/payload + memory_base/memory_size), pattern,
topology, [TXN/SEED/HOTSPOT].
**COMPUTE** for each node `i`: `dst_list = pattern(i)`; for each dst, copy base shape, set
`addr = (dst<<32) | alloc_unique_offset(...)`.
**OUTPUT** `node<i>/scenario.yaml` ×N.

Today only `neighbor` is base-driven (`gen_test_patterns.py:533-552`); `transpose` already
supports both modes (`:554-583`); `uniform_random`/`hotspot` are synthetic (`:585-620`). B
converts the synthetic three to consume the base shape via `--from` so all four share one
path. `alloc_unique_offset` (`:210-254`) already serves all patterns. `pattern(i)` is the
only per-pattern difference:

| pattern | dst | needs |
|---------|-----|-------|
| neighbor | `((x+1)%X, (y+1)%Y)` | base |
| transpose | `(y, x)` (square mesh) | base |
| uniform_random | per-txn uniform draw | base, SEED |
| hotspot | the `HOTSPOT` node(s) | base, HOTSPOT, SEED |

## Stages

| Stage | Deliverable | Gate |
|-------|-------------|------|
| **1. struct spike** | `sv_signals.py` emits `noc_chan_t`/`noc_credit_t` **in-package**; convert `mesh_2x1_vc8` fabric NoC face `noc_intf`→struct; **lock the full field-name contract (NoC + AXI)** | `verilator --lint-only` clean on the struct-array shape; one flit+credit handshake scoreboard PASS |
| **2. noc full struct** | NoC struct into ni/router/nsu wrap ports + `gen_tb_top` fabric → struct array + generate loop; delete `channel_model_wrap.sv` | `--lint-only` clean **and** co-sim PASS on vc1/2/4/8; `perf.json` byte-compare vs pre-refactor |
| **3. axi full struct** | `axi4_intf`→struct into master/slave/ni wrap + endpoint; tb_top → generate loop + struct array | co-sim PASS all topologies; VCS build of one topology green; tb_top line count down ≥50% |
| **4. A+B** | unified `make sim` (+ `--from` plumbing); four patterns base-driven; delete `run_regress.py`; `make check` smoke | each pattern PASS; `make check` green; `codegen --check` + `gen_tb_top --check` green |

## Risks

| risk | mitigation |
|------|------------|
| packed-struct port + `[N]` array + `genvar generate` elaboration on Verilator | Stage-1 spike on `mesh_2x1_vc8` (multi-VC `credit[NUM_VC]`-in-struct-in-`[LINK_PORTS]` is the novel shape; precedent for Verilator aggregate-type pickiness: `router_wrap.sv:164-181`); `--lint-only` gate before Stage 2 |
| VCS divergence — internal module struct port + generated filelist elaboration through the VCS tb_top build (`sim/vcs/Makefile:190-196`); tb_top has no SV ports, so the risk is internal, not a `-top` port | VCS build of one topology added as a Stage-3 gate (not deferred to the end) |
| `perf.json` drift from re-emit ordering | byte-compare gate in Stage 2/3 (perf parity is a gate, not an assumed property) |
| Stage-1 field names don't anticipate AXI side | lock NoC **and** AXI field-name contract in Stage 1 design output, before Stage-2 wrap edits |

## Verification

Every stage: build green, ctest 545/545 (pure C++, unaffected — `cmodel_dpi.cpp`/`.h` not
touched), `make sim` PASS for the stage's topologies, generator drift gates (`codegen
--check`, `gen_tb_top --check`) green. Spec/plan under
`docs/internal/superpowers/{specs,plans}/2026-06-25-floonoc-struct-refactor*`.
