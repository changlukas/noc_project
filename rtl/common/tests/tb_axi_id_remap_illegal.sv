`timescale 1ns / 1ps

module tb_axi_id_remap_illegal #(
    parameter int unsigned INVALID_CASE = 0
);

    localparam int unsigned AXI_ID_WIDTH = INVALID_CASE == 0 ? 0 :
                                           INVALID_CASE == 1 ? 9 : 3;
    localparam int unsigned NOC_ID_WIDTH = INVALID_CASE == 2 ? 2 :
                                           INVALID_CASE == 3 ? 4 : 3;

    initial begin
        if (AXI_ID_WIDTH < 1 || AXI_ID_WIDTH > 8) begin
            $fatal(1, "AXI_ID_WIDTH must be between 1 and 8");
        end
        if (NOC_ID_WIDTH != 3) begin
            $fatal(1, "NOC_ID_WIDTH must be fixed at 3");
        end
        $fatal(1, "illegal ID width unexpectedly accepted");
    end

endmodule
