// router_wrap — Task 4 DPI shell for one node's per-node Router component.
//
// One node owns its REQ+RSP routers at (x,0). Pins split into three faces:
//   noc_nmu_i — NMU-facing bundle (noc_intf.miso modport): receives this
//              node's NMU req_valid/req_flit + rsp_credit_return; drives
//              req_credit_return + rsp_valid/rsp_flit back to the NMU.
//   noc_nsu_o — NSU-facing bundle (noc_intf.mosi modport): drives the NSU
//              req_valid/req_flit + rsp_credit_return; receives
//              req_credit_return + rsp_valid/rsp_flit from the NSU.
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
// FLIT_WIDTH must match ni_params_pkg::NI_NOC_FLIT_WIDTH_DFLT = 408
// (c_model flit width). The noc_intf FLIT_WIDTH parameter is overridden at
// instantiation in tb_top.sv.
//
// The chandle ctx_i is created by tb_top (cmodel_router_create with x_coord);
// this wrap only imports set_inputs/tick/get_outputs.
//
// Reset: synchronous active-low (rst_ni). Output registers cleared on reset.

`timescale 1ns/1ps

`ifndef ROUTER_WRAP_SV
`define ROUTER_WRAP_SV

module router_wrap #(
    parameter int unsigned NUM_VC                = ni_params_pkg::NI_NOC_NUM_VC_DFLT,
    parameter int unsigned FLIT_WIDTH            = ni_params_pkg::NI_NOC_FLIT_WIDTH_DFLT,
    parameter int unsigned SLAVE_VC_BUFFER_DEPTH = ni_params_pkg::NI_NOC_SLAVE_VC_BUFFER_DEPTH_DFLT
) (
    input  logic                  clk_i,
    input  logic                  rst_ni,
    input  chandle                ctx_i,
    // NMU-facing bundle: receives req_*, drives rsp_* and req_credit_return.
    noc_intf.miso                 noc_nmu_i,
    // NSU-facing bundle: drives req_*, receives rsp_* and req_credit_return.
    noc_intf.mosi                 noc_nsu_o,
    // REQ-network LINK to peer node (plain signals; pulse credit).
    output logic                  link_req_out_valid,
    output logic [FLIT_WIDTH-1:0] link_req_out_flit,
    input  logic                  link_req_out_credit,
    input  logic                  link_req_in_valid,
    input  logic [FLIT_WIDTH-1:0] link_req_in_flit,
    output logic                  link_req_in_credit,
    // RSP-network LINK to peer node (plain signals; pulse credit).
    output logic                  link_rsp_out_valid,
    output logic [FLIT_WIDTH-1:0] link_rsp_out_flit,
    input  logic                  link_rsp_out_credit,
    input  logic                  link_rsp_in_valid,
    input  logic [FLIT_WIDTH-1:0] link_rsp_in_flit,
    output logic                  link_rsp_in_credit
);

    // -------------------------------------------------------------------------
    // PoC scope guard: single-VC only
    // -------------------------------------------------------------------------
    // c_model + DPI marshalling assume single-VC. Multi-VC support requires
    // plumbing per-VC credit_return through DPI; until then, fail elaboration
    // if NUM_VC > 1 instead of silently broadcasting a single-bit credit.
    initial begin
        if (NUM_VC != 1) begin
            $fatal(1, "%m: NUM_VC=%0d; PoC supports NUM_VC=1 only", NUM_VC);
        end
    end

    // -------------------------------------------------------------------------
    // DPI imports — 3-step pattern; arg order mirrors cmodel_dpi.h Router decls.
    // -------------------------------------------------------------------------

    // set_inputs: sample SV wire state into C++ input latch.
    import "DPI-C" context function void cmodel_router_set_inputs(
        input  chandle              ctx,
        input  bit                  req_in_valid,
        input  bit [FLIT_WIDTH-1:0] req_in_flit,
        input  bit                  req_in_credit_return,
        input  bit                  rsp_in_valid,
        input  bit [FLIT_WIDTH-1:0] rsp_in_flit,
        input  bit                  rsp_in_credit_return,
        input  bit                  link_req_out_credit,
        input  bit                  link_req_in_valid,
        input  bit [FLIT_WIDTH-1:0] link_req_in_flit,
        input  bit                  link_rsp_out_credit,
        input  bit                  link_rsp_in_valid,
        input  bit [FLIT_WIDTH-1:0] link_rsp_in_flit
    );

    // tick: advance C++ model one cycle.
    import "DPI-C" context function void cmodel_router_tick(input chandle ctx);

    // get_outputs: read C++ output latch into SV locals.
    import "DPI-C" context function void cmodel_router_get_outputs(
        input  chandle              ctx,
        output bit                  req_out_valid,
        output bit [FLIT_WIDTH-1:0] req_out_flit,
        output bit                  req_out_credit_return,
        output bit                  rsp_out_valid,
        output bit [FLIT_WIDTH-1:0] rsp_out_flit,
        output bit                  rsp_out_credit_return,
        output bit                  link_req_out_valid,
        output bit [FLIT_WIDTH-1:0] link_req_out_flit,
        output bit                  link_req_in_credit,
        output bit                  link_rsp_out_valid,
        output bit [FLIT_WIDTH-1:0] link_rsp_out_flit,
        output bit                  link_rsp_in_credit
    );

    // Lifecycle / error polling lives in tb_top.sv.

    // -------------------------------------------------------------------------
    // Output registers (beta-tick: registered one cycle behind DPI sample)
    // -------------------------------------------------------------------------

    bit                    req_out_valid_q;
    bit [FLIT_WIDTH-1:0]   req_out_flit_q;
    bit                    req_out_credit_return_q;
    bit                    rsp_out_valid_q;
    bit [FLIT_WIDTH-1:0]   rsp_out_flit_q;
    bit                    rsp_out_credit_return_q;
    bit                    link_req_out_valid_q;
    bit [FLIT_WIDTH-1:0]   link_req_out_flit_q;
    bit                    link_req_in_credit_q;
    bit                    link_rsp_out_valid_q;
    bit [FLIT_WIDTH-1:0]   link_rsp_out_flit_q;
    bit                    link_rsp_in_credit_q;

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
            link_req_out_flit_q      <= '0;
            link_req_in_credit_q     <= '0;
            link_rsp_out_valid_q     <= '0;
            link_rsp_out_flit_q      <= '0;
            link_rsp_in_credit_q     <= '0;
        end else begin
            // Step 1: push current wire values into C++ input latch.
            cmodel_router_set_inputs(
                ctx_i,
                noc_nmu_i.req_valid,
                noc_nmu_i.req_flit,
                noc_nsu_o.req_credit_return[0],
                noc_nsu_o.rsp_valid,
                noc_nsu_o.rsp_flit,
                noc_nmu_i.rsp_credit_return[0],
                link_req_out_credit,
                link_req_in_valid,
                link_req_in_flit,
                link_rsp_out_credit,
                link_rsp_in_valid,
                link_rsp_in_flit
            );

            // Step 2: advance C++ model one cycle.
            cmodel_router_tick(ctx_i);

            // Step 3: pull outputs into local temporaries (blocking to locals is
            // safe; avoids BLKANDNBLK with the nonblocking reset path above).
            begin : get_outputs_blk
                bit                    t_req_out_valid;
                bit [FLIT_WIDTH-1:0]   t_req_out_flit;
                bit                    t_req_out_credit_return;
                bit                    t_rsp_out_valid;
                bit [FLIT_WIDTH-1:0]   t_rsp_out_flit;
                bit                    t_rsp_out_credit_return;
                bit                    t_link_req_out_valid;
                bit [FLIT_WIDTH-1:0]   t_link_req_out_flit;
                bit                    t_link_req_in_credit;
                bit                    t_link_rsp_out_valid;
                bit [FLIT_WIDTH-1:0]   t_link_rsp_out_flit;
                bit                    t_link_rsp_in_credit;
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

    // NSU-facing side: drive req_valid/req_flit forward.
    assign noc_nsu_o.req_valid          = req_out_valid_q;
    assign noc_nsu_o.req_flit           = req_out_flit_q;
    // NMU-facing side: return req credits back upstream (level/stub).
    assign noc_nmu_i.req_credit_return  = {NUM_VC{req_out_credit_return_q}};

    // NMU-facing side: drive rsp_valid/rsp_flit back toward NMU.
    assign noc_nmu_i.rsp_valid          = rsp_out_valid_q;
    assign noc_nmu_i.rsp_flit           = rsp_out_flit_q;
    // NSU-facing side: return rsp credits back upstream (level/stub).
    assign noc_nsu_o.rsp_credit_return  = {NUM_VC{rsp_out_credit_return_q}};

    // REQ-network LINK: drive flit forward, return credit pulse upstream.
    assign link_req_out_valid           = link_req_out_valid_q;
    assign link_req_out_flit            = link_req_out_flit_q;
    assign link_req_in_credit           = link_req_in_credit_q;

    // RSP-network LINK: drive flit forward, return credit pulse upstream.
    assign link_rsp_out_valid           = link_rsp_out_valid_q;
    assign link_rsp_out_flit            = link_rsp_out_flit_q;
    assign link_rsp_in_credit           = link_rsp_in_credit_q;

endmodule

`endif  // ROUTER_WRAP_SV
