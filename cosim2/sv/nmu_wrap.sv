// nmu_wrap — Stage 5b DPI shell for the Nmu component.
//
// The Nmu is the most complex shell — it has BOTH an AXI slave side
// (incoming AW/W/AR from master, outgoing B/R + handshake to master) AND
// NoC sides (noc_req_o producer toward LoopbackNoc, noc_rsp_i consumer from
// LoopbackNoc). Beta-tick discipline and error checking follow the same
// pattern as axi_slave_wrap (T9) and loopback_noc_wrap (T7).
//
// Beta-tick discipline (spec §5.1): on every posedge clk_i the module
// samples the PREVIOUS cycle's registered wire inputs, pushes them to C++
// via cmodel_nmu_set_inputs, advances the model via cmodel_nmu_tick, pulls
// outputs via cmodel_nmu_get_outputs, then registers those outputs nonblocking
// so they are visible to SV wires from the NEXT cycle onward.
//
// FLIT_W must match ni::FLIT_WIDTH = 408. The noc_req_intf / noc_rsp_intf
// interface FLIT_W parameter is overridden at instantiation in tb_top.sv.
//
// Reset: synchronous active-low (rst_ni). Output registers cleared on reset.
// No async reset path — sync reset is the project default per rtl-style.
//
// Inline error check (spec §7.5): cmodel_check_error() called at end of
// every active always_ff body; non-zero triggers $fatal after cmodel_finalize.
//
// axi_intf.slave modport: slave reads AW/W/AR + bready/rready from axi_i;
//                         slave drives awready/wready/arready + B/R to axi_i.
// noc_req_intf.producer:  Nmu drives req flit + valid; reads credit_return.
// noc_rsp_intf.consumer:  Nmu reads rsp flit + valid; drives credit_return.

`ifndef NMU_WRAP_SV
`define NMU_WRAP_SV

module nmu_wrap #(
    parameter int ID_WIDTH   = 8,
    parameter int ADDR_WIDTH = 64,
    parameter int DATA_WIDTH = 256,
    parameter int NUM_VC     = 1,
    parameter int FLIT_W     = 408
) (
    input  logic              clk_i,
    input  logic              rst_ni,
    axi_intf.slave            axi_i,
    noc_req_intf.producer     noc_req_o,
    noc_rsp_intf.consumer     noc_rsp_i
);

    // -------------------------------------------------------------------------
    // DPI imports — 3-step pattern per spec §5.1
    // -------------------------------------------------------------------------

    import "DPI-C" context function void cmodel_nmu_set_inputs(
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
        input  bit                    bready,
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
        input  bit                    rready,
        input  bit                    noc_rsp_valid,
        input  bit [FLIT_W-1:0]       noc_rsp_flit,
        input  bit                    noc_req_credit_return
    );

    import "DPI-C" context function void cmodel_nmu_tick();

    import "DPI-C" context function void cmodel_nmu_get_outputs(
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
        output bit                    rlast,
        output bit                    noc_req_valid,
        output bit [FLIT_W-1:0]       noc_req_flit,
        output bit                    noc_rsp_credit_return
    );

    // Lifecycle / error polling (shared with all shells).
    import "DPI-C" context function int  cmodel_check_error(output string msg);
    import "DPI-C" context function void cmodel_finalize();

    // -------------------------------------------------------------------------
    // Output registers (beta-tick: registered one cycle behind DPI sample)
    // -------------------------------------------------------------------------

    // AXI slave side outputs (Nmu drives)
    bit                    awready_q;
    bit                    wready_q;
    bit                    arready_q;

    bit                    bvalid_q;
    bit [ID_WIDTH-1:0]     bid_q;
    bit [1:0]              bresp_q;

    bit                    rvalid_q;
    bit [ID_WIDTH-1:0]     rid_q;
    bit [DATA_WIDTH-1:0]   rdata_q;
    bit [1:0]              rresp_q;
    bit                    rlast_q;

    // NoC req side outputs (Nmu drives toward LoopbackNoc)
    bit                    noc_req_valid_q;
    bit [FLIT_W-1:0]       noc_req_flit_q;

    // NoC rsp credit return (Nmu drives back upstream; PoC always 0)
    bit                    noc_rsp_credit_return_q;

    // -------------------------------------------------------------------------
    // always_ff: sync-reset, 3-step DPI call, registered outputs, error check
    // -------------------------------------------------------------------------

    always_ff @(posedge clk_i) begin
        if (!rst_ni) begin
            awready_q              <= '0;
            wready_q               <= '0;
            arready_q              <= '0;
            bvalid_q               <= '0;
            bid_q                  <= '0;
            bresp_q                <= '0;
            rvalid_q               <= '0;
            rid_q                  <= '0;
            rdata_q                <= '0;
            rresp_q                <= '0;
            rlast_q                <= '0;
            noc_req_valid_q        <= '0;
            noc_req_flit_q         <= '0;
            noc_rsp_credit_return_q <= '0;
        end else begin
            // Step 1: push current wire values into C++ input latch.
            cmodel_nmu_set_inputs(
                // AXI slave side — master drives these
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
                axi_i.bready,
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
                axi_i.rready,
                // NoC rsp side — rsp flit arriving from LoopbackNoc toward Nmu
                noc_rsp_i.valid,
                noc_rsp_i.flit,
                // NoC req credit — LoopbackNoc returns credit to Nmu
                noc_req_o.credit_return[0]
            );

            // Step 2: advance C++ model one cycle.
            cmodel_nmu_tick();

            // Step 3: pull outputs from C++ model into registered locals.
            cmodel_nmu_get_outputs(
                awready_q, wready_q, arready_q,
                bvalid_q,  bid_q,    bresp_q,
                rvalid_q,  rid_q,    rdata_q,  rresp_q, rlast_q,
                noc_req_valid_q, noc_req_flit_q,
                noc_rsp_credit_return_q
            );

            // Inline error check (spec §7.5): poll after every tick.
            begin : error_check
                string err_msg;
                int    err_code;
                err_code = cmodel_check_error(err_msg);
                if (err_code != 0) begin
                    $display("[nmu_wrap] DPI fatal (code=%0d): %s", err_code, err_msg);
                    cmodel_finalize();
                    $fatal(1, "nmu_wrap: DPI error, simulation aborted");
                end
            end
        end
    end

    // -------------------------------------------------------------------------
    // Drive interface outputs from registered state
    // -------------------------------------------------------------------------

    // AXI slave side — Nmu drives handshake + response channels
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

    // NoC req side — Nmu drives req flit toward LoopbackNoc
    assign noc_req_o.valid = noc_req_valid_q;
    assign noc_req_o.flit  = noc_req_flit_q;

    // NoC rsp credit — Nmu drives credit_return back upstream (PoC always 0)
    assign noc_rsp_i.credit_return = {NUM_VC{noc_rsp_credit_return_q}};

endmodule

`endif  // NMU_WRAP_SV
