# Fabric data-mismatch debug log — RESOLVED 2026-07-01

Round goal: fix the post-AR-drop data-mismatch worklist (`BUR-002`/`BUR-003`@hotspot). **Resolved.**
Full write-up in `docs/backlog.md` (Bugs → "Root cause — test-generator slot overlap").

## Verdict

Root cause = **test pattern generator slot overlap**, NOT a fabric / nmu / nsu / router / MetaBuffer
bug. `sim/tools/gen_test_patterns.py` `alloc_unique_offset` spaced slots by a fixed `_SLOT_STRIDE=0x40`
while a burst reserves `(len+1)*2**size` bytes (BUR-002=256B, BUR-003=8192B). Offsets were distinct in
value but their footprints overlapped; under many-to-one (hotspot) neighbouring sources overwrote each
other and the readback came back off by exactly one stride (0x40). `neighbor` passed because bijection
gives one writer per tile.

Fixed by `stride = max(stride, reserved)` + auto-grow `memory_size` (branch `fix/hotspot-slot-overlap`).
Verified: full `make sim-regress` 8 builds = pass=400 fail=0.

## Hypotheses — final status

| # | hypothesis | status |
|---|---|---|
| generator slot overlap (footprint > stride, many-to-one) | **CONFIRMED — root cause** |
| H-A | NSU MetaBuffer bid-only FIFO misroute (many-to-one same-id) | REFUTED — modeled path preserves same-id order (Codex read-only). Real but LATENT; deferred (FlooNoC `id_queue` id-remap). |
| H-E | MasterWrap B/R inject lacks a one-beat-per-handshake gate (double-inject) | REFUTED — the STR-001/ORD-002 B-assert was the `WireSlavePort` AW-replay (fixed separately, commit 85bd6a0), not a MasterWrap double-inject. |
| H-RAW | readback AR races the write (read-before-write) | REFUTED — the readback returned a valid NEIGHBOUR value (a clean −0x40 whole-burst shift), not a stale/zero value; RAW would read init/zero from a source's own unique slot. |

## How it was cracked

The captured `sim/regress/output/run_prune.log` already held the mismatch report: `actual = expected -
0x40`, a clean monotonic whole-burst shift (not ragged corruption). Combined with `_SLOT_STRIDE=0x40`
keyed on `src_node` in the allocator, that pinned it to slot overlap — diagnosed from artifacts + code,
no sim re-run. Discriminator worth reusing: a clean neighbour-value shift ⇒ overlap/misroute; a
stale/zero value ⇒ RAW.
