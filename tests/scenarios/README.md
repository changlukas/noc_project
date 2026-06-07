# tests/scenarios — AXI4 Scenario Tree

Single source of truth for AXI4 scenario YAMLs. Both c_model integration test
(`c_model/tests/axi/test_integration.cpp`) and cosim integration test
(`cosim/tests/test_cosim_integration.cpp`) consume the full set via a
CMake-generated header. Three scoped tests (`test_port_pair_loopback`,
`test_request_response_loopback`, `test_checker_fires_on_violation`) consume
hand-curated subsets.

## Naming convention — `AX4-CAT-NNN_slug`

| Code | Category | Scope |
|---|---|---|
| `BAS` | basic         | Basic serialized single-beat transfers |
| `HSH` | handshake     | Handshake stall, backpressure (IHI 0022H §A3.2) |
| `BUR` | burst         | INCR / WRAP / FIXED burst type and length (§A3.4.1) |
| `BND` | boundary      | Alignment, narrow transfer, 4 KB boundary (§A3.4.1) |
| `ORD` | ordering      | Multi-ID ordering (§A5, §A6) |
| `EXC` | exclusive     | Exclusive access (§A7.2.4) |
| `RSP` | response      | Error response — DECERR/SLVERR (§A3.4.5) |
| `STR` | stress        | Stress / concurrency |
| `INF` | infrastructure | Non-AXI4-spec; testbench / DPI / bringup fixtures |

ID format: `AX4-<CAT>-<NNN>_<slug>` where NNN is 3-digit zero-padded sequence
number within category (e.g. `AX4-BUR-002_incr_8beat`). IDs are not stable —
deleting a scenario renumbers later siblings; commit messages include the
rename map.

## YAML schema

```yaml
schema_version: 1                          # required
metadata:                                  # required
  name: AX4-BUR-002_incr_8beat             # equals parent directory basename
  category: burst                          # CAT prefix must agree

config:                                    # AXI scenario config (see scenario_parser.hpp)
  memory_base: 0x1000
  memory_size: 0x1000
  write_latency: 1
  read_latency: 1
  max_outstanding_write: 1                 # optional, default 1
  max_outstanding_read: 1                  # optional, default 1
  inject: { mode: aw_unstable, cycle: 1 }  # optional, INF-only

transactions:
  - op: write
    addr: 0x1000
    id: 0x5
    len: 7
    size: 5
    burst: INCR
    data_file: data.txt
```

Each scenario directory contains `scenario.yaml` plus per-beat `data.txt`
(and optionally `strb.txt`, `excl.txt`, indexed `data_<n>.txt`). `data_file:`
and friends use bare filenames; `scenario_parser` resolves them relative to
the scenario's own directory.

## Test layer consumption

| Test | List source | Skips |
|---|---|---|
| `c_model/tests/axi/test_integration.cpp` | `kAllAxi4Scenarios` | INF prefix |
| `cosim/tests/test_cosim_integration.cpp` | `kAllAxi4Scenarios` | INF prefix + `wb2axip_block_reason()` runtime predicate |
| `c_model/tests/integration/test_port_pair_loopback.cpp` | Curated 4 scenarios × delay sweep | n/a |
| `c_model/tests/integration/test_request_response_loopback.cpp` | Curated 7 scenarios × num_vc variants | n/a |
| `cosim/tests/test_checker_fires_on_violation.cpp` | INF-001 only | n/a |

`kAllAxi4Scenarios` is generated at CMake configure time from
`tests/scenarios/AX4-*/scenario.yaml` via `file(GLOB CONFIGURE_DEPENDS)`.
Adding a new pattern automatically propagates to both run-all tests on the
next build.

`wb2axip_block_reason()` (in `cosim/tests/wb2axip_block.hpp`) inspects each
scenario's parsed content against wb2axip's structural limits and returns a
SKIP reason on hit. No skip map is maintained. When wb2axip is replaced with
a full AXI4 BFM, deleting the helper body activates all previously skipped
scenarios.

## Adding a new scenario

1. Pick CAT + next NNN; create `tests/scenarios/AX4-CAT-NNN_slug/`
2. Write `scenario.yaml` with `schema_version: 1` and full `metadata:` block
3. Add `data.txt` (and any other data files referenced)
4. Run `make check` — lint + both integration tests pick it up automatically
5. If cosim SKIPs the new pattern with `WB2AXIP_*`, that's expected — wb2axip
   doesn't model that case. No action needed; SKIP is documentation
6. Commit with a body citing the IHI 0022H § or VIP test the scenario was
   derived from

## Reference: IHI 0022H sections covered per category

| CAT | IHI § |
|---|---|
| BAS | §A3.2 (basic VALID/READY) |
| HSH | §A3.2 (handshake stalls) |
| BUR | §A3.4.1 (burst type, length, size) |
| BND | §A3.4.1 (alignment, 4KB boundary) |
| ORD | §A5, §A6 (ID-based ordering) |
| EXC | §A7.2.4 (exclusive access) |
| RSP | §A3.4.5 (response codes) |
| STR | §A5 (multi-outstanding traffic) |
| INF | (none — testbench infrastructure) |
