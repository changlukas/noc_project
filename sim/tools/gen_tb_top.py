#!/usr/bin/env python3
"""Generate sim/sv/tb_top_<topology>.sv + sim/sv/noc_fabric_<topo>.sv from a topology config.

The fabric/tb split (S3):
  - noc_fabric_<topo>.sv : N nodes, each = NMU + REQ/RSP router_wrap + NSU, joined
    by inter-router directional (N/E/S/W) links with boundary tie-off + assertion.
    Every node exposes a clean per-node AXI port (master-side + slave-side). The
    DPI `ctx` handles arrive as PORTS — the fabric itself does no cmodel_*_create.
  - tb_top_<topology>.sv : clk/rst, plusargs, cmodel_*_create (incl. router/nmu/nsu/
    master/slave ctx), instantiates the fabric, attaches test master_wrap/slave_wrap
    + AXI perf monitors + scoreboard exit logic.

Generated artifacts: edit the generator or the topology YAML, never the emitted
.sv directly. tb_top_<topology>.sv includes the fabric (SV `include), so the fabric
is compiled via the existing -I sim/sv include path (no build_config.mk edit needed).

Usage:
    python3 gen_tb_top.py [--topology mesh_4x4_vc1] [--out sim/sv/tb_top_<topology>.sv]
    python3 gen_tb_top.py --check            # drift gate (exit 1 if stale)

Parameterised from topology YAML:
    - nodes list [(x,y), ...] from x_dim x y_dim
    - node_id = (y << X_WIDTH) | x  (coordinate-encoded; == linear index for 1-D)
    - per-node plusarg names (+scenario_node<i>), scenario strings, ctx handles
    - master at node k uses node k's scenario (identity pairing); addr bit 32+
      encodes the destination tile (neighbor or any address-driven dst)
    - inter-router links wired per XY direction; boundary directions tied off
    - PASS guard: cmodel_master_count() == len(nodes) AND reads_checked() >= len(nodes)

Constants kept as template (not derived from topology YAML):
    - clk/rst timing (10 ns clock, 4-cycle reset), TIMEOUT_CYCLES = 100000
    - localparam width constants from ni_params_pkg, DPI signatures
    - perf instrumentation, FSDB block, DPI error poll structure
"""

import argparse
import difflib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Coordinate-id composition must mirror c_model addr_trans / route_compute:
# dst_id = (y << X_WIDTH) | x, X in the low bits. X_WIDTH from the spec (4).
X_WIDTH = 4

# RouterPort enum (router.hpp): LOCAL=0, NORTH=1, EAST=2, SOUTH=3, WEST=4.
RP = {"LOCAL": 0, "NORTH": 1, "EAST": 2, "SOUTH": 3, "WEST": 4}
LINK_PORTS = 5


def load_topology(name: str) -> dict:
    import yaml
    path = ROOT / "sim" / "topologies" / f"{name}.yaml"
    topo = yaml.safe_load(path.read_text())
    _check_flit_capacity(topo, path)
    return topo


# Y_WIDTH / VC_ID_WIDTH mirror the flit spec (ni_packet.json field_widths).
Y_WIDTH = 4
VC_ID_WIDTH = 3
DST_ID_WIDTH = X_WIDTH + Y_WIDTH  # 8 bits → 256 max nodes


def _check_flit_capacity(topo: dict, path) -> None:
    """Reject a topology whose mesh dims / num_vc exceed the flit field capacity.

    Mirrors specgen/ni_spec/invariants.py:check_mesh_within_flit for the
    sim-topology-YAML path (X/Y/node + VC bounds).  Fails with a clear message so
    the user knows to reduce dims / num_vc or widen the flit fields (via the
    specgen constants).
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

    idx is the linear emit index (0..N-1); coord_id is the routing id. For a 1-D
    mesh the two coincide, so 2x1 output stays byte-identical to the prior gen.
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


# Live-neighbor map / opposite-port logic now lives in the emitted SV genvar
# generate (localparams HAS_N/E/S/W + PEER_N/E/S/W; +y=NORTH, RP_* opposite
# pairs), driven by the raster index. The Python model only needs _nodes() to
# stamp per-node coords/ids; the fabric instances are a single generate loop.

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
    w(f"// Fabric for topology {name} ({x_dim}x{y_dim}, num_vc={num_vc}).")
    w("// DO NOT EDIT - modify the generator or sim/topologies/*.yaml instead.")
    w("//")
    w("// N nodes, each = ni_wrap (nmu+nsu) + REQ/RSP router_wrap, joined by")
    w("// inter-router directional links (N/E/S/W). Boundary directions are tied")
    w("// off; a tied-off direction DRIVING a valid flit is a $fatal (guards a fabric")
    w("// wiring mistake; the C++ route leak is caught by route_compute's abort). The")
    w("// DPI ctx handles arrive as ports;")
    w("// the fabric does no cmodel_*_create. Each node exposes a master-side AXI")
    w("// port (NMU ingress) and a slave-side AXI port (NSU egress).")
    w("")
    w(f"`ifndef {guard}")
    w(f"`define {guard}")
    w("")
    w(f"module noc_fabric_{name} #(")
    w("    parameter int unsigned ID_WIDTH              = ni_params_pkg::AXI_ID_WIDTH_DFLT,")
    w("    parameter int unsigned ADDR_WIDTH            = ni_params_pkg::AXI_ADDR_WIDTH_DFLT,")
    w("    parameter int unsigned DATA_WIDTH            = ni_params_pkg::AXI_DATA_WIDTH_DFLT,")
    w(f"    parameter int unsigned NUM_VC                = {num_vc},")
    w("    parameter int unsigned FLIT_WIDTH            = ni_params_pkg::NOC_FLIT_WIDTH_DFLT,")
    w("    parameter int unsigned SLAVE_VC_BUFFER_DEPTH = "
      "ni_params_pkg::NOC_SLAVE_VC_BUFFER_DEPTH_DFLT,")
    w("    // ROUTER_VC_DEPTH: per-VC input FIFO depth inside the router; this is")
    w("    // the credit window for INTER-ROUTER links (distinct from SLAVE_VC_BUFFER_DEPTH")
    w("    // which governs the router->NSU eject path).  Both derive from constants.yaml;")
    w("    // they coincide at the default of 4 but must be threaded separately so the")
    w("    // link_perf_monitor assertion holds at ANY configurable depth.")
    w("    parameter int unsigned ROUTER_VC_DEPTH       = "
      "ni_params_pkg::NOC_ROUTER_VC_DEPTH_DFLT")
    w(") (")
    w("    input  logic clk_i,")
    w("    input  logic rst_ni,")
    # ctx handles + AXI faces as PER-NODE ARRAYS (longint unsigned chandle-subst
    # + packed-struct), so the node instances collapse into a genvar generate.
    w(f"    // Per-node DPI ctx handle arrays (chandle-substitute longint unsigned).")
    w(f"    input  longint unsigned router_ctx [{n}],")
    w(f"    input  longint unsigned nmu_ctx    [{n}],")
    w(f"    input  longint unsigned nsu_ctx    [{n}],")
    w(f"    // Per-node AXI faces (struct arrays): NMU ingress (driven by tb master)")
    w(f"    input  ni_signals_pkg::axi_req_t  master_axi_req [{n}],")
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

    # Per-node NoC struct arrays (NI<->router connections).
    w("    // Per-node NoC struct arrays: NI<->router connections.")
    w(f"    ni_signals_pkg::noc_chan_t          node_req         [{n}];  // NMU req -> router")
    w(f"    noc_types_pkg::noc_credit_t         node_req_cred    [{n}];  // router credit -> NMU")
    w(f"    ni_signals_pkg::noc_chan_t          node_rsp         [{n}];  // router rsp -> NMU")
    w(f"    noc_types_pkg::noc_credit_t         node_rsp_cred    [{n}];  // NMU rsp credit -> router")
    w(f"    ni_signals_pkg::noc_chan_t          node_nsu_req     [{n}];  // router req -> NSU")
    w(f"    noc_types_pkg::noc_credit_t         node_nsu_req_cred[{n}];  // NSU credit -> router")
    w(f"    ni_signals_pkg::noc_chan_t          node_nsu_rsp     [{n}];  // NSU rsp -> router")
    w(f"    noc_types_pkg::noc_credit_t         node_nsu_rsp_cred[{n}];  // router rsp credit -> NSU")
    w("")

    # Per-node LINK face arrays, indexed [node][LINK_PORTS]. Replaces the
    # Python-unrolled n{i}_link_* nets so the wiring/instances can be a genvar loop.
    w("    // Per-node inter-router LINK faces, indexed [node][direction].")
    w(f"    logic [LINK_PORTS-1:0]   link_req_out_valid  [{n}];")
    w(f"    logic [LINK_PORTS-1:0]   link_rsp_out_valid  [{n}];")
    w(f"    logic [FLIT_WIDTH-1:0]   link_req_out_flit   [{n}][LINK_PORTS];")
    w(f"    logic [FLIT_WIDTH-1:0]   link_rsp_out_flit   [{n}][LINK_PORTS];")
    w(f"    logic [NUM_VC-1:0]       link_req_in_credit  [{n}][LINK_PORTS];")
    w(f"    logic [NUM_VC-1:0]       link_rsp_in_credit  [{n}][LINK_PORTS];")
    w(f"    logic [LINK_PORTS-1:0]   link_req_in_valid   [{n}];")
    w(f"    logic [LINK_PORTS-1:0]   link_rsp_in_valid   [{n}];")
    w(f"    logic [FLIT_WIDTH-1:0]   link_req_in_flit    [{n}][LINK_PORTS];")
    w(f"    logic [FLIT_WIDTH-1:0]   link_rsp_in_flit    [{n}][LINK_PORTS];")
    w(f"    logic [NUM_VC-1:0]       link_req_out_credit [{n}][LINK_PORTS];")
    w(f"    logic [NUM_VC-1:0]       link_rsp_out_credit [{n}][LINK_PORTS];")
    w("")

    # ------------------------------------------------------------------
    # Node generate loop: ni_wrap (= NMU+NSU) + router_wrap + per-node link
    # wiring + boundary tie-off + perf monitors. Coordinates from the linear
    # index mirror _nodes() raster order: X = i % X_DIM, Y = i / X_DIM, so the
    # routing id (y<<X_WIDTH)|x is preserved for every node. Neighbor indices
    # are computed inline (NORTH=i+X_DIM, EAST=i+1, SOUTH=i-X_DIM, WEST=i-1)
    # with the same boundary guards _neighbors() applies in the Python model.
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
    w("            .NUM_VC(NUM_VC), .FLIT_WIDTH(FLIT_WIDTH), "
      ".SLAVE_VC_BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH)")
    w("        ) u_ni (")
    w("            .clk_i(clk_i), .rst_ni(rst_ni),")
    w("            .nmu_ctx_i(nmu_ctx[i]), .nsu_ctx_i(nsu_ctx[i]),")
    w("            .master_axi_req_i(master_axi_req[i]), .master_axi_rsp_o(master_axi_rsp[i]),")
    w("            .slave_axi_req_o(slave_axi_req[i]),   .slave_axi_rsp_i(slave_axi_rsp[i]),")
    w("            .noc_req_o(node_req[i]), .noc_req_cred_i(node_req_cred[i]),")
    w("            .noc_rsp_i(node_rsp[i]), .noc_rsp_cred_o(node_rsp_cred[i]),")
    w("            .noc_req_i(node_nsu_req[i]), .noc_req_cred_o(node_nsu_req_cred[i]),")
    w("            .noc_rsp_o(node_nsu_rsp[i]), .noc_rsp_cred_i(node_nsu_rsp_cred[i])")
    w("        );")
    w("")
    w("        router_wrap #(")
    w("            .NUM_VC(NUM_VC), .FLIT_WIDTH(FLIT_WIDTH), "
      ".SLAVE_VC_BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH),")
    w("            .LINK_PORTS(LINK_PORTS)")
    w("        ) u_router (")
    w("            .clk_i(clk_i), .rst_ni(rst_ni), .ctx_i(router_ctx[i]),")
    w("            .noc_nmu_req_i(node_req[i]), .noc_nmu_req_cred_o(node_req_cred[i]),")
    w("            .noc_nmu_rsp_o(node_rsp[i]), .noc_nmu_rsp_cred_i(node_rsp_cred[i]),")
    w("            .noc_nsu_req_o(node_nsu_req[i]), .noc_nsu_req_cred_i(node_nsu_req_cred[i]),")
    w("            .noc_nsu_rsp_i(node_nsu_rsp[i]), .noc_nsu_rsp_cred_o(node_nsu_rsp_cred[i]),")
    w("            .link_req_out_valid(link_req_out_valid[i]),")
    w("            .link_req_out_flit(link_req_out_flit[i]),")
    w("            .link_req_out_credit(link_req_out_credit[i]),")
    w("            .link_req_in_valid(link_req_in_valid[i]),")
    w("            .link_req_in_flit(link_req_in_flit[i]),")
    w("            .link_req_in_credit(link_req_in_credit[i]),")
    w("            .link_rsp_out_valid(link_rsp_out_valid[i]),")
    w("            .link_rsp_out_flit(link_rsp_out_flit[i]),")
    w("            .link_rsp_out_credit(link_rsp_out_credit[i]),")
    w("            .link_rsp_in_valid(link_rsp_in_valid[i]),")
    w("            .link_rsp_in_flit(link_rsp_in_flit[i]),")
    w("            .link_rsp_in_credit(link_rsp_in_credit[i])")
    w("        );")
    w("")
    # Per-node IN-face wiring: drive every direction slot. A live direction takes
    # the peer's OPPOSITE OUT slot (data) + the peer's returned in_credit; a
    # boundary direction stays at the tie-off default. One always_comb per net.
    for net in ("req", "rsp"):
        w(f"        always_comb begin : link_{net}_in")
        w(f"            for (int p = 0; p < LINK_PORTS; p++) begin")
        w(f"                link_{net}_in_valid[i][p]   = 1'b0;")
        w(f"                link_{net}_in_flit[i][p]    = '0;")
        w(f"                link_{net}_out_credit[i][p] = '0;")
        w(f"            end")
        # NORTH<->peer SOUTH, EAST<->peer WEST, SOUTH<->peer NORTH, WEST<->peer EAST.
        for d, has, peer, pd in (
            ("NORTH", "HAS_N", "PEER_N", "SOUTH"),
            ("EAST",  "HAS_E", "PEER_E", "WEST"),
            ("SOUTH", "HAS_S", "PEER_S", "NORTH"),
            ("WEST",  "HAS_W", "PEER_W", "EAST"),
        ):
            w(f"            if ({has}) begin  // {d}: <- peer {pd} OUT")
            w(f"                link_{net}_in_valid[i][RP_{d}]   = "
              f"link_{net}_out_valid[{peer}][RP_{pd}];")
            w(f"                link_{net}_in_flit[i][RP_{d}]    = "
              f"link_{net}_out_flit[{peer}][RP_{pd}];")
            w(f"                link_{net}_out_credit[i][RP_{d}] = "
              f"link_{net}_in_credit[{peer}][RP_{pd}];")
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
    for d, has in (("NORTH", "HAS_N"), ("EAST", "HAS_E"),
                   ("SOUTH", "HAS_S"), ("WEST", "HAS_W")):
        for net in ("req", "rsp"):
            w(f"                if (!{has} && link_{net}_out_valid[i][RP_{d}])")
            w(f'                    $fatal(1, "noc_fabric: node%0d drove a flit on '
              f'tied-off {d} ({net}) - fabric link wiring mistake", i);')
    w("        end")
    w("        end")
    w("")
    # Link perf monitors: one per live neighbor direction (req+rsp). Mirrors the
    # prior emit — every directed (node -> peer) edge gets a monitor, named
    # "{net}_{i}to{peer}". Wrapped in `if (HAS_*) generate` so boundary dirs emit
    # nothing.  noc.links is dropped by check_perf_parity, but the slot/edge set
    # is preserved 1:1 anyway.
    w("        // Inter-router link perf monitors (passive): one per live")
    w("        // neighbor direction, named {net}_{i}to{peer}. vc_id bit window")
    w("        // from ni_flit_pkg; credit_pulse is per-VC (not OR-collapsed).")
    for d, has, peer in (("NORTH", "HAS_N", "PEER_N"), ("EAST", "HAS_E", "PEER_E"),
                         ("SOUTH", "HAS_S", "PEER_S"), ("WEST", "HAS_W", "PEER_W")):
        w(f"        if ({has}) begin : g_perf_{d.lower()}")
        for net in ("req", "rsp"):
            flit_wire = f"link_{net}_out_flit[i][RP_{d}]"
            credit_wire = f"link_{net}_out_credit[i][RP_{d}]"
            vc_slice = f"{flit_wire}[ni_flit_pkg::VC_ID_MSB:ni_flit_pkg::VC_ID_LSB]"
            w(f"            link_perf_monitor #(")
            w(f'                .LINK_NAME($sformatf("{net}_%0dto%0d", i, {peer})),')
            w(f"                .BUFFER_DEPTH(ROUTER_VC_DEPTH),")
            w(f"                .NUM_VC(NUM_VC), .VC_ID_WIDTH(ni_flit_pkg::VC_ID_WIDTH)")
            w(f"            ) u_perf_link_{net} (")
            w(f"                .clk_i, .rst_ni,")
            w(f"                .valid(link_{net}_out_valid[i][RP_{d}]),")
            w(f"                .vc_id({vc_slice}),")
            w(f"                .credit_pulse({credit_wire})")
            w(f"            );")
        w(f"        end")
    w("    end : g_node")
    w("")

    w(f"endmodule")
    w("")
    w(f"`endif  // {guard}")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# tb_top emitter — instantiates the fabric + test master/slave + exit logic
# ---------------------------------------------------------------------------

def emit_tb_top(topo: dict) -> str:
    name = topo["topology"]["name"]
    nodes, x_dim, y_dim = _nodes(topo)
    n = len(nodes)
    num_vc = topo["topology"]["num_vc"]

    # Identity pairing: master_i / slave_i both use scn_node{i}.
    # Destination is encoded in addr bits 32+ by the scenario generator (gen_test_patterns).
    coord_id = {i: c for (i, _x, _y, c) in nodes}

    lines = []
    w = lines.append

    w("`timescale 1ns/1ps")
    w("")
    w("// AUTO-GENERATED by sim/tools/gen_tb_top.py")
    w(f"// Topology: {name}  ({x_dim}x{y_dim}, num_vc={num_vc})")
    w("// DO NOT EDIT - modify the generator or sim/topologies/*.yaml instead.")
    w("//")
    w(f"// {n} nodes live inside noc_fabric_{name} (ni_wrap=NMU+NSU + REQ/RSP router per")
    w("// node, joined by directional links). tb_top creates the DPI handles, attaches a")
    w("// user_node_endpoint (test master_wrap + slave_wrap + perf monitors) to each")
    w("// node's master/slave AXI faces, and owns the scoreboard exit logic.")
    w("// Identity pairing: master/slave at node k both use scn_node{k}. Destination")
    w("// is address-driven: addr bit 32+ encodes the dst tile (set by gen_test_patterns).")
    w("//")
    w("// Self-clocked: clk_i/rst_ni are internal logic (10 ns clock, 4-cycle reset).")
    w(f"// Plusargs +scenario_node0=<path> ... +scenario_node{n-1}=<path> kick off the run.")
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
    w("    localparam int unsigned TIMEOUT_CYCLES = 100000;")
    w("    initial begin")
    w("        repeat (TIMEOUT_CYCLES) @(posedge clk_i);")
    w('        $fatal(1, "tb_top: timeout after %0d cycles", TIMEOUT_CYCLES);')
    w("    end")
    w("")
    w("    // -------------------------------------------------------------------------")
    w("    // Parameters")
    w("    // -------------------------------------------------------------------------")
    w("    localparam int unsigned ID_WIDTH              = ni_params_pkg::AXI_ID_WIDTH_DFLT;")
    w("    localparam int unsigned ADDR_WIDTH            = ni_params_pkg::AXI_ADDR_WIDTH_DFLT;")
    w("    localparam int unsigned DATA_WIDTH            = ni_params_pkg::AXI_DATA_WIDTH_DFLT;")
    w(f"    localparam int unsigned NUM_VC                = {num_vc};  // from topology YAML")
    w("    localparam int unsigned FLIT_WIDTH            = ni_params_pkg::NOC_FLIT_WIDTH_DFLT;")
    w("    localparam int unsigned SLAVE_VC_BUFFER_DEPTH = "
      "ni_params_pkg::NOC_SLAVE_VC_BUFFER_DEPTH_DFLT;")
    w("    // ROUTER_VC_DEPTH: credit window for inter-router links; passed to fabric so")
    w("    // link_perf_monitor tracks the ACTUAL receiving buffer depth (not SLAVE_VC_BUFFER_DEPTH).")
    w("    localparam int unsigned ROUTER_VC_DEPTH       = "
      "ni_params_pkg::NOC_ROUTER_VC_DEPTH_DFLT;")
    w("")
    w("    // -------------------------------------------------------------------------")
    w("    // DPI lifecycle")
    w("    // -------------------------------------------------------------------------")
    w('    import "DPI-C" context function void    cmodel_init(input string path);')
    w('    import "DPI-C" context function void    cmodel_finalize();')
    w('    import "DPI-C" context function int     cmodel_done();')
    w('    import "DPI-C" context function int     cmodel_scoreboard_clean();')
    w('    import "DPI-C" context function void    cmodel_dump_scoreboard();')
    w('    import "DPI-C" context function longint unsigned cmodel_router_create(input string name,')
    w('                                                                  input int x_coord, input int y_coord,')
    w('                                                                  input int mesh_x_dim, input int mesh_y_dim,')
    w('                                                                  input int num_vc);')
    w('    import "DPI-C" context function longint unsigned cmodel_master_create(input string name,')
    w('                                                                 input string scenario_path);')
    w('    import "DPI-C" context function longint unsigned cmodel_slave_create(input string name,')
    w('                                                                input string scenario_path);')
    w('    import "DPI-C" context function longint unsigned cmodel_nmu_create(input string name,')
    w('                                                              input int src_id, input int num_vc);')
    w('    import "DPI-C" context function longint unsigned cmodel_nsu_create(input string name,')
    w('                                                              input int src_id, input int num_vc);')
    w('    import "DPI-C" context function int     cmodel_master_count();')
    w('    import "DPI-C" context function int     cmodel_reads_checked();')
    w("")

    # Per-node scenario strings + ctx handle ARRAYS (chandle-substitute longint
    # unsigned). Arrays let the fabric take them as array ports and instantiate
    # nodes via a genvar generate loop.
    for (i, x, y, c) in nodes:
        w(f"    string  scn_node{i};")
    w(f"    longint unsigned router_ctx [{n}];")
    w(f"    longint unsigned m_ctx      [{n}];")
    w(f"    longint unsigned s_ctx      [{n}];")
    w(f"    longint unsigned nmu_ctx    [{n}];")
    w(f"    longint unsigned nsu_ctx    [{n}];")
    w("")

    # Plusargs + cmodel_init + per-node create.
    w("    initial begin")
    cond = [f'!$value$plusargs("scenario_node{i}=%s", scn_node{i})' for (i, *_r) in nodes]
    indent = "        "
    if len(cond) == 1:
        w(f"{indent}if ({cond[0]}) begin")
    else:
        w(f"{indent}if ({cond[0]} ||")
        for c in cond[1:-1]:
            w(f"{indent}    {c} ||")
        w(f"{indent}    {cond[-1]}) begin")
    scn_args = " ".join(f"+scenario_node{i}=<path>" for (i, *_r) in nodes)
    w(f'            $display("ERROR: {scn_args} required");')
    w("            $finish(1);")
    w("        end")
    last_i = nodes[-1][0]
    w(f"        cmodel_init(scn_node{last_i});  // shared config; any variant is fine")
    # Router creates: full (x,y,mesh_x,mesh_y,num_vc). Fill ctx array element-by-
    # element (raster index == fabric genvar index).
    for (i, x, y, _c) in nodes:
        w(f'        router_ctx[{i}] = cmodel_router_create("router_{i}", {x}, {y}, '
          f'{x_dim}, {y_dim}, NUM_VC);')
    # Per-node master/slave/nmu/nsu creates. Identity pairing: master_i/slave_i <- scn_node{i}.
    for (i, x, y, c) in nodes:
        w(f"        // node{i}: master and slave both use scn_node{i}; dst encoded in addr bits 32+.")
        w(f'        m_ctx[{i}]   = cmodel_master_create("master_{i}", scn_node{i});')
        w(f'        s_ctx[{i}]   = cmodel_slave_create ("slave_{i}",  scn_node{i});')
        w(f'        nmu_ctx[{i}] = cmodel_nmu_create("nmu_{i}", {c}, NUM_VC);  '
          f'// src_id = node{i} coord {c}')
        w(f'        nsu_ctx[{i}] = cmodel_nsu_create("nsu_{i}", {c}, NUM_VC);')
    w("    end")
    w("")

    # Per-node AXI buses as STRUCT ARRAYS (master-side into the fabric NMU,
    # slave-side out of NSU), shared by the fabric + the endpoint generate loop.
    w("    // -------------------------------------------------------------------------")
    w("    // Per-node AXI buses (struct arrays): master-side into NMU, slave-side out of NSU")
    w("    // -------------------------------------------------------------------------")
    w(f"    ni_signals_pkg::axi_req_t  master_axi_req [{n}];  // tb master -> NMU")
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
    w("        .NUM_VC(NUM_VC), .FLIT_WIDTH(FLIT_WIDTH), "
      ".SLAVE_VC_BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH),")
    w("        .ROUTER_VC_DEPTH(ROUTER_VC_DEPTH)")
    w("    ) u_fabric (")
    w("        .clk_i(clk_i), .rst_ni(rst_ni),")
    w("        .router_ctx(router_ctx), .nmu_ctx(nmu_ctx), .nsu_ctx(nsu_ctx),")
    w("        .master_axi_req(master_axi_req), .master_axi_rsp(master_axi_rsp),")
    w("        .slave_axi_req(slave_axi_req),   .slave_axi_rsp(slave_axi_rsp)")
    w("    );")
    w("")

    # Test endpoints per node via genvar generate. user_node_endpoint = test
    # master + slave + AXI perf monitors. user_node_endpoint.sv is USER-OWNED
    # (committed, hand-written); the generator only INSTANTIATES it. SLOT_NAME
    # strings come from $sformatf so perf.json slot labels stay "node<i>.manager"
    # / "node<i>.subordinate" — byte-identical to the prior inline emission.
    w("    // -------------------------------------------------------------------------")
    w("    // Test endpoints - one user_node_endpoint per node (test master/slave/monitors)")
    w("    // user_node_endpoint.sv is user-owned and NOT regenerated by this script.")
    w("    // -------------------------------------------------------------------------")
    w(f"    for (genvar i = 0; i < {n}; i++) begin : g_endpoint")
    w("        user_node_endpoint #(")
    w("            .NODE_ID(i),")
    w("            .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),")
    w('            .MASTER_SLOT_NAME($sformatf("node%0d.manager", i)),')
    w('            .SLAVE_SLOT_NAME($sformatf("node%0d.subordinate", i))')
    w("        ) u_endpoint (")
    w("            .clk_i(clk_i), .rst_ni(rst_ni),")
    w("            .master_ctx_i(m_ctx[i]), .slave_ctx_i(s_ctx[i]),")
    w("            .master_axi_req_o(master_axi_req[i]), .master_axi_rsp_i(master_axi_rsp[i]),")
    w("            .slave_axi_req_i(slave_axi_req[i]),   .slave_axi_rsp_o(slave_axi_rsp[i])")
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

    # Exit logic.
    w("    // -------------------------------------------------------------------------")
    w("    // Exit logic - non-vacuous PASS guard")
    w("    // -------------------------------------------------------------------------")
    w(f"    // PASS requires scoreboard clean AND all {n} masters created AND at least")
    w(f"    // {n} reads checked (one per node — guards weakened scenarios")
    w("    // where fewer than all nodes complete a read).")
    w("    always @(posedge clk_i) begin")
    w("        /* verilator lint_off WIDTHTRUNC */")
    w("        if (rst_ni && (cmodel_done() != 0)) begin")
    w(f"            if (cmodel_scoreboard_clean() != 0 &&")
    w(f"                cmodel_master_count() == {n} && cmodel_reads_checked() >= {n}) begin")
    w("        /* verilator lint_on WIDTHTRUNC */")
    w('                $display("PASS: scenario complete, scoreboard clean");')
    w("                cmodel_dump_scoreboard();")
    w("                $finish(0);")
    w("            end else begin")
    w(f'                $display("FAIL: scoreboard mismatch or vacuous run (masters=%0d reads=%0d, need>={n})",')
    w("                         cmodel_master_count(), cmodel_reads_checked());")
    w("                cmodel_dump_scoreboard();")
    w('                $fatal(1, "tb_top: run failed");')
    w("            end")
    w("        end")
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
    w("                cmodel_dump_scoreboard();")
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
    return out_path.parent / f"noc_fabric_{topo['topology']['name']}.sv"


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate tb_top.sv + noc_fabric_<topo>.sv.")
    ap.add_argument("--topology", default="mesh_4x4_vc1",
                    help="Topology name (matches sim/topologies/<name>.yaml)")
    ap.add_argument("--out", default=None,
                    help="Output tb_top.sv path (default: sim/sv/tb_top_<topology>.sv; "
                         "fabric emitted alongside)")
    ap.add_argument("--check", action="store_true",
                    help="Drift gate: regenerate both files and diff vs committed; exit 1 if different")
    a = ap.parse_args()

    topo = load_topology(a.topology)
    tb_text = emit_tb_top(topo)
    fab_text = emit_fabric(topo)
    out_path = Path(a.out) if a.out is not None else \
        ROOT / "sim" / "sv" / f"tb_top_{a.topology}.sv"
    fab_path = _fabric_path(out_path, topo)

    if a.check:
        rc = 0
        for path, text, label in ((out_path, tb_text, "tb_top.sv"),
                                  (fab_path, fab_text, fab_path.name)):
            if not path.exists():
                print(f"DRIFT: {path} does not exist (never generated)")
                rc = 1
                continue
            cur = path.read_text(encoding="utf-8")
            if cur != text:
                sys.stdout.writelines(difflib.unified_diff(
                    cur.splitlines(True), text.splitlines(True), "committed", "regenerated"))
                print(f"DRIFT: {label} differs from generator output")
                rc = 1
        return rc

    out_path.write_text(tb_text, encoding="utf-8")
    fab_path.write_text(fab_text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
