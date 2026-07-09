# Verification environment

The DUT is the NI (NMU + NoC routers + NSU). Each mesh node wraps the DUT in a
`user_node_endpoint` (`sim/tb/user_node_endpoint.sv`) that presents two
`AXI_BUS_DV` interfaces: `master_dv` on the manager side (feeds the NMU ingress)
and `slave_dv` on the subordinate side (the NSU egress drives it). A manager BFM
drives traffic into `master_dv`. A subordinate BFM answers on `slave_dv`.

```
manager BFM --> master_dv --> NMU --> routers --> NSU --> slave_dv --> subordinate BFM
                (checkers tap here)                       (compare taps here too)
```

## Verification IP

Imported at commit `5332300`: pulp `axi` 0.39.7, `common_verification` 0.2.5, and
the FlooNoC test components. We patched `axi_reorder_compare` (Task 9). The rest
run unmodified.

| VIP | source | role | connects to |
|---|---|---|---|
| `axi_file_master` | pulp axi | directed driver, two-phase (write, barrier, read) | drives `master_dv` |
| `axi_rand_master` | pulp axi | constrained-random driver (INCR/FIXED/WRAP + exclusive) | drives `master_dv` |
| `axi_rand_slave` | pulp axi | subordinate model: `MAPPED` memory (directed), `RAND_RESP` (CR) | answers on `slave_dv` |
| `axi_scoreboard` | pulp axi | data-integrity checker (per-transaction write vs readback) | monitors `master_dv` |
| `axi_reorder_compare` | FlooNoC | transport + ID-order checker | monitors `master_dv` and `slave_dv` |
| `axi_bw_monitor` | FlooNoC | throughput + latency monitor | monitors `master_dv` |

## Two axes

Set by `+define+TB_DIRECTED` (the run recipe picks it from `PATTERN`).

| axis | driver | checker | subordinate |
|---|---|---|---|
| directed | `axi_file_master` (replays stimulus files) | `axi_scoreboard` on the manager face | `axi_rand_slave` MAPPED as tile memory |
| constrained-random | `axi_rand_master` | `axi_reorder_compare` (tb level, one per manager) | `axi_rand_slave` RAND_RESP |

`axi_reorder_compare` uses permutation pairing: manager `m` targets only node
`N-1-m`, so the checker attributes every subordinate-face beat to one manager.

## Traffic patterns

Directed patterns come from `gen_test_patterns.py` (booksim2 spatial patterns).
`PATTERN=` selects one; `constrained_random` selects the CR axis.

| pattern | destination rule | note |
|---|---|---|
| `neighbor` | `dst = ((x+1) mod X, (y+1) mod Y)` | booksim2 NeighborTrafficPattern: diagonal +1 per axis, wraps |
| `transpose` | `dst = (y, x)` | requires `X == Y`, power of two |
| `uniform_random` | random destination per transaction | |
| `hotspot` | many managers target one tile | many-to-one congestion |
| `constrained_random` | random within each manager's region | not a spatial pattern, the CR axis |

## Injection rate

`INJECTION_MODE` (`make sim` var, read as `+injection_mode`) selects the run shape:

| mode | run shape | scoreboard | `INJECTION_COUNT` default |
|---|---|---|---|
| `0` (default) | directed two-phase: drain all writes, then reads | armed | `4` |
| `1` | continuous, reads/writes interleaved, paced by `INJECTION_RATE` | disarmed (write-before-read precondition doesn't hold); `axi_bw_monitor` gates instead | `200` |

```
make sim TB=<topo> PATTERN=<p> INJECTION_MODE=1 INJECTION_RATE=<r> \
    [INJECTION_COUNT=<n> MAX_UNIQUE_IDS=<n> MAX_OUTSTANDING=<n>] [SEED=<n>]
```

- **`INJECTION_RATE`** (`0.0`..`1.0`, `+injection_rate`): a per-cycle Bernoulli
  gate. This is booksim2's `BernoulliInjectionProcess`. `r=1.0` is greedy
  (offer every cycle).
- **`MAX_UNIQUE_IDS`** / **`MAX_OUTSTANDING`** (`+max_unique_ids`,
  `+max_outstanding`): NSU knobs, not injection-side. `max_unique_ids=1`
  (default) collapses every manager onto one downstream ID; `max_outstanding`
  (default `32`) sizes the shared MetaBuffer pool per direction.
- Each mode-1 run writes `sim/verilator/output/<tag>/result.csv`.

`make sim-injection-sweep PATTERN=<p>` loops the four VC topologies
(`vc1/vc2/vc4/vc8`) at nine rates in mode 1, then runs
`sim/tools/plot_injection_sweep.py <pattern> [--dark]`. The script always
prints the merged rate/throughput/latency table; it also renders
`injection_sweep.png` / `_dark.png` when `matplotlib` is importable.
`matplotlib` is absent from the WSL interpreter this project builds/runs
under, so the sweep prints the table there; render the PNGs separately with
the Windows `py -3`.

Read the accepted throughput and latency numbers per [cosim-log.md](cosim-log.md).
