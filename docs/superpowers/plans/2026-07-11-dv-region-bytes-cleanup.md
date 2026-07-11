# DV region_bytes cleanup

Unify the per-tile "test address window" naming to `region_bytes`, auto-derive the directed
generator's window (drop the `--memory-size` knob), and move the CR window out of the DUT topology
YAML into a DV-side constant. Settled interactively 2026-07-11; no separate spec.

## Background (read once)

Three distinct "size" concepts exist; only the DV ones change here:

| concept | role | touches DUT? |
|---|---|---|
| `tile_size` (YAML `address_map.tile_size`, 4 GiB) | SAM decode stride, `REGION_BASE = coord_id*tile_size` | yes — leave untouched |
| `test_aperture` -> `REGION_BYTES` (YAML, 0x1000) | CR rand_master window + reorder_compare region + watchdog | no (DV-only) |
| `--memory-size` (Makefile knob, 0x40000) | directed file_master stimulus per-tile offset layout ceiling | no (stimulus-gen-only) |

The co-sim slave (pulp `axi_rand_slave` MAPPED, `sim/dv/axi-0.39.7/src/axi_test.sv:1364`) is an
unbounded associative array `byte_t memory_q[addr_t]` with no size param, so neither DV window bounds
any real memory. `--memory-size` only gates `alloc_unique_offset`'s per-tile slot layout.

The pulp VIP's own term for this window is **region** (`rand_master.add_memory_region(...)`,
`user_node_endpoint.sv:349`), and the emitted SV param is already `REGION_BYTES`. So `region_bytes` is
the standard name; `memory_size` and `test_aperture` both get retired in favor of it.

## Global Constraints

- **Naming.** The per-tile test address window is `region_bytes` everywhere. Retire `memory_size`
  (directed) and `test_aperture` (CR) as names for this concept. Do NOT invent `stim_span` or similar.
- **SV params unchanged.** `REGION_BYTES` / `REGION_BASE` (emitted, `user_node_endpoint.sv`) keep their
  names — they are already correct.
- **Do NOT touch the c_model scenario `memory_size`.** A different, unrelated concept (the unit-test
  AX4 scenario's declared memory window): `src/c_model/include/axi/scenario_parser.hpp`,
  `axi_slave.hpp`, `tests/axi/test_axi_master*.cpp`, `test_scenario_metadata.cpp`, and the AX4
  `scenario.yaml` files. Out of scope. Only the co-sim stimulus/tb generator `memory_size` /
  `test_aperture` change.
- **No new config file.** No `dv_config.yaml`, no new loader. The CR default lives as a Python constant
  in `gen_tb_top.py`.
- **Docs.** Only edit live docs (`docs/development.md`, `docs/architecture.md`) IF they reference the
  removed knob. Historical records under `docs/superpowers/**` and `docs/internal/**` are NOT edited.
- **Branch `chore/dv-region-bytes-cleanup`, no push.** Stop at the working tree / local commits.
- **Verify on Windows** with `py -3 -m pytest sim/tools/`. The WSL co-sim run is the controller's job,
  not the implementer's.

## Task 1: directed stimulus — auto-derive region_bytes, drop the --memory-size knob

**Files:** `sim/tools/gen_test_patterns.py`, `sim/tools/test_gen_test_patterns_filemaster.py`,
`Makefile`, `sim/verilator/Makefile`.

**Working-tree edits to revert first.** The branch carries two uncommitted prototype edits that go the
WRONG direction (they hoist `--memory-size` into a `MEMORY_SIZE` knob; we are removing it instead).
Revert both as part of this task:
- `Makefile`: the `$(if $(MEMORY_SIZE),MEMORY_SIZE=$(MEMORY_SIZE))` line added to `_INJ_ARGS`.
- `sim/verilator/Makefile`: the `MEMORY_SIZE ?= 0x40000` block and the `--memory-size $(MEMORY_SIZE)`
  substitution in `run-directed`.

**Then:**
1. `gen_test_patterns.py`: remove the `--memory-size` argparse argument entirely. Auto-derive the
   window inside `main()`:
   `region_bytes = n_nodes * transactions_per_node * stride`, where
   `stride = max(_SLOT_STRIDE, burst_footprint)` and `burst_footprint = (axi_len + 1) * (1 << axi_size)`
   — i.e. the same `stride`/`reserved` `alloc_unique_offset` already uses. This is a safe upper bound on
   `alloc_unique_offset`'s max `offset + reserved`; verify by reading the offset formula
   (`offset = base + src_node*stride + seq*(n_nodes*stride)`) that `seq`'s max is
   `< transactions_per_node`.
2. Rename the internal `memory_size` variable/parameter to `region_bytes` throughout
   `gen_test_patterns.py` (including `alloc_unique_offset`'s parameter and its docstring/error string).
3. KEEP the `alloc_unique_offset` overflow `ValueError` assert as an internal invariant net (it should
   now never fire; it guards against a future formula regression).
4. `sim/verilator/Makefile` `run-directed`: drop `--memory-size ...` from the `gen_test_patterns.py`
   invocation (the generator now derives it).
5. `test_gen_test_patterns_filemaster.py`: update any test that passes `--memory-size` / asserts on it.
6. **Add a root-cause pytest** (`test_gen_test_patterns_filemaster.py`): the injection-mode + burst combo
   that previously raised `ValueError` under the fixed `0x40000` window — high `transactions_per_node`
   (e.g. 200) with a non-zero burst len (`--len > 0`, `--size 5`) on a 16-node topology — now generates
   successfully (no `ValueError`), and every emitted slot's footprint stays disjoint. Assert the derived
   window is large enough (no exception) and that two different `(src, seq)` offsets do not overlap.

**Success:** `py -3 -m pytest sim/tools/` green; no `--memory-size` anywhere in the generator or
Makefiles; the new burst+injection pytest passes.

## Task 2: CR window — region_bytes as a DV constant, topology YAML DUT-only

**Files:** `sim/tools/gen_tb_top.py`, `sim/topologies/*.yaml` (all 7).

1. `gen_tb_top.py`: rename the constant `_DEFAULT_TEST_APERTURE` -> `_DEFAULT_REGION_BYTES` (value
   `0x1000` unchanged).
2. `_address_map()` stops reading `test_aperture` from the YAML — it returns `tile_size` only (the DUT
   SAM field). The topology YAML is now DUT-only for the address map.
3. The `region_bytes` value comes from a new argparse `--region-bytes` argument, default
   `_DEFAULT_REGION_BYTES`. Rewire every downstream consumer of the old `test_aperture` value (the
   `REGION_BYTES` emit, the reorder_compare region, the watchdog `MAX_BURST_BEATS`) to read this value.
4. Update the `_address_map` docstring and the `_DEFAULT_*` comment to describe `region_bytes` / drop
   the `test_aperture` wording.
5. Topology YAMLs (all 7 under `sim/topologies/`): delete the `test_aperture: 0x1000` line. Keep
   `address_map:` and `tile_size:`.
6. **Optional** Makefile passthrough: if `gen_tb_top.py` is invoked from a Makefile with args, wire a
   `REGION_BYTES ?=` passthrough to `--region-bytes`. If it is not cleanly Make-invoked with args, the
   CLI arg alone satisfies "optional override" — do not manufacture a knob. State which you did in the
   report.

**Success:** `py -3 -m pytest sim/tools/` green; no `test_aperture` in any topology YAML or in
`gen_tb_top.py`; `gen_tb_top.py` still emits `REGION_BYTES` correctly (spot-check one generated
`tb_top_*.sv` or the generator's unit test).
