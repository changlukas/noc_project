// nmu_wrap — Stage 5b DPI shell for the Nmu component.
//
// The Nmu is the most complex shell — it has BOTH an AXI slave side
// (incoming AW/W/AR from master, outgoing B/R + handshake to master) AND
// a NoC side: noc_mosi_o (noc_intf.mosi modport) drives req flit + valid
// toward ChannelModel and drives rsp_credit_return back; it reads rsp
// flit + valid + req_credit_return from ChannelModel. Beta-tick discipline
// and error checking follow the same pattern as axi_slave_wrap (T9) and
// channel_model_wrap (T7).
//
// Beta-tick discipline (spec §5.1): on every posedge clk_i the module
// samples the PREVIOUS cycle's registered wire inputs, pushes them to C++
// via cmodel_nmu_set_inputs, advances the model via cmodel_nmu_tick, pulls
// outputs via cmodel_nmu_get_outputs, then registers those outputs nonblocking
// so they are visible to SV wires from the NEXT cycle onward.
//
// FLIT_WIDTH must match ni_params_pkg::NI_NOC_FLIT_WIDTH_DFLT = 408. The
// noc_intf FLIT_WIDTH parameter is overridden at instantiation in tb_top.sv.
//
// Reset: synchronous active-low (rst_ni). Output registers cleared on reset.
// No async reset path — sync reset is the project default per rtl-style.
//
// Error polling is centralized in tb_top.sv (T1.4); this wrap no longer
// calls cmodel_check_error/cmodel_finalize itself.
//
// axi4_intf.slave modport: slave reads AW/W/AR + bready/rready from axi_i;
//                          slave drives awready/wready/arready + B/R to axi_i.
// noc_intf.mosi modport:   Nmu drives req_valid/req_flit + rsp_credit_return;
//                          Nmu reads req_credit_return + rsp_valid/rsp_flit.

`timescale 1ns/1ps

`ifndef NMU_WRAP_SV
`define NMU_WRAP_SV

module nmu_wrap #(
    parameter int unsigned ID_WIDTH              = ni_params_pkg::NI_AXI_ID_WIDTH_DFLT,
    parameter int unsigned ADDR_WIDTH            = ni_params_pkg::NI_AXI_ADDR_WIDTH_DFLT,
    parameter int unsigned DATA_WIDTH            = ni_params_pkg::NI_AXI_DATA_WIDTH_DFLT,
    parameter int unsigned NUM_VC                = ni_params_pkg::NI_NOC_NUM_VC_DFLT,
    parameter int unsigned FLIT_WIDTH            = ni_params_pkg::NI_NOC_FLIT_WIDTH_DFLT,
    parameter int unsigned SLAVE_VC_BUFFER_DEPTH = ni_params_pkg::NI_NOC_SLAVE_VC_BUFFER_DEPTH_DFLT
) (
    input  logic              clk_i,
    input  logic              rst_ni,
    input  chandle            ctx_i,
    axi4_intf.slave           axi_i,
    noc_intf.mosi             noc_mosi_o
);

    // -------------------------------------------------------------------------
    // PoC scope guard (T1.3): single-VC only
    // -------------------------------------------------------------------------
    // c_model + DPI marshalling assume single-VC. Multi-VC support requires
    // plumbing per-VC credit_return through DPI; until then, fail elaboration
    // if NUM_VC > 1 instead of silently reducing credit_return to bit 0.
    initial begin
        if (NUM_VC != 1) begin
            $fatal(1, "%m: NUM_VC=%0d; PoC supports NUM_VC=1 only", NUM_VC);
        end
    end

    // -------------------------------------------------------------------------
    // DPI imports — 3-step pattern per spec §5.1
    // -------------------------------------------------------------------------

    import "DPI-C" context function void cmodel_nmu_set_inputs(
        input  chandle                ctx,
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
        input  bit [FLIT_WIDTH-1:0]   noc_rsp_flit,
        input  bit                    noc_req_credit_return
    );

    import "DPI-C" context function void cmodel_nmu_tick(
        input  chandle                ctx
    );

    import "DPI-C" context function void cmodel_nmu_get_outputs(
        input  chandle                ctx,
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
        output bit [FLIT_WIDTH-1:0]   noc_req_flit,
        output bit                    noc_rsp_credit_return
    );

    // Lifecycle / error polling lives in tb_top.sv (T1.4).

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

    // NoC req side outputs (Nmu drives toward ChannelModel)
    bit                    noc_req_valid_q;
    bit [FLIT_WIDTH-1:0]   noc_req_flit_q;

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
                ctx_i,
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
                // NoC rsp side — rsp flit arriving from ChannelModel toward Nmu
                noc_mosi_o.rsp_valid,
                noc_mosi_o.rsp_flit,
                // NoC req credit — ChannelModel returns credit to Nmu
                noc_mosi_o.req_credit_return[0]
            );

            // Step 2: advance C++ model one cycle.
            cmodel_nmu_tick(ctx_i);

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
                bit                    t_noc_req_valid;
                bit [FLIT_WIDTH-1:0]   t_noc_req_flit;
                bit                    t_noc_rsp_credit_return;
                cmodel_nmu_get_outputs(
                    ctx_i,
                    t_awready, t_wready, t_arready,
                    t_bvalid,  t_bid,    t_bresp,
                    t_rvalid,  t_rid,    t_rdata,  t_rresp, t_rlast,
                    t_noc_req_valid, t_noc_req_flit,
                    t_noc_rsp_credit_return
                );
                awready_q               <= t_awready;
                wready_q                <= t_wready;
                arready_q               <= t_arready;
                bvalid_q                <= t_bvalid;
                bid_q                   <= t_bid;
                bresp_q                 <= t_bresp;
                rvalid_q                <= t_rvalid;
                rid_q                   <= t_rid;
                rdata_q                 <= t_rdata;
                rresp_q                 <= t_rresp;
                rlast_q                 <= t_rlast;
                noc_req_valid_q         <= t_noc_req_valid;
                noc_req_flit_q          <= t_noc_req_flit;
                noc_rsp_credit_return_q <= t_noc_rsp_credit_return;
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

    // NoC req side — Nmu drives req flit toward ChannelModel
    assign noc_mosi_o.req_valid = noc_req_valid_q;
    assign noc_mosi_o.req_flit  = noc_req_flit_q;

    // NoC rsp credit — Nmu drives rsp_credit_return back upstream (PoC always 0)
    assign noc_mosi_o.rsp_credit_return = {NUM_VC{noc_rsp_credit_return_q}};

endmodule

`endif  // NMU_WRAP_SV
