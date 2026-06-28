# NoC co-sim regression matrix

Sweeps the wire-level mesh co-sim across topology x rob x stimulus and gates each cell on
the scoreboard PASS marker `PASS: scenario complete, scoreboard clean`.

## Approach

Every cell is one invocation of the existing single-point runner
`sim/tools/run_benchmark.py topology --from <transaction> --pattern <traffic>`. The stimulus
is the product of two independent axes:

- **Traffic pattern** (`--pattern`): a spatial algorithm that maps each source node to a
  destination tile (the dst tile is encoded in `addr[63:32]`). Decides *who talks to whom*.
- **Transaction pattern** (`--from`): a curated AX4 scenario supplying the *transaction shapes*
  (op / burst / len / size / id / address). Decides *what each access looks like*.

**INPUT** `sim/regress/matrix.yaml` declares tiers, the topology/rob axes, the stimulus list,
and exclusions.
**COMPUTE** `sim/regress/run_regress.py` expands the cross-product, applies exclusions,
prebuilds the distinct topology binaries serially, runs each cell with a unique output dir,
and gates on the PASS marker.
**OUTPUT** `sim/regress/output/<tier>/matrix.json` + a console `pass/fail/skip` summary; exit
non-zero if any cell failed.

**Wire-verifiable filter (BIST-style).** The wire scoreboard verifies by write then readback,
so it only checks scenarios that produce OKAY reads of written data. Write-only and
error-response (`metadata.category: response`) scenarios produce no verified read and are
auto-skipped with a reason. They stay covered by the Layer 2 c_model integration suite.

Run: `make sim-regress TIER=nightly`.

## Traffic patterns (`--pattern`, spatial)

Ported from BookSim2 `src/traffic.cpp`. Each maps a source tile to a destination tile on the
4x4 mesh.

| Pattern | Mapping | Note |
|---|---|---|
| `neighbor` | `(x,y) -> ((x+1) mod X, (y+1) mod Y)` | Bijection: every dst has exactly one src. The default for `--preserve-addr` cells (no converging-source collision). |
| `uniform_random` | each packet draws a uniform random dst in `[0, N)` | Average-case load; converging sources, so default offset reallocation is required. |
| `transpose` | bit-half-swap of the node id | Diagonal nodes map to self; structured contention. |
| `hotspot` | weighted draw toward one or more hotspot tiles | `--hotspot <ids>` / `--hotspot-rates`; concentrates load. |

## Transaction patterns (`--from`, AX4 curated scenarios)

37 `AX4-*` scenarios by family. **Wire-verifiable** = produces an OKAY read the scoreboard
checks (runs on the matrix); otherwise auto-skipped (Layer 2 covers it).

| Family | Tests | Count | Wire-verifiable |
|---|---|---|---|
| `AX4-BAS-*` | basic single/multi write+read | 5 | 4 (BAS-001 write-only -> skip) |
| `AX4-BND-*` | narrow / unaligned / sparse / 4KB-boundary | 7 | 7 |
| `AX4-BUR-*` | INCR / FIXED / WRAP burst variants | 9 | 9 |
| `AX4-EXC-*` | exclusive access pair / fail cases | 4 | 4 |
| `AX4-HSH-*` | handshake backpressure / retry | 2 | 2 |
| `AX4-ORD-*` | multi-txn ordering (same-id / diff-id) | 2 | 2 |
| `AX4-QOS-*` | AWQOS round-trip | 1 | 0 (write-only -> skip) |
| `AX4-RSP-*` | DECERR / OOB response conformity | 3 | 0 (`category: response` -> skip) |
| `AX4-STR-*` | latency / outstanding / multi-dst stress | 3 | 3 |
| `AX4-INF-*` | intentional DPI-fatal | 1 | excluded (not swept) |

## Axes and exclusions

- Topology: `{mesh_4x4_vc1, vc2, vc4, vc8}` x rob `{disabled, enabled}` = 8 generated builds
  (rob via the `_rob` name suffix, no hand-copied YAML).
- `--preserve-addr` (used by the AX4 group with `neighbor`): keeps each transaction's original
  local offset (only the dst tile is OR-ed into `addr[63:32]`), so address-sensitive scenarios
  (OOB / 4KB-boundary) keep their condition while still routing across the fabric.
- Exclusions live in `matrix.yaml` with a reason string. The `nightly` tier is a discovery
  run: sweeping the curated set through the concurrent 16-node fabric surfaces pre-existing
  co-sim bugs, which are added to the exclusion list and backlogged as found.

## Deferred (next rounds)

Traffic-pattern multi-id stimulus (`--id-policy`); per-id VC binding re-eval; SAM-remap address
generation; JUnit XML; injection-rate saturation sweeps.
