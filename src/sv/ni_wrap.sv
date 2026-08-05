// ni_wrap — Network Interface: bundles nmu_wrap + nsu_wrap + dat_merge_wrap
// for one mesh node, three physical networks (S3a T5).
//
// Ports:
//   master_axi_req_i/master_axi_rsp_o — NMU AXI slave face (struct): test master
//                    drives req (AW/W/AR) in; NMU drives rsp (awready/B/R) out.
//   slave_axi_req_o/slave_axi_rsp_i   — NSU AXI master face (struct): NSU drives
//                    req (AW/W/AR) out toward test slave; slave drives rsp in.
//   REQ (ready/valid, forwarded from nmu_wrap's egress):
//     tx_req_valid_o/tx_req_flit_o — NMU's transmit toward the router.
//     tx_req_ready_i               — router's readiness (input).
//   RSP (ready/valid, forwarded from nsu_wrap's egress):
//     tx_rsp_valid_o/tx_rsp_flit_o — NSU's transmit toward the router.
//     tx_rsp_ready_i               — router's readiness (input).
//   REQ ingress (forwarded to nsu_wrap):
//     rx_req_valid_i/rx_req_flit_i — router's transmit toward NSU.
//     rx_req_ready_o               — NSU's readiness, tied constant true.
//   RSP ingress (forwarded to nmu_wrap):
//     rx_rsp_valid_i/rx_rsp_flit_i — router's transmit toward NMU.
//     rx_rsp_ready_o               — NMU's readiness, tied constant true.
//   DAT (credit, routed through dat_merge_wrap -- spec §4.3's single LOCAL
//   rx/tx pair is shared by NMU's DataAw/DataW egress and NSU's DataR egress,
//   and by NSU's DataAw/DataW ingress and NMU's DataR ingress; see
//   dat_merge_wrap.hpp for why this needs a merge instead of a direct
//   connection like REQ/RSP have):
//     tx_dat_valid_o/tx_dat_flit_o — merged transmit toward the router.
//     tx_dat_crdvalid_i            — router's credit-return for our sends.
//     rx_dat_valid_i/rx_dat_flit_i — router's transmit (ejected LOCAL flit).
//     rx_dat_crdvalid_o            — our credit-return to the router.
//
// Does NO cmodel_*_create. The nmu_ctx_i/nsu_ctx_i/dat_merge_ctx_i DPI handles
// arrive as ports from tb_top. This file is COMMITTED (hand-written, reusable
// NoC infra); the generated fabric includes it but never regenerates it.

`timescale 1ns/1ps

`ifndef NI_WRAP_SV
`define NI_WRAP_SV

// nmu_wrap / nsu_wrap / dat_merge_wrap are provided by the filelist
// (build_config.mk TB_TOP_SV_SRC, listed before ni_wrap.sv); no in-file
// `include needed.

module ni_wrap #(
    parameter int unsigned ID_WIDTH       = ni_params_pkg::AXI_ID_WIDTH_DFLT,
    parameter int unsigned ADDR_WIDTH     = ni_params_pkg::AXI_ADDR_WIDTH_DFLT,
    parameter int unsigned DATA_WIDTH     = ni_params_pkg::AXI_DATA_WIDTH_DFLT,
    parameter int unsigned DAT_NUM_VC     = ni_params_pkg::NOC_DAT_NUM_VC_DFLT,
    parameter int unsigned REQ_FLIT_WIDTH = ni_params_pkg::NOC_REQ_FLIT_WIDTH_DFLT,
    parameter int unsigned RSP_FLIT_WIDTH = ni_params_pkg::NOC_RSP_FLIT_WIDTH_DFLT,
    parameter int unsigned DAT_FLIT_WIDTH = ni_params_pkg::NOC_DAT_FLIT_WIDTH_DFLT
) (
    input  logic              clk_i,
    input  logic              rst_ni,
    input  longint unsigned   nmu_ctx_i,
    input  longint unsigned   nsu_ctx_i,
    input  longint unsigned   dat_merge_ctx_i,

    // NMU AXI slave face (struct)
    input  ni_signals_pkg::axi_req_t   master_axi_req_i,
    output ni_signals_pkg::axi_rsp_t   master_axi_rsp_o,
    // NSU AXI master face (struct)
    output ni_signals_pkg::axi_req_t   slave_axi_req_o,
    input  ni_signals_pkg::axi_rsp_t   slave_axi_rsp_i,

    // REQ face (egress from NMU, ready/valid).
    output logic                      tx_req_valid_o,
    output logic [REQ_FLIT_WIDTH-1:0] tx_req_flit_o,
    input  logic                      tx_req_ready_i,
    // REQ face (ingress to NSU, ready/valid).
    input  logic                      rx_req_valid_i,
    input  logic [REQ_FLIT_WIDTH-1:0] rx_req_flit_i,
    output logic                      rx_req_ready_o,

    // RSP face (egress from NSU, ready/valid).
    output logic                      tx_rsp_valid_o,
    output logic [RSP_FLIT_WIDTH-1:0] tx_rsp_flit_o,
    input  logic                      tx_rsp_ready_i,
    // RSP face (ingress to NMU, ready/valid).
    input  logic                      rx_rsp_valid_i,
    input  logic [RSP_FLIT_WIDTH-1:0] rx_rsp_flit_i,
    output logic                      rx_rsp_ready_o,

    // DAT face (merged NMU+NSU, credit) — dat_merge_wrap's router-facing side.
    output logic                      tx_dat_valid_o,
    output logic [DAT_FLIT_WIDTH-1:0] tx_dat_flit_o,
    input  logic [DAT_NUM_VC-1:0]     tx_dat_crdvalid_i,
    input  logic                      rx_dat_valid_i,
    input  logic [DAT_FLIT_WIDTH-1:0] rx_dat_flit_i,
    output logic [DAT_NUM_VC-1:0]     rx_dat_crdvalid_o
);

    // NMU <-> dat_merge_wrap DAT pins.
    logic                      nmu_tx_dat_valid;
    logic [DAT_FLIT_WIDTH-1:0] nmu_tx_dat_flit;
    logic [DAT_NUM_VC-1:0]     nmu_tx_dat_crdvalid;
    logic                      nmu_rx_dat_valid;
    logic [DAT_FLIT_WIDTH-1:0] nmu_rx_dat_flit;

    // NSU <-> dat_merge_wrap DAT pins.
    logic                      nsu_tx_dat_valid;
    logic [DAT_FLIT_WIDTH-1:0] nsu_tx_dat_flit;
    logic [DAT_NUM_VC-1:0]     nsu_tx_dat_crdvalid;
    logic                      nsu_rx_dat_valid;
    logic [DAT_FLIT_WIDTH-1:0] nsu_rx_dat_flit;

    nmu_wrap #(
        .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),
        .DAT_NUM_VC(DAT_NUM_VC),
        .REQ_FLIT_WIDTH(REQ_FLIT_WIDTH), .RSP_FLIT_WIDTH(RSP_FLIT_WIDTH),
        .DAT_FLIT_WIDTH(DAT_FLIT_WIDTH)
    ) u_nmu (
        .clk_i(clk_i), .rst_ni(rst_ni), .ctx_i(nmu_ctx_i),
        .axi_req_i(master_axi_req_i), .axi_rsp_o(master_axi_rsp_o),
        .tx_req_valid_o(tx_req_valid_o), .tx_req_flit_o(tx_req_flit_o),
        .tx_req_ready_i(tx_req_ready_i),
        .rx_rsp_valid_i(rx_rsp_valid_i), .rx_rsp_flit_i(rx_rsp_flit_i),
        .rx_rsp_ready_o(rx_rsp_ready_o),
        .tx_dat_valid_o(nmu_tx_dat_valid), .tx_dat_flit_o(nmu_tx_dat_flit),
        .tx_dat_crdvalid_i(nmu_tx_dat_crdvalid),
        .rx_dat_valid_i(nmu_rx_dat_valid), .rx_dat_flit_i(nmu_rx_dat_flit),
        .rx_dat_crdvalid_o()  // NMU's own ingress credit-return has no
                              // consumer once the merge's demux is unbuffered
                              // same-cycle (dat_merge_wrap.hpp class comment)
    );

    nsu_wrap #(
        .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),
        .DAT_NUM_VC(DAT_NUM_VC),
        .REQ_FLIT_WIDTH(REQ_FLIT_WIDTH), .RSP_FLIT_WIDTH(RSP_FLIT_WIDTH),
        .DAT_FLIT_WIDTH(DAT_FLIT_WIDTH)
    ) u_nsu (
        .clk_i(clk_i), .rst_ni(rst_ni), .ctx_i(nsu_ctx_i),
        .rx_req_valid_i(rx_req_valid_i), .rx_req_flit_i(rx_req_flit_i),
        .rx_req_ready_o(rx_req_ready_o),
        .tx_rsp_valid_o(tx_rsp_valid_o), .tx_rsp_flit_o(tx_rsp_flit_o),
        .tx_rsp_ready_i(tx_rsp_ready_i),
        .tx_dat_valid_o(nsu_tx_dat_valid), .tx_dat_flit_o(nsu_tx_dat_flit),
        .tx_dat_crdvalid_i(nsu_tx_dat_crdvalid),
        .rx_dat_valid_i(nsu_rx_dat_valid), .rx_dat_flit_i(nsu_rx_dat_flit),
        .rx_dat_crdvalid_o(),  // see u_nmu's rx_dat_crdvalid_o comment
        .axi_req_o(slave_axi_req_o), .axi_rsp_i(slave_axi_rsp_i)
    );

    dat_merge_wrap #(
        .DAT_NUM_VC(DAT_NUM_VC), .DAT_FLIT_WIDTH(DAT_FLIT_WIDTH)
    ) u_dat_merge (
        .clk_i(clk_i), .rst_ni(rst_ni), .ctx_i(dat_merge_ctx_i),
        .nmu_tx_dat_valid_i(nmu_tx_dat_valid), .nmu_tx_dat_flit_i(nmu_tx_dat_flit),
        .nmu_tx_dat_crdvalid_o(nmu_tx_dat_crdvalid),
        .nmu_rx_dat_valid_o(nmu_rx_dat_valid), .nmu_rx_dat_flit_o(nmu_rx_dat_flit),
        .nsu_tx_dat_valid_i(nsu_tx_dat_valid), .nsu_tx_dat_flit_i(nsu_tx_dat_flit),
        .nsu_tx_dat_crdvalid_o(nsu_tx_dat_crdvalid),
        .nsu_rx_dat_valid_o(nsu_rx_dat_valid), .nsu_rx_dat_flit_o(nsu_rx_dat_flit),
        .tx_dat_valid_o(tx_dat_valid_o), .tx_dat_flit_o(tx_dat_flit_o),
        .tx_dat_crdvalid_i(tx_dat_crdvalid_i),
        .rx_dat_valid_i(rx_dat_valid_i), .rx_dat_flit_i(rx_dat_flit_i),
        .rx_dat_crdvalid_o(rx_dat_crdvalid_o)
    );

endmodule

`endif  // NI_WRAP_SV
