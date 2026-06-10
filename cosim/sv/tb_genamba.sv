// gen_amba role-1 testbench top — T2 BFM<->mem_axi direct wiring
`timescale 1ns/1ps
`include "wb2axip/sim_wrapper.svh"

module tb_genamba;
    reg ACLK = 0;
    always #5 ACLK = ~ACLK;          // 10 ns period
    reg ARESETn = 0;

    // BFM <-> mem_axi direct wiring (no bridge yet — T2 baseline)
    // separate bfm_awid + bfm_arid wires (BFM outputs both; tying both to
    // one wire would short-circuit two driver outputs)
    wire [7:0]   bfm_awid, bfm_arid;
    wire [63:0]  bfm_awaddr, bfm_araddr;
    wire [7:0]   bfm_awlen,  bfm_arlen;
    wire [2:0]   bfm_awsize, bfm_arsize;
    wire [1:0]   bfm_awburst, bfm_arburst;
    wire         bfm_awlock, bfm_arlock;
    wire [3:0]   bfm_awcache, bfm_arcache;
    wire [2:0]   bfm_awprot,  bfm_arprot;
    wire [3:0]   bfm_awqos,   bfm_arqos;
    wire         bfm_awvalid, bfm_awready, bfm_arvalid, bfm_arready;
    wire [255:0] bfm_wdata; wire [31:0] bfm_wstrb;
    wire         bfm_wlast, bfm_wvalid, bfm_wready;
    wire [7:0]   mem_bid; wire [1:0] mem_bresp; wire mem_bvalid, bfm_bready;
    wire [7:0]   mem_rid; wire [255:0] mem_rdata; wire [1:0] mem_rresp;
    wire         mem_rlast, mem_rvalid, bfm_rready;

    genamba_master_bfm #(.WIDTH_AD(64), .WIDTH_DA(256), .WIDTH_ID(8), .P_MST_ID(0)) u_bfm (
        .ACLK(ACLK), .ARESETn(ARESETn),
        .AWID(bfm_awid), .AWADDR(bfm_awaddr), .AWLEN(bfm_awlen), .AWSIZE(bfm_awsize),
        .AWBURST(bfm_awburst), .AWLOCK(bfm_awlock), .AWCACHE(bfm_awcache),
        .AWPROT(bfm_awprot), .AWQOS(bfm_awqos),
        .AWVALID(bfm_awvalid), .AWREADY(bfm_awready),
        .WDATA(bfm_wdata), .WSTRB(bfm_wstrb), .WLAST(bfm_wlast),
        .WVALID(bfm_wvalid), .WREADY(bfm_wready),
        .BID(mem_bid), .BRESP(mem_bresp), .BVALID(mem_bvalid), .BREADY(bfm_bready),
        .ARID(bfm_arid), .ARADDR(bfm_araddr), .ARLEN(bfm_arlen), .ARSIZE(bfm_arsize),
        .ARBURST(bfm_arburst), .ARLOCK(bfm_arlock), .ARCACHE(bfm_arcache),
        .ARPROT(bfm_arprot), .ARQOS(bfm_arqos),
        .ARVALID(bfm_arvalid), .ARREADY(bfm_arready),
        .RID(mem_rid), .RDATA(mem_rdata), .RRESP(mem_rresp), .RLAST(mem_rlast),
        .RVALID(mem_rvalid), .RREADY(bfm_rready)
    );

    mem_axi #(
        .AXI_WIDTH_CID(0), .AXI_WIDTH_ID(8), .AXI_WIDTH_AD(64),
        .AXI_WIDTH_DA(256), .SIZE_IN_BYTES(16384)
    ) u_mem (
        .ARESETn(ARESETn), .ACLK(ACLK), .CSYSREQ(1'b1), .CSYSACK(), .CACTIVE(),
        .AWID(bfm_awid), .AWADDR(bfm_awaddr), .AWLEN(bfm_awlen), .AWSIZE(bfm_awsize),
        .AWBURST(bfm_awburst), .AWLOCK(bfm_awlock), .AWCACHE(bfm_awcache),
        .AWPROT(bfm_awprot), .AWQOS(bfm_awqos), .AWREGION(4'b0),
        .AWVALID(bfm_awvalid), .AWREADY(bfm_awready),
        .WDATA(bfm_wdata), .WSTRB(bfm_wstrb), .WLAST(bfm_wlast),
        .WVALID(bfm_wvalid), .WREADY(bfm_wready),
        .BID(mem_bid), .BRESP(mem_bresp), .BVALID(mem_bvalid), .BREADY(bfm_bready),
        .ARID(bfm_arid), .ARADDR(bfm_araddr), .ARLEN(bfm_arlen), .ARSIZE(bfm_arsize),
        .ARBURST(bfm_arburst), .ARLOCK(bfm_arlock), .ARCACHE(bfm_arcache),
        .ARPROT(bfm_arprot), .ARQOS(bfm_arqos), .ARREGION(4'b0),
        .ARVALID(bfm_arvalid), .ARREADY(bfm_arready),
        .RID(mem_rid), .RDATA(mem_rdata), .RRESP(mem_rresp), .RLAST(mem_rlast),
        .RVALID(mem_rvalid), .RREADY(bfm_rready)
    );

    initial begin
        repeat (4) @(posedge ACLK);
        ARESETn = 1'b1;
        repeat (10) @(posedge ACLK);
        u_bfm.test_baseline_mem_test;      // Task A
        repeat (20) @(posedge ACLK);
        $display("[%0t] tb_genamba: T2 PASS (BFM<->mem_axi mem_test)", $time);
        $finish;
    end
endmodule
