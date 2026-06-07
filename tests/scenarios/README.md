# tests/scenarios — Unified AXI scenario tree

Single source of truth for AXI scenario YAMLs and per-beat payload data. Both
the c_model unit tests (`c_model/tests/axi/test_integration.cpp` +
`c_model/tests/integration/test_*_loopback.cpp`) and the Verilator wire-level
co-sim tests (`cosim/tests/test_cosim_wire_smoke.cpp` +
`cosim/tests/test_checker_fires_on_violation.cpp`) consume scenarios from
here. The scenario YAML schema is parsed by
`c_model/include/axi/scenario_parser.hpp`.

## Layered scheme

```
tests/scenarios/
├── common/             scenarios consumed by 2+ test layers
├── sv-cosim-only/      L1 (SV co-sim) only (debug_multi1, injection_aw_unstable)
├── cpp-adapter-only/   L2 (adapter unit tests) — empty today; see its README
└── c-model-only/       L3 (c_model unit + loopback tests) only
```

Placement rules:

- `common/` — scenarios both L1 and L3 exercise (basic AXI patterns: single
  R/W, INCR bursts, multi-id, simple backpressure).
- `c-model-only/` — scenarios that require c_model features the SV path
  doesn't yet model: `lock: exclusive`, sparse `strb_file`, DECERR/OOB,
  WRAP/FIXED bursts, narrow transfers, 4 KB auto-split, multi-NSU stress.
- `sv-cosim-only/` — scenarios specific to the SV path: `config.inject`
  fault injection, developer ad-hoc debug fixtures.
- `cpp-adapter-only/` — reserved for L2 (NMU/NSU adapter) scenarios; today
  the adapter unit tests construct stimulus in-memory.

## Per-pattern directory convention

Each scenario sits in its own dir:

```
<scenario_name>/
├── scenario.yaml      canonical YAML name; never renamed per-pattern
├── data.txt           single data file (or the diff-target concat)
├── data_<n>.txt       indexed data files (e.g. multi-outstanding writes)
├── data_<x>.txt       lettered data files (e.g. multi_txn_same_id _a _b)
├── strb.txt           per-beat WSTRB (optional, c-model-only feature)
└── excl.txt           exclusive monitor stimulus (optional)
```

Inside `scenario.yaml`, `data_file:` / `dump_file:` / `strb_file:` use SHORT
bare filenames (e.g. `data_file: data.txt`). `scenario_parser` resolves them
relative to the scenario.yaml's own directory, so a scenario works from any
cwd.

Cross-scenario reuse is allowed via relative path:

```yaml
data_file: ../../common/burst_incr_2beat/data.txt
```

`burst_crosses_oob_boundary` uses this idiom.

## Consuming scenarios

### From the c_model unit tests

`SCENARIO_TREE_ROOT` is injected at compile time by CMake (see
`c_model/tests/axi/CMakeLists.txt` and `c_model/tests/integration/CMakeLists.txt`)
and points at this directory. Test code builds the full YAML path:

```cpp
std::string yaml_path = std::string(SCENARIO_TREE_ROOT) + p.yaml + "/scenario.yaml";
```

where `p.yaml` is encoded as `"<layer>/<scenario>"` (e.g.
`"common/burst_incr_8beat"`, `"c-model-only/sparse_multibeat"`).

### From the SV co-sim tests

Same `SCENARIO_TREE_ROOT` mechanism; see `cosim/tests/CMakeLists.txt`.

### From `make sim`

```
make sim                                       # common/single_write_read_aligned
make sim SCENARIO=multi_txn_diff_id            # common/multi_txn_diff_id
make sim SCENARIO=debug_multi1 LAYER=sv-cosim-only
make sim SCENARIO=sparse_multibeat LAYER=c-model-only
```

## Data file format

- One AXI beat per line.
- `DATA_BYTES = 32` (= `WSTRB_WIDTH`) hex bytes per beat, space-separated.
- For sub-beat transactions (`size < 5`), only the first `1 << size` bytes of
  each line are observed on the bus; the data file still provides a full
  32-byte line (extra bytes are ignored on the read side).
- For diff-pass scenarios, the `data.txt` payload equals the expected
  `dump_file` output byte-for-byte (one line per beat, in receive order).

## Adding scenarios

1. Decide layer: `common/` if 2+ layers will consume it, otherwise the
   layer-specific sub-dir.
2. Create `<layer>/<name>/scenario.yaml` plus the data files.
3. Use bare filenames in `data_file:` etc.; `scenario_parser` resolves them.
4. Register the scenario in the relevant test's `INSTANTIATE_TEST_SUITE_P`
   list (test code path), encoding `"<layer>/<name>"`.
