// router_wrap — DPI wrapper for one node's three physical-network routers.
//
// One node owns its REQ + RSP + DAT routers at (x_coord, y_coord) in an NxM
// mesh. Every network's pins are ONE uniform per-PORT array sized
// LINK_PORTS (router::ROUTER_PORT_COUNT = 5: LOCAL + N/E/S/W). Index LOCAL
// carries this node's own NMU/NSU traffic (wired by the fabric to the
// nmu_wrap/nsu_wrap scalar ports); indices N/E/S/W carry the inter-router
// links to each existing neighbor. Boundary directions with no neighbor are
// left unwired by the fabric (their wires stay tied to 0; a violation is a
// fabric-generated $fatal, not a check in this module).
//
// Naming, node's own view (spec §4.3, S3a T5 mechanical rename):
//   tx_<net>_*  : this node's transmit side (`*_out_*` before this stage)
//   rx_<net>_*  : this node's receive side  (`*_in_*` before this stage)
//
// REQ / RSP (SimpleRouter, ready/valid, single VC per S1 Q2): scalar
// ready/valid per port, no credit. TXREQREADY-class pins are packed
// [LINK_PORTS-1:0] vectors (one bit per port), matching spec §7.
//
// DAT (Router, credit, DAT_NUM_VC virtual channels): unchanged FlooNoC
// pulse-credit mechanism, now applied uniformly to LOCAL too instead of
// LOCAL-special-cased + LINK-looped. Credit is a per-VC pulse vector, one
// word per port (unpacked array [LINK_PORTS] of [DAT_NUM_VC-1:0] packed).
//
// Registered-DPI-tick discipline (shared by the NI wraps nmu_wrap/nsu_wrap): on every posedge clk_i
// the module samples the PREVIOUS cycle's registered wire inputs, pushes them to the
// C++ model via DPI set_inputs, advances the model via tick, pulls outputs
// via get_outputs, and registers those outputs nonblocking so they are
// visible to SV wires from the NEXT cycle onward.
//
// DPI split one-call-per-network (S3a T5 debug finding): set_inputs/
// get_outputs are split into three calls each (req/rsp/dat), one tick() call
// shared. The original single combined call married three DIFFERENT flit
// widths (136/126/633 b) as [LINK_PORTS]-sized unpacked-array arguments in
// one DPI signature -- the only place in this codebase asking one DPI call to
// marshal more than one parameterized per-element width. Co-sim showed
// rx_req_ready/rx_rsp_ready permanently stuck at 0 despite the standalone
// C++ model proving correct in isolation (ready=1 after one idle tick,
// matching SimpleRouter's own math) -- i.e. the fault was in DPI/SV
// marshalling, not model logic. Splitting removes the mixed-width construct
// outright: every call below now marshals exactly one flit width.
//
// The longint unsigned ctx_i is created by tb_top (cmodel_router_create with x_coord);
// this wrap only imports set_inputs/tick/get_outputs.
//
// Reset: synchronous active-low (rst_ni). Output registers cleared on reset.

`timescale 1ns/1ps

`ifndef ROUTER_WRAP_SV
`define ROUTER_WRAP_SV

module router_wrap #(
    parameter int unsigned DAT_NUM_VC     = ni_params_pkg::NOC_DAT_NUM_VC_DFLT,
    parameter int unsigned REQ_FLIT_WIDTH = ni_params_pkg::NOC_REQ_FLIT_WIDTH_DFLT,
    parameter int unsigned RSP_FLIT_WIDTH = ni_params_pkg::NOC_RSP_FLIT_WIDTH_DFLT,
    parameter int unsigned DAT_FLIT_WIDTH = ni_params_pkg::NOC_DAT_FLIT_WIDTH_DFLT,
    // Router port count (LOCAL + N/E/S/W). Mirrors c_model ROUTER_PORT_COUNT /
    // ROUTER_LINK_PORTS; every network's pins are marshalled port-major over
    // these. Fixed at 5; not overridden (kept as a parameter so the port
    // list can use it).
    parameter int unsigned LINK_PORTS     = 5
) (
    input  logic                  clk_i,
    input  logic                  rst_ni,
    input  longint unsigned       ctx_i,

    // REQ network (ready/valid, single VC): per-port.
    output logic [LINK_PORTS-1:0]     tx_req_valid,
    output logic [REQ_FLIT_WIDTH-1:0] tx_req_flit  [LINK_PORTS],
    input  logic [LINK_PORTS-1:0]     tx_req_ready,
    input  logic [LINK_PORTS-1:0]     rx_req_valid,
    input  logic [REQ_FLIT_WIDTH-1:0] rx_req_flit  [LINK_PORTS],
    output logic [LINK_PORTS-1:0]     rx_req_ready,

    // RSP network (ready/valid, single VC): per-port.
    output logic [LINK_PORTS-1:0]     tx_rsp_valid,
    output logic [RSP_FLIT_WIDTH-1:0] tx_rsp_flit  [LINK_PORTS],
    input  logic [LINK_PORTS-1:0]     tx_rsp_ready,
    input  logic [LINK_PORTS-1:0]     rx_rsp_valid,
    input  logic [RSP_FLIT_WIDTH-1:0] rx_rsp_flit  [LINK_PORTS],
    output logic [LINK_PORTS-1:0]     rx_rsp_ready,

    // DAT network (credit, DAT_NUM_VC virtual channels): per-port.
    output logic [LINK_PORTS-1:0]     tx_dat_valid,
    output logic [DAT_FLIT_WIDTH-1:0] tx_dat_flit     [LINK_PORTS],
    input  logic [DAT_NUM_VC-1:0]     tx_dat_crdvalid [LINK_PORTS],
    input  logic [LINK_PORTS-1:0]     rx_dat_valid,
    input  logic [DAT_FLIT_WIDTH-1:0] rx_dat_flit     [LINK_PORTS],
    output logic [DAT_NUM_VC-1:0]     rx_dat_crdvalid [LINK_PORTS]
);

    // Elaboration guard: noc_types_pkg::noc_credit_t width must match DAT_NUM_VC.
    initial begin
        if ($bits(noc_types_pkg::noc_credit_t) != DAT_NUM_VC) begin
            $fatal(1, "%m: noc_credit_t width %0d != DAT_NUM_VC %0d; use matching noc_types_pkg_vc{N}.sv",
                   $bits(noc_types_pkg::noc_credit_t), DAT_NUM_VC);
        end
    end

    // -------------------------------------------------------------------------
    // DPI imports — 3-step pattern, one call per network per step (except
    // tick, shared); arg order mirrors cmodel_dpi.h Router decls. valid/ready
    // are packed [LINK_PORTS-1:0] vectors (one word, bit per port). flit
    // arrays are unpacked [LINK_PORTS], <NET>_VEC_WORDS-per-port (port-major).
    // DAT credit is an unpacked [LINK_PORTS] array of [DAT_NUM_VC-1:0] packed
    // words (one word per port, bit per VC).
    // -------------------------------------------------------------------------

    import "DPI-C" context function void cmodel_router_req_set_inputs(
        input  longint unsigned              ctx,
        input  bit [LINK_PORTS-1:0]     rx_req_valid,
        input  bit [REQ_FLIT_WIDTH-1:0] rx_req_flit  [LINK_PORTS],
        input  bit [LINK_PORTS-1:0]     tx_req_ready
    );
    import "DPI-C" context function void cmodel_router_rsp_set_inputs(
        input  longint unsigned              ctx,
        input  bit [LINK_PORTS-1:0]     rx_rsp_valid,
        input  bit [RSP_FLIT_WIDTH-1:0] rx_rsp_flit  [LINK_PORTS],
        input  bit [LINK_PORTS-1:0]     tx_rsp_ready
    );
    import "DPI-C" context function void cmodel_router_dat_set_inputs(
        input  longint unsigned              ctx,
        input  bit [LINK_PORTS-1:0]     rx_dat_valid,
        input  bit [DAT_FLIT_WIDTH-1:0] rx_dat_flit  [LINK_PORTS],
        input  bit [DAT_NUM_VC-1:0]     tx_dat_crdvalid [LINK_PORTS]
    );

    // tick: advance C++ model one cycle (all three networks together).
    import "DPI-C" context function void cmodel_router_tick(input longint unsigned ctx);

    // get_outputs: read C++ output latch into SV locals, one call per network.
    import "DPI-C" context function void cmodel_router_req_get_outputs(
        input  longint unsigned              ctx,
        output bit [LINK_PORTS-1:0]     tx_req_valid,
        output bit [REQ_FLIT_WIDTH-1:0] tx_req_flit  [LINK_PORTS],
        output bit [LINK_PORTS-1:0]     rx_req_ready
    );
    import "DPI-C" context function void cmodel_router_rsp_get_outputs(
        input  longint unsigned              ctx,
        output bit [LINK_PORTS-1:0]     tx_rsp_valid,
        output bit [RSP_FLIT_WIDTH-1:0] tx_rsp_flit  [LINK_PORTS],
        output bit [LINK_PORTS-1:0]     rx_rsp_ready
    );
    import "DPI-C" context function void cmodel_router_dat_get_outputs(
        input  longint unsigned              ctx,
        output bit [LINK_PORTS-1:0]     tx_dat_valid,
        output bit [DAT_FLIT_WIDTH-1:0] tx_dat_flit  [LINK_PORTS],
        output bit [DAT_NUM_VC-1:0]     rx_dat_crdvalid [LINK_PORTS]
    );

    // Lifecycle / error polling lives in tb_top.sv.

    // -------------------------------------------------------------------------
    // Output registers (registered one cycle behind DPI sample)
    // -------------------------------------------------------------------------

    bit [LINK_PORTS-1:0]     tx_req_valid_q;
    logic [REQ_FLIT_WIDTH-1:0] tx_req_flit_q [LINK_PORTS];
    bit [LINK_PORTS-1:0]     rx_req_ready_q;
    bit [LINK_PORTS-1:0]     tx_rsp_valid_q;
    logic [RSP_FLIT_WIDTH-1:0] tx_rsp_flit_q [LINK_PORTS];
    bit [LINK_PORTS-1:0]     rx_rsp_ready_q;
    bit [LINK_PORTS-1:0]     tx_dat_valid_q;
    logic [DAT_FLIT_WIDTH-1:0] tx_dat_flit_q [LINK_PORTS];
    logic [DAT_NUM_VC-1:0]     rx_dat_crdvalid_q [LINK_PORTS];

    // -------------------------------------------------------------------------
    // always_ff: sync-reset, 3-step DPI call, registered outputs
    // -------------------------------------------------------------------------

    always_ff @(posedge clk_i) begin
        if (!rst_ni) begin
            tx_req_valid_q <= '0;
            rx_req_ready_q <= '0;
            tx_rsp_valid_q <= '0;
            rx_rsp_ready_q <= '0;
            tx_dat_valid_q <= '0;
            // Unpacked-array regs cleared element-wise (Verilator rejects '0 here).
            for (int p = 0; p < LINK_PORTS; p++) begin
                tx_req_flit_q[p]     <= '0;
                tx_rsp_flit_q[p]     <= '0;
                tx_dat_flit_q[p]     <= '0;
                rx_dat_crdvalid_q[p] <= '0;
            end
        end else begin
            // Step 1: push current wire values into C++ input latch, one call
            // per network. The flit/credit ports are `logic` unpacked arrays;
            // the DPI imports declare `bit` unpacked arrays. Verilator
            // requires an exact element type match when passing whole
            // unpacked arrays, so copy the `logic` ports into `bit` mirrors
            // first (4-state -> 2-state, sim-clean here). Packed vectors
            // (valid/ready) pass directly.
            begin : set_inputs_blk
                bit [REQ_FLIT_WIDTH-1:0] b_rx_req_flit [LINK_PORTS];
                bit [RSP_FLIT_WIDTH-1:0] b_rx_rsp_flit [LINK_PORTS];
                bit [DAT_FLIT_WIDTH-1:0] b_rx_dat_flit [LINK_PORTS];
                bit [DAT_NUM_VC-1:0]     b_tx_dat_crdvalid [LINK_PORTS];
                for (int p = 0; p < LINK_PORTS; p++) begin
                    b_rx_req_flit[p]     = rx_req_flit[p];
                    b_rx_rsp_flit[p]     = rx_rsp_flit[p];
                    b_rx_dat_flit[p]     = rx_dat_flit[p];
                    b_tx_dat_crdvalid[p] = tx_dat_crdvalid[p];
                end
                cmodel_router_req_set_inputs(ctx_i, rx_req_valid, b_rx_req_flit, tx_req_ready);
                cmodel_router_rsp_set_inputs(ctx_i, rx_rsp_valid, b_rx_rsp_flit, tx_rsp_ready);
                cmodel_router_dat_set_inputs(ctx_i, rx_dat_valid, b_rx_dat_flit, b_tx_dat_crdvalid);
            end

            // Step 2: advance C++ model one cycle.
            cmodel_router_tick(ctx_i);

            // Step 3: pull outputs into local temporaries (blocking to locals is
            // safe; avoids BLKANDNBLK with the nonblocking reset path above).
            // One call per network.
            begin : get_outputs_blk
                bit [LINK_PORTS-1:0]     t_tx_req_valid;
                bit [REQ_FLIT_WIDTH-1:0] t_tx_req_flit [LINK_PORTS];
                bit [LINK_PORTS-1:0]     t_rx_req_ready;
                bit [LINK_PORTS-1:0]     t_tx_rsp_valid;
                bit [RSP_FLIT_WIDTH-1:0] t_tx_rsp_flit [LINK_PORTS];
                bit [LINK_PORTS-1:0]     t_rx_rsp_ready;
                bit [LINK_PORTS-1:0]     t_tx_dat_valid;
                bit [DAT_FLIT_WIDTH-1:0] t_tx_dat_flit [LINK_PORTS];
                bit [DAT_NUM_VC-1:0]     t_rx_dat_crdvalid [LINK_PORTS];
                cmodel_router_req_get_outputs(ctx_i, t_tx_req_valid, t_tx_req_flit, t_rx_req_ready);
                cmodel_router_rsp_get_outputs(ctx_i, t_tx_rsp_valid, t_tx_rsp_flit, t_rx_rsp_ready);
                cmodel_router_dat_get_outputs(ctx_i, t_tx_dat_valid, t_tx_dat_flit, t_rx_dat_crdvalid);
                tx_req_valid_q <= t_tx_req_valid;
                rx_req_ready_q <= t_rx_req_ready;
                tx_rsp_valid_q <= t_tx_rsp_valid;
                rx_rsp_ready_q <= t_rx_rsp_ready;
                tx_dat_valid_q <= t_tx_dat_valid;
                // Unpacked arrays: bit temp -> logic reg element-wise. 5.048
                // enforces IEEE 1800-2023 6.22.2 element-type equivalence on
                // whole unpacked-array assigns; per-port packed 2->4 state
                // stays legal.
                for (int p = 0; p < LINK_PORTS; p++) begin
                    tx_req_flit_q[p]     <= t_tx_req_flit[p];
                    tx_rsp_flit_q[p]     <= t_tx_rsp_flit[p];
                    tx_dat_flit_q[p]     <= t_tx_dat_flit[p];
                    rx_dat_crdvalid_q[p] <= t_rx_dat_crdvalid[p];
                end
            end
        end
    end

    // -------------------------------------------------------------------------
    // Drive outputs from registered state
    // -------------------------------------------------------------------------

    assign tx_req_valid    = tx_req_valid_q;
    assign tx_req_flit     = tx_req_flit_q;
    assign rx_req_ready    = rx_req_ready_q;

    assign tx_rsp_valid    = tx_rsp_valid_q;
    assign tx_rsp_flit     = tx_rsp_flit_q;
    assign rx_rsp_ready    = rx_rsp_ready_q;

    assign tx_dat_valid    = tx_dat_valid_q;
    assign tx_dat_flit     = tx_dat_flit_q;
    assign rx_dat_crdvalid = rx_dat_crdvalid_q;

endmodule

`endif  // ROUTER_WRAP_SV
