#!/usr/bin/env python3
"""Generate sim/tb/test/tb_top_<topology>.sv from a topology config.

The fabric/tb split:
  - ref_model/top/noc_fabric.sv : CHECKED IN, not generated. One parameterized
    module for every topology: X_DIM x Y_DIM nodes, each = NMU + REQ/RSP/DAT
    router_wrap + NSU, joined by inter-router directional (N/E/S/W) links with
    boundary tie-off + assertion, plus N_PERIPH off-mesh NIs. Every endpoint
    exposes a clean AXI port (master-side + slave-side). The DPI `ctx` handles
    arrive as PORTS — the fabric itself does no cmodel_*_create.
  - tb_top_<topology>.sv : clk/rst, watchdog, cmodel_*_create (router/nmu/nsu ctx),
    instantiates noc_fabric with the geometry the topology implies, attaches a
    user_node_endpoint (pulp axi_file_master + axi_delayer + axi_sim_mem +
    in-endpoint scoreboard + bw monitor) per node + exit logic.

Generated artifacts: edit the generator or the config file, never the emitted
.sv directly. tb_top_<topology>.sv includes the fabric (SV `include), resolved via
the -I ref_model/top include path.

Usage:
    python3 gen_tb_top.py [--topology mesh_4x4] [--out sim/tb/test/tb_top_<topology>.sv]

Parameterised from the FlooNoC-shaped config (sim/configs/*.yml):
    - nodes list [(x,y), ...] from the router array
    - node_id = (y << X_WIDTH) | x  (coordinate-encoded; == linear index for 1-D)
    - per-node router/nmu/nsu ctx handles; TILE_BASE_ADDR / TILE_SIZE, each node's
      own tile-crossbar windows, from address_map.node_windows() (see
      tile_targets below), stamped into each endpoint
    - inter-router links wired per XY direction; boundary directions tied off
    - PASS guard: all endpoints done (end_of_sim) AND every node non-vacuous
      (txn_cnt > 0)

Constants kept as template (not derived from the config):
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


def load_topology(name: str) -> dict:
    import yaml
    path = ROOT / "sim" / "configs" / f"{name}.yml"
    topo = yaml.safe_load(path.read_text())
    _check_flit_capacity(topo, path)
    return topo


def num_vc() -> int:
    """DAT virtual-channel count, from where the parameter is defined.

    noc.DAT_NUM_VC in specgen/source/constants.yaml. The config files do not
    carry it: it is a DUT parameter and does not vary with the geometry, so
    changing it is an edit to that file and a rebuild.
    """
    return _constant("noc", "DAT_NUM_VC")


def read_rob_enabled() -> int:
    """NMU read reorder buffer mode, from where the parameter is defined.

    nmu.READ_ROB_ENABLED in specgen/source/constants.yaml. Like num_vc() above:
    a DUT parameter, not a property of the geometry, so switching modes is an
    edit to that file and a rebuild.
    """
    return _constant("nmu", "READ_ROB_ENABLED")


def _constant(domain: str, name: str) -> int:
    import yaml
    c = yaml.safe_load((ROOT / "specgen" / "source" / "constants.yaml").read_text())
    return int(c[domain][name]["default"])


# Y_WIDTH / VC_ID_WIDTH mirror the flit spec (ni_packet.json field_widths).
Y_WIDTH = 4
VC_ID_WIDTH = 3
DST_ID_WIDTH = X_WIDTH + Y_WIDTH  # 8 bits → 256 max nodes
ADDR_WIDTH = _constant("axi", "ADDR_WIDTH")
_SAM_ALIGNMENT = 0x1000


def _check_flit_capacity(cfg: dict, path) -> None:
    """Reject a topology whose mesh dims / num_vc exceed the flit field capacity,
    or whose mesh dims fall below the per-dimension minimum.

    Mirrors specgen/ni_spec/invariants.py:check_mesh_within_flit for the
    sim-topology-YAML path (X/Y/node + VC bounds).  Fails with a clear message so
    the user knows to reduce dims / num_vc or widen the flit fields (via the
    specgen constants).

    Mesh dim minimum is 2 per dimension (mesh_x_dim >= 2 AND mesh_y_dim >= 2):
    a mesh communicating through NI + router needs at least 2x2. 1x1 and 1xN
    meshes are illegal (specgen/source/constants.yaml MESH_X_DIM/MESH_Y_DIM min).
    """
    # Read straight off the router array rather than through
    # address_map.router_array(): this is a second, independent reader, and its
    # per-axis power-of-two report below is the stronger of the two checks.
    array = cfg["routers"][0]["array"]
    x_dim, y_dim = int(array[0]), int(array[1])
    dat_num_vc = num_vc()
    cap_x = 1 << X_WIDTH
    cap_y = 1 << Y_WIDTH
    cap_nodes = 1 << DST_ID_WIDTH
    cap_vc = 1 << VC_ID_WIDTH
    errors = []
    if x_dim < 2:
        errors.append(f"x_dim={x_dim} < 2 (mesh dimension minimum is 2; 1x1/1xN meshes are illegal)")
    if y_dim < 2:
        errors.append(f"y_dim={y_dim} < 2 (mesh dimension minimum is 2; 1x1/1xN meshes are illegal)")
    # Mirrors sam_yaml.hpp's load-time assert. Caught here so a non-power-of-two
    # topology fails at generate time rather than after elaborating a testbench
    # the model then aborts on.
    for axis, dim in (("x", x_dim), ("y", y_dim)):
        if dim & (dim - 1):
            errors.append(
                f"{axis}_dim={dim} is not a power of two (a collective mask wildcards a "
                f"clog2(dim)-bit coordinate field, so every index it names must be a node)")
    if x_dim > cap_x:
        errors.append(f"x_dim={x_dim} > 2^X_WIDTH={cap_x}")
    if y_dim > cap_y:
        errors.append(f"y_dim={y_dim} > 2^Y_WIDTH={cap_y}")
    if x_dim * y_dim > cap_nodes:
        errors.append(f"x_dim*y_dim={x_dim * y_dim} > 2^DST_ID_WIDTH={cap_nodes}")
    if dat_num_vc > cap_vc:
        errors.append(f"num_vc={dat_num_vc} > 2^VC_ID_WIDTH={cap_vc}")
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

    idx is the linear emit index (0..N-1) and doubles as the ARRAY position:
    array x = idx % x_dim, array y = idx // x_dim, which is what the emitted
    generate loop derives its neighbour wiring and boundary tie-off from. A
    router's route coordinate IS its array position, and coord_id is the routing
    id built from it.
    """
    x_dim, y_dim = address_map.router_array(topo)
    out = []
    idx = 0
    for y in range(y_dim):
        for x in range(x_dim):
            out.append((idx, x, y, _coord_id(x, y)))
            idx += 1
    return out, x_dim, y_dim


# XYDirections (FlooNoC routing.py:245-252). EJECT is the LOCAL port a tile
# owns; the four compass values are the boundary ports a peripheral hangs off.
_EJECT = 4
_DIR_NAME = {0: "NORTH", 1: "EAST", 2: "SOUTH", 3: "WEST"}


def _peripherals(topo: dict):
    """Endpoints attached to a boundary port instead of EJECT, in member order.

    A peripheral has an NI and a test endpoint but no router. It SHARES its host
    router's coordinate and is told apart by the boundary port it hangs off:
    EAST/WEST are port 1, NORTH/SOUTH port 2. dst_dir is read as that port and
    never as a coordinate offset -- FlooNoC derives a non-router NI's coordinate
    from it, which here would place the peripheral one step outside the mesh.

    A corner router is legal: it has two free faces, and dst_dir says which one
    is meant.

    Returns [{"x", "y", "cid", "port", "router_idx", "dir"}, ...].
    Peripherals extend the ENDPOINT index space, not the node index space:
    nodes 0..N-1 stay the routers and peripheral p is endpoint N + p.
    """
    x_dim, y_dim = address_map.router_array(topo)
    out = []
    taken = set()
    for ep in topo["endpoints"]:
        for a in address_map.attachments(topo, ep["name"], address_map.members(ep), x_dim):
            if a["dir"] == _EJECT:
                continue
            x, y, direction = a["x"], a["y"], _DIR_NAME[a["dir"]]
            if not (0 <= x < x_dim and 0 <= y < y_dim):
                raise SystemExit(
                    f"gen_tb_top: peripheral {ep['name']} at (x={x},y={y}) is outside the "
                    f"{x_dim}x{y_dim} router array -- a peripheral shares a router's "
                    f"coordinate, it does not take one of its own")
            # Same rule sam_yaml.hpp's loader applies, checked here because the
            # generator is a second, independent reader of the same config: on an
            # edge router the named port has no neighbour and is terminal, while
            # on an interior router it carries a live inter-router link and
            # hanging a peripheral off it closes a channel dependency cycle.
            on_edge = {"WEST": x == 0, "EAST": x == x_dim - 1,
                       "SOUTH": y == 0, "NORTH": y == y_dim - 1}[direction]
            if not on_edge:
                raise SystemExit(
                    f"gen_tb_top: peripheral {ep['name']} at (x={x},y={y}) hangs off the "
                    f"{direction} port, an edge this coordinate is not on -- an interior "
                    f"router's port carries a live inter-router link, and hanging a "
                    f"peripheral off it closes a channel dependency cycle")
            if (x, y, direction) in taken:
                raise SystemExit(
                    f"gen_tb_top: two peripherals both claim (x={x},y={y}) {direction}")
            taken.add((x, y, direction))
            out.append({"x": x, "y": y, "cid": _coord_id(x, y), "port": a["port"],
                        "router_idx": y * x_dim + x, "dir": direction})
    return out


def _endpoints(nodes, peripherals):
    """Router nodes followed by peripherals, in (idx, x, y, coord_id, port) shape.

    The endpoint index space: 0..N-1 are the routers, N..N+P-1 the peripherals.
    Each entry carries its own ni_wrap, user_node_endpoint and NMU / NSU /
    dat_merge context; only the first N carry a router.

    port is what separates two endpoints at one coordinate: a router node is the
    tile on port 0 (LOCAL), a peripheral is the boundary port it hangs off.
    """
    return [(idx, x, y, cid, 0) for (idx, x, y, cid) in nodes] + \
           [(len(nodes) + p, per["x"], per["y"], per["cid"], per["port"])
            for p, per in enumerate(peripherals)]


# REGION_BYTES: the per-node tile-memory window. DV-side tb constant
# (FlooNoC mesh tb pattern), not a runtime knob. Distinct from
# gen_test_patterns.py's auto-derived directed-side region_bytes (a different,
# per-run-derived value of the same name).
_DEFAULT_REGION_BYTES = 0x1000

# Size of a crossbar-window pad. A pad exists only to square off the rectangular
# TILE_BASE_ADDR / TILE_SIZE arrays for an endpoint owning fewer windows than the
# widest one; it is parked above every real window and nothing addresses it. It
# still has to be a NON-EMPTY range -- see the note in tile_targets().
_PAD_BYTES = 0x1000

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

# Master-face consumer backpressure, response side only. Same two mechanisms as
# the memory profile above, on the delayer between the file master and the tile
# crossbar.
#
#   ideal   the delayer is wires. The master accepts R the cycle it arrives, so
#           the NMU always sinks read data and the DAT dependency cycle
#           noc-target-spec.md argues against cannot form. Every measurement to
#           date assumed this, and sim-injection-sweep needs it: a consumer that
#           stalls first measures the consumer, not fabric saturation.
#   random  the master sometimes cannot keep up. R backs up through the
#           crossbar into the NMU, which is what makes the deadlock question
#           reachable at all.
#
# The two tops take different profiles because they answer different questions.
# The directed and continuous tops replay transactions this project chose, so a
# stalling consumer costs them nothing they were measuring. The DMA top exists to
# put a real AXI manager under the fabric, and a manager whose every response
# beat waits a random 0 to 15 cycles never gets to issue: at "random" the busiest
# link of a 4x4 write run carried 0.101 flits per cycle and the run took 16908
# cycles, against 0.912 and 1864 for the same run at "ideal". Nine tenths of that
# gate was measuring the delayer.
_MST_BACKPRESSURE_PROFILES = {
    "ideal": (0, 0),
    "random": (1, 0),
}
_MST_BACKPRESSURE = "random"
_MST_BACKPRESSURE_DMA = "ideal"


def tile_targets(topo: dict, endpoints):
    """Each endpoint's own crossbar windows, in target PORT ORDER (m0 = config,
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

    Returns ({endpoint_idx: [{"space", "base", "size"}, ...]}, noc_egress_base).
    """
    _bases, entries = address_map.pack_config(topo)
    out = {}
    for idx, _x, _y, cid, port in endpoints:
        windows = address_map.node_windows(entries, cid, port)
        order = [w["space"] for w in windows]
        # Spelled out here rather than read back from address_map.SPACE_ORDER:
        # this is the cross-check on that constant, not a restatement of it.
        # Checked BEFORE the padding below, or every short row would read as
        # ragged-then-padded and the check would say nothing.
        expected = ["config", "memory"] if port == 0 else ["peripheral"]
        if order != expected:
            raise SystemExit(
                f"gen_tb_top: endpoint {idx} (port {port}) window order {order} must be "
                f"{expected} -- user_node_endpoint puts the config memory on target 0 and "
                f"the data memory on the last target (see address_map.SPACE_ORDER)")
        out[idx] = windows
    # The emitted parameters are RECTANGULAR -- [n_ep-1:0][TILE_TARGETS-1:0] --
    # so a one-window peripheral row beside a two-window tile row will not
    # elaborate. Pad the short rows to the widest instead of reshaping.
    #
    # A pad is a REAL range parked above every window, never an empty one.
    # size 0 at base 0 looks inert and is the opposite: addr_decode
    # (sim/dv/common_cells-1.37.0/src/addr_decode_dync.sv:110-112) matches on
    #     addr >= start_addr && (addr < end_addr || end_addr == '0)
    # and end_addr == '0 means "end of address space" (documented at :56-57),
    # so start = end = 0 is a WILDCARD that matches every address. The match
    # loop has no break and the last match wins, and a pad sits at a higher
    # rule index than the real window it pads, so it would swallow the whole
    # map: fabric traffic would land in the pad's memory instead of the real
    # one, the local initiator would stop falling through to the NMU, and
    # nothing would ever be undecoded so the DECERR gate would go dead. None of
    # that fails elaboration.
    #
    # base = 2 * noc_egress_base is the first address above the egress aperture
    # -- user_node_endpoint.sv makes it [NOC_EGRESS_BASE, 2 * NOC_EGRESS_BASE),
    # end-exclusive -- so the pad overlaps nothing and warns about nothing.
    # start < end keeps check_start quiet (it fatals on start == end unless end
    # is the wildcard zero) and end != 0 keeps the pad out of the wildcard
    # branch. A zero SIZE is not an option here in either direction.
    egress = address_map.noc_egress_base(entries)
    width = max(len(w) for w in out.values())
    for idx, windows in out.items():
        # Staggered, so two pads in one row could never be the same rule twice.
        windows.extend({"space": None, "base": 2 * egress + k * _PAD_BYTES,
                        "size": _PAD_BYTES}
                       for k in range(width - len(windows)))
    return out, egress


# Link topology (live-neighbor map, opposite-port pairing, per-network flit
# widths and flow) lives in noc_fabric.sv, which derives all of it from X_DIM /
# Y_DIM / N_PERIPH. The Python model only needs _nodes() and _peripherals() to
# stamp the endpoints' coords/ids and the fabric's parameter overrides.

# Job geometry defaults for the DMA top. Mirror gen_dma_jobs.py's own argparse
# defaults: the top's preload and region compare address the bytes jobs.txt
# names, so the two generators have to be run with the same pair.
_DMA_JOBS_PER_NODE = 100
_DMA_JOB_BYTES = 0x400
_DMA_RW = "read"


def _dma_check(topo, n_ep, jobs_per_node, job_bytes, rw):
    """The DMA top's memory backdoor, preload, region compare and exit logic.

    Emitted only under --dma. The job geometry comes from
    gen_dma_jobs.job_table() -- the same function that writes jobs.txt -- so
    what is preloaded and compared is what the stimulus moves.
    """
    import gen_dma_jobs   # deferred: gen_dma_jobs imports this module

    # job_table rows are (node_idx, src_ep, dst_ep, src_addr, dst_addr, axi_id),
    # grouped by node in node order. src_ep is NOT node_idx: a read job's source
    # is another node's window.
    _SRC_EP, _DST_EP, _SRC_ADDR, _DST_ADDR = 1, 2, 3, 4
    jobs = gen_dma_jobs.job_table(topo, jobs_per_node, job_bytes, rw)
    n = len(jobs) // jobs_per_node
    # Packed, descending, as the tile-window parameters above: field j is job j
    # and row i is node i.
    def _rows(col, fmt="ADDR_WIDTH'(64'h{:X})"):
        return "{" + ", ".join(
            "{" + ", ".join(fmt.format(jobs[i * jobs_per_node + j][col])
                            for j in reversed(range(jobs_per_node))) + "}"
            for i in reversed(range(n))) + "}"

    lines = []
    w = lines.append
    w("    // -------------------------------------------------------------------------")
    w("    // Job geometry - stamped from sim/tools/gen_dma_jobs.job_table(), the same")
    w("    // function that writes the jobs.txt this run replays. A run whose job files")
    w("    // were emitted with a different --jobs-per-node / --length disagrees with")
    w("    // these tables, and the issued-vs-JOBS_PER_NODE check below is what says so.")
    w("    // -------------------------------------------------------------------------")
    w(f"    localparam int unsigned JOBS_PER_NODE = {jobs_per_node};")
    w(f"    localparam int unsigned JOB_BYTES     = {job_bytes};")
    w("    // The data memory. Target 0 is the config aperture and the LAST target is")
    w("    // the memory-space window (gen_tb_top.tile_targets).")
    w("    localparam int unsigned MEM_TARGET    = TILE_TARGETS - 1;")
    w(f"    localparam logic [{n - 1}:0][JOBS_PER_NODE-1:0][ADDR_WIDTH-1:0] JOB_SRC_ADDR = "
      f"{_rows(_SRC_ADDR)};")
    w(f"    localparam logic [{n - 1}:0][JOBS_PER_NODE-1:0][ADDR_WIDTH-1:0] JOB_DST_ADDR = "
      f"{_rows(_DST_ADDR)};")
    w("    // Endpoints a job's bytes leave and arrive at, PER JOB: a read job takes")
    w("    // its source from another node's window, so neither end follows the node")
    w("    // whose file issued the job. 8 b is the flit's own node-index field width,")
    w("    // so it holds every endpoint a topology can carry.")
    ep_fmt = "8'd{}"
    w(f"    localparam logic [{n - 1}:0][JOBS_PER_NODE-1:0][7:0] JOB_SRC_EP = "
      f"{_rows(_SRC_EP, ep_fmt)};")
    w(f"    localparam logic [{n - 1}:0][JOBS_PER_NODE-1:0][7:0] JOB_DST_EP = "
      f"{_rows(_DST_EP, ep_fmt)};")
    w("")
    w("    // -------------------------------------------------------------------------")
    w("    // Memory backdoor (iDMA test/include/tb_tasks.svh compares regions the same")
    w("    // way). The endpoint index is a case over CONSTANTS: Verilator 5.048 rejects")
    w("    // g_endpoint[<non-constant>] with \"Could not expand constant selection inside")
    w("    // dotted reference\", so a function cannot take the endpoint as an index.")
    w("    // -------------------------------------------------------------------------")
    mem = "u_endpoint.g_tile_mem[MEM_TARGET].i_mem.i_sim_mem.mem"
    w("    function automatic logic [7:0] mem_byte(input int unsigned ep,")
    w("                                           input logic [ADDR_WIDTH-1:0] a);")
    w("        case (ep)")
    for i in range(n_ep):
        w(f"            {i}: return g_endpoint[{i}].{mem}[a];")
    w("            default: begin")
    w('                $fatal(1, "tb_top_dma: mem_byte endpoint %0d out of range", ep);')
    w("                return 'x;")
    w("            end")
    w("        endcase")
    w("    endfunction")
    w("")
    w("    task automatic mem_poke(input int unsigned ep, input logic [ADDR_WIDTH-1:0] a,")
    w("                            input logic [7:0] d);")
    w("        case (ep)")
    for i in range(n_ep):
        w(f"            {i}: g_endpoint[{i}].{mem}[a] = d;")
    w('            default: $fatal(1, "tb_top_dma: mem_poke endpoint %0d out of range", ep);')
    w("        endcase")
    w("    endtask")
    w("")
    w("    // Preload: a byte's value is a function of its own offset within the job window")
    w("    // (the pattern only sees address[23:0], so it does not vary with the node), so a")
    w("    // byte that lands at the wrong offset is caught instead of matching by luck.")
    w("    // Source regions only -- a destination byte the DMA never wrote has no mem[] key")
    w("    // and reads 0, which differs from its source's pattern at 255 of every 256")
    w("    // addresses, so an unwritten region fails the compare on nearly every byte.")
    w("    function automatic logic [7:0] mem_pattern(input logic [ADDR_WIDTH-1:0] a);")
    w("        return a[7:0] ^ a[15:8] ^ a[23:16] ^ 8'ha5;")
    w("    endfunction")
    w("")
    w("    initial begin")
    w("        for (int unsigned i = 0; i < NUM_NODES; i++)")
    w("            for (int unsigned j = 0; j < JOBS_PER_NODE; j++)")
    w("                for (int unsigned k = 0; k < JOB_BYTES; k++)")
    w("                    mem_poke(JOB_SRC_EP[i][j], JOB_SRC_ADDR[i][j] + k,")
    w("                             mem_pattern(JOB_SRC_ADDR[i][j] + k));")
    w("    end")
    w("")
    w("    function automatic int unsigned compare_region(input int unsigned src_ep,")
    w("                                                   input int unsigned dst_ep,")
    w("                                                   input logic [ADDR_WIDTH-1:0] src_base,")
    w("                                                   input logic [ADDR_WIDTH-1:0] dst_base,")
    w("                                                   input int unsigned n_bytes);")
    w("        int unsigned bad = 0;")
    w("        for (int unsigned k = 0; k < n_bytes; k++) begin")
    w("            logic [7:0] a = mem_byte(src_ep, src_base + k);")
    w("            logic [7:0] b = mem_byte(dst_ep, dst_base + k);")
    w("            // A preload that silently never ran would make every job pass:")
    w("            // axi_sim_mem never creates mem[] for a byte nothing wrote, and an")
    w("            // absent associative-array key reads as the type default, which")
    w("            // this simulator renders 0 rather than X -- so both sides read 0")
    w("            // and compare equal. $isunknown cannot see it; the pattern can.")
    w("            if (a !== mem_pattern(src_base + k))")
    w('                $fatal(1, "tb_top_dma: source byte ep%0d+%0h reads %02h, not the preloaded %02h",')
    w("                       src_ep, src_base + k, a, mem_pattern(src_base + k));")
    w("            if (a !== b) begin")
    w('                if (bad < 8) $display("[DMA] mismatch ep%0d+%0h = %02h, ep%0d+%0h = %02h",')
    w("                                      src_ep, src_base + k, a, dst_ep, dst_base + k, b);")
    w("                bad++;")
    w("            end")
    w("        end")
    w("        return bad;")
    w("    endfunction")
    w("")
    w("    // -------------------------------------------------------------------------")
    w("    // Exit logic - every DMA retires every job its file holds.")
    w("    //")
    w("    // The B that retires a job is the DESTINATION memory's own: axi_sim_mem")
    w("    // writes mem[] on W acceptance and pushes B only inside its last-beat")
    w("    // branch (axi_sim_mem.sv:177 then :182-187), so a retired job is that")
    w("    // endpoint's own statement that the payload is in the array the compare")
    w("    // reads. The real check is the compare: a byte the DMA never wrote has no")
    w("    // mem[] key, reads 0, and differs from its source's pattern.")
    w("    // -------------------------------------------------------------------------")
    w("    localparam int unsigned SETTLE_CYCLES = 100;")
    w("    initial begin")
    w("        bit all_done;")
    w("        int unsigned bad, bad_total;")
    w("        int unsigned aw_idle, aw_same, aw_alloc, ar_idle, ar_same, ar_alloc;")
    w("        int unsigned list_hwm, wtxn_hwm, rtxn_hwm;")
    w("        bad_total = 0;")
    w("        do begin")
    w("            @(posedge clk_i);")
    w("            all_done = rst_ni;")
    w("            for (int i = 0; i < NUM_NODES; i++)")
    w("                all_done &= (jobs_retired[i] >= JOBS_PER_NODE);")
    w("        end while (!all_done);")
    w("        repeat (SETTLE_CYCLES) @(posedge clk_i);")
    w("        // Against the STAMPED count, not against itself. The tables above")
    w("        // describe exactly JOBS_PER_NODE jobs per node, so a file holding more")
    w("        // reaches issued == retired with its extra jobs neither preloaded nor")
    w("        // compared, and a self-comparison would call that a pass.")
    w("        for (int i = 0; i < NUM_NODES; i++)")
    w("            if (jobs_issued[i] != JOBS_PER_NODE || jobs_retired[i] != jobs_issued[i])")
    w('                $fatal(1, "tb_top_dma: node%0d issued %0d and retired %0d jobs; this top is stamped for %0d",')
    w("                       i, jobs_issued[i], jobs_retired[i], JOBS_PER_NODE);")
    w("        for (int i = 0; i < NUM_NODES; i++)")
    w("            for (int j = 0; j < JOBS_PER_NODE; j++) begin")
    w("                bad = compare_region(JOB_SRC_EP[i][j], JOB_DST_EP[i][j],")
    w("                                     JOB_SRC_ADDR[i][j], JOB_DST_ADDR[i][j],")
    w("                                     JOB_BYTES);")
    w("                if (bad != 0)")
    w('                    $display("FAIL: node%0d job%0d: %0d of %0d bytes differ",')
    w("                             i, j, bad, JOB_BYTES);")
    w("                bad_total += bad;")
    w("            end")
    w('        if (bad_total != 0) $fatal(1, "tb_top_dma: %0d bytes differ", bad_total);')
    w(f"        for (int i = 0; i < {n_ep}; i++) begin")
    w("            cmodel_nmu_admission_stats(nmu_ctx[i], aw_idle, aw_same, aw_alloc,")
    w("                                           ar_idle, ar_same, ar_alloc,")
    w("                                           list_hwm, wtxn_hwm, rtxn_hwm);")
    w('            $display("[HWM] node=%0d read_slot_hwm=%0d order_list_hwm=%0d write_txns_hwm=%0d read_txns_hwm=%0d aw_clause={idle=%0d same_dest=%0d alloc=%0d} ar_clause={idle=%0d same_dest=%0d alloc=%0d}",')
    w("                     i, cmodel_nmu_read_slot_hwm(nmu_ctx[i]),")
    w("                     list_hwm, wtxn_hwm, rtxn_hwm,")
    w("                     aw_idle, aw_same, aw_alloc, ar_idle, ar_same, ar_alloc);")
    w("        end")
    w('        $display("PASS: all %0d nodes retired %0d jobs, every region intact",')
    w("                 NUM_NODES, JOBS_PER_NODE);")
    w("        $finish(0);")
    w("    end")
    return lines


def _dpi_error_poll():
    """The DPI error poll and the module tail, shared by both tops."""
    return [
        "",
        "    // -------------------------------------------------------------------------",
        "    // Centralized DPI error poll",
        "    // -------------------------------------------------------------------------",
        '    import "DPI-C" context function int cmodel_check_error(output string msg);',
        "",
        "    always_ff @(posedge clk_i) begin",
        "        /* verilator lint_off WIDTHTRUNC */",
        "        if (rst_ni) begin",
        "            string dpi_err_msg;",
        "            int    dpi_err_code;",
        "            dpi_err_code = cmodel_check_error(dpi_err_msg);",
        "            if (dpi_err_code != 0) begin",
        '                $display("[tb_top] DPI fatal (code=%0d): %s",',
        "                         dpi_err_code, dpi_err_msg);",
        "                cmodel_finalize();",
        '                $fatal(1, "tb_top: DPI error, simulation aborted");',
        "            end",
        "        end",
        "        /* verilator lint_on WIDTHTRUNC */",
        "    end",
        "",
        "endmodule",
        "",
        "`endif  // TB_TOP_SV",
    ]


# ---------------------------------------------------------------------------
# tb_top emitter — instantiates the fabric + pulp VIP endpoints + exit logic
# ---------------------------------------------------------------------------

def emit_tb_top(topo: dict, dma: bool = False,
                jobs_per_node: int = _DMA_JOBS_PER_NODE,
                job_bytes: int = _DMA_JOB_BYTES,
                rw: str = _DMA_RW) -> str:
    rob_enabled = bool(read_rob_enabled())
    name = topo["name"]
    nodes, x_dim, y_dim = _nodes(topo)
    n = len(nodes)
    peripherals = _peripherals(topo)
    endpoints = _endpoints(nodes, peripherals)
    n_ep = len(endpoints)
    dat_num_vc = num_vc()
    # Every loop that walks the initiators walks ENDPOINTS on a topology with a
    # peripheral: each one injects, each one can wedge, and each one has an NMU.
    exit_n = "NUM_ENDPOINTS" if peripherals else "NUM_NODES"

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
    mst_backpressure = _MST_BACKPRESSURE_DMA if dma else _MST_BACKPRESSURE
    mst_stall_out, mst_delay_out = _MST_BACKPRESSURE_PROFILES[mst_backpressure]
    # A third per-beat stall source on the response path, so it lands in the
    # watchdog budget beside the memory's. Zero under "ideal".
    mst_cyc_per_beat = _STALL_RANDOM_MAX_CYCLES if mst_stall_out else mst_delay_out
    per_node, noc_egress_base = tile_targets(topo, endpoints)
    # The widest row, not endpoint 0's: a peripheral owns one window where a
    # tile owns two, and tile_targets pads the short rows out to this width.
    n_targets = max(len(w) for w in per_node.values())
    # ADDR_WIDTH'(...) casts, not sized literals: the field width has to follow
    # ni_params_pkg::AXI_ADDR_WIDTH_DFLT, or a width change would silently
    # mis-align the concatenation.
    def _rows(key):
        return ", ".join(
            "{" + ", ".join(f"ADDR_WIDTH'(64'h{t[key]:X})"
                             for t in reversed(per_node[i])) + "}"
            for i in reversed(range(n_ep)))
    tile_base_addr = _rows("base")
    tile_size = _rows("size")
    lines = []
    w = lines.append

    w("`timescale 1ns/1ps")
    w("")
    w("// AUTO-GENERATED by sim/tools/gen_tb_top.py")
    w(f"// Topology: {name}  ({x_dim}x{y_dim}, dat_num_vc={dat_num_vc})")
    w("// DO NOT EDIT - modify the generator or sim/configs/*.yml instead.")
    w("//")
    w(f"// {n} nodes live inside noc_fabric (ni_wrap=NMU+NSU + REQ/RSP router per")
    w("// node, joined by directional links). tb_top creates the DPI handles, attaches a")
    if dma:
        w("// dma_node_endpoint (pulp iDMA backend + axi_delayer/axi_sim_mem + FlooNoC")
        w("// axi_bw_monitor) to each node's master/slave AXI faces, and owns the exit logic.")
        w("// Stimulus: <stim_dir>/node<i>/jobs.txt, one iDMA job per eleven fields.")
    else:
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
    w('`include "noc_fabric.sv"')
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
    if peripherals:
        w(f"    // Endpoints, not nodes: {len(peripherals)} peripheral(s) have an NI and an")
        w("    // endpoint but no router. Each carries stimulus of its own, so the exit")
        w("    // logic gates on all of them.")
        w(f"    localparam int unsigned NUM_ENDPOINTS = {n_ep};")
    w("    localparam int unsigned ID_WIDTH      = ni_params_pkg::AXI_ID_WIDTH_DFLT;")
    w("    localparam int unsigned ADDR_WIDTH    = ni_params_pkg::AXI_ADDR_WIDTH_DFLT;")
    w("    localparam int unsigned DATA_WIDTH    = ni_params_pkg::AXI_DATA_WIDTH_DFLT;")
    w(f"    localparam int unsigned DAT_NUM_VC     = {dat_num_vc};"
      "  // specgen constants.yaml noc.DAT_NUM_VC")
    w("    // NMU read reorder buffer: 1 = the reorder-buffer response path")
    w("    // docs/noc-target-spec.md section 3 describes, 0 = the RoBless bypass with")
    w("    // its per-id single-outstanding interlock. int unsigned, not bit: it goes")
    w("    // straight into cmodel_nmu_create_ex's `input int rob_enabled`.")
    w(f"    localparam int unsigned READ_ROB_ENABLED = {1 if rob_enabled else 0};"
      "  // specgen constants.yaml nmu.READ_ROB_ENABLED")
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
    w(f"    localparam logic [{n_ep - 1}:0][TILE_TARGETS-1:0][ADDR_WIDTH-1:0] TILE_BASE_ADDR = "
      f"{{{tile_base_addr}}};")
    w(f"    localparam logic [{n_ep - 1}:0][TILE_TARGETS-1:0][ADDR_WIDTH-1:0] TILE_SIZE = "
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
    w(f'    // Master-face backpressure profile "{mst_backpressure}" (gen_tb_top.py')
    w("    // _MST_BACKPRESSURE_PROFILES). One axi_delayer per endpoint between the")
    w("    // file master and the tile crossbar, response side only.")
    w(f"    localparam bit          MST_STALL_RANDOM_OUTPUT = 1'b{mst_stall_out};")
    w(f"    localparam int unsigned MST_FIXED_DELAY_OUTPUT  = {mst_delay_out};")
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
    w(f"    int unsigned last_progress  [{exit_n}];")
    w(f"    int unsigned axi_outstanding[{exit_n}];")
    w("")
    w("    always_ff @(posedge clk_i) begin")
    w("        if (!rst_ni) begin")
    w("            live_cyc <= 0;")
    w(f"            for (int i = 0; i < {exit_n}; i++) begin")
    w("                last_progress[i]   <= 0;")
    w("                axi_outstanding[i] <= 0;")
    w("            end")
    w("        end else begin")
    w("            live_cyc <= live_cyc + 1;")
    w(f"            for (int i = 0; i < {exit_n}; i++) begin")
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
    w(f"    localparam int unsigned MST_CYC_PER_BEAT    = {mst_cyc_per_beat};")
    w("    localparam int unsigned K_CYC_PER_BEAT  = FABRIC_CYC_PER_BEAT + MEM_CYC_PER_BEAT")
    w("                                            + MST_CYC_PER_BEAT;")
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
    w(f"            + K_CYC_PER_BEAT * (tb_num_reads + tb_num_writes) * MAX_BURST_BEATS * {exit_n};")
    w("        // Forensics override: fire the watchdog just past a known freeze")
    w("        // point so the state dump lands without waiting out the formula.")
    w('        void\'($value$plusargs("timeout_cycles=%d", timeout_cycles));')
    w("        repeat (timeout_cycles) @(posedge clk_i);")
    w("        // Per-node SV-side summary, then the c_model fabric state dump.")
    w(f"        for (int i = 0; i < {exit_n}; i++) begin")
    w('            $display("[WATCHDOG] node%0d txn_cnt=%0d end_of_sim=%0d outstanding=%0d last_progress=%0d (idle %0d cyc) mst[awv=%0d wv=%0d arv=%0d rr=%0d br=%0d] slv[awv=%0d wv=%0d arv=%0d rv=%0d bv=%0d]",')
    w("                     i, txn_cnt[i], end_of_sim[i],")
    w("                     axi_outstanding[i], last_progress[i], live_cyc - last_progress[i],")
    w("                     master_axi_req[i].awvalid, master_axi_req[i].wvalid,")
    w("                     master_axi_req[i].arvalid, master_axi_req[i].rready,")
    w("                     master_axi_req[i].bready,")
    w("                     slave_axi_req[i].awvalid, slave_axi_req[i].wvalid,")
    w("                     slave_axi_req[i].arvalid,")
    w("                     slave_axi_rsp[i].rvalid, slave_axi_rsp[i].bvalid);")
    if dma:
        w("            // What the DMA exit condition is waiting on: a node short of its")
        w("            // job count stalled in the backend, with the shortfall telling")
        w("            // apart a request never issued from a response never returned.")
        w('            $display("[WATCHDOG] node%0d dma issued=%0d retired=%0d",')
        w("                     i, jobs_issued[i], jobs_retired[i]);")
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
    w('    import "DPI-C" context function void cmodel_nmu_admission_stats(input longint unsigned ctx,')
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
    w('                                                                 input int port_id,')
    w('                                                                 input string config_path);')
    w('    import "DPI-C" context function longint unsigned cmodel_nsu_create(input string name,')
    w('                                                              input int src_id, input int num_vc,')
    w('                                                              input int max_unique_ids,')
    w('                                                              input int max_outstanding,')
    w('                                                              input int port_id,')
    w('                                                              input string config_path);')
    w('    import "DPI-C" context function longint unsigned cmodel_dat_merge_create(input string name,')
    w('                                                                    input int dat_num_vc);')
    w("")

    # ctx handle ARRAYS (chandle-substitute longint unsigned). Arrays let the
    # fabric take them as array ports and instantiate nodes via a genvar loop.
    w(f"    longint unsigned router_ctx     [{n}];")
    w(f"    longint unsigned nmu_ctx        [{n_ep}];")
    w(f"    longint unsigned nsu_ctx        [{n_ep}];")
    w(f"    longint unsigned dat_merge_ctx  [{n_ep}];")
    w("")
    w("    // SAM config: the sim/configs/ file, with its endpoints block. Empty (the")
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
    # NI creates cover the ENDPOINT space: the routers first, then one per
    # peripheral. src_id is the route coordinate the topology states for it --
    # that id is stamped into every request the endpoint emits and is what its
    # responses come back to. A peripheral now SHARES its router's coordinate,
    # so src_id no longer tells the two apart: port_id does, and it is the same
    # port the SAM entry for this endpoint's region carries.
    for (i, x, y, c, port) in endpoints:
        which = f"node{i}, port 0 = LOCAL" if port == 0 else f"peripheral{i - n}, port {port}"
        w(f'        nmu_ctx[{i}] = cmodel_nmu_create_ex("nmu_{i}", {c}, DAT_NUM_VC, '
          f'READ_ROB_ENABLED, b_rob_depth, r_rob_depth, max_txns_per_id, {port}, '
          f'sam_config_path);  '
          f'// src_id = coord {c}, {which}')
        w(f'        nsu_ctx[{i}] = cmodel_nsu_create("nsu_{i}", {c}, DAT_NUM_VC, max_unique_ids, '
          f'max_outstanding, {port}, sam_config_path);')
        w(f'        dat_merge_ctx[{i}] = cmodel_dat_merge_create("dat_merge_{i}", DAT_NUM_VC);')
    w("    end")
    w("")

    # Per-node AXI buses as STRUCT ARRAYS (master-side into the fabric NMU,
    # slave-side out of NSU), shared by the fabric + the endpoint generate loop.
    w("    // -------------------------------------------------------------------------")
    w("    // Per-node AXI buses (struct arrays): master-side into NMU, slave-side out of NSU")
    w("    // -------------------------------------------------------------------------")
    w(f"    ni_signals_pkg::axi_req_t  master_axi_req [{n_ep}];  // tb master -> NMU")
    w(f"    logic [ni_params_pkg::AXI_AWUSER_WIDTH_DFLT-1:0] master_awuser [{n_ep}];  // AWUSER sideband")
    w(f"    ni_signals_pkg::axi_rsp_t  master_axi_rsp [{n_ep}];  // NMU -> tb master")
    w(f"    ni_signals_pkg::axi_req_t  slave_axi_req  [{n_ep}];  // NSU -> tb slave")
    w(f"    ni_signals_pkg::axi_rsp_t  slave_axi_rsp  [{n_ep}];  // tb slave -> NSU")
    w("")

    # Fabric instance: ctx + AXI arrays passed whole.
    w("    // -------------------------------------------------------------------------")
    w(f"    // NoC fabric ({n} nodes, directional links)")
    w("    // -------------------------------------------------------------------------")
    w("    noc_fabric #(")
    w(f"        .X_DIM({x_dim}), .Y_DIM({y_dim}),")
    w("        .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),")
    w("        .DAT_NUM_VC(DAT_NUM_VC), .REQ_FLIT_WIDTH(REQ_FLIT_WIDTH),")
    w("        .RSP_FLIT_WIDTH(RSP_FLIT_WIDTH), .DAT_FLIT_WIDTH(DAT_FLIT_WIDTH),")
    if peripherals:
        w("        .ROUTER_VC_DEPTH(ROUTER_VC_DEPTH),")
        # Packed, descending, as the tile-window parameters above: field p is
        # peripheral p and peripheral p is endpoint NUM_NODES + p. PERIPH_PORT
        # carries the FACE (1 = x, 2 = y); the host router's own edge picks
        # which port of that axis is meant.
        w(f"        .N_PERIPH({len(peripherals)}),")
        w("        .PERIPH_NODE({"
          + ", ".join(f"8'd{per['router_idx']}" for per in reversed(peripherals))
          + "}),")
        w("        .PERIPH_PORT({"
          + ", ".join(f"8'd{per['port']}" for per in reversed(peripherals))
          + "})")
    else:
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
    if dma:
        w("    // Test endpoints - one dma_node_endpoint per node (pulp iDMA backend +")
        w("    // axi_xbar tile crossbar + two axi_delayer/axi_sim_mem targets +")
        w("    // bw monitor). dma_node_endpoint.sv is user-owned and NOT regenerated.")
    else:
        w("    // Test endpoints - one user_node_endpoint per node (pulp file_master +")
        w("    // axi_xbar tile crossbar + two axi_delayer/axi_sim_mem targets +")
        w("    // in-endpoint scoreboard +")
        w("    // bw monitor). user_node_endpoint.sv is user-owned and NOT regenerated.")
    if peripherals:
        w(f"    // {n_ep} endpoints, not NUM_NODES: endpoints {n}..{n_ep - 1} are the")
        w("    // peripherals, which have an NI and an endpoint but no router. They carry")
        w("    // their own stimulus, so the exit logic below gates on all of them.")
    w("    // -------------------------------------------------------------------------")
    w(f"    logic        end_of_sim [{n_ep}];")
    w(f"    int unsigned txn_cnt    [{n_ep}];")
    if dma:
        w("    // The job driver's own counts, brought up one endpoint at a time: the")
        w("    // exit logic indexes them with a variable, which a hierarchical name")
        w("    // through g_endpoint cannot be.")
        w(f"    int unsigned jobs_issued  [{n_ep}];")
        w(f"    int unsigned jobs_retired [{n_ep}];")
    w(f"    for (genvar i = 0; i < {n_ep}; i++) begin : g_endpoint")
    w(f"        {'dma_node_endpoint' if dma else 'user_node_endpoint'} #(")
    w("            .NODE_ID(i),")
    w("            .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),")
    w("            .TILE_TARGETS(TILE_TARGETS), .TILE_BASE_ADDR(TILE_BASE_ADDR[i]),")
    w("            .TILE_SIZE(TILE_SIZE[i]), .NOC_EGRESS_BASE(NOC_EGRESS_BASE),")
    w("            .MEM_STALL_RANDOM_INPUT(MEM_STALL_RANDOM_INPUT),")
    w("            .MEM_STALL_RANDOM_OUTPUT(MEM_STALL_RANDOM_OUTPUT),")
    w("            .MEM_FIXED_DELAY_INPUT(MEM_FIXED_DELAY_INPUT),")
    w("            .MEM_FIXED_DELAY_OUTPUT(MEM_FIXED_DELAY_OUTPUT),")
    w("            .MST_STALL_RANDOM_OUTPUT(MST_STALL_RANDOM_OUTPUT),")
    w("            .MST_FIXED_DELAY_OUTPUT(MST_FIXED_DELAY_OUTPUT)")
    w("        ) u_endpoint (")
    w("            .clk_i(clk_i), .rst_ni(rst_ni),")
    w("            .master_axi_req_o(master_axi_req[i]), .master_awuser_o(master_awuser[i]),")
    w("            .master_axi_rsp_i(master_axi_rsp[i]),")
    w("            .slave_axi_req_i(slave_axi_req[i]),   .slave_axi_rsp_o(slave_axi_rsp[i]),")
    w("            .end_of_sim_o(end_of_sim[i]), .txn_cnt_o(txn_cnt[i])")
    w("        );")
    if dma:
        w("        assign jobs_issued[i]  = u_endpoint.jobs_issued;")
        w("        assign jobs_retired[i] = u_endpoint.jobs_retired;")
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
    # require every node moved at least one AW/AR handshake. Under --dma the DMA
    # top's own version replaces it: end_of_sim is tied 0 there, and "every job
    # retired, every region intact" is what done means for a DMA.
    if dma:
        lines.extend(_dma_check(topo, n_ep, jobs_per_node, job_bytes, rw))
        lines.extend(_dpi_error_poll())
        return "\n".join(lines) + "\n"
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
    w(f"            for (int i = 0; i < {exit_n}; i++)")
    w("                all_done &= end_of_sim[i];  // scoreboard is in-endpoint")
    w("        end while (!all_done);")
    w("        repeat (SETTLE_CYCLES) @(posedge clk_i);")
    w("        vacuous = 1'b0;")
    w(f"        for (int i = 0; i < {exit_n}; i++) begin")
    w("            if (txn_cnt[i] == 0) begin")
    w("                vacuous = 1'b1;")
    w('                $display("FAIL: node%0d completed zero transactions (vacuous)", i);')
    w("            end")
    w("        end")
    w('        if (vacuous) $fatal(1, "tb_top: vacuous run");')
    w("        // Sizing statistics per node: RoB slot peak, the SPEC 17 admission")
    w("        // clause split, the per-id order-list peak and the shared-pool peaks.")
    w(f"        for (int i = 0; i < {exit_n}; i++) begin")
    w("            cmodel_nmu_admission_stats(nmu_ctx[i], aw_idle, aw_same, aw_alloc,")
    w("                                           ar_idle, ar_same, ar_alloc,")
    w("                                           list_hwm, wtxn_hwm, rtxn_hwm);")
    w('            $display("[HWM] node=%0d read_slot_hwm=%0d order_list_hwm=%0d write_txns_hwm=%0d read_txns_hwm=%0d aw_clause={idle=%0d same_dest=%0d alloc=%0d} ar_clause={idle=%0d same_dest=%0d alloc=%0d}",')
    w("                     i, cmodel_nmu_read_slot_hwm(nmu_ctx[i]),")
    w("                     list_hwm, wtxn_hwm, rtxn_hwm,")
    w("                     aw_idle, aw_same, aw_alloc, ar_idle, ar_same, ar_alloc);")
    w("        end")
    w(f'        $display("PASS: all %0d nodes done, non-vacuous", {exit_n});')
    w("        $finish(0);")
    w("    end")
    lines.extend(_dpi_error_poll())
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# topology_pkg emitter -- the address map, for the testbench to `import`
# instead of a generated top declaring it inline.
# ---------------------------------------------------------------------------

def _is_power_of_two(value: int) -> bool:
    return value > 0 and (value & (value - 1)) == 0


def _sam_range_error(endpoint_name: str, detail: str) -> None:
    raise SystemExit(f"gen_tb_top: endpoint {endpoint_name}: {detail}")


def _validate_sam_ranges(topo: dict) -> None:
    """Reject ranges that cannot be represented by the generated address type."""
    addr_limit = 1 << ADDR_WIDTH
    for endpoint in topo["endpoints"]:
        if endpoint.get("sbr_port_protocol") is None:
            continue
        member_count = address_map.members(endpoint)
        for rule in endpoint["addr_range"]:
            try:
                base = int(rule["base"])
                size = int(rule["size"])
                stride = int(rule["stride"]) if rule.get("stride") is not None else size
            except (KeyError, TypeError, ValueError) as error:
                _sam_range_error(endpoint["name"], f"range has a non-integer field ({error})")
            if size <= 0 or size % _SAM_ALIGNMENT != 0:
                _sam_range_error(endpoint["name"],
                                 "range size must be positive and 4 KB aligned")
            if base < 0 or stride <= 0 or base % _SAM_ALIGNMENT != 0 or \
                    stride % _SAM_ALIGNMENT != 0:
                _sam_range_error(endpoint["name"],
                                 "range base and stride must be 4 KB aligned and non-negative")
            for member in range(member_count):
                start = base + stride * member
                end = start + size
                if start >= end or start >= addr_limit or end >= addr_limit:
                    _sam_range_error(endpoint["name"],
                                     "range expansion does not fit the canonical address width")


def _collective_selectors(entries: list, x_dim: int, y_dim: int) -> dict:
    """Return representable X/Y selector pairs for each collective-capable space."""
    selectors = {}
    for space in ("config", "memory"):
        space_entries = [entry for entry in entries
                         if entry["space"] == space and entry["port"] == 0]
        by_coord = {(entry["x"], entry["y"]): entry for entry in space_entries}
        if len(space_entries) != x_dim * y_dim or len(by_coord) != len(space_entries):
            continue
        origin = by_coord.get((0, 0))
        x_neighbor = by_coord.get((1, 0))
        if origin is None or x_neighbor is None:
            continue
        stride = x_neighbor["base"] - origin["base"]
        if not _is_power_of_two(stride):
            continue
        if any(entry["size"] != origin["size"] for entry in space_entries):
            continue
        if any(entry["base"] != origin["base"] + (y * x_dim + x) * stride
               for (x, y), entry in by_coord.items()):
            continue
        x_len = (x_dim - 1).bit_length()
        y_len = (y_dim - 1).bit_length()
        offset = stride.bit_length() - 1
        field_mask = (((1 << x_len) - 1) << offset) | \
                     (((1 << y_len) - 1) << (offset + x_len))
        if origin["base"] & field_mask or origin["size"] > stride:
            continue
        if offset + x_len + y_len > ADDR_WIDTH:
            continue
        selectors[space] = ((offset, x_len), (offset + x_len, y_len))
    return selectors


def _sam_rules(topo: dict):
    """Expand YAML rules and attach collective metadata for package emission."""
    _validate_sam_ranges(topo)
    _bases, entries = address_map.pack_config(topo)
    _nodes_, x_dim, y_dim = _nodes(topo)
    selectors = _collective_selectors(entries, x_dim, y_dim)

    rules = []
    for entry in entries:
        collective_enabled = bool(entry.get("en_collective", False))
        if collective_enabled:
            if entry["space"] not in selectors:
                raise SystemExit("gen_tb_top: collective layout is not representable for "
                                 f"{entry['space']} space")
            mask_x, mask_y = selectors[entry["space"]]
        else:
            mask_x = (0, 0)
            mask_y = (0, 0)
        rules.append({
            **entry,
            "collective_en": collective_enabled,
            "mask_x": mask_x,
            "mask_y": mask_y,
        })
    return rules

def emit_topology_pkg(topo: dict) -> str:
    """Address-map package for the selected configuration: TILE_BASE_ADDR /
    TILE_SIZE / NOC_EGRESS_BASE / the peripheral table, computed exactly as
    emit_tb_top computes them for tb_top's own parameter block -- relocated
    here, not re-derived. Mirrors FlooNoC's floo_axi_mesh_noc_pkg supplying
    Sam[] to tb_floo_axi_mesh.sv.

    The package NAME is fixed, its CONTENTS vary with CONFIG: sim/tb/
    tb_noc_mesh.sv carries one `import`, so two configurations cannot coexist
    in a build tree without regenerating -- FlooNoC's property too.
    """
    nodes, x_dim, y_dim = _nodes(topo)
    n = len(nodes)
    peripherals = _peripherals(topo)
    endpoints = _endpoints(nodes, peripherals)
    n_ep = len(endpoints)
    sam_rules = _sam_rules(topo)
    per_node, noc_egress_base = tile_targets(topo, endpoints)
    n_targets = max(len(w) for w in per_node.values())
    # Same cast and row order as emit_tb_top's TILE_BASE_ADDR/TILE_SIZE: packed,
    # descending, field t is target t and row i is node i.
    def _rows(key):
        return ", ".join(
            "{" + ", ".join(f"ADDR_WIDTH'(64'h{t[key]:X})"
                             for t in reversed(per_node[i])) + "}"
            for i in reversed(range(n_ep)))
    tile_base_addr = _rows("base")
    tile_size = _rows("size")

    geom = topo["name"]
    pkg = "topology_pkg"
    guard = pkg.upper() + "_SVH"
    # Sized max(N_PERIPH, 1): a packed array cannot have zero elements
    # (noc_fabric.sv's N_PERIPH_MAX parameter mirrors the same constraint).
    periph_width = max(len(peripherals), 1)
    # Packed, descending, as PERIPH_NODE/PERIPH_PORT in the fabric
    # instantiation: field p is peripheral p, so element [0] is the LSB byte.
    node_bits = ", ".join(f"8'd{per['router_idx']}" for per in reversed(peripherals)) \
        if peripherals else "8'd0"
    port_bits = ", ".join(f"8'd{per['port']}" for per in reversed(peripherals)) \
        if peripherals else "8'd0"

    lines = []
    w = lines.append
    w("`timescale 1ns/1ps")
    w("")
    w("// AUTO-GENERATED by sim/tools/gen_tb_top.py --emit-topology-pkg")
    w(f"// Geometry: {geom}  ({x_dim}x{y_dim}, {len(peripherals)} peripheral(s))")
    w("// DO NOT EDIT - modify the generator or sim/configs/*.yml instead.")
    w("//")
    w("// Address-map constants for the selected configuration: TILE_BASE_ADDR /")
    w("// TILE_SIZE / NOC_EGRESS_BASE / the peripheral table, the same values emit_tb_top")
    w("// stamps into tb_top's own parameter block. Mirrors FlooNoC's")
    w("// floo_axi_mesh_noc_pkg supplying Sam[] to tb_floo_axi_mesh.sv.")
    w("")
    w(f"`ifndef {guard}")
    w(f"`define {guard}")
    w("")
    w(f"package {pkg};")
    w("")
    w(f"    localparam int unsigned X_DIM = {x_dim};")
    w(f"    localparam int unsigned Y_DIM = {y_dim};")
    w(f"    localparam int unsigned NUM_NODES     = {n};")
    w(f"    localparam int unsigned NUM_ENDPOINTS = {n_ep};")
    w("    localparam int unsigned ADDR_WIDTH = ni_flit_pkg::AXI_ADDR_WIDTH;")
    w(f"    localparam int unsigned SAM_NUM_RULES = {len(sam_rules)};")
    w("    localparam int unsigned SAM_MASK_SEL_WIDTH = $clog2(ADDR_WIDTH + 1);")
    w("")
    w("    typedef logic [ADDR_WIDTH-1:0] sam_addr_t;")
    w("    typedef struct packed {")
    w("        logic [SAM_MASK_SEL_WIDTH-1:0] offset;")
    w("        logic [SAM_MASK_SEL_WIDTH-1:0] len;")
    w("    } sam_mask_sel_t;")
    w("    typedef struct packed {")
    w("        logic [ni_flit_pkg::DST_ID_WIDTH-1:0] dst_id;")
    w("        logic [ni_flit_pkg::DST_PORT_ID_WIDTH-1:0] dst_port_id;")
    w("        logic is_data;")
    w("        logic collective_en;")
    w("        sam_mask_sel_t mask_x;")
    w("        sam_mask_sel_t mask_y;")
    w("    } sam_idx_t;")
    w("    typedef struct packed {")
    w("        sam_idx_t idx;")
    w("        sam_addr_t start_addr;")
    w("        sam_addr_t end_addr;")
    w("    } sam_rule_t;")
    w("    localparam sam_rule_t [SAM_NUM_RULES-1:0] SAM = '{")
    for authored_index, rule in enumerate(sam_rules):
        generated_index = len(sam_rules) - 1 - authored_index
        comma = "," if generated_index else ""
        w(f"        // Authored rule {authored_index}: SAM[{generated_index}]")
        w("        " +
          f"{generated_index}: '{{idx: '{{dst_id: "
          f"ni_flit_pkg::DST_ID_WIDTH'({rule['dst_id']}), "
          f"dst_port_id: ni_flit_pkg::DST_PORT_ID_WIDTH'({rule['port']}), "
          f"is_data: 1'b{int(rule['space'] != 'config')}, "
          f"collective_en: 1'b{int(rule['collective_en'])}, "
          f"mask_x: '{{offset: SAM_MASK_SEL_WIDTH'({rule['mask_x'][0]}), "
          f"len: SAM_MASK_SEL_WIDTH'({rule['mask_x'][1]})}}, "
          f"mask_y: '{{offset: SAM_MASK_SEL_WIDTH'({rule['mask_y'][0]}), "
          f"len: SAM_MASK_SEL_WIDTH'({rule['mask_y'][1]})}}}}, "
          f"start_addr: ADDR_WIDTH'(64'h{rule['base']:012X}), "
          f"end_addr: ADDR_WIDTH'(64'h{rule['base'] + rule['size']:012X})}}{comma}")
    w("    };")
    w(f"    localparam int unsigned TILE_TARGETS = {n_targets};")
    w(f"    localparam logic [{n_ep - 1}:0][TILE_TARGETS-1:0][ADDR_WIDTH-1:0] TILE_BASE_ADDR = "
      f"{{{tile_base_addr}}};")
    w(f"    localparam logic [{n_ep - 1}:0][TILE_TARGETS-1:0][ADDR_WIDTH-1:0] TILE_SIZE = "
      f"{{{tile_size}}};")
    w(f"    localparam logic [ADDR_WIDTH-1:0] NOC_EGRESS_BASE = "
      f"ADDR_WIDTH'(64'h{noc_egress_base:X});")
    w(f"    localparam longint unsigned REGION_BYTES = 64'h{_DEFAULT_REGION_BYTES:X};")
    w("")
    w(f"    localparam int unsigned N_PERIPH = {len(peripherals)};")
    w(f"    localparam logic [{periph_width - 1}:0][7:0] PERIPH_NODE = {{{node_bits}}};")
    w(f"    localparam logic [{periph_width - 1}:0][7:0] PERIPH_PORT = {{{port_bits}}};")
    w("")
    w(f"endpackage : {pkg}")
    w("")
    w(f"`endif  // {guard}")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description="Generate tb_top_<topology>.sv.")
    ap.add_argument("--topology", default="mesh_4x4",
                    help="Configuration name (matches sim/configs/<name>.yml)")
    ap.add_argument("--out", default=None,
                    help="Output tb_top.sv path (default: sim/tb/test/tb_top_<topology>.sv)")
    ap.add_argument("--dma", action="store_true",
                    help="Emit the iDMA top instead: dma_node_endpoint per node and "
                         "sim/tb/soc/tb_top_dma_<topology>.sv as the default output. It "
                         "instantiates the same noc_fabric with the same overrides -- the "
                         "endpoint's port list toward it does not move.")
    ap.add_argument("--jobs-per-node", type=int, default=_DMA_JOBS_PER_NODE,
                    help="--dma only: jobs each node's file holds. Must match the "
                         "gen_dma_jobs.py run that wrote them -- the top preloads and "
                         "compares the regions those jobs name.")
    ap.add_argument("--length", type=lambda v: int(v, 0), default=_DMA_JOB_BYTES,
                    help="--dma only: bytes moved per job. Must match gen_dma_jobs.py.")
    ap.add_argument("--rw", choices=("read", "write"), default=_DMA_RW,
                    help="--dma only: direction for every job. Must match the "
                         "gen_dma_jobs.py run that wrote them.")
    ap.add_argument("--print-num-vc", action="store_true",
                    help="Print noc.DAT_NUM_VC (specgen/source/constants.yaml) and exit. "
                         "sim/build_config.mk picks the per-VC noc_types_pkg with it, so that "
                         "value is read where it is declared instead of parsed back out of "
                         "the topology name. The config is still loaded first, so the "
                         "flit-capacity check runs on this branch too.")
    ap.add_argument("--emit-topology-pkg", action="store_true",
                    help="Emit the address-map package instead of tb_top: topology_pkg.sv, "
                         "exporting the same TILE_BASE_ADDR / TILE_SIZE / NOC_EGRESS_BASE / "
                         "peripheral table tb_top stamps inline, for sim/tb/tb_noc_mesh.sv "
                         "to `import`. Default output is build/generated/<CONFIG>/topology_pkg.sv. "
                         "The name is fixed and the contents follow --topology, so switching "
                         "configuration means regenerating it.")
    a = ap.parse_args()

    topo = load_topology(a.topology)
    if a.print_num_vc:
        print(num_vc())
        return 0
    if a.emit_topology_pkg:
        default_out = ROOT / "build" / "generated" / a.topology / "topology_pkg.sv"
        out_path = Path(a.out) if a.out is not None else default_out
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(emit_topology_pkg(topo), encoding="utf-8")
        return 0
    # The DMA flavour cannot run a peripheral topology at all
    # (docs/known-limitations.md): gen_dma_jobs.job_table walks the router array
    # only, so a peripheral endpoint gets no jobs.txt and its idma_job_driver
    # $fatals on the missing file. Refused here rather than emitted, because the
    # top it would emit is also wrong in a quieter way -- _dma_check's
    # MEM_TARGET is the LAST target, which on a peripheral's padded row is the
    # pad rather than that endpoint's memory, so the preload and the region
    # compare would address a window parked above the map with nothing behind
    # it. Not a zero-size window: addr_decode_dync reads a zero end_addr as the
    # END-OF-ADDRESS-SPACE WILDCARD, which is why the pad is a real
    # _PAD_BYTES range instead.
    if a.dma and _peripherals(topo):
        raise SystemExit(
            f"gen_tb_top: --dma does not support configuration {a.topology}, which attaches an "
            f"endpoint to a boundary port -- gen_dma_jobs.job_table emits jobs for the router "
            f"array only, so a peripheral endpoint has no jobs.txt and its idma_job_driver "
            f"$fatals on the missing file (docs/known-limitations.md). Use a configuration "
            f"whose endpoints are all on EJECT, or the directed top (no --dma)")
    tb_text = emit_tb_top(topo, a.dma, a.jobs_per_node, a.length, a.rw)
    default_out = ROOT / "sim" / "tb" / "soc" / f"tb_top_dma_{a.topology}.sv" if a.dma \
        else ROOT / "sim" / "tb" / "test" / f"tb_top_{a.topology}.sv"
    out_path = Path(a.out) if a.out is not None else default_out
    out_path.write_text(tb_text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
