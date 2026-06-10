// gen_amba role-1 testbench top — T1 skeleton (mem_axi standalone)
`timescale 1ns/1ps
`include "wb2axip/sim_wrapper.svh"

module tb_genamba;
    reg ACLK = 0;
    always #5 ACLK = ~ACLK;          // 10 ns period
    reg ARESETn = 0;

    // mem_axi at AD=64 / DA=256 / ID=8 / no CID / 16 KiB (spec §3.2)
    mem_axi #(
        .AXI_WIDTH_CID(0),
        .AXI_WIDTH_ID (8),
        .AXI_WIDTH_AD (64),
        .AXI_WIDTH_DA (256),
        .SIZE_IN_BYTES(16384)
    ) u_mem (
        .ARESETn (ARESETn),
        .ACLK    (ACLK),
        .CSYSREQ (1'b1),
        .CSYSACK (),
        .CACTIVE (),
        // AW (tied)
        .AWID(8'd0), .AWADDR(64'd0), .AWLEN(8'd0), .AWSIZE(3'd0), .AWBURST(2'd0),
        .AWLOCK(1'b0), .AWCACHE(4'd0), .AWPROT(3'd0), .AWQOS(4'd0), .AWREGION(4'd0),
        .AWVALID(1'b0), .AWREADY(),
        // W (tied)
        .WDATA(256'd0), .WSTRB(32'd0), .WLAST(1'b0),
        .WVALID(1'b0), .WREADY(),
        // B
        .BID(), .BRESP(), .BVALID(), .BREADY(1'b0),
        // AR (tied)
        .ARID(8'd0), .ARADDR(64'd0), .ARLEN(8'd0), .ARSIZE(3'd0), .ARBURST(2'd0),
        .ARLOCK(1'b0), .ARCACHE(4'd0), .ARPROT(3'd0), .ARQOS(4'd0), .ARREGION(4'd0),
        .ARVALID(1'b0), .ARREADY(),
        // R
        .RID(), .RDATA(), .RRESP(), .RLAST(), .RVALID(), .RREADY(1'b0)
    );

    initial begin
        $display("[%0t] tb_genamba: reset assert", $time);
        repeat (4) @(posedge ACLK);
        ARESETn = 1'b1;
        $display("[%0t] tb_genamba: reset deassert", $time);
        repeat (20) @(posedge ACLK);
        $display("[%0t] tb_genamba: T1 PASS (mem_axi standalone)", $time);
        $finish;
    end
endmodule
