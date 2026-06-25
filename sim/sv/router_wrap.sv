// router_wrap — Task 4 DPI wrapper for one node's per-node Router component.
//
// One node owns its REQ+RSP routers at (x,0). Pins split into three faces:
//   noc_nmu_req_i / noc_nmu_req_cred_o — NMU-facing REQ channel (struct):
//              receives this node's NMU req flit; drives req credit back.
//   noc_nmu_rsp_o / noc_nmu_rsp_cred_i — NMU-facing RSP channel (struct):
//              drives rsp flit back to NMU; receives rsp credit from NMU.
//   noc_nsu_req_o / noc_nsu_req_cred_i — NSU-facing REQ channel (struct):
//              drives req flit to NSU; receives req credit back from NSU.
//   noc_nsu_rsp_i / noc_nsu_rsp_cred_o — NSU-facing RSP channel (struct):
//              receives rsp flit from NSU; drives rsp credit back to NSU.
//   link_*    — per-network (req/rsp) LINK to the peer node's router. Plain
//              signals (NO modport) so the SAME module serves node0 and node1;
//              tb_top cross-wires node0.link_*_out -> node1.link_*_in and
//              vice versa. LINK credit is a PULSE (one cycle per flit), unlike
//              the level/stub credit on the NI (NMU/NSU) faces.
//
// Beta-tick discipline (mirrors channel_model_wrap): on every posedge clk_i
// the module samples the PREVIOUS cycle's registered wire inputs, pushes them to the
// C++ model via DPI set_inputs, advances the model via tick, pulls outputs
// via get_outputs, and registers those outputs nonblocking so they are
// visible to SV wires from the NEXT cycle onward.
//
// FLIT_WIDTH must match ni_params_pkg::NOC_FLIT_WIDTH_DFLT = 408
// (c_model flit width). The noc_intf FLIT_WIDTH parameter is overridden at
// instantiation in tb_top.sv.
//
// The longint unsigned ctx_i is created by tb_top (cmodel_router_create with x_coord);
// this wrap only imports set_inputs/tick/get_outputs.
//
// Reset: synchronous active-low (rst_ni). Output registers cleared on reset.

`timescale 1ns/1ps

`ifndef ROUTER_WRAP_SV
`define ROUTER_WRAP_SV

module router_wrap #(
    parameter int unsigned NUM_VC                = ni_params_pkg::NOC_NUM_VC_DFLT,
    parameter int unsigned FLIT_WIDTH            = ni_params_pkg::NOC_FLIT_WIDTH_DFLT,
    parameter int unsigned SLAVE_VC_BUFFER_DEPTH = ni_params_pkg::NOC_SLAVE_VC_BUFFER_DEPTH_DFLT,
    // Router port count (LOCAL + N/E/S/W). Mirrors c_model ROUTER_PORT_COUNT /
    // ROUTER_LINK_PORTS; the DPI marshals the LINK face port-major over these.
    // Fixed at 5; not overridden (kept as a parameter so the port list can use it).
    parameter int unsigned LINK_PORTS            = 5
) (
    input  logic                  clk_i,
    input  logic                  rst_ni,
    input  longint unsigned                ctx_i,
    // NMU-facing struct ports: req IN from NMU + credit OUT; rsp OUT to NMU + credit IN.
    input  ni_signals_pkg::noc_chan_t  noc_nmu_req_i,
    output noc_types_pkg::noc_credit_t noc_nmu_req_cred_o,
    output ni_signals_pkg::noc_chan_t  noc_nmu_rsp_o,
    input  noc_types_pkg::noc_credit_t noc_nmu_rsp_cred_i,
    // NSU-facing struct ports: req OUT to NSU + credit IN; rsp IN from NSU + credit OUT.
    output ni_signals_pkg::noc_chan_t  noc_nsu_req_o,
    input  noc_types_pkg::noc_credit_t noc_nsu_req_cred_i,
    input  ni_signals_pkg::noc_chan_t  noc_nsu_rsp_i,
    output noc_types_pkg::noc_credit_t noc_nsu_rsp_cred_o,
    // REQ-network LINK to peer node(s): per-DIRECTION arrays (router has 5 ports;
    // LOCAL slot unused on the LINK face, N/E/S/W carry inter-router links). At
    // 2-node only one direction is live; Task 7 fills the rest. Credit is a
    // per-VC pulse vector.
    output logic [LINK_PORTS-1:0]                 link_req_out_valid,
    output logic [FLIT_WIDTH-1:0]                 link_req_out_flit   [LINK_PORTS],
    input  logic [NUM_VC-1:0]                     link_req_out_credit [LINK_PORTS],
    input  logic [LINK_PORTS-1:0]                 link_req_in_valid,
    input  logic [FLIT_WIDTH-1:0]                 link_req_in_flit    [LINK_PORTS],
    output logic [NUM_VC-1:0]                     link_req_in_credit  [LINK_PORTS],
    // RSP-network LINK to peer node(s): per-DIRECTION arrays (mirror of REQ).
    output logic [LINK_PORTS-1:0]                 link_rsp_out_valid,
    output logic [FLIT_WIDTH-1:0]                 link_rsp_out_flit   [LINK_PORTS],
    input  logic [NUM_VC-1:0]                     link_rsp_out_credit [LINK_PORTS],
    input  logic [LINK_PORTS-1:0]                 link_rsp_in_valid,
    input  logic [FLIT_WIDTH-1:0]                 link_rsp_in_flit    [LINK_PORTS],
    output logic [NUM_VC-1:0]                     link_rsp_in_credit  [LINK_PORTS]
);

    // Elaboration guard: noc_types_pkg::noc_credit_t width must match NUM_VC.
    initial begin
        if ($bits(noc_types_pkg::noc_credit_t) != NUM_VC) begin
            $fatal(1, "%m: noc_credit_t width %0d != NUM_VC %0d; use matching noc_types_pkg_vc{N}.sv",
                   $bits(noc_types_pkg::noc_credit_t), NUM_VC);
        end
    end

    // -------------------------------------------------------------------------
    // Multi-VC: NI credit_return + per-direction LINK credit are marshalled
    // per-VC across DPI as [NUM_VC-1:0] vectors (bit vc = credit pulse on VC vc).
    // -------------------------------------------------------------------------

    // -------------------------------------------------------------------------
    // DPI imports — 3-step pattern; arg order mirrors cmodel_dpi.h Router decls.
    // LINK face is port-indexed: valid = bit-per-port packed vector; flit =
    // unpacked [LINK_PORTS] array (port-major words); credit = one [NUM_VC-1:0]
    // word per port (unpacked [LINK_PORTS]).
    // -------------------------------------------------------------------------

    // set_inputs: sample SV wire state into C++ input latch.
    import "DPI-C" context function void cmodel_router_set_inputs(
        input  longint unsigned              ctx,
        input  bit                  req_in_valid,
        input  bit [FLIT_WIDTH-1:0] req_in_flit,
        input  bit [NUM_VC-1:0]     req_in_credit_return,
        input  bit                  rsp_in_valid,
        input  bit [FLIT_WIDTH-1:0] rsp_in_flit,
        input  bit [NUM_VC-1:0]     rsp_in_credit_return,
        input  bit [NUM_VC-1:0]     link_req_out_credit [LINK_PORTS],
        input  bit [LINK_PORTS-1:0] link_req_in_valid,
        input  bit [FLIT_WIDTH-1:0] link_req_in_flit    [LINK_PORTS],
        input  bit [NUM_VC-1:0]     link_rsp_out_credit [LINK_PORTS],
        input  bit [LINK_PORTS-1:0] link_rsp_in_valid,
        input  bit [FLIT_WIDTH-1:0] link_rsp_in_flit    [LINK_PORTS]
    );

    // tick: advance C++ model one cycle.
    import "DPI-C" context function void cmodel_router_tick(input longint unsigned ctx);

    // get_outputs: read C++ output latch into SV locals.
    import "DPI-C" context function void cmodel_router_get_outputs(
        input  longint unsigned              ctx,
        output bit                  req_out_valid,
        output bit [FLIT_WIDTH-1:0] req_out_flit,
        output bit [NUM_VC-1:0]     req_out_credit_return,
        output bit                  rsp_out_valid,
        output bit [FLIT_WIDTH-1:0] rsp_out_flit,
        output bit [NUM_VC-1:0]     rsp_out_credit_return,
        output bit [LINK_PORTS-1:0] link_req_out_valid,
        output bit [FLIT_WIDTH-1:0] link_req_out_flit   [LINK_PORTS],
        output bit [NUM_VC-1:0]     link_req_in_credit  [LINK_PORTS],
        output bit [LINK_PORTS-1:0] link_rsp_out_valid,
        output bit [FLIT_WIDTH-1:0] link_rsp_out_flit   [LINK_PORTS],
        output bit [NUM_VC-1:0]     link_rsp_in_credit  [LINK_PORTS]
    );

    // Lifecycle / error polling lives in tb_top.sv.

    // -------------------------------------------------------------------------
    // Output registers (beta-tick: registered one cycle behind DPI sample)
    // -------------------------------------------------------------------------

    bit                            req_out_valid_q;
    bit [FLIT_WIDTH-1:0]           req_out_flit_q;
    bit [NUM_VC-1:0]              req_out_credit_return_q;
    bit                            rsp_out_valid_q;
    bit [FLIT_WIDTH-1:0]           rsp_out_flit_q;
    bit [NUM_VC-1:0]              rsp_out_credit_return_q;
    bit [LINK_PORTS-1:0]          link_req_out_valid_q;
    bit [FLIT_WIDTH-1:0]           link_req_out_flit_q  [LINK_PORTS];
    bit [NUM_VC-1:0]              link_req_in_credit_q [LINK_PORTS];
    bit [LINK_PORTS-1:0]          link_rsp_out_valid_q;
    bit [FLIT_WIDTH-1:0]           link_rsp_out_flit_q  [LINK_PORTS];
    bit [NUM_VC-1:0]              link_rsp_in_credit_q [LINK_PORTS];

    // -------------------------------------------------------------------------
    // always_ff: sync-reset, 3-step DPI call, registered outputs
    // -------------------------------------------------------------------------

    always_ff @(posedge clk_i) begin
        if (!rst_ni) begin
            req_out_valid_q          <= '0;
            req_out_flit_q           <= '0;
            req_out_credit_return_q  <= '0;
            rsp_out_valid_q          <= '0;
            rsp_out_flit_q           <= '0;
            rsp_out_credit_return_q  <= '0;
            link_req_out_valid_q     <= '0;
            link_rsp_out_valid_q     <= '0;
            // Unpacked-array regs cleared element-wise (Verilator rejects '0 here).
            for (int p = 0; p < LINK_PORTS; p++) begin
                link_req_out_flit_q[p]  <= '0;
                link_req_in_credit_q[p] <= '0;
                link_rsp_out_flit_q[p]  <= '0;
                link_rsp_in_credit_q[p] <= '0;
            end
        end else begin
            // Step 1: push current wire values into C++ input latch.
            // The LINK-face ports are `logic` unpacked arrays; the DPI imports
            // declare `bit` unpacked arrays. Verilator requires an exact element
            // type match when passing whole unpacked arrays, so copy the `logic`
            // ports into `bit` mirrors first (4-state -> 2-state, sim-clean here).
            begin : set_inputs_blk
                bit [NUM_VC-1:0]     b_link_req_out_credit [LINK_PORTS];
                bit [LINK_PORTS-1:0] b_link_req_in_valid;
                bit [FLIT_WIDTH-1:0] b_link_req_in_flit    [LINK_PORTS];
                bit [NUM_VC-1:0]     b_link_rsp_out_credit [LINK_PORTS];
                bit [LINK_PORTS-1:0] b_link_rsp_in_valid;
                bit [FLIT_WIDTH-1:0] b_link_rsp_in_flit    [LINK_PORTS];
                b_link_req_in_valid = link_req_in_valid;
                b_link_rsp_in_valid = link_rsp_in_valid;
                for (int p = 0; p < LINK_PORTS; p++) begin
                    b_link_req_out_credit[p] = link_req_out_credit[p];
                    b_link_req_in_flit[p]    = link_req_in_flit[p];
                    b_link_rsp_out_credit[p] = link_rsp_out_credit[p];
                    b_link_rsp_in_flit[p]    = link_rsp_in_flit[p];
                end
                cmodel_router_set_inputs(
                    ctx_i,
                    noc_nmu_req_i.valid,
                    noc_nmu_req_i.flit,
                    noc_nsu_req_cred_i.credit[NUM_VC-1:0],
                    noc_nsu_rsp_i.valid,
                    noc_nsu_rsp_i.flit,
                    noc_nmu_rsp_cred_i.credit[NUM_VC-1:0],
                    b_link_req_out_credit,
                    b_link_req_in_valid,
                    b_link_req_in_flit,
                    b_link_rsp_out_credit,
                    b_link_rsp_in_valid,
                    b_link_rsp_in_flit
                );
            end

            // Step 2: advance C++ model one cycle.
            cmodel_router_tick(ctx_i);

            // Step 3: pull outputs into local temporaries (blocking to locals is
            // safe; avoids BLKANDNBLK with the nonblocking reset path above).
            begin : get_outputs_blk
                bit                            t_req_out_valid;
                bit [FLIT_WIDTH-1:0]           t_req_out_flit;
                bit [NUM_VC-1:0]              t_req_out_credit_return;
                bit                            t_rsp_out_valid;
                bit [FLIT_WIDTH-1:0]           t_rsp_out_flit;
                bit [NUM_VC-1:0]              t_rsp_out_credit_return;
                bit [LINK_PORTS-1:0]          t_link_req_out_valid;
                bit [FLIT_WIDTH-1:0]           t_link_req_out_flit  [LINK_PORTS];
                bit [NUM_VC-1:0]              t_link_req_in_credit [LINK_PORTS];
                bit [LINK_PORTS-1:0]          t_link_rsp_out_valid;
                bit [FLIT_WIDTH-1:0]           t_link_rsp_out_flit  [LINK_PORTS];
                bit [NUM_VC-1:0]              t_link_rsp_in_credit [LINK_PORTS];
                cmodel_router_get_outputs(
                    ctx_i,
                    t_req_out_valid,
                    t_req_out_flit,
                    t_req_out_credit_return,
                    t_rsp_out_valid,
                    t_rsp_out_flit,
                    t_rsp_out_credit_return,
                    t_link_req_out_valid,
                    t_link_req_out_flit,
                    t_link_req_in_credit,
                    t_link_rsp_out_valid,
                    t_link_rsp_out_flit,
                    t_link_rsp_in_credit
                );
                req_out_valid_q         <= t_req_out_valid;
                req_out_flit_q          <= t_req_out_flit;
                req_out_credit_return_q <= t_req_out_credit_return;
                rsp_out_valid_q         <= t_rsp_out_valid;
                rsp_out_flit_q          <= t_rsp_out_flit;
                rsp_out_credit_return_q <= t_rsp_out_credit_return;
                link_req_out_valid_q    <= t_link_req_out_valid;
                link_req_out_flit_q     <= t_link_req_out_flit;
                link_req_in_credit_q    <= t_link_req_in_credit;
                link_rsp_out_valid_q    <= t_link_rsp_out_valid;
                link_rsp_out_flit_q     <= t_link_rsp_out_flit;
                link_rsp_in_credit_q    <= t_link_rsp_in_credit;
            end
        end
    end

    // -------------------------------------------------------------------------
    // Drive interface + link outputs from registered state
    // -------------------------------------------------------------------------

    // NSU-facing side: drive req flit toward NSU.
    assign noc_nsu_req_o.valid          = req_out_valid_q;
    assign noc_nsu_req_o.flit           = req_out_flit_q;
    // NMU-facing side: return req credit pulse vector back to NMU (registered
    // per-VC FlooNoC pulse from the C model; pure pass-through).
    assign noc_nmu_req_cred_o.credit    = req_out_credit_return_q;

    // NMU-facing side: drive rsp flit back toward NMU.
    assign noc_nmu_rsp_o.valid          = rsp_out_valid_q;
    assign noc_nmu_rsp_o.flit           = rsp_out_flit_q;
    // NSU-facing side: return rsp credit pulse vector back to NSU (registered
    // per-VC FlooNoC pulse from the C model; pure pass-through).
    assign noc_nsu_rsp_cred_o.credit    = rsp_out_credit_return_q;

    // REQ-network LINK: per-direction flit forward + credit pulse upstream.
    assign link_req_out_valid           = link_req_out_valid_q;
    assign link_req_out_flit            = link_req_out_flit_q;
    assign link_req_in_credit           = link_req_in_credit_q;

    // RSP-network LINK: per-direction flit forward + credit pulse upstream.
    assign link_rsp_out_valid           = link_rsp_out_valid_q;
    assign link_rsp_out_flit            = link_rsp_out_flit_q;
    assign link_rsp_in_credit           = link_rsp_in_credit_q;

endmodule

`endif  // ROUTER_WRAP_SV
