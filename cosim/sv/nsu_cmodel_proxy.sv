// NSU C-model proxy: snapshots the NSU AXI4 bundle from the DPI-C bridge
// and drives it onto axi_if each clock cycle.
//
// NOTE: this proxy does NOT call cmodel_tick() — only the NMU proxy does.
// The C-model advances exactly once per SV clock edge (in nmu_cmodel_proxy).

module nsu_cmodel_proxy #(
    parameter int ID_WIDTH   = 8,
    parameter int ADDR_WIDTH = 64,
    parameter int DATA_WIDTH = 256
) (
    input logic clk,
    input logic rst_n,
    axi_if      aif  // no modport: proxy drives all wires in the bundle
);
    import "DPI-C" context function void cmodel_nsu_get_aw(
        output bit                  valid,
        output bit                  ready,
        output bit [ID_WIDTH-1:0]   id,
        output bit [ADDR_WIDTH-1:0] addr,
        output bit [7:0]            len,
        output bit [2:0]            size,
        output bit [1:0]            burst,
        output bit                  lock,
        output bit [3:0]            cache,
        output bit [2:0]            prot,
        output bit [3:0]            qos);
    import "DPI-C" context function void cmodel_nsu_get_w(
        output bit                    valid,
        output bit                    ready,
        output bit [DATA_WIDTH-1:0]   data,
        output bit [DATA_WIDTH/8-1:0] strb,
        output bit                    last);
    import "DPI-C" context function void cmodel_nsu_get_ar(
        output bit                  valid,
        output bit                  ready,
        output bit [ID_WIDTH-1:0]   id,
        output bit [ADDR_WIDTH-1:0] addr,
        output bit [7:0]            len,
        output bit [2:0]            size,
        output bit [1:0]            burst,
        output bit                  lock,
        output bit [3:0]            cache,
        output bit [2:0]            prot,
        output bit [3:0]            qos);
    import "DPI-C" context function void cmodel_nsu_get_b(
        output bit                valid,
        output bit                ready,
        output bit [ID_WIDTH-1:0] id,
        output bit [1:0]          resp);
    import "DPI-C" context function void cmodel_nsu_get_r(
        output bit                  valid,
        output bit                  ready,
        output bit [ID_WIDTH-1:0]   id,
        output bit [DATA_WIDTH-1:0] data,
        output bit [1:0]            resp,
        output bit                  last);

    always_ff @(posedge clk) begin
        if (!rst_n) begin
            aif.awvalid <= '0; aif.awready <= '0;
            aif.awid    <= '0; aif.awaddr  <= '0;
            aif.awlen   <= '0; aif.awsize  <= '0; aif.awburst <= '0;
            aif.awlock  <= '0; aif.awcache <= '0;
            aif.awprot  <= '0; aif.awqos   <= '0;
            aif.wvalid  <= '0; aif.wready  <= '0; aif.wlast <= '0;
            aif.wdata   <= '0; aif.wstrb   <= '0;
            aif.bvalid  <= '0; aif.bready  <= '0;
            aif.bid     <= '0; aif.bresp   <= '0;
            aif.arvalid <= '0; aif.arready <= '0;
            aif.arid    <= '0; aif.araddr  <= '0;
            aif.arlen   <= '0; aif.arsize  <= '0; aif.arburst <= '0;
            aif.arlock  <= '0; aif.arcache <= '0;
            aif.arprot  <= '0; aif.arqos   <= '0;
            aif.rvalid  <= '0; aif.rready  <= '0; aif.rlast <= '0;
            aif.rid     <= '0; aif.rdata   <= '0; aif.rresp <= '0;
        end else begin
            cmodel_nsu_get_aw(aif.awvalid, aif.awready,
                              aif.awid, aif.awaddr,
                              aif.awlen, aif.awsize, aif.awburst,
                              aif.awlock, aif.awcache,
                              aif.awprot, aif.awqos);
            cmodel_nsu_get_w(aif.wvalid, aif.wready,
                             aif.wdata, aif.wstrb, aif.wlast);
            cmodel_nsu_get_ar(aif.arvalid, aif.arready,
                              aif.arid, aif.araddr,
                              aif.arlen, aif.arsize, aif.arburst,
                              aif.arlock, aif.arcache,
                              aif.arprot, aif.arqos);
            cmodel_nsu_get_b(aif.bvalid, aif.bready, aif.bid, aif.bresp);
            cmodel_nsu_get_r(aif.rvalid, aif.rready, aif.rid, aif.rdata,
                             aif.rresp, aif.rlast);
        end
    end
endmodule
