// axi_slave_wrap — Stage 5b DPI shell for AxiSlave component.
//
// Beta-tick discipline (spec §5.1): on every posedge clk_i the module
// samples the PREVIOUS cycle's registered wire inputs (from axi_i.awvalid/
// wvalid/arvalid + bready/rready channels), pushes them to C++ via
// cmodel_slave_set_inputs, advances the model via cmodel_slave_tick, pulls
// outputs via cmodel_slave_get_outputs, then registers those outputs
// nonblocking so they are visible from the NEXT cycle onward.
//
// Reset: synchronous active-low (rst_ni). Output registers cleared on reset;
// no async reset path — sync reset is the project default per rtl-style.
//
// Error polling is centralized in tb_top.sv (T1.4); this wrap no longer
// calls cmodel_check_error/cmodel_finalize itself.
//
// axi4_intf.slave modport: slave reads AW/W/AR + bready/rready from axi_i;
//                         slave drives awready/wready/arready + B/R to axi_i.

`ifndef AXI_SLAVE_WRAP_SV
`define AXI_SLAVE_WRAP_SV

module axi_slave_wrap #(
    parameter int unsigned ID_WIDTH   = ni_params_pkg::NI_AXI_ID_WIDTH_DFLT,
    parameter int unsigned ADDR_WIDTH = ni_params_pkg::NI_AXI_ADDR_WIDTH_DFLT,
    parameter int unsigned DATA_WIDTH = ni_params_pkg::NI_AXI_DATA_WIDTH_DFLT
) (
    input  logic    clk_i,
    input  logic    rst_ni,
    axi4_intf.slave axi_i
);

    // -------------------------------------------------------------------------
    // DPI imports — 3-step pattern per spec §5.1
    // -------------------------------------------------------------------------

    import "DPI-C" context function void cmodel_slave_set_inputs(
        input  bit                    awvalid,
        input  bit [ID_WIDTH-1:0]     awid,
        input  bit [ADDR_WIDTH-1:0]   awaddr,
        input  bit [7:0]              awlen,
        input  bit [2:0]              awsize,
        input  bit [1:0]              awburst,
        input  bit                    awlock,
        input  bit [3:0]              awcache,
        input  bit [2:0]              awprot,
        input  bit [3:0]              awqos,
        input  bit                    wvalid,
        input  bit [DATA_WIDTH-1:0]   wdata,
        input  bit [DATA_WIDTH/8-1:0] wstrb,
        input  bit                    wlast,
        input  bit                    arvalid,
        input  bit [ID_WIDTH-1:0]     arid,
        input  bit [ADDR_WIDTH-1:0]   araddr,
        input  bit [7:0]              arlen,
        input  bit [2:0]              arsize,
        input  bit [1:0]              arburst,
        input  bit                    arlock,
        input  bit [3:0]              arcache,
        input  bit [2:0]              arprot,
        input  bit [3:0]              arqos,
        input  bit                    bready,
        input  bit                    rready
    );

    import "DPI-C" context function void cmodel_slave_tick();

    import "DPI-C" context function void cmodel_slave_get_outputs(
        output bit                    awready,
        output bit                    wready,
        output bit                    arready,
        output bit                    bvalid,
        output bit [ID_WIDTH-1:0]     bid,
        output bit [1:0]              bresp,
        output bit                    rvalid,
        output bit [ID_WIDTH-1:0]     rid,
        output bit [DATA_WIDTH-1:0]   rdata,
        output bit [1:0]              rresp,
        output bit                    rlast
    );

    // Lifecycle / error polling lives in tb_top.sv (T1.4).

    // -------------------------------------------------------------------------
    // Output registers (beta-tick: registered one cycle behind DPI sample)
    // -------------------------------------------------------------------------

    bit awready_q;
    bit wready_q;
    bit arready_q;

    bit                    bvalid_q;
    bit [ID_WIDTH-1:0]     bid_q;
    bit [1:0]              bresp_q;

    bit                    rvalid_q;
    bit [ID_WIDTH-1:0]     rid_q;
    bit [DATA_WIDTH-1:0]   rdata_q;
    bit [1:0]              rresp_q;
    bit                    rlast_q;

    // -------------------------------------------------------------------------
    // always_ff: sync-reset, 3-step DPI call, registered outputs, error check
    // -------------------------------------------------------------------------

    always_ff @(posedge clk_i) begin
        if (!rst_ni) begin
            awready_q <= '0;
            wready_q  <= '0;
            arready_q <= '0;
            bvalid_q  <= '0;
            bid_q     <= '0;
            bresp_q   <= '0;
            rvalid_q  <= '0;
            rid_q     <= '0;
            rdata_q   <= '0;
            rresp_q   <= '0;
            rlast_q   <= '0;
        end else begin
            // Step 1: push current master-side wire values into C++ input latch.
            cmodel_slave_set_inputs(
                axi_i.awvalid,
                axi_i.awid,
                axi_i.awaddr,
                axi_i.awlen,
                axi_i.awsize,
                axi_i.awburst,
                axi_i.awlock,
                axi_i.awcache,
                axi_i.awprot,
                axi_i.awqos,
                axi_i.wvalid,
                axi_i.wdata,
                axi_i.wstrb,
                axi_i.wlast,
                axi_i.arvalid,
                axi_i.arid,
                axi_i.araddr,
                axi_i.arlen,
                axi_i.arsize,
                axi_i.arburst,
                axi_i.arlock,
                axi_i.arcache,
                axi_i.arprot,
                axi_i.arqos,
                axi_i.bready,
                axi_i.rready
            );

            // Step 2: advance C++ model one cycle.
            cmodel_slave_tick();

            // Step 3: pull outputs into local temporaries (blocking to locals is
            // safe; avoids BLKANDNBLK with the nonblocking reset path above).
            begin : get_outputs_blk
                bit                    t_awready;
                bit                    t_wready;
                bit                    t_arready;
                bit                    t_bvalid;
                bit [ID_WIDTH-1:0]     t_bid;
                bit [1:0]              t_bresp;
                bit                    t_rvalid;
                bit [ID_WIDTH-1:0]     t_rid;
                bit [DATA_WIDTH-1:0]   t_rdata;
                bit [1:0]              t_rresp;
                bit                    t_rlast;
                cmodel_slave_get_outputs(
                    t_awready,
                    t_wready,
                    t_arready,
                    t_bvalid, t_bid, t_bresp,
                    t_rvalid, t_rid, t_rdata, t_rresp, t_rlast
                );
                awready_q <= t_awready;
                wready_q  <= t_wready;
                arready_q <= t_arready;
                bvalid_q  <= t_bvalid;
                bid_q     <= t_bid;
                bresp_q   <= t_bresp;
                rvalid_q  <= t_rvalid;
                rid_q     <= t_rid;
                rdata_q   <= t_rdata;
                rresp_q   <= t_rresp;
                rlast_q   <= t_rlast;
            end
        end
    end

    // -------------------------------------------------------------------------
    // Drive interface outputs from registered state
    // -------------------------------------------------------------------------

    assign axi_i.awready = awready_q;
    assign axi_i.wready  = wready_q;
    assign axi_i.arready = arready_q;

    assign axi_i.bvalid  = bvalid_q;
    assign axi_i.bid     = bid_q;
    assign axi_i.bresp   = bresp_q;

    assign axi_i.rvalid  = rvalid_q;
    assign axi_i.rid     = rid_q;
    assign axi_i.rdata   = rdata_q;
    assign axi_i.rresp   = rresp_q;
    assign axi_i.rlast   = rlast_q;

endmodule

`endif  // AXI_SLAVE_WRAP_SV
