// router_channel_wrap — DPI shell for the production RouterChannel component.
//
// 2-node bidirectional NoC fabric. Per node it exposes two noc_intf bundles,
// identical in shape to channel_model_wrap's single pair:
//   nodeK_nmu_i — node K NMU-facing side (noc_intf.miso modport):
//              receives NMU req_valid/req_flit + rsp_credit_return;
//              drives req_credit_return + rsp_valid/rsp_flit back to NMU.
//   nodeK_nsu_o — node K NSU-facing side (noc_intf.mosi modport):
//              drives NSU req_valid/req_flit + rsp_credit_return;
//              receives req_credit_return + rsp_valid/rsp_flit from NSU.
//
// Beta-tick discipline (spec §5.1): on every posedge clk_i the module samples
// the PREVIOUS cycle's registered wire inputs for BOTH nodes, pushes them to
// the C++ model via DPI set_inputs (12 args, _n0/_n1 per node), advances the
// model via tick, pulls outputs via get_outputs, and registers those outputs
// nonblocking so they are visible to SV wires from the NEXT cycle onward.
//
// FLIT_WIDTH must match ni_params_pkg::NI_NOC_FLIT_WIDTH_DFLT = 408
// (c_model flit width). The noc_intf FLIT_WIDTH parameter is overridden
// at instantiation in tb_top.sv.
//
// Error polling is centralized in tb_top.sv (T1.4); this wrap no longer
// calls cmodel_check_error/cmodel_finalize itself.
//
// Reset: synchronous active-low (rst_ni). Output registers cleared on reset.
// No async reset path — sync reset is the project default per rtl-style.

`timescale 1ns/1ps

`ifndef ROUTER_CHANNEL_WRAP_SV
`define ROUTER_CHANNEL_WRAP_SV

module router_channel_wrap #(
    parameter int unsigned NUM_VC                = ni_params_pkg::NI_NOC_NUM_VC_DFLT,
    parameter int unsigned FLIT_WIDTH            = ni_params_pkg::NI_NOC_FLIT_WIDTH_DFLT,
    parameter int unsigned SLAVE_VC_BUFFER_DEPTH = ni_params_pkg::NI_NOC_SLAVE_VC_BUFFER_DEPTH_DFLT
) (
    input  logic   clk_i,
    input  logic   rst_ni,
    input  chandle ctx_i,
    // node0 NMU-facing (NMU drives req, wrap drives rsp).
    noc_intf.miso  node0_nmu_i,
    // node0 NSU-facing (wrap drives req, NSU drives rsp).
    noc_intf.mosi  node0_nsu_o,
    // node1 NMU-facing.
    noc_intf.miso  node1_nmu_i,
    // node1 NSU-facing.
    noc_intf.mosi  node1_nsu_o
);

    // -------------------------------------------------------------------------
    // PoC scope guard (T1.3): single-VC only
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
    // DPI imports — 3-step pattern per spec §5.1, 2-node (_n0/_n1) signatures
    // -------------------------------------------------------------------------

    // set_inputs: sample SV wire state into C++ input latch (both nodes).
    // svBitVecVal carries FLIT_WIDTH bits as ceil(FLIT_WIDTH/32) 32-bit words.
    import "DPI-C" context function void cmodel_router_channel_set_inputs(
        input  chandle              ctx,
        input  bit                  req_in_valid_n0,
        input  bit [FLIT_WIDTH-1:0] req_in_flit_n0,
        input  bit                  req_in_credit_return_n0,
        input  bit                  rsp_in_valid_n0,
        input  bit [FLIT_WIDTH-1:0] rsp_in_flit_n0,
        input  bit                  rsp_in_credit_return_n0,
        input  bit                  req_in_valid_n1,
        input  bit [FLIT_WIDTH-1:0] req_in_flit_n1,
        input  bit                  req_in_credit_return_n1,
        input  bit                  rsp_in_valid_n1,
        input  bit [FLIT_WIDTH-1:0] rsp_in_flit_n1,
        input  bit                  rsp_in_credit_return_n1
    );

    // tick: advance C++ model one cycle.
    import "DPI-C" context function void cmodel_router_channel_tick(input chandle ctx);

    // get_outputs: read C++ output latch into SV locals (both nodes).
    import "DPI-C" context function void cmodel_router_channel_get_outputs(
        input  chandle              ctx,
        output bit                  req_out_valid_n0,
        output bit [FLIT_WIDTH-1:0] req_out_flit_n0,
        output bit                  req_out_credit_return_n0,
        output bit                  rsp_out_valid_n0,
        output bit [FLIT_WIDTH-1:0] rsp_out_flit_n0,
        output bit                  rsp_out_credit_return_n0,
        output bit                  req_out_valid_n1,
        output bit [FLIT_WIDTH-1:0] req_out_flit_n1,
        output bit                  req_out_credit_return_n1,
        output bit                  rsp_out_valid_n1,
        output bit [FLIT_WIDTH-1:0] rsp_out_flit_n1,
        output bit                  rsp_out_credit_return_n1
    );

    // Lifecycle / error polling lives in tb_top.sv (T1.4).

    // -------------------------------------------------------------------------
    // Output registers (beta-tick: registered one cycle behind DPI sample)
    // -------------------------------------------------------------------------

    bit                    req_out_valid_n0_q;
    bit [FLIT_WIDTH-1:0]   req_out_flit_n0_q;
    bit                    req_out_credit_return_n0_q;
    bit                    rsp_out_valid_n0_q;
    bit [FLIT_WIDTH-1:0]   rsp_out_flit_n0_q;
    bit                    rsp_out_credit_return_n0_q;

    bit                    req_out_valid_n1_q;
    bit [FLIT_WIDTH-1:0]   req_out_flit_n1_q;
    bit                    req_out_credit_return_n1_q;
    bit                    rsp_out_valid_n1_q;
    bit [FLIT_WIDTH-1:0]   rsp_out_flit_n1_q;
    bit                    rsp_out_credit_return_n1_q;

    // -------------------------------------------------------------------------
    // always_ff: sync-reset, 3-step DPI call, registered outputs
    // -------------------------------------------------------------------------

    always_ff @(posedge clk_i) begin
        if (!rst_ni) begin
            req_out_valid_n0_q         <= '0;
            req_out_flit_n0_q          <= '0;
            req_out_credit_return_n0_q <= '0;
            rsp_out_valid_n0_q         <= '0;
            rsp_out_flit_n0_q          <= '0;
            rsp_out_credit_return_n0_q <= '0;
            req_out_valid_n1_q         <= '0;
            req_out_flit_n1_q          <= '0;
            req_out_credit_return_n1_q <= '0;
            rsp_out_valid_n1_q         <= '0;
            rsp_out_flit_n1_q          <= '0;
            rsp_out_credit_return_n1_q <= '0;
        end else begin
            // Step 1: push current wire values for both nodes into C++ latch.
            cmodel_router_channel_set_inputs(
                ctx_i,
                node0_nmu_i.req_valid,
                node0_nmu_i.req_flit,
                node0_nsu_o.req_credit_return[0],
                node0_nsu_o.rsp_valid,
                node0_nsu_o.rsp_flit,
                node0_nmu_i.rsp_credit_return[0],
                node1_nmu_i.req_valid,
                node1_nmu_i.req_flit,
                node1_nsu_o.req_credit_return[0],
                node1_nsu_o.rsp_valid,
                node1_nsu_o.rsp_flit,
                node1_nmu_i.rsp_credit_return[0]
            );

            // Step 2: advance C++ model one cycle.
            cmodel_router_channel_tick(ctx_i);

            // Step 3: pull outputs into local temporaries (blocking to locals is
            // safe; avoids BLKANDNBLK with the nonblocking reset path above).
            begin : get_outputs_blk
                bit                    t_req_out_valid_n0;
                bit [FLIT_WIDTH-1:0]   t_req_out_flit_n0;
                bit                    t_req_out_credit_return_n0;
                bit                    t_rsp_out_valid_n0;
                bit [FLIT_WIDTH-1:0]   t_rsp_out_flit_n0;
                bit                    t_rsp_out_credit_return_n0;
                bit                    t_req_out_valid_n1;
                bit [FLIT_WIDTH-1:0]   t_req_out_flit_n1;
                bit                    t_req_out_credit_return_n1;
                bit                    t_rsp_out_valid_n1;
                bit [FLIT_WIDTH-1:0]   t_rsp_out_flit_n1;
                bit                    t_rsp_out_credit_return_n1;
                cmodel_router_channel_get_outputs(
                    ctx_i,
                    t_req_out_valid_n0,
                    t_req_out_flit_n0,
                    t_req_out_credit_return_n0,
                    t_rsp_out_valid_n0,
                    t_rsp_out_flit_n0,
                    t_rsp_out_credit_return_n0,
                    t_req_out_valid_n1,
                    t_req_out_flit_n1,
                    t_req_out_credit_return_n1,
                    t_rsp_out_valid_n1,
                    t_rsp_out_flit_n1,
                    t_rsp_out_credit_return_n1
                );
                req_out_valid_n0_q         <= t_req_out_valid_n0;
                req_out_flit_n0_q          <= t_req_out_flit_n0;
                req_out_credit_return_n0_q <= t_req_out_credit_return_n0;
                rsp_out_valid_n0_q         <= t_rsp_out_valid_n0;
                rsp_out_flit_n0_q          <= t_rsp_out_flit_n0;
                rsp_out_credit_return_n0_q <= t_rsp_out_credit_return_n0;
                req_out_valid_n1_q         <= t_req_out_valid_n1;
                req_out_flit_n1_q          <= t_req_out_flit_n1;
                req_out_credit_return_n1_q <= t_req_out_credit_return_n1;
                rsp_out_valid_n1_q         <= t_rsp_out_valid_n1;
                rsp_out_flit_n1_q          <= t_rsp_out_flit_n1;
                rsp_out_credit_return_n1_q <= t_rsp_out_credit_return_n1;
            end
        end
    end

    // -------------------------------------------------------------------------
    // Drive interface outputs from registered state
    // -------------------------------------------------------------------------

    // node0: NSU-facing drives req forward; NMU-facing returns req credits and
    // drives rsp back toward NMU; NSU-facing returns rsp credits.
    assign node0_nsu_o.req_valid         = req_out_valid_n0_q;
    assign node0_nsu_o.req_flit          = req_out_flit_n0_q;
    assign node0_nmu_i.req_credit_return = {NUM_VC{req_out_credit_return_n0_q}};
    assign node0_nmu_i.rsp_valid         = rsp_out_valid_n0_q;
    assign node0_nmu_i.rsp_flit          = rsp_out_flit_n0_q;
    assign node0_nsu_o.rsp_credit_return = {NUM_VC{rsp_out_credit_return_n0_q}};

    // node1: same mapping.
    assign node1_nsu_o.req_valid         = req_out_valid_n1_q;
    assign node1_nsu_o.req_flit          = req_out_flit_n1_q;
    assign node1_nmu_i.req_credit_return = {NUM_VC{req_out_credit_return_n1_q}};
    assign node1_nmu_i.rsp_valid         = rsp_out_valid_n1_q;
    assign node1_nmu_i.rsp_flit          = rsp_out_flit_n1_q;
    assign node1_nsu_o.rsp_credit_return = {NUM_VC{rsp_out_credit_return_n1_q}};

endmodule

`endif  // ROUTER_CHANNEL_WRAP_SV
