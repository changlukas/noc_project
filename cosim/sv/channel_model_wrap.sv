// channel_model_wrap — Stage 5b DPI shell for ChannelModel component.
//
// Two NoC ports, both noc_intf bundles (req + rsp channels combined):
//   noc_miso — NMU-facing side (noc_intf.miso modport):
//              receives NMU req_valid/req_flit + rsp_credit_return;
//              drives req_credit_return + rsp_valid/rsp_flit back to NMU.
//   noc_mosi — NSU-facing side (noc_intf.mosi modport):
//              drives NSU req_valid/req_flit + rsp_credit_return;
//              receives req_credit_return + rsp_valid/rsp_flit from NSU.
//
// Beta-tick discipline (spec §5.1): on every posedge clk_i the module
// samples the PREVIOUS cycle's registered wire inputs, pushes them to
// the C++ model via DPI set_inputs, advances the model via tick, pulls
// outputs via get_outputs, and registers those outputs nonblocking so
// they are visible to SV wires from the NEXT cycle onward.
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

`ifndef CHANNEL_MODEL_WRAP_SV
`define CHANNEL_MODEL_WRAP_SV

module channel_model_wrap #(
    parameter int unsigned NUM_VC                = ni_params_pkg::NI_NOC_NUM_VC_DFLT,
    parameter int unsigned FLIT_WIDTH            = ni_params_pkg::NI_NOC_FLIT_WIDTH_DFLT,
    parameter int unsigned SLAVE_VC_BUFFER_DEPTH = ni_params_pkg::NI_NOC_SLAVE_VC_BUFFER_DEPTH_DFLT
) (
    input  logic              clk_i,
    input  logic              rst_ni,
    // NMU-facing bundle: receives req_*, drives rsp_* and req_credit_return.
    noc_intf.miso             noc_miso,
    // NSU-facing bundle: drives req_*, receives rsp_* and req_credit_return.
    noc_intf.mosi             noc_mosi
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
    // DPI imports — 3-step pattern per spec §5.1
    // -------------------------------------------------------------------------

    // set_inputs: sample SV wire state into C++ input latch.
    // svBitVecVal carries FLIT_WIDTH bits as ceil(FLIT_WIDTH/32) 32-bit words.
    import "DPI-C" context function void cmodel_channel_model_set_inputs(
        input  bit                  req_in_valid,
        input  bit [FLIT_WIDTH-1:0] req_in_flit,
        input  bit                  req_in_credit_return,
        input  bit                  rsp_in_valid,
        input  bit [FLIT_WIDTH-1:0] rsp_in_flit,
        input  bit                  rsp_in_credit_return
    );

    // tick: advance C++ model one cycle.
    import "DPI-C" context function void cmodel_channel_model_tick();

    // get_outputs: read C++ output latch into SV locals.
    import "DPI-C" context function void cmodel_channel_model_get_outputs(
        output bit                  req_out_valid,
        output bit [FLIT_WIDTH-1:0] req_out_flit,
        output bit                  req_out_credit_return,
        output bit                  rsp_out_valid,
        output bit [FLIT_WIDTH-1:0] rsp_out_flit,
        output bit                  rsp_out_credit_return
    );

    // Lifecycle / error polling lives in tb_top.sv (T1.4).

    // -------------------------------------------------------------------------
    // Output registers (beta-tick: registered one cycle behind DPI sample)
    // -------------------------------------------------------------------------

    bit                    req_out_valid_q;
    bit [FLIT_WIDTH-1:0]   req_out_flit_q;
    bit                    req_out_credit_return_q;
    bit                    rsp_out_valid_q;
    bit [FLIT_WIDTH-1:0]   rsp_out_flit_q;
    bit                    rsp_out_credit_return_q;

    // -------------------------------------------------------------------------
    // always_ff: sync-reset, 3-step DPI call, registered outputs, error check
    // -------------------------------------------------------------------------

    always_ff @(posedge clk_i) begin
        if (!rst_ni) begin
            req_out_valid_q          <= '0;
            req_out_flit_q           <= '0;
            req_out_credit_return_q  <= '0;
            rsp_out_valid_q          <= '0;
            rsp_out_flit_q           <= '0;
            rsp_out_credit_return_q  <= '0;
        end else begin
            // Step 1: push current wire values into C++ input latch.
            cmodel_channel_model_set_inputs(
                noc_miso.req_valid,
                noc_miso.req_flit,
                noc_mosi.req_credit_return[0],
                noc_mosi.rsp_valid,
                noc_mosi.rsp_flit,
                noc_miso.rsp_credit_return[0]
            );

            // Step 2: advance C++ model one cycle.
            cmodel_channel_model_tick();

            // Step 3: pull outputs into local temporaries (blocking to locals is
            // safe; avoids BLKANDNBLK with the nonblocking reset path above).
            begin : get_outputs_blk
                bit                    t_req_out_valid;
                bit [FLIT_WIDTH-1:0]   t_req_out_flit;
                bit                    t_req_out_credit_return;
                bit                    t_rsp_out_valid;
                bit [FLIT_WIDTH-1:0]   t_rsp_out_flit;
                bit                    t_rsp_out_credit_return;
                cmodel_channel_model_get_outputs(
                    t_req_out_valid,
                    t_req_out_flit,
                    t_req_out_credit_return,
                    t_rsp_out_valid,
                    t_rsp_out_flit,
                    t_rsp_out_credit_return
                );
                req_out_valid_q         <= t_req_out_valid;
                req_out_flit_q          <= t_req_out_flit;
                req_out_credit_return_q <= t_req_out_credit_return;
                rsp_out_valid_q         <= t_rsp_out_valid;
                rsp_out_flit_q          <= t_rsp_out_flit;
                rsp_out_credit_return_q <= t_rsp_out_credit_return;
            end
        end
    end

    // -------------------------------------------------------------------------
    // Drive interface outputs from registered state
    // -------------------------------------------------------------------------

    // NSU-facing side: drive req_valid/req_flit forward.
    assign noc_mosi.req_valid             = req_out_valid_q;
    assign noc_mosi.req_flit              = req_out_flit_q;
    // NMU-facing side: return req credits back upstream.
    assign noc_miso.req_credit_return     = {NUM_VC{req_out_credit_return_q}};

    // NMU-facing side: drive rsp_valid/rsp_flit back toward NMU.
    assign noc_miso.rsp_valid             = rsp_out_valid_q;
    assign noc_miso.rsp_flit              = rsp_out_flit_q;
    // NSU-facing side: return rsp credits back upstream.
    assign noc_mosi.rsp_credit_return     = {NUM_VC{rsp_out_credit_return_q}};

endmodule

`endif  // CHANNEL_MODEL_WRAP_SV
