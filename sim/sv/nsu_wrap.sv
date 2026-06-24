// nsu_wrap — Stage 5b DPI wrapper for the Nsu component.
//
// The Nsu is the NoC-side inverse of nmu_wrap. Its single NoC port
// noc_miso_i (noc_intf.miso modport) reads req flit + valid + rsp_credit_return
// from ChannelModel and drives rsp flit + valid + req_credit_return back.
// On the AXI side it acts as master toward the downstream subordinate.
//
// Beta-tick discipline (spec §5.1): on every posedge clk_i the module
// samples the PREVIOUS cycle's registered wire inputs, pushes them to C++
// via cmodel_nsu_set_inputs, advances the model via cmodel_nsu_tick, pulls
// outputs via cmodel_nsu_get_outputs, then registers those outputs nonblocking
// so they are visible to SV wires from the NEXT cycle onward.
//
// FLIT_WIDTH must match ni_params_pkg::NOC_FLIT_WIDTH_DFLT = 408. The
// noc_intf FLIT_WIDTH parameter is overridden at instantiation in tb_top.sv.
//
// Reset: synchronous active-low (rst_ni). Output registers cleared on reset.
// No async reset path — sync reset is the project default per rtl-style.
//
// Error polling is centralized in tb_top.sv (T1.4); this wrap no longer
// calls cmodel_check_error/cmodel_finalize itself.
//
// axi4_intf.master modport: master drives AW/W/AR + bready/rready to axi_o;
//                           master reads awready/wready/arready + B/R from axi_o.
// noc_intf.miso modport:   Nsu reads req_valid/req_flit + rsp_credit_return;
//                          Nsu drives req_credit_return + rsp_valid/rsp_flit.

`timescale 1ns/1ps

`ifndef NSU_WRAP_SV
`define NSU_WRAP_SV

module nsu_wrap #(
    parameter int unsigned ID_WIDTH              = ni_params_pkg::AXI_ID_WIDTH_DFLT,
    parameter int unsigned ADDR_WIDTH            = ni_params_pkg::AXI_ADDR_WIDTH_DFLT,
    parameter int unsigned DATA_WIDTH            = ni_params_pkg::AXI_DATA_WIDTH_DFLT,
    parameter int unsigned NUM_VC                = ni_params_pkg::NOC_NUM_VC_DFLT,
    parameter int unsigned FLIT_WIDTH            = ni_params_pkg::NOC_FLIT_WIDTH_DFLT,
    parameter int unsigned SLAVE_VC_BUFFER_DEPTH = ni_params_pkg::NOC_SLAVE_VC_BUFFER_DEPTH_DFLT
) (
    input  logic              clk_i,
    input  logic              rst_ni,
    input  longint unsigned            ctx_i,
    noc_intf.miso             noc_miso_i,
    axi4_intf.master          axi_o
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

    import "DPI-C" context function void cmodel_nsu_set_inputs(
        input  longint unsigned                ctx,
        input  bit                    noc_req_valid,
        input  bit [FLIT_WIDTH-1:0]       noc_req_flit,
        input  bit                    noc_rsp_credit_return,
        input  bit                    awready,
        input  bit                    wready,
        input  bit                    bvalid,
        input  bit [ID_WIDTH-1:0]     bid,
        input  bit [1:0]              bresp,
        input  bit                    arready,
        input  bit                    rvalid,
        input  bit [ID_WIDTH-1:0]     rid,
        input  bit [DATA_WIDTH-1:0]   rdata,
        input  bit [1:0]              rresp,
        input  bit                    rlast
    );

    import "DPI-C" context function void cmodel_nsu_tick(
        input  longint unsigned                ctx
    );

    import "DPI-C" context function void cmodel_nsu_get_outputs(
        input  longint unsigned                ctx,
        output bit                    noc_rsp_valid,
        output bit [FLIT_WIDTH-1:0]       noc_rsp_flit,
        output bit                    noc_req_credit_return,
        output bit                    awvalid,
        output bit [ID_WIDTH-1:0]     awid,
        output bit [ADDR_WIDTH-1:0]   awaddr,
        output bit [7:0]              awlen,
        output bit [2:0]              awsize,
        output bit [1:0]              awburst,
        output bit                    awlock,
        output bit [3:0]              awcache,
        output bit [2:0]              awprot,
        output bit [3:0]              awqos,
        output bit                    wvalid,
        output bit [DATA_WIDTH-1:0]   wdata,
        output bit [DATA_WIDTH/8-1:0] wstrb,
        output bit                    wlast,
        output bit                    bready,
        output bit                    arvalid,
        output bit [ID_WIDTH-1:0]     arid,
        output bit [ADDR_WIDTH-1:0]   araddr,
        output bit [7:0]              arlen,
        output bit [2:0]              arsize,
        output bit [1:0]              arburst,
        output bit                    arlock,
        output bit [3:0]              arcache,
        output bit [2:0]              arprot,
        output bit [3:0]              arqos,
        output bit                    rready
    );

    // Lifecycle / error polling lives in tb_top.sv (T1.4).

    // -------------------------------------------------------------------------
    // Output registers (beta-tick: registered one cycle behind DPI sample)
    // -------------------------------------------------------------------------

    // NoC rsp side outputs (Nsu drives toward ChannelModel)
    bit                    noc_rsp_valid_q;
    bit [FLIT_WIDTH-1:0]       noc_rsp_flit_q;

    // NoC req credit return (Nsu drives back upstream; PoC always 0)
    bit                    noc_req_credit_return_q;

    // AXI master side outputs (Nsu drives toward subordinate)
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
            noc_rsp_valid_q           <= '0;
            noc_rsp_flit_q            <= '0;
            noc_req_credit_return_q   <= '0;
            awvalid_q                 <= '0;
            awid_q                    <= '0;
            awaddr_q                  <= '0;
            awlen_q                   <= '0;
            awsize_q                  <= '0;
            awburst_q                 <= '0;
            awlock_q                  <= '0;
            awcache_q                 <= '0;
            awprot_q                  <= '0;
            awqos_q                   <= '0;
            wvalid_q                  <= '0;
            wdata_q                   <= '0;
            wstrb_q                   <= '0;
            wlast_q                   <= '0;
            bready_q                  <= '0;
            arvalid_q                 <= '0;
            arid_q                    <= '0;
            araddr_q                  <= '0;
            arlen_q                   <= '0;
            arsize_q                  <= '0;
            arburst_q                 <= '0;
            arlock_q                  <= '0;
            arcache_q                 <= '0;
            arprot_q                  <= '0;
            arqos_q                   <= '0;
            rready_q                  <= '0;
        end else begin
            // Step 1: push current wire values into C++ input latch.
            cmodel_nsu_set_inputs(
                ctx_i,
                // NoC req side — req flit arriving from ChannelModel toward Nsu
                noc_miso_i.req_valid,
                noc_miso_i.req_flit,
                // NoC rsp credit — ChannelModel returns credit to Nsu
                noc_miso_i.rsp_credit_return[0],
                // AXI master side — subordinate drives ready + B/R
                axi_o.awready,
                axi_o.wready,
                axi_o.bvalid,
                axi_o.bid,
                axi_o.bresp,
                axi_o.arready,
                axi_o.rvalid,
                axi_o.rid,
                axi_o.rdata,
                axi_o.rresp,
                axi_o.rlast
            );

            // Step 2: advance C++ model one cycle.
            cmodel_nsu_tick(ctx_i);

            // Step 3: pull outputs into local temporaries (blocking to locals is
            // safe; avoids BLKANDNBLK with the nonblocking reset path above).
            begin : get_outputs_blk
                bit                    t_noc_rsp_valid;
                bit [FLIT_WIDTH-1:0]       t_noc_rsp_flit;
                bit                    t_noc_req_credit_return;
                bit                    t_awvalid;
                bit [ID_WIDTH-1:0]     t_awid;
                bit [ADDR_WIDTH-1:0]   t_awaddr;
                bit [7:0]              t_awlen;
                bit [2:0]              t_awsize;
                bit [1:0]              t_awburst;
                bit                    t_awlock;
                bit [3:0]              t_awcache;
                bit [2:0]              t_awprot;
                bit [3:0]              t_awqos;
                bit                    t_wvalid;
                bit [DATA_WIDTH-1:0]   t_wdata;
                bit [DATA_WIDTH/8-1:0] t_wstrb;
                bit                    t_wlast;
                bit                    t_bready;
                bit                    t_arvalid;
                bit [ID_WIDTH-1:0]     t_arid;
                bit [ADDR_WIDTH-1:0]   t_araddr;
                bit [7:0]              t_arlen;
                bit [2:0]              t_arsize;
                bit [1:0]              t_arburst;
                bit                    t_arlock;
                bit [3:0]              t_arcache;
                bit [2:0]              t_arprot;
                bit [3:0]              t_arqos;
                bit                    t_rready;
                cmodel_nsu_get_outputs(
                    ctx_i,
                    t_noc_rsp_valid, t_noc_rsp_flit, t_noc_req_credit_return,
                    t_awvalid, t_awid, t_awaddr, t_awlen, t_awsize, t_awburst,
                    t_awlock, t_awcache, t_awprot, t_awqos,
                    t_wvalid, t_wdata, t_wstrb, t_wlast,
                    t_bready,
                    t_arvalid, t_arid, t_araddr, t_arlen, t_arsize, t_arburst,
                    t_arlock, t_arcache, t_arprot, t_arqos,
                    t_rready
                );
                noc_rsp_valid_q         <= t_noc_rsp_valid;
                noc_rsp_flit_q          <= t_noc_rsp_flit;
                noc_req_credit_return_q <= t_noc_req_credit_return;
                awvalid_q               <= t_awvalid;
                awid_q                  <= t_awid;
                awaddr_q                <= t_awaddr;
                awlen_q                 <= t_awlen;
                awsize_q                <= t_awsize;
                awburst_q               <= t_awburst;
                awlock_q                <= t_awlock;
                awcache_q               <= t_awcache;
                awprot_q                <= t_awprot;
                awqos_q                 <= t_awqos;
                wvalid_q                <= t_wvalid;
                wdata_q                 <= t_wdata;
                wstrb_q                 <= t_wstrb;
                wlast_q                 <= t_wlast;
                bready_q                <= t_bready;
                arvalid_q               <= t_arvalid;
                arid_q                  <= t_arid;
                araddr_q                <= t_araddr;
                arlen_q                 <= t_arlen;
                arsize_q                <= t_arsize;
                arburst_q               <= t_arburst;
                arlock_q                <= t_arlock;
                arcache_q               <= t_arcache;
                arprot_q                <= t_arprot;
                arqos_q                 <= t_arqos;
                rready_q                <= t_rready;
            end
        end
    end

    // -------------------------------------------------------------------------
    // Drive interface outputs from registered state
    // -------------------------------------------------------------------------

    // NoC rsp side — Nsu drives rsp flit toward ChannelModel
    assign noc_miso_i.rsp_valid = noc_rsp_valid_q;
    assign noc_miso_i.rsp_flit  = noc_rsp_flit_q;

    // NoC req credit — Nsu drives req_credit_return back upstream (registered
    // FlooNoC consumer pulse from the C model; pure pass-through bit)
    assign noc_miso_i.req_credit_return = {NUM_VC{noc_req_credit_return_q}};

    // AXI master side — Nsu drives request channels toward subordinate
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

`endif  // NSU_WRAP_SV
