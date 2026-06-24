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


def _neighbors(x, y, x_dim, y_dim):
    """Map of live directions -> (peer_x, peer_y) for a node at (x,y). +y=NORTH."""
    nbr = {}
    if y + 1 < y_dim:
        nbr["NORTH"] = (x, y + 1)
    if x + 1 < x_dim:
        nbr["EAST"] = (x + 1, y)
    if y - 1 >= 0:
        nbr["SOUTH"] = (x, y - 1)
    if x - 1 >= 0:
        nbr["WEST"] = (x - 1, y)
    return nbr


_OPPOSITE = {"NORTH": "SOUTH", "SOUTH": "NORTH", "EAST": "WEST", "WEST": "EAST"}


# ---------------------------------------------------------------------------
# Fabric emitter — noc_fabric_<topo>.sv
# ---------------------------------------------------------------------------

def emit_fabric(topo: dict) -> str:
    name = topo["topology"]["name"]
    nodes, x_dim, y_dim = _nodes(topo)
    n = len(nodes)
    num_vc = topo["topology"]["num_vc"]
    guard = f"NOC_FABRIC_{name.upper()}_SV"

    # idx -> (x,y) and (x,y) -> idx lookups.
    idx_of = {(x, y): i for (i, x, y, _c) in nodes}

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
    # ctx ports + AXI interface ports, one set per node.
    for (i, x, y, _c) in nodes:
        w(f"    // node{i} (x={x}, y={y}) DPI handles + AXI faces")
        w(f"    input  longint unsigned router{i}_ctx,")
        w(f"    input  longint unsigned nmu{i}_ctx,")
        w(f"    input  longint unsigned nsu{i}_ctx,")
        w(f"    axi4_intf.slave  master_axi_{i},   // NMU ingress (driven by tb master)")
        last = (i == n - 1)
        comma = "" if last else ","
        w(f"    axi4_intf.master slave_axi_{i}{comma}     // NSU egress (consumed by tb slave)")
    w(");")
    w("")
    w(f"    localparam int unsigned LINK_PORTS = {LINK_PORTS};  // LOCAL + N/E/S/W")
    w("    // RouterPort direction indices (router.hpp enum).")
    for d, v in (("LOCAL", 0), ("NORTH", 1), ("EAST", 2), ("SOUTH", 3), ("WEST", 4)):
        w(f"    localparam int unsigned RP_{d} = {v};")
    w("")

    # Per-node internal NoC interfaces (NMU<->router, router<->NSU).
    noc_params = ("        .NUM_VC(NUM_VC), .FLIT_WIDTH(FLIT_WIDTH), "
                  ".SLAVE_VC_BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH)")
    for (i, _x, _y, _c) in nodes:
        w(f"    noc_intf #(")
        w(f"{noc_params}")
        w(f"    ) node{i}_nmu ();")
        w(f"    noc_intf #(")
        w(f"{noc_params}")
        w(f"    ) node{i}_nsu ();")
    w("")

    # Per-node LINK face arrays (whole [LINK_PORTS] bundle per node).
    for (i, _x, _y, _c) in nodes:
        w(f"    logic [LINK_PORTS-1:0]   n{i}_link_req_out_valid,  n{i}_link_rsp_out_valid;")
        w(f"    logic [FLIT_WIDTH-1:0]   n{i}_link_req_out_flit   [LINK_PORTS];")
        w(f"    logic [FLIT_WIDTH-1:0]   n{i}_link_rsp_out_flit   [LINK_PORTS];")
        w(f"    logic [NUM_VC-1:0]       n{i}_link_req_in_credit  [LINK_PORTS];")
        w(f"    logic [NUM_VC-1:0]       n{i}_link_rsp_in_credit  [LINK_PORTS];")
        w(f"    logic [LINK_PORTS-1:0]   n{i}_link_req_in_valid,   n{i}_link_rsp_in_valid;")
        w(f"    logic [FLIT_WIDTH-1:0]   n{i}_link_req_in_flit    [LINK_PORTS];")
        w(f"    logic [FLIT_WIDTH-1:0]   n{i}_link_rsp_in_flit    [LINK_PORTS];")
        w(f"    logic [NUM_VC-1:0]       n{i}_link_req_out_credit [LINK_PORTS];")
        w(f"    logic [NUM_VC-1:0]       n{i}_link_rsp_out_credit [LINK_PORTS];")
        w("")

    # Node instances: ni_wrap (= NMU+NSU) + router_wrap.
    for (i, x, y, _c) in nodes:
        w("    // -------------------------------------------------------------------------")
        w(f"    // node{i} (x={x}, y={y}): ni_wrap (nmu+nsu) + router_wrap")
        w("    // -------------------------------------------------------------------------")
        w(f"    ni_wrap #(")
        w(f"        .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),")
        w(f"        .NUM_VC(NUM_VC), .FLIT_WIDTH(FLIT_WIDTH), "
          f".SLAVE_VC_BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH)")
        w(f"    ) u_ni_{i} (")
        w(f"        .clk_i(clk_i), .rst_ni(rst_ni),")
        w(f"        .nmu_ctx_i(nmu{i}_ctx), .nsu_ctx_i(nsu{i}_ctx),")
        w(f"        .master_axi_i(master_axi_{i}), .slave_axi_o(slave_axi_{i}),")
        w(f"        .noc_nmu_o(node{i}_nmu.mosi), .noc_nsu_i(node{i}_nsu.miso)")
        w(f"    );")
        w("")
        w(f"    router_wrap #(")
        w(f"        .NUM_VC(NUM_VC), .FLIT_WIDTH(FLIT_WIDTH), "
          f".SLAVE_VC_BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH),")
        w(f"        .LINK_PORTS(LINK_PORTS)")
        w(f"    ) u_router_{i} (")
        w(f"        .clk_i(clk_i), .rst_ni(rst_ni), .ctx_i(router{i}_ctx),")
        w(f"        .noc_nmu_i(node{i}_nmu.miso),")
        w(f"        .noc_nsu_o(node{i}_nsu.mosi),")
        w(f"        .link_req_out_valid(n{i}_link_req_out_valid),")
        w(f"        .link_req_out_flit(n{i}_link_req_out_flit),")
        w(f"        .link_req_out_credit(n{i}_link_req_out_credit),")
        w(f"        .link_req_in_valid(n{i}_link_req_in_valid),")
        w(f"        .link_req_in_flit(n{i}_link_req_in_flit),")
        w(f"        .link_req_in_credit(n{i}_link_req_in_credit),")
        w(f"        .link_rsp_out_valid(n{i}_link_rsp_out_valid),")
        w(f"        .link_rsp_out_flit(n{i}_link_rsp_out_flit),")
        w(f"        .link_rsp_out_credit(n{i}_link_rsp_out_credit),")
        w(f"        .link_rsp_in_valid(n{i}_link_rsp_in_valid),")
        w(f"        .link_rsp_in_flit(n{i}_link_rsp_in_flit),")
        w(f"        .link_rsp_in_credit(n{i}_link_rsp_in_credit)")
        w(f"    );")
        w("")

    # Per-node IN-face wiring: drive every direction slot. Live directions take
    # the peer's matching OUT slot (data) + the peer's returned credit; boundary
    # directions are tied off. Two procedural blocks per node (req/rsp).
    w("    // -------------------------------------------------------------------------")
    w("    // Inter-router link wiring (per direction) + boundary tie-off")
    w("    // -------------------------------------------------------------------------")
    for (i, x, y, _c) in nodes:
        nbr = _neighbors(x, y, x_dim, y_dim)
        for net in ("req", "rsp"):
            w(f"    always_comb begin : link_{net}_in_n{i}")
            w(f"        for (int p = 0; p < LINK_PORTS; p++) begin")
            w(f"            n{i}_link_{net}_in_valid[p]   = 1'b0;")
            w(f"            n{i}_link_{net}_in_flit[p]    = '0;")
            w(f"            n{i}_link_{net}_out_credit[p] = '0;")
            w(f"        end")
            for d, (px, py) in nbr.items():
                pi = idx_of[(px, py)]
                pd = _OPPOSITE[d]
                w(f"        // {d}: data IN <- node{pi} {pd} OUT; out_credit <- "
                  f"node{pi} {pd} in_credit.")
                w(f"        n{i}_link_{net}_in_valid[RP_{d}]   = n{pi}_link_{net}_out_valid[RP_{pd}];")
                w(f"        n{i}_link_{net}_in_flit[RP_{d}]    = n{pi}_link_{net}_out_flit[RP_{pd}];")
                w(f"        n{i}_link_{net}_out_credit[RP_{d}] = n{pi}_link_{net}_in_credit[RP_{pd}];")
            w(f"    end")
            w("")

    # Tie-off assertion: any boundary direction (no neighbor) asserting OUT valid
    # -> $fatal. Scope note: the C++ route leak (a dst outside the mesh) is already
    # caught upstream by route_compute's assert+abort (router.hpp), and a boundary
    # direction's eject adapter is never constructed, so a *misrouted in-mesh* flit
    # would stall silently in the unwired output FIFO rather than drive OUT valid.
    # What this assertion actually guards is a fabric WIRING mistake — a future
    # generator/edit that drives a boundary OUT net (e.g. a mis-derived neighbor
    # map) surfaces here as a $fatal instead of a silent hang. Defense-in-depth on
    # the SV side; not a substitute for the C++ abort.
    w("    // -------------------------------------------------------------------------")
    w("    // Boundary tie-off assertion: a boundary direction (no neighbor) must")
    w("    // never drive OUT valid. Fires on a fabric wiring mistake; the C++ route")
    w("    // leak (dst outside mesh) is caught upstream by route_compute's abort.")
    w("    // -------------------------------------------------------------------------")
    w("    always_ff @(posedge clk_i) begin")
    w("        if (rst_ni) begin")
    for (i, x, y, _c) in nodes:
        nbr = _neighbors(x, y, x_dim, y_dim)
        for d in ("NORTH", "EAST", "SOUTH", "WEST"):
            if d in nbr:
                continue
            for net in ("req", "rsp"):
                w(f"            if (n{i}_link_{net}_out_valid[RP_{d}])")
                w(f'                $fatal(1, "noc_fabric: node{i} drove a flit on '
                  f'tied-off {d} ({net}) - fabric link wiring mistake");')
    w("        end")
    w("    end")
    w("")

    # Link perf monitors: per directed edge, the OUT valid + the credit received.
    w("    // -------------------------------------------------------------------------")
    w("    // Inter-router link perf monitors (passive)")
    w("    // -------------------------------------------------------------------------")
    seen = set()
    for (i, x, y, _c) in nodes:
        nbr = _neighbors(x, y, x_dim, y_dim)
        for d, (px, py) in nbr.items():
            pi = idx_of[(px, py)]
            key = (i, pi)
            if key in seen:
                continue
            seen.add(key)
            for net in ("req", "rsp"):
                # vc_id is flit header bits [VC_ID_MSB:VC_ID_LSB] = [21:19].
                # Decode from the flit bus; truncate to $clog2(NUM_VC) bits so
                # the monitor index stays in range. credit_pulse is per-VC
                # (NOT OR-collapsed): each bit corresponds to one VC slot freed.
                flit_wire = f"n{i}_link_{net}_out_flit[RP_{d}]"
                credit_wire = f"n{i}_link_{net}_out_credit[RP_{d}]"
                w(f"    link_perf_monitor #(")
                w(f'        .LINK_NAME("{net}_{i}to{pi}"), .BUFFER_DEPTH(ROUTER_VC_DEPTH),')
                w(f"        .NUM_VC(NUM_VC)")
                w(f"    ) u_perf_link_{net}_{i}_{pi} (")
                w(f"        .clk_i, .rst_ni,")
                w(f"        .valid(n{i}_link_{net}_out_valid[RP_{d}]),")
                w(f"        .vc_id({flit_wire}[21:19]),")
                w(f"        .credit_pulse({credit_wire})")
                w(f"    );")
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

    # Per-node scenario strings + ctx handles.
    for (i, x, y, c) in nodes:
        w(f"    string  scn_node{i};")
    for (i, _x, _y, _c) in nodes:
        w(f"    longint unsigned router{i}_ctx, m{i}_ctx, s{i}_ctx, n{i}_nmu_ctx, n{i}_nsu_ctx;")
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
    # Router creates: full (x,y,mesh_x,mesh_y,num_vc).
    for (i, x, y, _c) in nodes:
        w(f'        router{i}_ctx = cmodel_router_create("router_{i}", {x}, {y}, '
          f'{x_dim}, {y_dim}, NUM_VC);')
    # Per-node master/slave/nmu/nsu creates. Identity pairing: master_i/slave_i <- scn_node{i}.
    for (i, x, y, c) in nodes:
        w(f"        // node{i}: master and slave both use scn_node{i}; dst encoded in addr bits 32+.")
        w(f'        m{i}_ctx     = cmodel_master_create("master_{i}", scn_node{i});')
        w(f'        s{i}_ctx     = cmodel_slave_create ("slave_{i}",  scn_node{i});')
        w(f'        n{i}_nmu_ctx = cmodel_nmu_create("nmu_{i}", {c}, NUM_VC);  '
          f'// src_id = node{i} coord {c}')
        w(f'        n{i}_nsu_ctx = cmodel_nsu_create("nsu_{i}", {c}, NUM_VC);')
    w("    end")
    w("")

    # Per-node AXI interfaces (master-side + slave-side), driven by tb master/slave
    # and the fabric's NMU/NSU faces.
    axi_params = "        .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH)"
    w("    // -------------------------------------------------------------------------")
    w("    // Per-node AXI buses (master-side into the fabric NMU, slave-side out of NSU)")
    w("    // -------------------------------------------------------------------------")
    for (i, _x, _y, _c) in nodes:
        w(f"    axi4_intf #(")
        w(f"{axi_params}")
        w(f"    ) master_nmu_axi_{i} ();")
        w(f"    axi4_intf #(")
        w(f"{axi_params}")
        w(f"    ) nsu_slave_axi_{i} ();")
    w("")

    # Fabric instance.
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
    for k, (i, _x, _y, _c) in enumerate(nodes):
        last = (k == n - 1)
        comma = "" if last else ","
        w(f"        .router{i}_ctx(router{i}_ctx), .nmu{i}_ctx(n{i}_nmu_ctx), "
          f".nsu{i}_ctx(n{i}_nsu_ctx),")
        w(f"        .master_axi_{i}(master_nmu_axi_{i}.slave), "
          f".slave_axi_{i}(nsu_slave_axi_{i}.master){comma}")
    w("    );")
    w("")

    # Test endpoints per node: user_node_endpoint = test master + slave + AXI perf
    # monitors. user_node_endpoint.sv is USER-OWNED (committed, hand-written); the
    # generator only INSTANTIATES it, never emits/overwrites it. SLOT_NAME strings are
    # passed as params so the perf.json slot labels stay "node<i>.manager" /
    # "node<i>.subordinate" — byte-identical to the prior inline emission.
    w("    // -------------------------------------------------------------------------")
    w("    // Test endpoints - one user_node_endpoint per node (test master/slave/monitors)")
    w("    // user_node_endpoint.sv is user-owned and NOT regenerated by this script.")
    w("    // -------------------------------------------------------------------------")
    for (i, _x, _y, _c) in nodes:
        w(f"    user_node_endpoint #(")
        w(f"        .NODE_ID({i}),")
        w(f"        .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),")
        w(f'        .MASTER_SLOT_NAME("node{i}.manager"), '
          f'.SLAVE_SLOT_NAME("node{i}.subordinate")')
        w(f"    ) u_endpoint_{i} (")
        w(f"        .clk_i(clk_i), .rst_ni(rst_ni),")
        w(f"        .master_ctx_i(m{i}_ctx), .slave_ctx_i(s{i}_ctx),")
        w(f"        .master_axi_o(master_nmu_axi_{i}.master),")
        w(f"        .slave_axi_i(nsu_slave_axi_{i}.slave)")
        w(f"    );")
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
