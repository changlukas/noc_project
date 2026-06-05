// loopback_noc_wrap — Stage 5b DPI shell for LoopbackNoc component.
//
// Beta-tick discipline (spec §5.1): on every posedge clk_i the module
// samples the PREVIOUS cycle's registered wire inputs, pushes them to
// the C++ model via DPI set_inputs, advances the model via tick, pulls
// outputs via get_outputs, and registers those outputs nonblocking so
// they are visible to SV wires from the NEXT cycle onward.
//
// FLIT_W must match ni::FLIT_WIDTH = 408 (c_model flit width). The
// noc_req_intf / noc_rsp_intf interface parameter is overridden at
// instantiation in tb_top.sv.
//
// Inline error check (spec §7.5): cmodel_check_error() called at end of
// every active always_ff body; non-zero triggers $fatal after cmodel_finalize.
//
// Reset: synchronous active-low (rst_ni). Output registers cleared on reset.
// No async reset path — sync reset is the project default per rtl-style.

`ifndef LOOPBACK_NOC_WRAP_SV
`define LOOPBACK_NOC_WRAP_SV

module loopback_noc_wrap #(
    parameter int NUM_VC = 1,
    parameter int FLIT_W = 408
) (
    input  logic              clk_i,
    input  logic              rst_ni,
    noc_req_intf.consumer     req_from_nmu_i,
    noc_req_intf.producer     req_to_nsu_o,
    noc_rsp_intf.consumer     rsp_from_nsu_i,
    noc_rsp_intf.producer     rsp_to_nmu_o
);

    // -------------------------------------------------------------------------
    // DPI imports — 3-step pattern per spec §5.1
    // -------------------------------------------------------------------------

    // set_inputs: sample SV wire state into C++ input latch.
    // svBitVecVal carries FLIT_W bits as ceil(FLIT_W/32) 32-bit words.
    import "DPI-C" context function void cmodel_loopback_noc_set_inputs(
        input  bit              req_in_valid,
        input  bit [FLIT_W-1:0] req_in_flit,
        input  bit              req_in_credit_return,
        input  bit              rsp_in_valid,
        input  bit [FLIT_W-1:0] rsp_in_flit,
        input  bit              rsp_in_credit_return
    );

    // tick: advance C++ model one cycle.
    import "DPI-C" context function void cmodel_loopback_noc_tick();

    // get_outputs: read C++ output latch into SV locals.
    import "DPI-C" context function void cmodel_loopback_noc_get_outputs(
        output bit              req_out_valid,
        output bit [FLIT_W-1:0] req_out_flit,
        output bit              req_out_credit_return,
        output bit              rsp_out_valid,
        output bit [FLIT_W-1:0] rsp_out_flit,
        output bit              rsp_out_credit_return
    );

    // Lifecycle / error polling (shared with all shells).
    import "DPI-C" context function int  cmodel_check_error(output string msg);
    import "DPI-C" context function void cmodel_finalize();

    // -------------------------------------------------------------------------
    // Output registers (beta-tick: registered one cycle behind DPI sample)
    // -------------------------------------------------------------------------

    bit               req_out_valid_q;
    bit [FLIT_W-1:0]  req_out_flit_q;
    bit               req_out_credit_return_q;
    bit               rsp_out_valid_q;
    bit [FLIT_W-1:0]  rsp_out_flit_q;
    bit               rsp_out_credit_return_q;

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
            cmodel_loopback_noc_set_inputs(
                req_from_nmu_i.valid,
                req_from_nmu_i.flit,
                req_to_nsu_o.credit_return[0],
                rsp_from_nsu_i.valid,
                rsp_from_nsu_i.flit,
                rsp_to_nmu_o.credit_return[0]
            );

            // Step 2: advance C++ model one cycle.
            cmodel_loopback_noc_tick();

            // Step 3: pull outputs into local temporaries (blocking to locals is
            // safe; avoids BLKANDNBLK with the nonblocking reset path above).
            begin : get_outputs_blk
                bit               t_req_out_valid;
                bit [FLIT_W-1:0]  t_req_out_flit;
                bit               t_req_out_credit_return;
                bit               t_rsp_out_valid;
                bit [FLIT_W-1:0]  t_rsp_out_flit;
                bit               t_rsp_out_credit_return;
                cmodel_loopback_noc_get_outputs(
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

            // Inline error check (spec §7.5): poll after every tick.
            begin : error_check
                string err_msg;
                int    err_code;
                err_code = cmodel_check_error(err_msg);
                if (err_code != 0) begin
                    $display("[loopback_noc_wrap] DPI fatal (code=%0d): %s",
                             err_code, err_msg);
                    cmodel_finalize();
                    $fatal(1, "loopback_noc_wrap: DPI error, simulation aborted");
                end
            end
        end
    end

    // -------------------------------------------------------------------------
    // Drive interface outputs from registered state
    // -------------------------------------------------------------------------

    assign req_to_nsu_o.valid            = req_out_valid_q;
    assign req_to_nsu_o.flit             = req_out_flit_q;
    assign req_from_nmu_i.credit_return  =
        {NUM_VC{req_out_credit_return_q}};

    assign rsp_to_nmu_o.valid            = rsp_out_valid_q;
    assign rsp_to_nmu_o.flit             = rsp_out_flit_q;
    assign rsp_from_nsu_i.credit_return  =
        {NUM_VC{rsp_out_credit_return_q}};

endmodule

`endif  // LOOPBACK_NOC_WRAP_SV
