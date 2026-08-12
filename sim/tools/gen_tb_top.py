#!/usr/bin/env python3
"""Generate sim/tb/tb_top_<topology>.sv + src/sv/noc_fabric_<topo>.sv from a topology config.

The fabric/tb split:
  - noc_fabric_<topo>.sv : N nodes, each = NMU + REQ/RSP router_wrap + NSU, joined
    by inter-router directional (N/E/S/W) links with boundary tie-off + assertion.
    Every node exposes a clean per-node AXI port (master-side + slave-side). The
    DPI `ctx` handles arrive as PORTS — the fabric itself does no cmodel_*_create.
  - tb_top_<topology>.sv : clk/rst, watchdog, cmodel_*_create (router/nmu/nsu ctx),
    instantiates the fabric, attaches a user_node_endpoint (pulp axi_file_master +
    axi_delayer + axi_sim_mem + in-endpoint scoreboard + bw monitor) per node + exit
    logic.

Generated artifacts: edit the generator or the topology YAML, never the emitted
.sv directly. tb_top_<topology>.sv includes the fabric (SV `include), resolved via
the -I src/sv include path.

Usage:
    python3 gen_tb_top.py [--topology mesh_4x4_vc1] [--out sim/tb/tb_top_<topology>.sv]

Parameterised from topology YAML:
    - nodes list [(x,y), ...] from x_dim x y_dim
    - node_id = (y << X_WIDTH) | x  (coordinate-encoded; == linear index for 1-D)
    - per-node router/nmu/nsu ctx handles; TILE_BASE_ADDR / TILE_SIZE, each node's
      own tile-crossbar windows, from address_map.node_windows() (see
      tile_targets below), stamped into each endpoint
    - inter-router links wired per XY direction; boundary directions tied off
    - PASS guard: all endpoints done (end_of_sim) AND every node non-vacuous
      (txn_cnt > 0)

Constants kept as template (not derived from topology YAML):
    - clk/rst timing (10 ns clock, 4-cycle reset); load-scaled watchdog
    - localparam width constants from ni_params_pkg, DPI signatures
    - perf instrumentation, FSDB block, DPI error poll structure
"""

import argparse
import sys
from pathlib import Path

import address_map

ROOT = Path(__file__).resolve().parents[2]

# Coordinate-id composition must mirror c_model addr_trans / route_compute:
# dst_id = (y << X_WIDTH) | x, X in the low bits. X_WIDTH from the spec (4).
X_WIDTH = 4

# RouterPort enum (router.hpp): LOCAL=0, NORTH=1, EAST=2, SOUTH=3, WEST=4.
RP = {"LOCAL": 0, "NORTH": 1, "EAST": 2, "SOUTH": 3, "WEST": 4}
LINK_PORTS = 5


def load_topology(name: str) -> dict:
    import yaml
    base = name[:-4] if name.endswith("_rob") else name
    path = ROOT / "sim" / "topologies" / f"{base}.yaml"
    topo = yaml.safe_load(path.read_text())
    _check_flit_capacity(topo, path)
    return topo


# Y_WIDTH / VC_ID_WIDTH mirror the flit spec (ni_packet.json field_widths).
Y_WIDTH = 4
VC_ID_WIDTH = 3
DST_ID_WIDTH = X_WIDTH + Y_WIDTH  # 8 bits → 256 max nodes


def _check_flit_capacity(topo: dict, path) -> None:
    """Reject a topology whose mesh dims / num_vc exceed the flit field capacity,
    or whose mesh dims are below the per-dimension minimum.

    Mirrors specgen/ni_spec/invariants.py:check_mesh_within_flit for the
    sim-topology-YAML path (X/Y/node + VC bounds).  Fails with a clear message so
    the user knows to reduce dims / num_vc or widen the flit fields (via the
    specgen constants).

    Mesh dim minimum is 2 per dimension (mesh_x_dim >= 2 AND mesh_y_dim >= 2):
    a mesh communicating through NI + router needs at least 2x2. 1x1 and 1xN
    meshes are illegal (specgen/source/constants.yaml MESH_X_DIM/MESH_Y_DIM min).
    """
    t = topo["topology"]
    x_dim = int(t["x_dim"])
    y_dim = int(t["y_dim"])
    num_vc = int(t["num_vc"])
    cap_x = 1 << X_WIDTH
    cap_y = 1 << Y_WIDTH
    cap_nodes = 1 << DST_ID_WIDTH
    cap_vc = 1 << VC_ID_WIDTH
    errors = []
    if x_dim < 2:
        errors.append(f"x_dim={x_dim} < 2 (mesh dimension minimum is 2; 1x1/1xN meshes are illegal)")
    if y_dim < 2:
        errors.append(f"y_dim={y_dim} < 2 (mesh dimension minimum is 2; 1x1/1xN meshes are illegal)")
    if x_dim > cap_x:
        errors.append(f"x_dim={x_dim} > 2^X_WIDTH={cap_x}")
    if y_dim > cap_y:
        errors.append(f"y_dim={y_dim} > 2^Y_WIDTH={cap_y}")
    if x_dim * y_dim > cap_nodes:
        errors.append(f"x_dim*y_dim={x_dim * y_dim} > 2^DST_ID_WIDTH={cap_nodes}")
    if num_vc > cap_vc:
        errors.append(f"num_vc={num_vc} > 2^VC_ID_WIDTH={cap_vc}")
    if errors:
        raise SystemExit(
            f"gen_tb_top: flit-capacity violated in {path}:\n"
            + "\n".join(f"  {e}" for e in errors)
        )


# ---------------------------------------------------------------------------
# Topology model
# ---------------------------------------------------------------------------

def _coord_id(x: int, y: int) -> int:
    """Coordinate-encoded node id = route_compute dst_id = (y<<X_WIDTH)|x."""
    return (y << X_WIDTH) | x


def _nodes(topo: dict):
    """Return ordered node list: [(idx, x, y, coord_id), ...] in (y,x) raster order.

    idx is the linear emit index (0..N-1); coord_id is the routing id. The two
    coincide only for nodes in mesh row y=0 (coord_id's y field is 0 there), so
    a 2x2 mesh's first row stays byte-identical to the prior 1-D gen.
    """
    x_dim = topo["topology"]["x_dim"]
    y_dim = topo["topology"]["y_dim"]
    out = []
    idx = 0
    for y in range(y_dim):
        for x in range(x_dim):
            out.append((idx, x, y, _coord_id(x, y)))
            idx += 1
    return out, x_dim, y_dim


# REGION_BYTES: the per-node tile-memory window. DV-side tb constant
# (FlooNoC mesh tb pattern), not a runtime knob. Distinct from
# gen_test_patterns.py's auto-derived directed-side region_bytes (a different,
# per-run-derived value of the same name).
_DEFAULT_REGION_BYTES = 0x1000

# Tile-memory latency, as axi_delayer settings for every endpoint's two
# memories: (stall_random_input, stall_random_output, fixed_delay_input,
# fixed_delay_output). Input covers AW/W/AR, output covers B/R. DV-side tb
# constants like REGION_BYTES above, not a runtime knob: switch profile by
# editing _MEM_LATENCY and rebuilding.
#
#   ideal   fixed delay 0 with no random stall takes stream_delay's
#           gen_pass_through branch, so the delayer is wires. The FABRIC is the
#           bottleneck, which is what every measurement to date assumed and
#           what the injection sweep needs -- a stalling memory hides fabric
#           saturation.
#   random  upstream's random-stall mode, both directions. lfsr_16bit drives a
#           $clog2(16)=4-bit reload, so a stalled handshake waits 0-15 cycles.
#           Backpressure reaches the NSU and travels back through the fabric,
#           which is how the NMU outstanding pool and the RoB fill get
#           exercised at all.
_MEM_LATENCY_PROFILES = {
    "ideal": (0, 0, 0, 0),
    "random": (1, 1, 0, 0),
}
_MEM_LATENCY = "ideal"

# Worst-case cycles a stalled handshake holds, from lfsr_16bit's
# refill_way_bin width ($clog2(16) = 4 bits). The watchdog is sized off it.
_STALL_RANDOM_MAX_CYCLES = 15


def tile_targets(topo: dict, nodes):
    """Each node's own crossbar windows, in target PORT ORDER (m0 = config,
    m1 = data).

    Port order and field packing are ONE coupled invariant: target t occupies
    field t of the packed parameters, and user_node_endpoint puts the config
    memory on target 0 and the data memory on the last target. The check below
    is what stops an address_map.py SPACE_ORDER edit from transposing the two
    silently.

    Per node, not shared: the crossbar decodes on THIS node's windows so that
    anything the local initiator addresses elsewhere falls through to the
    default master port and goes onto the NoC. A request arriving from the
    fabric always lands in these windows -- the NSU rewrites its
    node-coordinate field to this node first (nsu::Depacketize::rebase_).

    Returns ({node_idx: [{"space", "base", "size"}, ...]}, noc_egress_base).
    """
    x_dim = topo["topology"]["x_dim"]
    y_dim = topo["topology"]["y_dim"]
    _bases, entries = address_map.pack(topo.get("address_map"), x_dim, y_dim)
    out = {}
    for idx, _x, _y, cid in nodes:
        windows = address_map.node_windows(entries, cid)
        order = [w["space"] for w in windows]
        # Spelled out here rather than read back from address_map.SPACE_ORDER:
        # this is the cross-check on that constant, not a restatement of it.
        if order != ["config", "memory"]:
            raise SystemExit(
                f"gen_tb_top: node {idx} tile space order {order} must be "
                f"config-then-memory -- user_node_endpoint puts the config memory "
                f"on target 0 and the data memory on the last target "
                f"(see address_map.SPACE_ORDER)")
        out[idx] = windows
    return out, address_map.noc_egress_base(entries)


# Live-neighbor map / opposite-port logic now lives in the emitted SV genvar
# generate (localparams HAS_N/E/S/W + PEER_N/E/S/W; +y=NORTH, RP_* opposite
# pairs), driven by the raster index. The Python model only needs _nodes() to
# stamp per-node coords/ids; the fabric instances are a single generate loop.

# ---------------------------------------------------------------------------
# Network descriptor — S3a T5. Three physical networks, each carried
# uniformly across LOCAL + N/E/S/W (router_wrap.sv's own per-port array
# shape). flow selects the field/width shape:
#   ready_valid (REQ/RSP): scalar-per-port [LINK_PORTS-1:0] valid + a
#     [LINK_PORTS-1:0] "return" vector (ready), single VC (S1 Q2).
#   credit (DAT): scalar-per-port [LINK_PORTS-1:0] valid + a per-port
#     [DAT_NUM_VC-1:0] "return" vector (crdvalid), DAT_NUM_VC virtual channels.
# DAT additionally routes its LOCAL port through dat_merge_wrap (the NI-level
# merge point, S3a T5 controller ruling) instead of connecting ni_wrap
# directly to router_wrap like REQ/RSP do.
_NETWORKS = (
    # (name, flit_width_sym, flow)
    ("req", "NOC_REQ_FLIT_WIDTH_DFLT", "ready_valid"),
    ("rsp", "NOC_RSP_FLIT_WIDTH_DFLT", "ready_valid"),
    ("dat", "NOC_DAT_FLIT_WIDTH_DFLT", "credit"),
)

# Directional pairing for the inter-node LINK wiring (mirrors _nodes()'s
# +y=NORTH raster convention): this node's direction <- peer's OPPOSITE
# direction OUT.
_LINK_DIRS = (
    ("NORTH", "HAS_N", "PEER_N", "SOUTH"),
    ("EAST",  "HAS_E", "PEER_E", "WEST"),
    ("SOUTH", "HAS_S", "PEER_S", "NORTH"),
    ("WEST",  "HAS_W", "PEER_W", "EAST"),
)


# ---------------------------------------------------------------------------
# Fabric emitter — noc_fabric_<topo>.sv
# ---------------------------------------------------------------------------

def emit_fabric(topo: dict) -> str:
    name = topo["topology"]["name"]
    nodes, x_dim, y_dim = _nodes(topo)
    n = len(nodes)
    num_vc = topo["topology"]["num_vc"]
    guard = f"NOC_FABRIC_{name.upper()}_SV"

    lines = []
    w = lines.append

    w("`timescale 1ns/1ps")
    w("")
    w(f"// AUTO-GENERATED by sim/tools/gen_tb_top.py")
    w(f"// Fabric for topology {name} ({x_dim}x{y_dim}, dat_num_vc={num_vc}).")
    w("// DO NOT EDIT - modify the generator or sim/topologies/*.yaml instead.")
    w("//")
    w("// N nodes, each = ni_wrap (nmu+nsu+dat_merge) + REQ/RSP/DAT router_wrap,")
    w("// joined by inter-router directional links (N/E/S/W). Boundary directions")
    w("// are tied off; a tied-off direction DRIVING a valid flit is a $fatal")
    w("// (guards a fabric wiring mistake; the C++ route leak is caught by")
    w("// route_compute's abort). The DPI ctx handles arrive as ports; the fabric")
    w("// does no cmodel_*_create. Each node exposes a master-side AXI port (NMU")
    w("// ingress) and a slave-side AXI port (NSU egress).")
    w("")
    w(f"`ifndef {guard}")
    w(f"`define {guard}")
    w("")
    w(f"module noc_fabric_{name} #(")
    w("    parameter int unsigned ID_WIDTH       = ni_params_pkg::AXI_ID_WIDTH_DFLT,")
    w("    parameter int unsigned ADDR_WIDTH     = ni_params_pkg::AXI_ADDR_WIDTH_DFLT,")
    w("    parameter int unsigned DATA_WIDTH     = ni_params_pkg::AXI_DATA_WIDTH_DFLT,")
    w(f"    parameter int unsigned DAT_NUM_VC     = {num_vc},")
    w("    parameter int unsigned REQ_FLIT_WIDTH = ni_params_pkg::NOC_REQ_FLIT_WIDTH_DFLT,")
    w("    parameter int unsigned RSP_FLIT_WIDTH = ni_params_pkg::NOC_RSP_FLIT_WIDTH_DFLT,")
    w("    parameter int unsigned DAT_FLIT_WIDTH = ni_params_pkg::NOC_DAT_FLIT_WIDTH_DFLT,")
    w("    // ROUTER_VC_DEPTH: per-VC input FIFO depth inside the DAT router; also the")
    w("    // credit seed the DAT merge point's downstream pool is initialized with, so")
    w("    // both ends of every link agree on the credit window.")
    w("    parameter int unsigned ROUTER_VC_DEPTH       = "
      "ni_params_pkg::NOC_ROUTER_VC_DEPTH_DFLT")
    w(") (")
    w("    input  logic clk_i,")
    w("    input  logic rst_ni,")
    # ctx handles + AXI faces as PER-NODE ARRAYS (longint unsigned chandle-subst
    # + packed-struct), so the node instances collapse into a genvar generate.
    w(f"    // Per-node DPI ctx handle arrays (chandle-substitute longint unsigned).")
    w(f"    input  longint unsigned router_ctx     [{n}],")
    w(f"    input  longint unsigned nmu_ctx        [{n}],")
    w(f"    input  longint unsigned nsu_ctx        [{n}],")
    w(f"    input  longint unsigned dat_merge_ctx  [{n}],")
    w(f"    // Per-node AXI faces (struct arrays): NMU ingress (driven by tb master)")
    w(f"    input  ni_signals_pkg::axi_req_t  master_axi_req [{n}],")
    w(f"    // AWUSER sideband: collective op + address mask.")
    w(f"    // Dedicated array beside the struct (axi_req_t has no awuser field).")
    w(f"    input  logic [ni_params_pkg::AXI_AWUSER_WIDTH_DFLT-1:0] master_awuser [{n}],")
    w(f"    output ni_signals_pkg::axi_rsp_t  master_axi_rsp [{n}],")
    w(f"    // Per-node AXI faces (struct arrays): NSU egress (consumed by tb slave)")
    w(f"    output ni_signals_pkg::axi_req_t  slave_axi_req  [{n}],")
    w(f"    input  ni_signals_pkg::axi_rsp_t  slave_axi_rsp  [{n}]")
    w(");")
    w("")
    w(f"    localparam int unsigned NUM_NODES = {n};")
    w(f"    localparam int unsigned X_DIM     = {x_dim};")
    w(f"    localparam int unsigned Y_DIM     = {y_dim};")
    w(f"    localparam int unsigned LINK_PORTS = {LINK_PORTS};  // LOCAL + N/E/S/W")
    w("    // RouterPort direction indices (router.hpp enum).")
    for d, v in (("LOCAL", 0), ("NORTH", 1), ("EAST", 2), ("SOUTH", 3), ("WEST", 4)):
        w(f"    localparam int unsigned RP_{d} = {v};")
    w("")

    # Per-network per-node per-port arrays. Every network (incl. DAT) uses the
    # SAME uniform shape (LOCAL + N/E/S/W in one array) matching router_wrap.sv;
    # only the "return" field (ready vs crdvalid) differs by flow.
    w("    // Per-network per-node per-port arrays (LOCAL + N/E/S/W uniformly).")
    for net, width_sym, flow in _NETWORKS:
        W = f"ni_params_pkg::{width_sym}"
        w(f"    logic [LINK_PORTS-1:0]        tx_{net}_valid [{n}];")
        w(f"    logic [{W}-1:0]  tx_{net}_flit  [{n}][LINK_PORTS];")
        w(f"    logic [LINK_PORTS-1:0]        rx_{net}_valid [{n}];")
        w(f"    logic [{W}-1:0]  rx_{net}_flit  [{n}][LINK_PORTS];")
        if flow == "ready_valid":
            w(f"    logic [LINK_PORTS-1:0]        tx_{net}_ready [{n}];  // input to router_wrap")
            w(f"    logic [LINK_PORTS-1:0]        rx_{net}_ready [{n}];  // output of router_wrap")
        else:
            w(f"    logic [DAT_NUM_VC-1:0]        tx_{net}_crdvalid [{n}][LINK_PORTS];  // input")
            w(f"    logic [DAT_NUM_VC-1:0]        rx_{net}_crdvalid [{n}][LINK_PORTS];  // output")
        w("")

    # ------------------------------------------------------------------
    # Node generate loop: ni_wrap (= NMU+NSU+dat_merge) + router_wrap +
    # per-node link wiring + boundary tie-off + perf monitors. Coordinates
    # from the linear index mirror _nodes() raster order: X = i % X_DIM,
    # Y = i / X_DIM, so the routing id (y<<X_WIDTH)|x is preserved for every
    # node. Neighbor indices are computed inline (NORTH=i+X_DIM, EAST=i+1,
    # SOUTH=i-X_DIM, WEST=i-1) with the same boundary guards _neighbors()
    # applies in the Python model.
    # ------------------------------------------------------------------
    w("    // -------------------------------------------------------------------------")
    w("    // Per-node generate: ni_wrap + router_wrap + link wiring + perf monitors")
    w("    // -------------------------------------------------------------------------")
    w("    for (genvar i = 0; i < NUM_NODES; i++) begin : g_node")
    w("        localparam int unsigned X = i % X_DIM;")
    w("        localparam int unsigned Y = i / X_DIM;")
    w("        // Live-neighbor flags + peer linear indices (boundary -> tied off).")
    w("        localparam bit HAS_N = (Y + 1 < Y_DIM);")
    w("        localparam bit HAS_E = (X + 1 < X_DIM);")
    w("        localparam bit HAS_S = (Y >= 1);")
    w("        localparam bit HAS_W = (X >= 1);")
    w("        localparam int unsigned PEER_N = i + X_DIM;")
    w("        localparam int unsigned PEER_E = i + 1;")
    w("        localparam int unsigned PEER_S = i - X_DIM;")
    w("        localparam int unsigned PEER_W = i - 1;")
    w("")
    w("        ni_wrap #(")
    w("            .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),")
    w("            .DAT_NUM_VC(DAT_NUM_VC), .REQ_FLIT_WIDTH(REQ_FLIT_WIDTH),")
    w("            .RSP_FLIT_WIDTH(RSP_FLIT_WIDTH), .DAT_FLIT_WIDTH(DAT_FLIT_WIDTH)")
    w("        ) u_ni (")
    w("            .clk_i(clk_i), .rst_ni(rst_ni),")
    w("            .nmu_ctx_i(nmu_ctx[i]), .nsu_ctx_i(nsu_ctx[i]),")
    w("            .dat_merge_ctx_i(dat_merge_ctx[i]),")
    w("            .master_axi_req_i(master_axi_req[i]), .master_awuser_i(master_awuser[i]),")
    w("            .master_axi_rsp_o(master_axi_rsp[i]),")
    w("            .slave_axi_req_o(slave_axi_req[i]),   .slave_axi_rsp_i(slave_axi_rsp[i]),")
    # ni_wrap's tx_*/rx_* are named from ITS OWN view (the opposite end of the
    # LOCAL link from router_wrap's view), so they cross here exactly like the
    # inter-node LINK wiring below crosses tx_*[peer] -> rx_*[this node]: NI's
    # tx_* (its own transmit) feeds the router's rx_*[LOCAL] (its receive),
    # and NI's rx_* (its own receive) is fed BY the router's tx_*[LOCAL].
    w("            .tx_req_valid_o(rx_req_valid[i][RP_LOCAL]),")
    w("            .tx_req_flit_o(rx_req_flit[i][RP_LOCAL]),")
    w("            .tx_req_ready_i(rx_req_ready[i][RP_LOCAL]),")
    w("            .rx_req_valid_i(tx_req_valid[i][RP_LOCAL]),")
    w("            .rx_req_flit_i(tx_req_flit[i][RP_LOCAL]),")
    w("            .rx_req_ready_o(tx_req_ready[i][RP_LOCAL]),")
    w("            .tx_rsp_valid_o(rx_rsp_valid[i][RP_LOCAL]),")
    w("            .tx_rsp_flit_o(rx_rsp_flit[i][RP_LOCAL]),")
    w("            .tx_rsp_ready_i(rx_rsp_ready[i][RP_LOCAL]),")
    w("            .rx_rsp_valid_i(tx_rsp_valid[i][RP_LOCAL]),")
    w("            .rx_rsp_flit_i(tx_rsp_flit[i][RP_LOCAL]),")
    w("            .rx_rsp_ready_o(tx_rsp_ready[i][RP_LOCAL]),")
    w("            .tx_dat_valid_o(rx_dat_valid[i][RP_LOCAL]),")
    w("            .tx_dat_flit_o(rx_dat_flit[i][RP_LOCAL]),")
    w("            .tx_dat_crdvalid_i(rx_dat_crdvalid[i][RP_LOCAL]),")
    w("            .rx_dat_valid_i(tx_dat_valid[i][RP_LOCAL]),")
    w("            .rx_dat_flit_i(tx_dat_flit[i][RP_LOCAL]),")
    w("            .rx_dat_crdvalid_o(tx_dat_crdvalid[i][RP_LOCAL])")
    w("        );")
    w("")
    w("        router_wrap #(")
    w("            .DAT_NUM_VC(DAT_NUM_VC), .REQ_FLIT_WIDTH(REQ_FLIT_WIDTH),")
    w("            .RSP_FLIT_WIDTH(RSP_FLIT_WIDTH), .DAT_FLIT_WIDTH(DAT_FLIT_WIDTH),")
    w("            .LINK_PORTS(LINK_PORTS)")
    w("        ) u_router (")
    w("            .clk_i(clk_i), .rst_ni(rst_ni), .ctx_i(router_ctx[i]),")
    for net, _width_sym, flow in _NETWORKS:
        ret = "ready" if flow == "ready_valid" else "crdvalid"
        w(f"            .tx_{net}_valid(tx_{net}_valid[i]), .tx_{net}_flit(tx_{net}_flit[i]),")
        w(f"            .tx_{net}_{ret}(tx_{net}_{ret}[i]),")
        w(f"            .rx_{net}_valid(rx_{net}_valid[i]), .rx_{net}_flit(rx_{net}_flit[i]),")
        w(f"            .rx_{net}_{ret}(rx_{net}_{ret}[i])"
          + ("," if net != _NETWORKS[-1][0] else ""))
    w("        );")
    w("")
    # Per-node IN-face wiring: drive every LINK direction slot (N/E/S/W --
    # LOCAL is driven by ni_wrap's direct port connection above, so this loop
    # explicitly excludes it). A live direction takes the peer's opposite OUT
    # slot; a boundary direction stays at the tie-off default (0). One
    # always_comb per network.
    for net, _width_sym, flow in _NETWORKS:
        ret = "ready" if flow == "ready_valid" else "crdvalid"
        w(f"        always_comb begin : link_{net}_in")
        w(f"            for (int p = 1; p < LINK_PORTS; p++) begin")
        w(f"                rx_{net}_valid[i][p]  = 1'b0;")
        w(f"                rx_{net}_flit[i][p]   = '0;")
        w(f"                tx_{net}_{ret}[i][p]  = '0;")
        w(f"            end")
        for d, has, peer, pd in _LINK_DIRS:
            w(f"            if ({has}) begin  // {d}: <- peer {pd} OUT")
            w(f"                rx_{net}_valid[i][RP_{d}] = tx_{net}_valid[{peer}][RP_{pd}];")
            w(f"                rx_{net}_flit[i][RP_{d}]  = tx_{net}_flit[{peer}][RP_{pd}];")
            w(f"                tx_{net}_{ret}[i][RP_{d}] = rx_{net}_{ret}[{peer}][RP_{pd}];")
            w(f"            end")
        w(f"        end")
        w("")
    # Boundary tie-off assertion: a boundary direction must never drive OUT valid.
    w("        // Boundary tie-off assertion: a boundary direction (no neighbor)")
    w("        // must never drive OUT valid. Fires on a fabric wiring mistake; the")
    w("        // C++ route leak (dst outside mesh) is caught upstream by")
    w("        // route_compute's abort.")
    w("        always_ff @(posedge clk_i) begin")
    w("            if (rst_ni) begin")
    for d, has, _peer, _pd in _LINK_DIRS:
        for net, _width_sym, _flow in _NETWORKS:
            w(f"                if (!{has} && tx_{net}_valid[i][RP_{d}])")
            w(f'                    $fatal(1, "noc_fabric: node%0d drove a flit on '
              f'tied-off {d} ({net}) - fabric link wiring mistake", i);')
    w("        end")
    w("        end")
    w("")
    # Link perf monitors: one per live neighbor direction, per network.
    # FLOW selects the credit/ready_valid branch inside link_perf_monitor
    # (S3a T5 §8): ready/valid counts stall_cyc as valid && !ready and drops
    # the credit array. Mirrors the prior emit — every directed (node -> peer)
    # edge gets a monitor, named "{net}_{i}to{peer}".
    w("        // Inter-router link perf monitors (passive): one per live")
    w("        // neighbor direction, named {net}_{i}to{peer}. vc_id bit window")
    w("        // from ni_flit_pkg; DAT credit_pulse is per-VC (not OR-collapsed).")
    for d, has, peer, _pd in _LINK_DIRS:
        w(f"        if ({has}) begin : g_perf_{d.lower()}")
        for net, _width_sym, flow in _NETWORKS:
            flit_wire = f"tx_{net}_flit[i][RP_{d}]"
            vc_slice = f"{flit_wire}[ni_flit_pkg::VC_ID_MSB:ni_flit_pkg::VC_ID_LSB]"
            w(f"            link_perf_monitor #(")
            w(f'                .LINK_NAME($sformatf("{net}_%0dto%0d", i, {peer})),')
            w(f'                .FLOW("{flow}"),')
            w(f"                .BUFFER_DEPTH(ROUTER_VC_DEPTH),")
            w(f"                .NUM_VC(DAT_NUM_VC), .VC_ID_WIDTH(ni_flit_pkg::VC_ID_WIDTH)")
            w(f"            ) u_perf_link_{net} (")
            w(f"                .clk_i, .rst_ni,")
            w(f"                .valid(tx_{net}_valid[i][RP_{d}]),")
            if flow == "ready_valid":
                w(f"                .ready(tx_{net}_ready[i][RP_{d}]),")
                w(f"                .vc_id('0),")
                w(f"                .credit_pulse('0)")
            else:
                w(f"                .ready(1'b0),")  # unused in credit flow
                w(f"                .vc_id({vc_slice}),")
                w(f"                .credit_pulse(tx_{net}_crdvalid[i][RP_{d}])")
            w(f"            );")
        w(f"        end")
    w("    end : g_node")
    w("")

    w(f"endmodule")
    w("")
    w(f"`endif  // {guard}")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# tb_top emitter — instantiates the fabric + pulp VIP endpoints + exit logic
# ---------------------------------------------------------------------------

def emit_tb_top(topo: dict, requested_name: str = "") -> str:
    name = topo["topology"]["name"]
    nodes, x_dim, y_dim = _nodes(topo)
    n = len(nodes)
    num_vc = topo["topology"]["num_vc"]
    rob_enabled = requested_name.endswith("_rob")

    # Tile crossbar windows, m0 first (see tile_targets). Emitted as PACKED-array
    # concatenations in descending index order (last first) so field t is target t
    # and row i is node i. Packed because Verilator 5.048 mis-sizes an
    # unpacked-array param override whose size depends on a sibling param
    # override (here TILE_TARGETS).
    #
    # Per node, not shared: each endpoint decodes on its OWN windows, so anything
    # its initiator addresses elsewhere misses both rules and falls through to the
    # default master port, which is the NMU.
    stall_in, stall_out, delay_in, delay_out = _MEM_LATENCY_PROFILES[_MEM_LATENCY]
    # Per handshake, whichever of the two mechanisms is armed on that direction.
    mem_cyc_per_beat = max(
        _STALL_RANDOM_MAX_CYCLES if stall_in else delay_in,
        _STALL_RANDOM_MAX_CYCLES if stall_out else delay_out,
    )
    per_node, noc_egress_base = tile_targets(topo, nodes)
    n_targets = len(per_node[0])
    # ADDR_WIDTH'(...) casts, not sized literals: the field width has to follow
    # ni_params_pkg::AXI_ADDR_WIDTH_DFLT, or a width change would silently
    # mis-align the concatenation.
    def _rows(key):
        return ", ".join(
            "{" + ", ".join(f"ADDR_WIDTH'(64'h{t[key]:X})"
                             for t in reversed(per_node[i])) + "}"
            for i in reversed(range(n)))
    tile_base_addr = _rows("base")
    tile_size = _rows("size")

    lines = []
    w = lines.append

    w("`timescale 1ns/1ps")
    w("")
    w("// AUTO-GENERATED by sim/tools/gen_tb_top.py")
    w(f"// Topology: {name}  ({x_dim}x{y_dim}, dat_num_vc={num_vc})")
    w("// DO NOT EDIT - modify the generator or sim/topologies/*.yaml instead.")
    w("//")
    w(f"// {n} nodes live inside noc_fabric_{name} (ni_wrap=NMU+NSU + REQ/RSP router per")
    w("// node, joined by directional links). tb_top creates the DPI handles, attaches a")
    w("// user_node_endpoint (pulp axi_file_master + axi_delayer/axi_sim_mem + FlooNoC")
    w("// axi_bw_monitor) to each node's master/slave AXI faces, and owns the exit logic.")
    w("// Checking: pulp axi_scoreboard lives inside each endpoint on master_dv,")
    w("// comparing read data end-to-end through the NoC against golden write data.")
    w("//")
    w("// Self-clocked: clk_i/rst_ni are internal logic (10 ns clock, 4-cycle reset).")
    w("// Plusargs: +num_reads=<n> +num_writes=<n> (per node); seed via +verilator+seed+<N>.")
    w("")
    w("`ifndef TB_TOP_SV")
    w("`define TB_TOP_SV")
    w("")
    w(f'`include "noc_fabric_{name}.sv"')
    w("")
    w("module tb_top;")
    w("    logic clk_i  = 1'b0;")
    w("    logic rst_ni = 1'b0;")
    w("    always #5 clk_i = ~clk_i;")
    w("    initial begin")
    w("        repeat (4) @(posedge clk_i);")
    w("        rst_ni = 1'b1;")
    w("    end")
    w("")
    w("    // -------------------------------------------------------------------------")
    w("    // Parameters")
    w("    // -------------------------------------------------------------------------")
    w(f"    localparam int unsigned NUM_NODES     = {n};")
    w("    localparam int unsigned ID_WIDTH      = ni_params_pkg::AXI_ID_WIDTH_DFLT;")
    w("    localparam int unsigned ADDR_WIDTH    = ni_params_pkg::AXI_ADDR_WIDTH_DFLT;")
    w("    localparam int unsigned DATA_WIDTH    = ni_params_pkg::AXI_DATA_WIDTH_DFLT;")
    w(f"    localparam int unsigned DAT_NUM_VC     = {num_vc};  // from topology YAML")
    w("    localparam int unsigned REQ_FLIT_WIDTH = ni_params_pkg::NOC_REQ_FLIT_WIDTH_DFLT;")
    w("    localparam int unsigned RSP_FLIT_WIDTH = ni_params_pkg::NOC_RSP_FLIT_WIDTH_DFLT;")
    w("    localparam int unsigned DAT_FLIT_WIDTH = ni_params_pkg::NOC_DAT_FLIT_WIDTH_DFLT;")
    w("    // ROUTER_VC_DEPTH: credit window for inter-router links; passed to fabric so")
    w("    // link_perf_monitor tracks the actual receiving buffer depth.")
    w("    localparam int unsigned ROUTER_VC_DEPTH       = "
      "ni_params_pkg::NOC_ROUTER_VC_DEPTH_DFLT;")
    w("    // Tile crossbar windows, one field per target in port order (m0 =")
    w("    // config, last = data), one row per node. Each endpoint decodes on its")
    w("    // OWN windows: a hit is tile-local and never touches the NoC, a miss")
    w("    // falls through to the default master port and goes onto the NoC.")
    w("    // REGION_BYTES = the DV region_bytes constant (NOT a tile size -- that")
    w("    // would blow up MAX_BURST_BEATS below).")
    w(f"    localparam int unsigned TILE_TARGETS = {n_targets};")
    w(f"    localparam logic [{n - 1}:0][TILE_TARGETS-1:0][ADDR_WIDTH-1:0] TILE_BASE_ADDR = "
      f"{{{tile_base_addr}}};")
    w(f"    localparam logic [{n - 1}:0][TILE_TARGETS-1:0][ADDR_WIDTH-1:0] TILE_SIZE = "
      f"{{{tile_size}}};")
    w("    // NoC egress aperture: where a collective write is offset to so the tile")
    w("    // crossbar routes it to the NI instead of answering it locally. Derived")
    w("    // from the map (address_map.noc_egress_base), so it can never collide.")
    w(f"    localparam logic [ADDR_WIDTH-1:0] NOC_EGRESS_BASE = "
      f"ADDR_WIDTH'(64'h{noc_egress_base:X});")
    w(f"    localparam longint unsigned REGION_BYTES = 64'h{_DEFAULT_REGION_BYTES:X};")
    w(f'    // Tile-memory latency profile "{_MEM_LATENCY}" (gen_tb_top.py')
    w("    // _MEM_LATENCY_PROFILES). Every endpoint's two memories sit behind an")
    w("    // axi_delayer carrying these settings; input covers AW/W/AR, output B/R.")
    w(f"    localparam bit          MEM_STALL_RANDOM_INPUT  = 1'b{stall_in};")
    w(f"    localparam bit          MEM_STALL_RANDOM_OUTPUT = 1'b{stall_out};")
    w(f"    localparam int unsigned MEM_FIXED_DELAY_INPUT   = {delay_in};")
    w(f"    localparam int unsigned MEM_FIXED_DELAY_OUTPUT  = {delay_out};")
    w("")
    w("    // -------------------------------------------------------------------------")
    w("    // Liveness trace. The watchdog below reports that time ran out; these two")
    w("    // registers say where it stopped. last_progress is the cycle of a node's")
    w("    // most recent master-side handshake, axi_outstanding the AW/AR it has")
    w("    // issued and not yet retired. A wedged node reads as a stale")
    w("    // last_progress with axi_outstanding > 0; a node that simply finished")
    w("    // reads as axi_outstanding == 0. Without these a timeout snapshot cannot")
    w("    // separate a freeze at cycle 500 from one at cycle 99999.")
    w("    // -------------------------------------------------------------------------")
    w("    int unsigned live_cyc = 0;")
    w("    int unsigned last_progress  [NUM_NODES];")
    w("    int unsigned axi_outstanding[NUM_NODES];")
    w("")
    w("    always_ff @(posedge clk_i) begin")
    w("        if (!rst_ni) begin")
    w("            live_cyc <= 0;")
    w("            for (int i = 0; i < NUM_NODES; i++) begin")
    w("                last_progress[i]   <= 0;")
    w("                axi_outstanding[i] <= 0;")
    w("            end")
    w("        end else begin")
    w("            live_cyc <= live_cyc + 1;")
    w("            for (int i = 0; i < NUM_NODES; i++) begin")
    w("                automatic logic aw = master_axi_req[i].awvalid && master_axi_rsp[i].awready;")
    w("                automatic logic ar = master_axi_req[i].arvalid && master_axi_rsp[i].arready;")
    w("                automatic logic wb = master_axi_req[i].wvalid  && master_axi_rsp[i].wready;")
    w("                automatic logic bh = master_axi_rsp[i].bvalid  && master_axi_req[i].bready;")
    w("                automatic logic rl = master_axi_rsp[i].rvalid  && master_axi_req[i].rready")
    w("                                     && master_axi_rsp[i].rlast;")
    w("                // W beats count as progress: a node mid-burst is moving, not wedged.")
    w("                if (aw || ar || wb || bh || rl) last_progress[i] <= live_cyc;")
    w("                axi_outstanding[i] <= axi_outstanding[i]")
    w("                                      + int'(aw) + int'(ar) - int'(bh) - int'(rl);")
    w("            end")
    w("        end")
    w("    end")
    w("")
    w("    // -------------------------------------------------------------------------")
    w("    // Watchdog - sized by worst-case beats in flight. Two per-beat costs,")
    w("    // named separately because only one of them moves with the latency")
    w("    // profile:")
    w("    //   fabric  measured vc1 rate is ~15-30 cycles per R/W beat (credit window")
    w("    //           4, all nodes contending); 40 adds margin.")
    w("    //   memory  the axi_delayer above. stream_delay holds one handshake at a")
    w("    //           time on each channel, so its bound is per beat on the")
    w("    //           request side and again on the response side, not per")
    w("    //           transaction.")
    w("    // MAX_BURST_BEATS caps the largest burst per REGION_BYTES.")
    w("    // -------------------------------------------------------------------------")
    w("    localparam int unsigned TIMEOUT_BASE        = 100000;")
    w("    localparam int unsigned FABRIC_CYC_PER_BEAT = 40;")
    w(f"    localparam int unsigned MEM_CYC_PER_BEAT    = {mem_cyc_per_beat};")
    w("    localparam int unsigned K_CYC_PER_BEAT  = FABRIC_CYC_PER_BEAT + MEM_CYC_PER_BEAT;")
    w("    localparam int unsigned MAX_BURST_BEATS = "
      "int'(REGION_BYTES) / (DATA_WIDTH / 8);")
    w("    int unsigned tb_num_reads  = 8;   // mirror endpoint defaults")
    w("    int unsigned tb_num_writes = 8;")
    w('    import "DPI-C" context function void cmodel_dump_fabric_state();')
    w("    initial begin")
    w("        int unsigned timeout_cycles;")
    w('        void\'($value$plusargs("num_reads=%d",  tb_num_reads));')
    w('        void\'($value$plusargs("num_writes=%d", tb_num_writes));')
    w("        timeout_cycles = TIMEOUT_BASE")
    w("            + K_CYC_PER_BEAT * (tb_num_reads + tb_num_writes) * MAX_BURST_BEATS * NUM_NODES;")
    w("        // Forensics override: fire the watchdog just past a known freeze")
    w("        // point so the state dump lands without waiting out the formula.")
    w('        void\'($value$plusargs("timeout_cycles=%d", timeout_cycles));')
    w("        repeat (timeout_cycles) @(posedge clk_i);")
    w("        // Per-node SV-side summary, then the c_model fabric state dump.")
    w("        for (int i = 0; i < NUM_NODES; i++) begin")
    w('            $display("[WATCHDOG] node%0d txn_cnt=%0d end_of_sim=%0d outstanding=%0d last_progress=%0d (idle %0d cyc) mst[awv=%0d wv=%0d arv=%0d rr=%0d br=%0d] slv[awv=%0d wv=%0d arv=%0d rv=%0d bv=%0d]",')
    w("                     i, txn_cnt[i], end_of_sim[i],")
    w("                     axi_outstanding[i], last_progress[i], live_cyc - last_progress[i],")
    w("                     master_axi_req[i].awvalid, master_axi_req[i].wvalid,")
    w("                     master_axi_req[i].arvalid, master_axi_req[i].rready,")
    w("                     master_axi_req[i].bready,")
    w("                     slave_axi_req[i].awvalid, slave_axi_req[i].wvalid,")
    w("                     slave_axi_req[i].arvalid,")
    w("                     slave_axi_rsp[i].rvalid, slave_axi_rsp[i].bvalid);")
    w("        end")
    w("        cmodel_dump_fabric_state();")
    w('        $fatal(1, "tb_top: timeout after %0d cycles", timeout_cycles);')
    w("    end")
    w("")
    w("    // -------------------------------------------------------------------------")
    w("    // DPI lifecycle")
    w("    // -------------------------------------------------------------------------")
    w('    import "DPI-C" context function void    cmodel_init();')
    w('    import "DPI-C" context function void    cmodel_finalize();')
    w('    import "DPI-C" context function longint unsigned cmodel_router_create(input string name,')
    w('                                                                  input int x_coord, input int y_coord,')
    w('                                                                  input int mesh_x_dim, input int mesh_y_dim,')
    w('                                                                  input int num_vc);')
    w('    import "DPI-C" context function int unsigned cmodel_nmu_read_slot_hwm(input longint unsigned ctx);')
    w('    import "DPI-C" context function void cmodel_nmu_admission_telemetry(input longint unsigned ctx,')
    w("                                                                 output int unsigned aw_idle_bypass,")
    w("                                                                 output int unsigned aw_same_dest_bypass,")
    w("                                                                 output int unsigned aw_fallback_alloc,")
    w("                                                                 output int unsigned ar_idle_bypass,")
    w("                                                                 output int unsigned ar_same_dest_bypass,")
    w("                                                                 output int unsigned ar_fallback_alloc,")
    w("                                                                 output int unsigned order_list_hwm,")
    w("                                                                 output int unsigned write_txns_hwm,")
    w("                                                                 output int unsigned read_txns_hwm);")
    # create_ex in both RoB modes: the RoB depths and the per-id order-list depth
    # apply to either, and create_ex is the call that carries them.
    w('    import "DPI-C" context function longint unsigned cmodel_nmu_create_ex(input string name,')
    w('                                                                 input int src_id, input int num_vc,')
    w('                                                                 input int rob_enabled,')
    w('                                                                 input int b_rob_depth,')
    w('                                                                 input int r_rob_depth,')
    w('                                                                 input int max_txns_per_id,')
    w('                                                                 input string config_path);')
    w('    import "DPI-C" context function longint unsigned cmodel_nsu_create(input string name,')
    w('                                                              input int src_id, input int num_vc,')
    w('                                                              input int max_unique_ids,')
    w('                                                              input int max_outstanding,')
    w('                                                              input string config_path);')
    w('    import "DPI-C" context function longint unsigned cmodel_dat_merge_create(input string name,')
    w('                                                                    input int dat_num_vc);')
    w("")

    # ctx handle ARRAYS (chandle-substitute longint unsigned). Arrays let the
    # fabric take them as array ports and instantiate nodes via a genvar loop.
    w(f"    longint unsigned router_ctx     [{n}];")
    w(f"    longint unsigned nmu_ctx        [{n}];")
    w(f"    longint unsigned nsu_ctx        [{n}];")
    w(f"    longint unsigned dat_merge_ctx  [{n}];")
    w("")
    w("    // SAM config: topology YAML with an address_map block. Empty (the")
    w("    // default) keeps each NMU's default 16x16 uniform, 4 GB/tile SAM.")
    w('    string sam_config_path = "";')
    w("")
    w("    // NSU knobs. max_unique_ids=1 collapses every master onto one downstream")
    w("    // AXI id (FlooNoC default); 2**AXI_ID_WIDTH passes the master's id through.")
    w("    // max_outstanding is the shared MetaBuffer pool per direction.")
    w("    int unsigned max_unique_ids  = ni_params_pkg::NSU_META_BUFFER_MAX_UNIQUE_IDS_DFLT;")
    w("    int unsigned max_outstanding = ni_params_pkg::NSU_META_BUFFER_MAX_OUTSTANDING_DFLT;")
    w("")
    w("    // NMU RoB pool depths, per direction. Both <= 256 (ordering_tag is 8 bits).")
    w("    int unsigned b_rob_depth = ni_params_pkg::NMU_ROB_B_DEPTH_DFLT;")
    w("    int unsigned r_rob_depth = ni_params_pkg::NMU_ROB_R_DEPTH_DFLT;")
    w("    // Per-AXI-ID order-list depth (FlooNoC MaxRoTxnsPerId).")
    w("    int unsigned max_txns_per_id = ni_params_pkg::NMU_MAX_TXNS_PER_ID_DFLT;")
    w("")

    # cmodel_init (no-arg) + per-node router/nmu/nsu create.
    w("    initial begin")
    w("        cmodel_init();")
    w('        void\'($value$plusargs("sam_config=%s", sam_config_path));')
    w('        void\'($value$plusargs("max_unique_ids=%d", max_unique_ids));')
    w('        void\'($value$plusargs("max_outstanding=%d", max_outstanding));')
    w('        $display("[Config] max_unique_ids=%0d max_outstanding=%0d", max_unique_ids, max_outstanding);')
    if rob_enabled:
        w('        void\'($value$plusargs("b_rob_depth=%d", b_rob_depth));')
        w('        void\'($value$plusargs("r_rob_depth=%d", r_rob_depth));')
        w('        void\'($value$plusargs("max_txns_per_id=%d", max_txns_per_id));')
    for (i, x, y, _c) in nodes:
        w(f'        router_ctx[{i}] = cmodel_router_create("router_{i}", {x}, {y}, '
          f'{x_dim}, {y_dim}, DAT_NUM_VC);')
    for (i, x, y, c) in nodes:
        w(f'        nmu_ctx[{i}] = cmodel_nmu_create_ex("nmu_{i}", {c}, DAT_NUM_VC, '
          f'{1 if rob_enabled else 0}, b_rob_depth, r_rob_depth, max_txns_per_id, '
          f'sam_config_path);  '
          f'// src_id = node{i} coord {c}, ROB {"Enabled" if rob_enabled else "Disabled"}')
        w(f'        nsu_ctx[{i}] = cmodel_nsu_create("nsu_{i}", {c}, DAT_NUM_VC, max_unique_ids, '
          f'max_outstanding, sam_config_path);')
        w(f'        dat_merge_ctx[{i}] = cmodel_dat_merge_create("dat_merge_{i}", DAT_NUM_VC);')
    w("    end")
    w("")

    # Per-node AXI buses as STRUCT ARRAYS (master-side into the fabric NMU,
    # slave-side out of NSU), shared by the fabric + the endpoint generate loop.
    w("    // -------------------------------------------------------------------------")
    w("    // Per-node AXI buses (struct arrays): master-side into NMU, slave-side out of NSU")
    w("    // -------------------------------------------------------------------------")
    w(f"    ni_signals_pkg::axi_req_t  master_axi_req [{n}];  // tb master -> NMU")
    w(f"    logic [ni_params_pkg::AXI_AWUSER_WIDTH_DFLT-1:0] master_awuser [{n}];  // AWUSER sideband")
    w(f"    ni_signals_pkg::axi_rsp_t  master_axi_rsp [{n}];  // NMU -> tb master")
    w(f"    ni_signals_pkg::axi_req_t  slave_axi_req  [{n}];  // NSU -> tb slave")
    w(f"    ni_signals_pkg::axi_rsp_t  slave_axi_rsp  [{n}];  // tb slave -> NSU")
    w("")

    # Fabric instance: ctx + AXI arrays passed whole.
    w("    // -------------------------------------------------------------------------")
    w(f"    // NoC fabric ({n} nodes, directional links)")
    w("    // -------------------------------------------------------------------------")
    w(f"    noc_fabric_{name} #(")
    w("        .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),")
    w("        .DAT_NUM_VC(DAT_NUM_VC), .REQ_FLIT_WIDTH(REQ_FLIT_WIDTH),")
    w("        .RSP_FLIT_WIDTH(RSP_FLIT_WIDTH), .DAT_FLIT_WIDTH(DAT_FLIT_WIDTH),")
    w("        .ROUTER_VC_DEPTH(ROUTER_VC_DEPTH)")
    w("    ) u_fabric (")
    w("        .clk_i(clk_i), .rst_ni(rst_ni),")
    w("        .router_ctx(router_ctx), .nmu_ctx(nmu_ctx), .nsu_ctx(nsu_ctx),")
    w("        .dat_merge_ctx(dat_merge_ctx),")
    w("        .master_axi_req(master_axi_req), .master_awuser(master_awuser),")
    w("        .master_axi_rsp(master_axi_rsp),")
    w("        .slave_axi_req(slave_axi_req),   .slave_axi_rsp(slave_axi_rsp)")
    w("    );")
    w("")

    # Test endpoints per node via genvar generate. user_node_endpoint = pulp
    # file_master + pulp axi_xbar tile crossbar + two delayed sim memories + bw monitor.
    # user_node_endpoint.sv is USER-OWNED (committed, hand-written); the
    # generator only INSTANTIATES it and stamps the tile windows.
    w("    // -------------------------------------------------------------------------")
    w("    // Test endpoints - one user_node_endpoint per node (pulp file_master +")
    w("    // axi_xbar tile crossbar + two axi_delayer/axi_sim_mem targets +")
    w("    // in-endpoint scoreboard +")
    w("    // bw monitor). user_node_endpoint.sv is user-owned and NOT regenerated.")
    w("    // -------------------------------------------------------------------------")
    w(f"    logic        end_of_sim [{n}];")
    w(f"    int unsigned txn_cnt    [{n}];")
    w(f"    for (genvar i = 0; i < {n}; i++) begin : g_endpoint")
    w("        user_node_endpoint #(")
    w("            .NODE_ID(i),")
    w("            .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),")
    w("            .TILE_TARGETS(TILE_TARGETS), .TILE_BASE_ADDR(TILE_BASE_ADDR[i]),")
    w("            .TILE_SIZE(TILE_SIZE[i]), .NOC_EGRESS_BASE(NOC_EGRESS_BASE),")
    w("            .MEM_STALL_RANDOM_INPUT(MEM_STALL_RANDOM_INPUT),")
    w("            .MEM_STALL_RANDOM_OUTPUT(MEM_STALL_RANDOM_OUTPUT),")
    w("            .MEM_FIXED_DELAY_INPUT(MEM_FIXED_DELAY_INPUT),")
    w("            .MEM_FIXED_DELAY_OUTPUT(MEM_FIXED_DELAY_OUTPUT)")
    w("        ) u_endpoint (")
    w("            .clk_i(clk_i), .rst_ni(rst_ni),")
    w("            .master_axi_req_o(master_axi_req[i]), .master_awuser_o(master_awuser[i]),")
    w("            .master_axi_rsp_i(master_axi_rsp[i]),")
    w("            .slave_axi_req_i(slave_axi_req[i]),   .slave_axi_rsp_o(slave_axi_rsp[i]),")
    w("            .end_of_sim_o(end_of_sim[i]), .txn_cnt_o(txn_cnt[i])")
    w("        );")
    w("    end : g_endpoint")
    w("")

    # Perf instrumentation.
    w("    // -------------------------------------------------------------------------")
    w("    // Perf instrumentation - sample every rising edge; dump on final")
    w("    // -------------------------------------------------------------------------")
    w('    import "DPI-C" context function void cmodel_perf_sample_tick();')
    w('    import "DPI-C" context function void cmodel_perf_set_run(input string scenario,')
    w('                                                             input longint total_cyc);')
    w('    import "DPI-C" context function void cmodel_perf_dump(input string path);')
    w("")
    w('    string        perf_out_path = "perf.json";')
    w('    string        perf_scn      = "";')
    w("    int unsigned  perf_cycle    = 0;")
    w("    initial begin")
    w('        void\'($value$plusargs("perf_out=%s", perf_out_path));')
    w('        void\'($value$plusargs("perf_scenario=%s", perf_scn));')
    w("    end")
    w("    always @(posedge clk_i) begin")
    w("        cmodel_perf_sample_tick();")
    w("        perf_cycle = perf_cycle + 1;")
    w("    end")
    w("")
    w("    final begin")
    w("        cmodel_perf_set_run(perf_scn, longint'(perf_cycle));")
    w("        cmodel_perf_dump(perf_out_path);")
    w("        cmodel_finalize();")
    w("    end")
    w("")
    w("    // FSDB waveform dump (VCS only; +define+FSDB_DUMP)")
    w("`ifdef FSDB_DUMP")
    w("    initial begin")
    w("        string fsdb_path;")
    w('        if (!$value$plusargs("fsdb=%s", fsdb_path))')
    w('            fsdb_path = "dump.fsdb";')
    w("        $fsdbDumpfile(fsdb_path);")
    w("        $fsdbDumpvars(0, tb_top);")
    w("    end")
    w("`endif")
    w("")

    # Exit logic — non-vacuous PASS guard: wait all endpoints done, settle, then
    # require every node moved at least one AW/AR handshake.
    w("    // -------------------------------------------------------------------------")
    w("    // Exit logic - non-vacuous PASS guard")
    w("    // -------------------------------------------------------------------------")
    w("    localparam int unsigned SETTLE_CYCLES = 100;")
    w("    initial begin")
    w("        bit vacuous;")
    w("        bit all_done;")
    w("        int unsigned aw_idle, aw_same, aw_alloc, ar_idle, ar_same, ar_alloc;")
    w("        int unsigned list_hwm, wtxn_hwm, rtxn_hwm;")
    w("        // clock-polled (not wait()): end_of_sim is driven through port")
    w("        // aliases; Verilator --timing wait() on it does not wake reliably.")
    w("        do begin")
    w("            @(posedge clk_i);")
    w("            all_done = rst_ni;")
    w("            for (int i = 0; i < NUM_NODES; i++)")
    w("                all_done &= end_of_sim[i];  // scoreboard is in-endpoint")
    w("        end while (!all_done);")
    w("        repeat (SETTLE_CYCLES) @(posedge clk_i);")
    w("        vacuous = 1'b0;")
    w("        for (int i = 0; i < NUM_NODES; i++) begin")
    w("            if (txn_cnt[i] == 0) begin")
    w("                vacuous = 1'b1;")
    w('                $display("FAIL: node%0d completed zero transactions (vacuous)", i);')
    w("            end")
    w("        end")
    w('        if (vacuous) $fatal(1, "tb_top: vacuous run");')
    w("        // Sizing telemetry per node: RoB slot peak, the SPEC 17 admission")
    w("        // clause split, the per-id order-list peak and the shared-pool peaks.")
    w("        for (int i = 0; i < NUM_NODES; i++) begin")
    w("            cmodel_nmu_admission_telemetry(nmu_ctx[i], aw_idle, aw_same, aw_alloc,")
    w("                                           ar_idle, ar_same, ar_alloc,")
    w("                                           list_hwm, wtxn_hwm, rtxn_hwm);")
    w('            $display("[HWM] node=%0d read_slot_hwm=%0d order_list_hwm=%0d write_txns_hwm=%0d read_txns_hwm=%0d aw_clause={idle=%0d same_dest=%0d alloc=%0d} ar_clause={idle=%0d same_dest=%0d alloc=%0d}",')
    w("                     i, cmodel_nmu_read_slot_hwm(nmu_ctx[i]),")
    w("                     list_hwm, wtxn_hwm, rtxn_hwm,")
    w("                     aw_idle, aw_same, aw_alloc, ar_idle, ar_same, ar_alloc);")
    w("        end")
    w('        $display("PASS: all %0d nodes done, non-vacuous", NUM_NODES);')
    w("        $finish(0);")
    w("    end")
    w("")
    w("    // -------------------------------------------------------------------------")
    w("    // Centralized DPI error poll")
    w("    // -------------------------------------------------------------------------")
    w('    import "DPI-C" context function int cmodel_check_error(output string msg);')
    w("")
    w("    always_ff @(posedge clk_i) begin")
    w("        /* verilator lint_off WIDTHTRUNC */")
    w("        if (rst_ni) begin")
    w("            string dpi_err_msg;")
    w("            int    dpi_err_code;")
    w("            dpi_err_code = cmodel_check_error(dpi_err_msg);")
    w("            if (dpi_err_code != 0) begin")
    w('                $display("[tb_top] DPI fatal (code=%0d): %s",')
    w("                         dpi_err_code, dpi_err_msg);")
    w("                cmodel_finalize();")
    w('                $fatal(1, "tb_top: DPI error, simulation aborted");')
    w("            end")
    w("        end")
    w("        /* verilator lint_on WIDTHTRUNC */")
    w("    end")
    w("")
    w("endmodule")
    w("")
    w("`endif  // TB_TOP_SV")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def _fabric_path(out_path: Path, topo: dict) -> Path:
    return ROOT / "src" / "sv" / f"noc_fabric_{topo['topology']['name']}.sv"


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate tb_top.sv + noc_fabric_<topo>.sv.")
    ap.add_argument("--topology", default="mesh_4x4_vc1",
                    help="Topology name (matches sim/topologies/<name>.yaml)")
    ap.add_argument("--out", default=None,
                    help="Output tb_top.sv path (default: sim/tb/tb_top_<topology>.sv; "
                         "fabric emitted alongside)")
    a = ap.parse_args()

    topo = load_topology(a.topology)
    tb_text = emit_tb_top(topo, a.topology)
    fab_text = emit_fabric(topo)
    out_path = Path(a.out) if a.out is not None else \
        ROOT / "sim" / "tb" / f"tb_top_{a.topology}.sv"
    fab_path = _fabric_path(out_path, topo)

    out_path.write_text(tb_text, encoding="utf-8")
    fab_path.write_text(fab_text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
