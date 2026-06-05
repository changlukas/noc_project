// axi_master_wrap — Stage 5b DPI shell for AxiMaster component.
//
// Beta-tick discipline (spec §5.1): on every posedge clk_i the module
// samples the PREVIOUS cycle's registered wire inputs (from axi_o.*ready +
// B/R channels), pushes them to C++ via cmodel_master_set_inputs, advances
// the model via cmodel_master_tick, pulls outputs via cmodel_master_get_outputs,
// then registers those outputs nonblocking so they are visible from the NEXT
// cycle onward.
//
// Reset: synchronous active-low (rst_ni). Output registers cleared on reset;
// no async reset path — sync reset is the project default per rtl-style.
//
// Inline error check (spec §7.5): cmodel_check_error() called at end of
// every active always_ff body; non-zero triggers $fatal after cmodel_finalize.
//
// axi_intf.master modport: master drives AW/W/AR + bready/rready;
//                          master reads awready/wready/arready + B/R.

`ifndef AXI_MASTER_WRAP_SV
`define AXI_MASTER_WRAP_SV

module axi_master_wrap #(
    parameter int ID_WIDTH   = 8,
    parameter int ADDR_WIDTH = 64,
    parameter int DATA_WIDTH = 256
) (
    input  logic    clk_i,
    input  logic    rst_ni,
    axi_intf.master axi_o
);

    // -------------------------------------------------------------------------
    // DPI imports — 3-step pattern per spec §5.1
    // -------------------------------------------------------------------------

    import "DPI-C" context function void cmodel_master_set_inputs(
        input  bit                  awready,
        input  bit                  wready,
        input  bit                  arready,
        input  bit                  bvalid,
        input  bit [ID_WIDTH-1:0]   bid,
        input  bit [1:0]            bresp,
        input  bit                  rvalid,
        input  bit [ID_WIDTH-1:0]   rid,
        input  bit [DATA_WIDTH-1:0] rdata,
        input  bit [1:0]            rresp,
        input  bit                  rlast
    );

    import "DPI-C" context function void cmodel_master_tick();

    import "DPI-C" context function void cmodel_master_get_outputs(
        output bit                  awvalid,
        output bit [ID_WIDTH-1:0]   awid,
        output bit [ADDR_WIDTH-1:0] awaddr,
        output bit [7:0]            awlen,
        output bit [2:0]            awsize,
        output bit [1:0]            awburst,
        output bit                  awlock,
        output bit [3:0]            awcache,
        output bit [2:0]            awprot,
        output bit [3:0]            awqos,
        output bit                  wvalid,
        output bit [DATA_WIDTH-1:0] wdata,
        output bit [DATA_WIDTH/8-1:0] wstrb,
        output bit                  wlast,
        output bit                  bready,
        output bit                  arvalid,
        output bit [ID_WIDTH-1:0]   arid,
        output bit [ADDR_WIDTH-1:0] araddr,
        output bit [7:0]            arlen,
        output bit [2:0]            arsize,
        output bit [1:0]            arburst,
        output bit                  arlock,
        output bit [3:0]            arcache,
        output bit [2:0]            arprot,
        output bit [3:0]            arqos,
        output bit                  rready
    );

    // Lifecycle / error polling (shared with all shells).
    import "DPI-C" context function int  cmodel_check_error(output string msg);
    import "DPI-C" context function void cmodel_finalize();

    // -------------------------------------------------------------------------
    // Output registers (beta-tick: registered one cycle behind DPI sample)
    // -------------------------------------------------------------------------

    bit                    awvalid_q;
    bit [ID_WIDTH-1:0]     awid_q;
    bit [ADDR_WIDTH-1:0]   awaddr_q;
    bit [7:0]              awlen_q;
    bit [2:0]              awsize_q;
    bit [1:0]              awburst_q;
    bit                    awlock_q;
    bit [3:0]              awcache_q;
    bit [2:0]              awprot_q;
    bit [3:0]              awqos_q;

    bit                    wvalid_q;
    bit [DATA_WIDTH-1:0]   wdata_q;
    bit [DATA_WIDTH/8-1:0] wstrb_q;
    bit                    wlast_q;

    bit                    bready_q;

    bit                    arvalid_q;
    bit [ID_WIDTH-1:0]     arid_q;
    bit [ADDR_WIDTH-1:0]   araddr_q;
    bit [7:0]              arlen_q;
    bit [2:0]              arsize_q;
    bit [1:0]              arburst_q;
    bit                    arlock_q;
    bit [3:0]              arcache_q;
    bit [2:0]              arprot_q;
    bit [3:0]              arqos_q;

    bit                    rready_q;

    // -------------------------------------------------------------------------
    // always_ff: sync-reset, 3-step DPI call, registered outputs, error check
    // -------------------------------------------------------------------------

    always_ff @(posedge clk_i) begin
        if (!rst_ni) begin
            awvalid_q <= '0;
            awid_q    <= '0;
            awaddr_q  <= '0;
            awlen_q   <= '0;
            awsize_q  <= '0;
            awburst_q <= '0;
            awlock_q  <= '0;
            awcache_q <= '0;
            awprot_q  <= '0;
            awqos_q   <= '0;
            wvalid_q  <= '0;
            wdata_q   <= '0;
            wstrb_q   <= '0;
            wlast_q   <= '0;
            bready_q  <= '0;
            arvalid_q <= '0;
            arid_q    <= '0;
            araddr_q  <= '0;
            arlen_q   <= '0;
            arsize_q  <= '0;
            arburst_q <= '0;
            arlock_q  <= '0;
            arcache_q <= '0;
            arprot_q  <= '0;
            arqos_q   <= '0;
            rready_q  <= '0;
        end else begin
            // Step 1: push current slave-side wire values into C++ input latch.
            cmodel_master_set_inputs(
                axi_o.awready,
                axi_o.wready,
                axi_o.arready,
                axi_o.bvalid,
                axi_o.bid,
                axi_o.bresp,
                axi_o.rvalid,
                axi_o.rid,
                axi_o.rdata,
                axi_o.rresp,
                axi_o.rlast
            );

            // Step 2: advance C++ model one cycle.
            cmodel_master_tick();

            // Step 3: pull outputs from C++ model into registered locals.
            cmodel_master_get_outputs(
                awvalid_q, awid_q, awaddr_q,
                awlen_q,   awsize_q, awburst_q,
                awlock_q,  awcache_q, awprot_q, awqos_q,
                wvalid_q,  wdata_q,  wstrb_q,  wlast_q,
                bready_q,
                arvalid_q, arid_q,  araddr_q,
                arlen_q,   arsize_q, arburst_q,
                arlock_q,  arcache_q, arprot_q, arqos_q,
                rready_q
            );

            // Inline error check (spec §7.5): poll after every tick.
            begin : error_check
                string err_msg;
                int    err_code;
                err_code = cmodel_check_error(err_msg);
                if (err_code != 0) begin
                    $display("[axi_master_wrap] DPI fatal (code=%0d): %s",
                             err_code, err_msg);
                    cmodel_finalize();
                    $fatal(1, "axi_master_wrap: DPI error, simulation aborted");
                end
            end
        end
    end

    // -------------------------------------------------------------------------
    // Drive interface outputs from registered state
    // -------------------------------------------------------------------------

    assign axi_o.awvalid = awvalid_q;
    assign axi_o.awid    = awid_q;
    assign axi_o.awaddr  = awaddr_q;
    assign axi_o.awlen   = awlen_q;
    assign axi_o.awsize  = awsize_q;
    assign axi_o.awburst = awburst_q;
    assign axi_o.awlock  = awlock_q;
    assign axi_o.awcache = awcache_q;
    assign axi_o.awprot  = awprot_q;
    assign axi_o.awqos   = awqos_q;

    assign axi_o.wvalid  = wvalid_q;
    assign axi_o.wdata   = wdata_q;
    assign axi_o.wstrb   = wstrb_q;
    assign axi_o.wlast   = wlast_q;

    assign axi_o.bready  = bready_q;

    assign axi_o.arvalid = arvalid_q;
    assign axi_o.arid    = arid_q;
    assign axi_o.araddr  = araddr_q;
    assign axi_o.arlen   = arlen_q;
    assign axi_o.arsize  = arsize_q;
    assign axi_o.arburst = arburst_q;
    assign axi_o.arlock  = arlock_q;
    assign axi_o.arcache = arcache_q;
    assign axi_o.arprot  = arprot_q;
    assign axi_o.arqos   = arqos_q;

    assign axi_o.rready  = rready_q;

endmodule

`endif  // AXI_MASTER_WRAP_SV
