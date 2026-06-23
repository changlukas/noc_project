# sim/test_patterns -- AXI4 Scenario Tree

Single source of truth for AXI4 scenario YAMLs. The c_model integration test
(`c_model/tests/axi/test_integration.cpp`) and the co-sim regression
(`sim/run_regress.py`, invoked via `make sim-regress`) both consume the
full set. Two scoped tests (`test_port_pair_loopback`,
`test_request_response_loopback`) consume hand-curated subsets.

## Naming convention -- `AX4-CAT-NNN_slug`

| Code | Category | Scope |
|---|---|---|
| `BAS` | basic         | Basic serialized single-beat transfers |
| `HSH` | handshake     | Handshake stall, backpressure (IHI 0022H sec. A3.2) |
| `BUR` | burst         | INCR / WRAP / FIXED burst type and length (sec. A3.4.1) |
| `BND` | boundary      | Alignment, narrow transfer, 4 KB boundary (sec. A3.4.1) |
| `ORD` | ordering      | Multi-ID ordering (sec. A5, sec. A6) |
| `EXC` | exclusive     | Exclusive access (sec. A7.2.4) |
| `RSP` | response      | Error response -- DECERR/SLVERR (sec. A3.4.5) |
| `STR` | stress        | Stress / concurrency |
| `INF` | infrastructure | Non-AXI4-spec; testbench / DPI / bringup fixtures |
| `QOS` | qos           | A8 QoS signaling; awqos / arqos passthrough end-to-end |

ID format: `AX4-<CAT>-<NNN>_<slug>` where NNN is 3-digit zero-padded sequence
number within category (e.g. `AX4-BUR-002_incr_8beat`). IDs are not stable --
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

Each scenario directory contains `scenario.yaml`. Scenarios that perform
real AXI4 transactions also contain `data.txt` (and optionally `strb.txt`,
`excl.txt`, indexed `data_<n>.txt`). `data_file:` and friends are paths
resolved relative to the scenario's own directory (`scenario_parser`):
most scenarios use bare filenames; some share inputs from the sibling
`_data/` directory via relative paths (e.g. AX4-BAS-004/-005 and
AX4-HSH-002 use `data_file: ../_data/data_aa.txt` style). Infrastructure
scenarios that deliberately point `data_file:` at a nonexistent path
(e.g. INF-001, which tests the DPI fatal propagation) do not ship a
data file.

## Test layer consumption

| Test | List source | Skips |
|---|---|---|
| `c_model/tests/axi/test_integration.cpp` | `kAllAxi4Scenarios` | INF prefix |
| `sim/run_regress.py` (`make sim-regress`) | all non-INF scenarios | INF prefix |
| `c_model/tests/integration/test_port_pair_loopback.cpp` | Curated 4 scenarios x delay sweep | n/a |
| `c_model/tests/integration/test_request_response_loopback.cpp` | Curated 6 distinct scenarios (7 FixtureParam entries at num_vc=1; re-run at num_vc in {2, 4, 8}) | n/a |

`kAllAxi4Scenarios` is generated at CMake configure time from
`sim/test_patterns/AX4-*/scenario.yaml` via `file(GLOB CONFIGURE_DEPENDS)`.
Adding a new pattern automatically propagates to the c_model integration test
on the next build; run `make sim-regress` to include it in the co-sim regression.

## Adding a new scenario

1. Pick CAT + next NNN; create `sim/test_patterns/AX4-CAT-NNN_slug/`
2. Write `scenario.yaml` with `schema_version: 1` and full `metadata:` block
3. Add `data.txt` (and any other data files referenced)
4. Run `make check` -- lint + c_model integration test pick it up automatically;
   run `make sim-regress` to verify in co-sim
5. Commit with a body citing the IHI 0022H sec. or VIP test the scenario was
   derived from

## Reference: IHI 0022H sections covered per category

| CAT | IHI sec. |
|---|---|
| BAS | sec. A3.2 (basic VALID/READY) |
| HSH | sec. A3.2 (handshake stalls) |
| BUR | sec. A3.4.1 (burst type, length, size) |
| BND | sec. A3.4.1 (alignment, 4KB boundary) |
| ORD | sec. A5, sec. A6 (ID-based ordering) |
| EXC | sec. A7.2.4 (exclusive access) |
| RSP | sec. A3.4.5 (response codes) |
| STR | sec. A5 (multi-outstanding traffic) |
| INF | (none -- testbench infrastructure) |
| QOS | sec. A8 (QoS signaling) |
