// NMU C-model proxy: snapshots the NMU AXI4 bundle from the DPI-C bridge
// and drives it onto axi_if each clock cycle.
//
// IMPORTANT: cmodel_tick() is called exactly ONCE per posedge clk (here,
// in the NMU proxy). The NSU proxy does NOT call cmodel_tick() — the
// C-model advances exactly once per SV clock edge.
//
// DPI-C output arguments are written via local intermediates so that
// nonblocking assignment (<=) to the interface signals is the only
// write path, satisfying Verilator BLKANDNBLK rules.

module nmu_cmodel_proxy #(
    parameter int ID_WIDTH   = 8,
    parameter int ADDR_WIDTH = 64,
    parameter int DATA_WIDTH = 256
) (
    input logic clk,
    input logic rst_n,
    axi_if      aif  // no modport: proxy drives all wires in the bundle
);
    import "DPI-C" context function void cmodel_tick();
    import "DPI-C" context function void cmodel_nmu_get_aw(
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
    import "DPI-C" context function void cmodel_nmu_get_w(
        output bit                    valid,
        output bit                    ready,
        output bit [DATA_WIDTH-1:0]   data,
        output bit [DATA_WIDTH/8-1:0] strb,
        output bit                    last);
    import "DPI-C" context function void cmodel_nmu_get_ar(
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
    import "DPI-C" context function void cmodel_nmu_get_b(
        output bit                valid,
        output bit                ready,
        output bit [ID_WIDTH-1:0] id,
        output bit [1:0]          resp);
    import "DPI-C" context function void cmodel_nmu_get_r(
        output bit                  valid,
        output bit                  ready,
        output bit [ID_WIDTH-1:0]   id,
        output bit [DATA_WIDTH-1:0] data,
        output bit [1:0]            resp,
        output bit                  last);

    // Local intermediates: DPI-C writes here (blocking), then we
    // nonblocking-assign to the interface signals below.
    bit                  t_awvalid, t_awready;
    bit [ID_WIDTH-1:0]   t_awid;
    bit [ADDR_WIDTH-1:0] t_awaddr;
    bit [7:0]            t_awlen;
    bit [2:0]            t_awsize;
    bit [1:0]            t_awburst;
    bit                  t_awlock;
    bit [3:0]            t_awcache;
    bit [2:0]            t_awprot;
    bit [3:0]            t_awqos;

    bit                    t_wvalid, t_wready, t_wlast;
    bit [DATA_WIDTH-1:0]   t_wdata;
    bit [DATA_WIDTH/8-1:0] t_wstrb;

    bit                  t_bvalid, t_bready;
    bit [ID_WIDTH-1:0]   t_bid;
    bit [1:0]            t_bresp;

    bit                  t_arvalid, t_arready;
    bit [ID_WIDTH-1:0]   t_arid;
    bit [ADDR_WIDTH-1:0] t_araddr;
    bit [7:0]            t_arlen;
    bit [2:0]            t_arsize;
    bit [1:0]            t_arburst;
    bit                  t_arlock;
    bit [3:0]            t_arcache;
    bit [2:0]            t_arprot;
    bit [3:0]            t_arqos;

    bit                  t_rvalid, t_rready, t_rlast;
    bit [ID_WIDTH-1:0]   t_rid;
    bit [DATA_WIDTH-1:0] t_rdata;
    bit [1:0]            t_rresp;

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
            // Advance the C-model by one cycle — called ONCE per clock edge.
            cmodel_tick();

            // DPI-C populates local intermediates (blocking to locals).
            cmodel_nmu_get_aw(t_awvalid, t_awready,
                              t_awid, t_awaddr,
                              t_awlen, t_awsize, t_awburst,
                              t_awlock, t_awcache,
                              t_awprot, t_awqos);
            cmodel_nmu_get_w(t_wvalid, t_wready,
                             t_wdata, t_wstrb, t_wlast);
            cmodel_nmu_get_ar(t_arvalid, t_arready,
                              t_arid, t_araddr,
                              t_arlen, t_arsize, t_arburst,
                              t_arlock, t_arcache,
                              t_arprot, t_arqos);
            cmodel_nmu_get_b(t_bvalid, t_bready, t_bid, t_bresp);
            cmodel_nmu_get_r(t_rvalid, t_rready, t_rid, t_rdata,
                             t_rresp, t_rlast);

            // Nonblocking transfer to interface signals (single write path).
            aif.awvalid <= t_awvalid; aif.awready <= t_awready;
            aif.awid    <= t_awid;    aif.awaddr  <= t_awaddr;
            aif.awlen   <= t_awlen;   aif.awsize  <= t_awsize;
            aif.awburst <= t_awburst;
            aif.awlock  <= t_awlock;  aif.awcache <= t_awcache;
            aif.awprot  <= t_awprot;  aif.awqos   <= t_awqos;
            aif.wvalid  <= t_wvalid;  aif.wready  <= t_wready;
            aif.wlast   <= t_wlast;
            aif.wdata   <= t_wdata;   aif.wstrb   <= t_wstrb;
            aif.bvalid  <= t_bvalid;  aif.bready  <= t_bready;
            aif.bid     <= t_bid;     aif.bresp   <= t_bresp;
            aif.arvalid <= t_arvalid; aif.arready <= t_arready;
            aif.arid    <= t_arid;    aif.araddr  <= t_araddr;
            aif.arlen   <= t_arlen;   aif.arsize  <= t_arsize;
            aif.arburst <= t_arburst;
            aif.arlock  <= t_arlock;  aif.arcache <= t_arcache;
            aif.arprot  <= t_arprot;  aif.arqos   <= t_arqos;
            aif.rvalid  <= t_rvalid;  aif.rready  <= t_rready;
            aif.rlast   <= t_rlast;
            aif.rid     <= t_rid;     aif.rdata   <= t_rdata;
            aif.rresp   <= t_rresp;
        end
    end
endmodule
