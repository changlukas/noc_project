# Master-face R backpressure

Design: `docs/superpowers/specs/2026-08-12-master-face-backpressure-design.md`. Read it first.
It carries the wiring, the parameter chain, the id-signal table and the known limitation.

Two stages. Stage 1 proves the insertion is transparent, Stage 2 turns it on. Splitting them is
the point: if a gate fails after the flip, Stage 1 has already ruled out the wiring.

## Stage 1: insert the delayer, profile `ideal`

Goal: `axi_delayer_intf` wired between `master_dv` and the tile crossbar, both parameters
plumbed from `gen_tb_top.py` through the generated tb to `user_node_endpoint`, with
`_MST_BACKPRESSURE = "ideal"`. At `(0, 0)` `stream_delay` takes its `gen_pass_through` branch, so
the path is wires and nothing should move.

Files: `sim/tb/user_node_endpoint.sv`, `sim/tools/gen_tb_top.py`.

Success Criteria:
- `mesh_2x2_vc1` and `mesh_4x4_vc1` `PATTERN=neighbor` DIRECTED PASS
- `mesh_2x2_vc1 PATTERN=multicast` DIRECTED PASS
- `make pytest` green, since `sim/tools/` is touched
- The four id signals follow the design's table. `mst_awid` / `mst_arid` move to
  `mst_post_delay`, `mst_bid` / `mst_rid` stay on `tile_axi[0]`

Status: Complete

## Stage 2: turn it on

Goal: `_MST_BACKPRESSURE = "random"`, the knob shown to act, the gate set re-run, and the two
documentation notes written.

Files: `sim/tools/gen_tb_top.py`, `docs/known-limitations.md`, wherever `sim-injection-sweep` is
run from.

Success Criteria:
- `master_axi_req_o.rready` observed low while `master_axi_rsp_i.rvalid` is high. The knob is
  proven to act before its presence counts as coverage
- The same four gates as Stage 1, now with backpressure on. **A failure here is a finding about
  the fabric or a checker and is this round's work. It is not a reason to change the default**
- `+mcast_fault` and `+decerr_fault` still trip their checkers
- `sim-injection-sweep`'s `"ideal"` requirement written where the sweep is run from
- The `Seed` limitation recorded in `docs/known-limitations.md`

Status: Not Started

## Next

Stage D of the NI id-width round runs after this lands. Its context is in `docs/backlog.md`.
