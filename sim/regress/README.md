# NoC co-sim regression matrix

Sweeps the wire-level mesh co-sim across topology x rob x stimulus and gates each cell on the
scoreboard marker `PASS: scenario complete, scoreboard clean`.

## Approach

Each cell is one invocation of the single-point runner
`sim/tools/run_benchmark.py --topology <topology> --pattern <traffic> --from <transaction>`.
Stimulus is the product of two independent axes:

- **Traffic pattern** (`--pattern`): a spatial map from each source node to a destination tile
  (the dst tile sits in `addr[63:32]`). Decides who talks to whom.
- **Transaction pattern** (`--from`): a base AX4 scenario supplying the transaction shapes
  (op, burst, len, size, id, address). Decides what each access looks like.

**INPUT** `sim/regress/matrix.yaml` declares the topology and rob axes, the stimulus list, and
the exclusions.
**COMPUTE** `sim/regress/run_regress.py` expands the cross-product, classifies every cell as
run, excluded, or skipped_self_check, builds each distinct topology binary once, runs every
run-cell in its own output dir, and gates on the PASS marker.
**OUTPUT** `sim/regress/output/<build>/matrix.json` plus a console accounting summary. The run
exits non-zero if any cell fails.

## Per-build execution

A build is one Verilator binary: a topology with an optional `_rob` suffix when rob is enabled.
Run one build or all of them:

```
make sim-regress                      # every build
make sim-regress BUILD=mesh_4x4_vc1   # one build
python3 sim/regress/run_regress.py --build mesh_4x4_vc1 --dry-run   # accounting only, no sim
```

Every cell lands in exactly one status:

| Status | Meaning |
|---|---|
| `pass` | ran, scoreboard clean |
| `fail` | ran, mismatch or hang |
| `xfail` | ran, known-fail, does not redden the run (none this round, by design) |
| `excluded` | not run, known fabric bug, reason in `matrix.yaml` |
| `skipped_self_check` | not run, not self-checking (write-only or error-response), Layer 2 c_model suite covers it |

Accounting prints up front and after the run:

```
[regress] build=mesh_4x4_vc1 raw=82 excluded=3 skipped_self_check=5 run=74
[regress] pass=.. fail=.. xfail=.. excluded=3 skipped_self_check=5 (coverage denom = pass+fail+xfail)
```

## Self-checking filter

The scoreboard verifies by write then readback, so it only checks scenarios that produce OKAY
reads of written data. Write-only and error-response (`metadata.category: response`) scenarios
produce no verified read. `run_regress.py` marks them `skipped_self_check` with a reason, and
the Layer 2 c_model integration suite covers them.

## Traffic patterns (`--pattern`, spatial)

Ported from BookSim2 `src/traffic.cpp`. Each maps a source tile to a destination tile.

| Pattern | Mapping |
|---|---|
| `neighbor` | `(x,y) -> ((x+1) mod X, (y+1) mod Y)`. A bijection, so every dst has one src. Default for address-dependent cells. |
| `uniform_random` | each packet draws a uniform random dst in `[0, N)`. Converging sources need offset reallocation. |
| `transpose` | bit-half-swap of the node id. Diagonal nodes map to self. Requires `x_dim == y_dim`. |
| `hotspot` | weighted draw toward an interior tile via `--hotspot <id>`. Default target is the interior node. |

## Transaction patterns (`--from`, AX4 base scenarios)

37 `AX4-*` scenarios. Self-checking means the scenario produces an OKAY read the scoreboard
checks and runs on the matrix. The rest become `skipped_self_check`.

| Family | Scenarios |
|---|---|
| `AX4-BAS-*` | basic single/multi write+read, 4 of 5 self-checking (BAS-001 write-only) |
| `AX4-BND-*` | narrow / unaligned / sparse / 4KB-boundary, 7 of 7 self-checking |
| `AX4-BUR-*` | INCR / FIXED / WRAP burst variants, 9 of 9 self-checking |
| `AX4-EXC-*` | exclusive access pair / fail cases, 4 of 4 self-checking |
| `AX4-HSH-*` | handshake backpressure / retry, 2 of 2 self-checking |
| `AX4-ORD-*` | multi-txn ordering (same-id / diff-id), 2 of 2 self-checking |
| `AX4-QOS-*` | AWQOS round-trip, 0 of 1 self-checking (write-only) |
| `AX4-RSP-*` | DECERR / OOB response conformity, 0 of 3 self-checking (response category) |
| `AX4-STR-*` | latency / outstanding / multi-dst stress, 3 of 3 self-checking |
| `AX4-INF-*` | intentional DPI-fatal, not swept |

### Address-mode classification

A base scenario carries a `metadata.address_mode` tag:

- **address-independent** (15 scenarios): up to 4 unique local offsets, within the fabric's
  4-slot offset capacity. The matrix sweeps these across all four traffic patterns, where offset
  reallocation is safe.
- **address-dependent** (default): address-sensitive (4KB-boundary or OOB) or more than 4 unique
  offsets. The matrix sweeps these on `neighbor` with `--preserve-addr`, which keeps each
  transaction's original local offset while only the dst tile enters `addr[63:32]`.

`run_regress.py` raises if an `independent`-tagged scenario exceeds the 4-offset bound, so a
misclassification fails the run before any sim.

## Multi-id stimulus (`--id-policy`)

`--id-policy round_robin:N` rewrites the base AXI ids round-robin across N ids, grouped by
unique base address so each write+read pair keeps one id. The matrix sweeps `AX4-BAS-002`
(4 W/R pairs) with `round_robin:4` on `neighbor` to drive concurrent different-id traffic to
one dst.

## Builds

Topology x rob gives 8 swept builds. The `_rob` name suffix selects rob, so no YAML is
hand-copied.

| Build | Configuration |
|---|---|
| `mesh_4x4_vc{1,2,4,8}` | 4x4 mesh, 1/2/4/8 virtual channels per link, rob disabled |
| `mesh_4x4_vc{1,2,4,8}_rob` | same four topologies, NMU response reorder buffer enabled |

`mesh_2x4_vc1` (non-square 4x2) is generator- and build-proven and stays out of the swept set.

## Exclusions

Exclusions live in `matrix.yaml`, each with a reason.

| Excluded | Reason |
|---|---|
| `AX4-BUR-003` @ rob | burst len 256 > ROB capacity 32 |
| `AX4-ORD-002` | multi-id concurrent-write co-sim hang (backlog) |
| `AX4-BND-005` | 4KB read-split hang under 16-node load (backlog) |
| `AX4-BND-006` | same 4KB-boundary class, unconfirmed, excluded preemptively (backlog) |

The first full run is a discovery run. Sweeping the set through the concurrent 16-node fabric
surfaces more pre-existing co-sim bugs, which join the exclusion list as found.

## Deferred (next rounds)

Per-id VC binding re-eval, SAM-remap address generation, injection-rate saturation sweeps, and
JUnit XML reporting.
