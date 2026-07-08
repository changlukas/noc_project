# SAM address remap — design

Date: 2026-07-08
Status: Draft (brainstormed + dual-AI cross-check on decisions AND written spec: this model + Codex gpt-5.5 SOUND-WITH-FIXES pass applied, FlooNoC gvsoc survey; pending final user review)

## Goal

Replace the NMU's hardcoded address **decode** (destination = `addr[39:32]` bit-slice, `local_addr = addr`
unmodified) with a configurable System Address Map **remap**: a per-tile table `{base, size, dst_id,
remove_offset}`; an incoming address is matched by range to a destination tile, and the address carried
to the subordinate is rebased (`local_addr = addr - remove_offset`). Aligns the model to FlooNoC's SAM
(`gvsoc-pulp/pulp/floonoc_v2/`); the wire packet format is unchanged.

Terminology (fixed with user): the current mechanism = **address decode**; the SAM mechanism = **address
remap**.

## Motivation

The subordinate must not receive a coordinate-bearing, >32-bit global address. Today `local_addr = addr`
passes the full global address (dst id still in bits [39:32]) to the slave. Remap subtracts the tile base
so each subordinate sees a clean 0-based local address, which is the point of a SAM.

## Reference: FlooNoC's SAM (what we port, what we drop)

FlooNoC v2 gvsoc model, single mechanism (no bit-slice path):

| element | FlooNoC | port? |
|---|---|---|
| entry struct `{base, size, x, y, remove_offset}` | `floonoc_v2.hpp:67-75` (`EntryV2`) | **port** (adapt: store flat `dst_id`, not x/y) |
| range lookup (linear first-match by start addr) | `floonoc_v2.cpp:220-231` (`get_entry`) | **port** (copy the scan) |
| rebase `addr - remove_offset`; `rm_base` ⇒ `remove_offset = base` | `floonoc_network_interface_v2.cpp:145`; `floonoc_v2.py:66-92` | **port** |
| per-SAM-entry burst split (`max_size` clamp, loop) | `floonoc_network_interface_v2.cpp:136-155` | **drop** (see Decision A) |
| beat-width split, narrow+wide dual network | same file | **drop** (not our model) |

FlooNoC's own tiles are non-uniform and not 4 GB (`tests/floonoc_v2/test.py`: cluster tiles
`cluster_size=0x100000`=1 MB, edge mem-group `0x10000000`=256 MB, all `rm_base=True`), which confirms
tile size must be per-tile configurable.

## Decisions (dual-AI cross-checked)

| # | decision | rationale |
|---|---|---|
| **A** — do NOT port the region-split | The NMU receives already-split, single-destination AXI bursts. Our `AxiMaster` splits INCR at the 4 KB / 256-beat boundary upstream (`axi_master.hpp` INCR split; 4 KB rule enforced by `protocol_rules.hpp::check_4kb_cross`). FlooNoC splits because its input is a raw gvsoc `IoReq` (arbitrary length, no 4 KB rule) with no AXI master upstream. A real AXI burst targets exactly one destination. |
| **B** — explicit `{base, size, dst_id, remove_offset}` table + linear scan | Tile size is per-tile configurable (uniform and non-uniform both supported), so arithmetic `addr/stride` is insufficient. Linear scan is negligible at mesh scale (N = 16..64). |
| **C1** — SAM miss ⇒ `assert` fail-loud | An unmapped address is a stimulus/config bug in a behavioural model. Documented as **model policy, not AXI-faithful**: a real interconnect returns DECERR on a decode miss; DECERR modelling stays a separate feature (spec RSP category, sec A3.4.5), out of scope here. |
| **C2** — no split; constrain tiles 4 KB-granular + runtime footprint assert | See "Memory-map rule" below. |
| **C3** — table stores flat `dst_id` (`(dst_y<<X_WIDTH)\|dst_x`) | Matches the flit header field, `addr_trans::Translated.dst_id`, and the router which re-splits `dst_id` into x/y (`router.hpp:63-65`). Zero conversion. |

## Mechanism

**INPUT** — `addr` (64-bit AXI address) at NMU packetize time.

**COMPUTE** — `translate(addr)`:
1. `entry = sam_lookup(addr)` — linear scan, first entry with `entry.base <= addr < entry.base + entry.size`.
2. miss ⇒ `assert` (C1).
3. `dst_id = entry.dst_id` (C3).
4. `local_addr = addr - entry.remove_offset` (`remove_offset = entry.base` when `rebase`, else 0).
5. runtime guard: `assert` the burst footprint lies inside `[entry.base, entry.base + entry.size)`
   (C2 belt-and-suspenders). Use **burst-aware** footprint math (WRAP wraps within its aligned window;
   FIXED stays at one address; INCR is linear) — reuse the slave's existing footprint computation
   (`axi_slave.hpp:305`, `:511`), not a flat `[addr, addr+total)`, or WRAP/FIXED bursts over/under-reject.

**OUTPUT** — `{dst_id, local_addr}` → the existing `addr_trans::Translated`, flowing into the flit header
`dst_id` and the AW/AR payload `awaddr`/`araddr` (already plumbed via `meta.local_addr`, "future
remap-safe", `packetize.hpp:109`).

`translate` replaces `xy_route`; call sites unchanged in shape (`packetize.hpp` push_aw/push_ar, `rob.hpp`).

## Memory-map rule (Decision C2, precise)

We do not split, so a burst must never cross a tile boundary. AXI4 guarantees a burst never crosses a
**4 KB-aligned** boundary (`check_4kb_cross`). Therefore every tile boundary must coincide with a 4 KB
boundary:

- `base` is a 4 KB multiple **and** `size` is a 4 KB multiple (so `base + size` is 4 KB-aligned too).
- Aligning only `base` is insufficient: e.g. `base=0x2000, size=0x1800` ends at `0x3800` (not 4 KB-aligned);
  a legal burst `[0x3700, 0x3840)` crosses the tile end without crossing 4 KB, straddling two destinations.

The runtime footprint assert (COMPUTE step 5) catches any residual misconfiguration fail-loud.

Consequence: the SAM is a memory-map design constraint (tiles 4 KB-granular), not a fabric feature. This
is normal — real address maps are at least page-granular.

## SAM config (YAML, single source of truth)

Project convention is YAML (`CLAUDE.md`: "Config: YAML; no JSON"). Extend the existing per-topology file
(`sim/topologies/mesh_*.yaml`) with an `address_map` block; no new file, no second sync surface. The block
drives the c_model table, the tb `REGION_BASE`, and `gen_test_patterns` (watch-list #3).

```yaml
topology: { name: mesh_4x4_vc1, x_dim: 4, y_dim: 4, num_vc: 1 }
address_map:
  tile_size: 0x100000000      # default per-tile SAM window; must be a 4 KB multiple
  rebase: true                # remove_offset = tile base (slave sees 0-based local)
  test_aperture: 0x1000       # tb-only: how much of each tile the stimulus exercises (<= tile_size)
  # optional non-uniform tiles (explicit per-tile base + size; see base-assignment note):
  # tiles:
  #   - { x: 0, y: 0, base: 0x000000000, size: 0x10000000 }
```

Table build:
- Default (uniform): for each mesh tile, `dst_id = coord_id = (y<<X_WIDTH)|x`, `base = coord_id * tile_size`,
  `size = tile_size`, `remove_offset = rebase ? base : 0`. With `tile_size = 4 GB` this reproduces today's
  `coord_id<<32` layout exactly, so existing stimulus and `REGION_BASE` stay consistent unchanged.
- Non-uniform (`tiles`): each override supplies an **explicit** per-tile `base` and `size` (Codex-recommended
  over contiguous packing: a prefix-sum packing would let one size change silently shift every later tile's
  base, and it breaks the `coord_id<<32` assumption the generators still hold). The validator (below) rejects
  overlaps and misalignment, so explicit bases are checked, not trusted.

**SAM tile size vs test aperture (distinct concepts):** the SAM `size` is the routing/rebase window and may
be large (GBs, per-tile). `test_aperture` is a testbench knob: the sub-window each stimulus actually pokes,
kept small (~4 KB). The tb uses it to cap `AXI_MAX_BURST_LEN` and bound the random master's address range
(`user_node_endpoint.sv:176,180`). Do NOT set the tb aperture to the SAM tile size — a 4 GB tile would make
`AXI_MAX_BURST_LEN` absurd. `REGION_BASE[s]` derives from the SAM `base`; the aperture stays separate.

## SAM config validator (Codex watch-list #1)

Only **explicit `tiles` overrides** need validation — derived uniform entries are correct by construction.
`get_entry` is first-match by start address, so validate at load (fail-loud): no overlapping ranges, no
zero `size`, no `base + size` overflow, `base` and `size` both 4 KB multiples, `dst_id` decodes to a tile
inside the mesh (`x < x_dim && y < y_dim`), and `remove_offset <= base` (else `local_addr = addr -
remove_offset` underflows). Longest-match is not supported.

Not validated: cross-tile local-range aliasing is **intended** (each subordinate is its own 0-based
memory; tile A local `0x40` and tile B local `0x40` are different memories). Unmapped gaps are fine —
a miss asserts (C1), so a full map is not required.

## Blast radius

| file | change |
|---|---|
| `src/c_model/include/nmu/addr_trans.hpp` | replace `xy_route` with SAM table + `translate` (port `EntryV2`/`get_entry`); add validator |
| `src/c_model/include/nmu/packetize.hpp`, `rob.hpp` | call `translate`; **`Packetize` ctor gains the SAM table** (today only `src_id`, `packetize.hpp:58`) |
| `src/c_model/include/nmu/nmu.hpp` | `NmuConfig` gains a SAM-table field (`:126` has none today) |
| `src/c_model/include/wrap/nmu_wrap.hpp` | `NmuWrap::init` builds the config **with** the map (`:47`) |
| `src/dpi/cmodel_dpi.cpp` | `cmodel_nmu_create` gains a config-path string arg (today name/src_id/num_vc/rob, `:394`); c_model reads the YAML itself (see delivery path) |
| `sim/topologies/mesh_*.yaml` | add `address_map` block (single source of truth) |
| `sim/tools/gen_tb_top.py` | derive `REGION_BASE[s]` from SAM `base` (today stamps `coord<<32`, `:381`); keep `test_aperture` a separate knob, not the SAM size |
| `sim/tools/gen_test_patterns.py` | source dst/base from the SAM config, not hardcoded `dst_cid<<32` (`:151-153`) |
| c_model subordinate config (`AxiSlave::set_memory_bounds` / `memory_base_addr`) | set tile-local base (0) under rebase (watch-list #4) |
| `sim/dv/floonoc-test/axi_reorder_compare.sv` (vendored) | normalize `.addr` to tile-local offset before compare (`:211-213`); small patch, see watch-list #5 |
| `docs/architecture.md` | rewrite sec 4.3 (`:70-75`) + the routing description (`:105-108`): SAM remap replaces the "no remap table" decode text. (Also `:99` "no router class" is separately stale; fix if touched, do not scope-creep.) |

**SAM delivery path:** the c_model reads the `address_map` YAML directly (via the existing yaml-cpp dep,
matching `CLAUDE.md` "c_model config = YAML"). The DPI `create` passes only the config path; no table is
marshalled across the DPI boundary. Both ctest and co-sim read the same YAML the generators read, so the
table cannot drift from the tb's `REGION_BASE`.

**Watch-list #5 (corrected — not uniformly transparent):**
- directed `axi_scoreboard` — **transparent**: master face only (`user_node_endpoint.sv:252`), keys on the
  master global address, symmetric write/read. No change.
- MAPPED `rand_slave` — **transparent**: sparse associative memory keyed by the received (now local) address;
  consistent write/read local addr matches.
- `axi_reorder_compare` (constrained_random) — **NOT transparent as-is**: `step_2` compares the whole AW/AR
  struct incl. `.addr`, masking only `.id` (`axi_reorder_compare.sv:211-213`), so it assumes master-face
  addr == slave-face addr. Under rebase they differ (global vs local) → false "AW mismatch".
  Resolution: **compare the tile-local offset, not the global address** — extend the existing `.id`-masking
  pattern (line 210-212, "modified in the NI") to normalize `.addr` to its within-tile offset on both faces
  before compare. The offset is the remap-invariant (unchanged whether rebased or not), so the check works
  for `rebase: true` and `false` alike, and the CR axis exercises rebase too. No coverage lost: mis-routing
  is still caught by the `aw_queue[slv_idx]` structure (`:179`,`:208`), low-bit corruption by the offset
  compare. Implementation (plan): pass the region base + a rebase flag so the slave face normalizes as
  `rebase ? addr : addr - base` (master is always `addr - base`); or mask `addr & (tile_size-1)` when tiles
  are uniform power-of-two. This is a small, justified patch to the vendored `axi_reorder_compare.sv`.

Response path needs no reverse map: NSU stores request `src_id` in the MetaBuffer for the return trip
(`depacketize.hpp`), B/R payloads carry no address.

## Testing (TDD)

1. `translate` unit tests: hit → correct `dst_id` + rebased `local_addr`; miss → assert; footprint-guard
   assert on a burst that would cross a tile boundary.
2. validator unit tests: reject overlap / zero-size / overflow / non-4 KB-aligned / out-of-mesh dst.
3. Default uniform `tile_size = 4 GB` reproduces today's routing bit-for-bit (regression anchor).
4. Co-sim: directed scoreboard clean on `mesh_4x4_vc1` with `rebase: true` (slave sees 0-based local,
   round-trip data matches); non-uniform tile map smoke.
5. Confirm co-sim SV masters (`axi_file_master` / `axi_rand_master`) never issue a burst crossing a tile
   boundary for INCR / FIXED / WRAP — the COMPUTE-step-5 footprint guard is the live check; a directed
   WRAP/FIXED case near a tile edge exercises it. (Codex flagged pulp masters' FIXED/WRAP 4 KB behavior as
   unverified; the guard makes any violation fail loud rather than mis-route.)

## Out of scope (YAGNI)

- Region-split at the NI (Decision A) — the AXI master pre-splits.
- DECERR response modelling on SAM miss (separate RSP-category feature).
- Narrowing `AXI_ADDR_WIDTH` 64→32 (FlooNoC keeps full width; optimization only).
- Arbitrary irregular maps beyond per-tile size + the chosen base-assignment scheme.
