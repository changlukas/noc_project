// nmu_wrap — DPI wrapper for the Nmu component, three physical networks (S3a T5).
//
// The Nmu is the most complex wrap — it has BOTH an AXI slave side
// (incoming AW/W/AR from master, outgoing B/R + handshake to master) AND
// three NoC faces: REQ egress (ready/valid), RSP ingress (ready/valid), DAT
// ingress+egress (credit, unchanged mechanism). Registered-DPI-tick
// discipline and error checking follow the same pattern as every wrap in
// this directory.
//
// Registered-DPI-tick discipline: on every posedge clk_i the module
// samples the PREVIOUS cycle's registered wire inputs, pushes them to C++
// via cmodel_nmu_set_inputs, advances the model via cmodel_nmu_tick, pulls
// outputs via cmodel_nmu_get_outputs, then registers those outputs nonblocking
// so they are visible to SV wires from the NEXT cycle onward.
//
// Reset: synchronous active-low (rst_ni). Output registers cleared on reset.
// No async reset path — sync reset is the project default.
//
// Error polling is centralized in tb_top.sv; this wrap no longer
// calls cmodel_check_error/cmodel_finalize itself.
//
// AXI struct ports (slave view): slave reads axi_req_i (AW/W/AR + bready/
//   rready); slave drives axi_rsp_o (awready/wready/arready + B/R).
// NoC ports, node's own view (spec §4.3, S3a T5 mechanical rename):
//   tx_req_valid_o/tx_req_flit_o : Nmu drives req flit toward the router (egress).
//   tx_req_ready_i               : router's readiness for tx_req (input).
//   rx_rsp_valid_i/rx_rsp_flit_i : router drives rsp flit toward Nmu (ingress).
//   rx_rsp_ready_o               : Nmu's readiness, returned to the router.
//     Tied constant true — the c_model's ingress queue is unbounded
//     (Depacketize always drains what's injected); see nmu_wrap.hpp.
//   tx_dat_valid_o/tx_dat_flit_o : Nmu drives DAT (AW/W) flit toward the router.
//   tx_dat_crdvalid_i            : credit pulse/VC from the router for sent DAT flits.
//   rx_dat_valid_i/rx_dat_flit_i : router drives DAT (R) flit toward Nmu.
//   rx_dat_crdvalid_o            : credit pulse/VC Nmu returns for consumed DAT flits.

`timescale 1ns/1ps

`ifndef NMU_WRAP_SV
`define NMU_WRAP_SV

module nmu_wrap #(
    parameter int unsigned ID_WIDTH       = ni_params_pkg::AXI_ID_WIDTH_DFLT,
    parameter int unsigned ADDR_WIDTH     = ni_params_pkg::AXI_ADDR_WIDTH_DFLT,
    parameter int unsigned DATA_WIDTH     = ni_params_pkg::AXI_DATA_WIDTH_DFLT,
    parameter int unsigned DAT_NUM_VC     = ni_params_pkg::NOC_DAT_NUM_VC_DFLT,
    parameter int unsigned REQ_FLIT_WIDTH = ni_params_pkg::NOC_REQ_FLIT_WIDTH_DFLT,
    parameter int unsigned RSP_FLIT_WIDTH = ni_params_pkg::NOC_RSP_FLIT_WIDTH_DFLT,
    parameter int unsigned DAT_FLIT_WIDTH = ni_params_pkg::NOC_DAT_FLIT_WIDTH_DFLT,
    // AWUSER width (docs/noc-target-spec.md §6: [7:0] user, [9:8] collective_op,
    // [57:10] collective address mask). Dedicated port beside axi_req_i: the
    // generated axi_req_t struct has no awuser field.
    parameter int unsigned AWUSER_WIDTH   = ni_params_pkg::AXI_AWUSER_WIDTH_DFLT
) (
    input  logic              clk_i,
    input  logic              rst_ni,
    input  longint unsigned            ctx_i,
    input  ni_signals_pkg::axi_req_t   axi_req_i,
    input  logic [AWUSER_WIDTH-1:0]    awuser_i,
    output ni_signals_pkg::axi_rsp_t   axi_rsp_o,

    // REQ face (egress, ready/valid).
    output logic                      tx_req_valid_o,
    output logic [REQ_FLIT_WIDTH-1:0] tx_req_flit_o,
    input  logic                      tx_req_ready_i,

    // RSP face (ingress, ready/valid).
    input  logic                      rx_rsp_valid_i,
    input  logic [RSP_FLIT_WIDTH-1:0] rx_rsp_flit_i,
    output logic                      rx_rsp_ready_o,

    // DAT face (both directions, credit).
    output logic                  tx_dat_valid_o,
    output logic [DAT_FLIT_WIDTH-1:0] tx_dat_flit_o,
    input  logic [DAT_NUM_VC-1:0]     tx_dat_crdvalid_i,
    input  logic                      rx_dat_valid_i,
    input  logic [DAT_FLIT_WIDTH-1:0] rx_dat_flit_i,
    output logic [DAT_NUM_VC-1:0]     rx_dat_crdvalid_o
);

    // -------------------------------------------------------------------------
    // DPI imports — 3-step pattern (set_inputs/tick/get_outputs)
    // -------------------------------------------------------------------------

    import "DPI-C" context function void cmodel_nmu_set_inputs(
        input  longint unsigned                ctx,
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
        input  bit [AWUSER_WIDTH-1:0] awuser,
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
        input  bit                    tx_req_ready,
        input  bit                    rx_rsp_valid,
        input  bit [RSP_FLIT_WIDTH-1:0] rx_rsp_flit,
        input  bit                    rx_dat_valid,
        input  bit [DAT_FLIT_WIDTH-1:0] rx_dat_flit,
        input  bit [DAT_NUM_VC-1:0]   tx_dat_crdvalid
    );

    import "DPI-C" context function void cmodel_nmu_tick(
        input  longint unsigned                ctx
    );

    import "DPI-C" context function void cmodel_nmu_get_outputs(
        input  longint unsigned                ctx,
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
        output bit                    tx_req_valid,
        output bit [REQ_FLIT_WIDTH-1:0] tx_req_flit,
        output bit                    rx_rsp_ready,
        output bit                    tx_dat_valid,
        output bit [DAT_FLIT_WIDTH-1:0] tx_dat_flit,
        output bit [DAT_NUM_VC-1:0]   rx_dat_crdvalid
    );

    // Lifecycle / error polling lives in tb_top.sv.

    // -------------------------------------------------------------------------
    // Output registers (registered one cycle behind DPI sample)
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

    bit                    tx_req_valid_q;
    bit [REQ_FLIT_WIDTH-1:0] tx_req_flit_q;
    logic                  tx_req_model_ready;
    bit                    rx_rsp_ready_q;
    bit                    tx_dat_valid_q;
    bit [DAT_FLIT_WIDTH-1:0] tx_dat_flit_q;
    bit [DAT_NUM_VC-1:0]     rx_dat_crdvalid_q;

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
            tx_req_valid_q         <= '0;
            tx_req_flit_q          <= '0;
            rx_rsp_ready_q         <= '0;
            tx_dat_valid_q         <= '0;
            tx_dat_flit_q          <= '0;
            rx_dat_crdvalid_q      <= '0;
        end else begin
            // Step 1: push current wire values into C++ input latch.
            cmodel_nmu_set_inputs(
                ctx_i,
                // AXI slave side — master drives these
                axi_req_i.awvalid,
                axi_req_i.awid,
                axi_req_i.awaddr,
                axi_req_i.awlen,
                axi_req_i.awsize,
                axi_req_i.awburst,
                axi_req_i.awlock,
                axi_req_i.awcache,
                axi_req_i.awprot,
                axi_req_i.awqos,
                awuser_i,
                axi_req_i.wvalid,
                axi_req_i.wdata,
                axi_req_i.wstrb,
                axi_req_i.wlast,
                axi_req_i.bready,
                axi_req_i.arvalid,
                axi_req_i.arid,
                axi_req_i.araddr,
                axi_req_i.arlen,
                axi_req_i.arsize,
                axi_req_i.arburst,
                axi_req_i.arlock,
                axi_req_i.arcache,
                axi_req_i.arprot,
                axi_req_i.arqos,
                axi_req_i.rready,
                // REQ egress ready, RSP ingress, DAT both directions
                tx_req_model_ready,
                rx_rsp_valid_i,
                rx_rsp_flit_i,
                rx_dat_valid_i,
                rx_dat_flit_i,
                tx_dat_crdvalid_i
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
                bit                    t_tx_req_valid;
                bit [REQ_FLIT_WIDTH-1:0] t_tx_req_flit;
                bit                    t_rx_rsp_ready;
                bit                    t_tx_dat_valid;
                bit [DAT_FLIT_WIDTH-1:0] t_tx_dat_flit;
                bit [DAT_NUM_VC-1:0]     t_rx_dat_crdvalid;
                cmodel_nmu_get_outputs(
                    ctx_i,
                    t_awready, t_wready, t_arready,
                    t_bvalid,  t_bid,    t_bresp,
                    t_rvalid,  t_rid,    t_rdata,  t_rresp, t_rlast,
                    t_tx_req_valid, t_tx_req_flit,
                    t_rx_rsp_ready,
                    t_tx_dat_valid, t_tx_dat_flit,
                    t_rx_dat_crdvalid
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
                // REQ egress hold register: the strobe loads it, the wire
                // handshake frees it; a strobe may land on the freeing edge
                // (load wins, the old flit was consumed at that edge). See
                // router_wrap.sv's identical staging.
                if (t_tx_req_valid) begin
                    tx_req_valid_q <= 1'b1;
                    tx_req_flit_q  <= t_tx_req_flit;
                end else if (tx_req_ready_i) begin
                    tx_req_valid_q <= 1'b0;
                end
                rx_rsp_ready_q          <= t_rx_rsp_ready;
                tx_dat_valid_q          <= t_tx_dat_valid;
                tx_dat_flit_q           <= t_tx_dat_flit;
                rx_dat_crdvalid_q       <= t_rx_dat_crdvalid;
            end
        end
    end

    // -------------------------------------------------------------------------
    // Drive interface outputs from registered state
    // -------------------------------------------------------------------------

    // AXI slave side — Nmu drives handshake + response channels
    assign axi_rsp_o.awready = awready_q;
    assign axi_rsp_o.wready  = wready_q;
    assign axi_rsp_o.arready = arready_q;

    assign axi_rsp_o.bvalid  = bvalid_q;
    assign axi_rsp_o.bid     = bid_q;
    assign axi_rsp_o.bresp   = bresp_q;

    assign axi_rsp_o.rvalid  = rvalid_q;
    assign axi_rsp_o.rid     = rid_q;
    assign axi_rsp_o.rdata   = rdata_q;
    assign axi_rsp_o.rresp   = rresp_q;
    assign axi_rsp_o.rlast   = rlast_q;

    // The C++ model emits a one-cycle REQ strobe after sampling ready; the
    // hold register above owns the RTL-facing held-valid contract, and the
    // model may pop when it is empty or its flit's handshake completes this
    // cycle (full rate, single stage).
    assign tx_req_model_ready = !tx_req_valid_q || tx_req_ready_i;
    assign tx_req_valid_o = tx_req_valid_q;
    assign tx_req_flit_o  = tx_req_flit_q;

    // Egress-hold checker, explicit sampled-history form (SVA $stable in a
    // |=> consequent false-fires under Verilator on the first backpressured
    // cycle — see router_wrap.sv).
    logic chk_req_v_q, chk_req_r_q;
    logic [REQ_FLIT_WIDTH-1:0] chk_req_flit_q;
    always_ff @(posedge clk_i) begin
        chk_req_v_q    <= rst_ni && tx_req_valid_o;
        chk_req_r_q    <= tx_req_ready_i;
        chk_req_flit_q <= tx_req_flit_o;
        if (rst_ni && chk_req_v_q && !chk_req_r_q &&
            (!tx_req_valid_o || tx_req_flit_o !== chk_req_flit_q))
            $error("nmu_wrap: REQ changed before valid/ready handshake");
    end

    assign rx_rsp_ready_o    = rx_rsp_ready_q;
    assign tx_dat_valid_o    = tx_dat_valid_q;
    assign tx_dat_flit_o     = tx_dat_flit_q;
    assign rx_dat_crdvalid_o = rx_dat_crdvalid_q;

endmodule

`endif  // NMU_WRAP_SV
