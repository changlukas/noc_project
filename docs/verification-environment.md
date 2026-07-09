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

The directed run defaults to two-phase (drain all writes, then reads). Pass
`+traffic_inj_ratio=<r>` to switch to steady-state injection instead
(`user_node_endpoint.sv:294`).

- **`+traffic_inj_ratio=<r>`** (`0.0`..`1.0`): a per-cycle Bernoulli gate. Each
  cycle the driver injects with probability `r`, else stalls one cycle
  (`while $urandom_range(0,99) >= r*100`). This is booksim2's
  `BernoulliInjectionProcess`. `r=1.0` is greedy (offer every cycle).
- **`--ids-per-tile=<n>`** (`gen_test_patterns.py`): gives each tile a
  non-overlapping AXI ID block. This is a concurrency knob (a single ID caps
  outstanding transactions and starves the fabric). It is not VC spread: VC
  allocation is ID-agnostic.
- **`make sim-saturation`** sweeps VC count at `inj=1.0`, `ids_per_tile=16`, and
  reports saturation throughput per config from `axi_bw_monitor`.

Read the accepted throughput and latency numbers per [cosim-log.md](cosim-log.md).
