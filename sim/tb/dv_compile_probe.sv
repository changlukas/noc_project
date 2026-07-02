// Temporary compile gate for sim/dv import; superseded by user_node_endpoint.
module dv_compile_probe;
    logic clk = 0;
    AXI_BUS_DV #(.AXI_ADDR_WIDTH(32), .AXI_DATA_WIDTH(64),
                 .AXI_ID_WIDTH(4), .AXI_USER_WIDTH(1)) dv (clk);
    typedef axi_test::axi_rand_master #(.AW(32), .DW(64), .IW(4), .UW(1),
        .TA(2ns), .TT(8ns)) rand_mst_t;
    typedef axi_test::axi_rand_slave #(.AW(32), .DW(64), .IW(4), .UW(1),
        .TA(2ns), .TT(8ns), .MAPPED(1)) rand_slv_t;
    initial begin $display("DV_COMPILE_OK"); $finish; end
endmodule
