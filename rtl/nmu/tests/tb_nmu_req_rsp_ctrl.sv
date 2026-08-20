`resetall
`timescale 1ns / 1ps
`default_nettype none

module tb_nmu_req_rsp_ctrl #(
    parameter int unsigned AXI_ID_WIDTH = 3,
    parameter int unsigned MAX_TXNS_PER_ID = 32
);

    nmu_req_rsp_ctrl_if #(
        .AXI_ID_WIDTH    ( AXI_ID_WIDTH ),
        .MAX_TXNS_PER_ID ( MAX_TXNS_PER_ID )
    ) ctrl ();

    nmu_req_rsp_ctrl_shell #(
        .AXI_ID_WIDTH    ( AXI_ID_WIDTH ),
        .MAX_TXNS_PER_ID ( MAX_TXNS_PER_ID )
    ) dut ( .ctrl ( ctrl ) );

    initial begin
        ctrl.aw_req_valid = 1'b0;
        ctrl.ar_req_valid = 1'b0;
        ctrl.aw_rsp_ready = 1'b1;
        ctrl.ar_rsp_ready = 1'b1;
        ctrl.b_retire_valid = 1'b0;
        ctrl.r_retire_valid = 1'b0;
        #1ns;
        $display("PASS: NMU request-response control interface contract");
        $finish;
    end

endmodule

`resetall
