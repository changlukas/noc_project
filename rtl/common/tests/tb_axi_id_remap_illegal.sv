`timescale 1ns / 1ps

module tb_axi_id_remap_illegal;

    localparam int unsigned AXI_ID_WIDTH = 0;
    localparam int unsigned NOC_ID_WIDTH = 3;

    initial begin
        if (AXI_ID_WIDTH < 1 || AXI_ID_WIDTH > 8 || NOC_ID_WIDTH != 3) begin
            $fatal(1, "illegal AXI/NoC ID-width contract");
        end
        $fatal(1, "illegal AXI ID width unexpectedly accepted");
    end

endmodule
