`timescale 1ns/1ps

module tb_nmu_req_hold;

    localparam int unsigned REQ_FLIT_WIDTH = ni_params_pkg::NOC_REQ_FLIT_WIDTH_DFLT;

    logic clk_i = 1'b0;
    logic rst_ni = 1'b0;
    logic tx_req_ready_i = 1'b1;
    logic tx_req_valid_o;
    logic [REQ_FLIT_WIDTH-1:0] tx_req_flit_o;
    ni_signals_pkg::axi_req_t axi_req_i = '0;
    ni_signals_pkg::axi_rsp_t axi_rsp_o;
    int unsigned handshakes = 0;

    always #5 clk_i = ~clk_i;

    always_ff @(posedge clk_i) begin
        if (!rst_ni) begin
            handshakes <= 0;
        end else if (tx_req_valid_o && tx_req_ready_i) begin
            assert (tx_req_flit_o[31:0] == 32'h89abcdef + handshakes)
                else $fatal(1, "REQ payload/order mismatch at transfer %0d", handshakes);
            handshakes <= handshakes + 1;
        end
    end

    nmu_wrap u_dut (
        .clk_i,
        .rst_ni,
        .ctx_i(64'd1),
        .axi_req_i,
        .awuser_i('0),
        .axi_rsp_o,
        .tx_req_valid_o,
        .tx_req_flit_o,
        .tx_req_ready_i,
        .rx_rsp_valid_i(1'b0),
        .rx_rsp_flit_i('0),
        .rx_rsp_ready_o(),
        .tx_dat_valid_o(),
        .tx_dat_flit_o(),
        .tx_dat_crdvalid_i('0),
        .rx_dat_valid_i(1'b0),
        .rx_dat_flit_i('0),
        .rx_dat_crdvalid_o()
    );

    initial begin : run_test
        logic [REQ_FLIT_WIDTH-1:0] held_flit;
        int unsigned wait_cycles = 0;

        repeat (2) @(posedge clk_i);
        @(negedge clk_i);
        rst_ni = 1'b1;

        while (!tx_req_valid_o && wait_cycles < 8) begin
            @(negedge clk_i);
            wait_cycles++;
        end
        assert (tx_req_valid_o)
            else $fatal(1, "REQ model strobe did not reach the wrapper output");

        held_flit = tx_req_flit_o;
        tx_req_ready_i = 1'b0;

        repeat (3) begin
            @(posedge clk_i);
            #1;
            assert (tx_req_valid_o)
                else $fatal(1, "REQ valid dropped before the RTL-side handshake");
            assert (tx_req_flit_o == held_flit)
                else $fatal(1, "REQ payload changed while stalled");
        end

        @(negedge clk_i);
        tx_req_ready_i = 1'b1;
        @(posedge clk_i);
        #1;
        assert (handshakes == 1)
            else $fatal(1, "REQ transfer count was %0d instead of 1", handshakes);
        assert (!tx_req_valid_o || tx_req_flit_o[31:0] == 32'h89abcdf0)
            else $fatal(1, "REQ source repeated the retired flit");

        // Keep model-facing randomized-stall coverage after the directed loss
        // sequence.  A fixed simulator seed makes failures reproducible.
        wait_cycles = 0;
        while (handshakes < 16 && wait_cycles < 200) begin
            @(negedge clk_i);
            tx_req_ready_i = $urandom_range(0, 1);
            wait_cycles++;
        end
        assert (handshakes == 16)
            else $fatal(1, "randomized REQ stalls retired %0d of 16 flits", handshakes);

        $display("PASS: model REQ strobes held through directed and randomized RTL-side stalls");
        $finish;
    end

endmodule
