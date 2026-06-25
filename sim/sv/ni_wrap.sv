// ni_wrap — Network Interface: bundles nmu_wrap + nsu_wrap for one mesh node.
//
// Ports:
//   master_axi_i   — AXI slave modport; test master drives AW/W/AR into the NMU.
//   slave_axi_o    — AXI master modport; NSU drives AW/W/AR out toward test slave.
//   NMU NoC side (forwarded from nmu_wrap):
//     noc_req_o      — ni_signals_pkg::noc_chan_t: NMU drives req flit toward router.
//     noc_req_cred_i — noc_types_pkg::noc_credit_t: router returns req credit.
//     noc_rsp_i      — ni_signals_pkg::noc_chan_t: router drives rsp flit toward NMU.
//     noc_rsp_cred_o — noc_types_pkg::noc_credit_t: NMU returns rsp credit.
//   NSU NoC side (forwarded from nsu_wrap):
//     noc_req_i      — ni_signals_pkg::noc_chan_t: router drives req flit toward NSU.
//     noc_req_cred_o — noc_types_pkg::noc_credit_t: NSU returns req credit.
//     noc_rsp_o      — ni_signals_pkg::noc_chan_t: NSU drives rsp flit toward router.
//     noc_rsp_cred_i — noc_types_pkg::noc_credit_t: router returns rsp credit.
//
// Does NO cmodel_*_create. The nmu_ctx_i/nsu_ctx_i DPI handles arrive as ports
// from tb_top. This file is COMMITTED (hand-written, reusable NoC infra); the
// generated fabric includes it but never regenerates it.

`timescale 1ns/1ps

`ifndef NI_WRAP_SV
`define NI_WRAP_SV

// nmu_wrap / nsu_wrap are provided by the filelist (build_config.mk TB_TOP_SV_SRC,
// listed before ni_wrap.sv); no in-file `include needed.

module ni_wrap #(
    parameter int unsigned ID_WIDTH              = ni_params_pkg::AXI_ID_WIDTH_DFLT,
    parameter int unsigned ADDR_WIDTH            = ni_params_pkg::AXI_ADDR_WIDTH_DFLT,
    parameter int unsigned DATA_WIDTH            = ni_params_pkg::AXI_DATA_WIDTH_DFLT,
    parameter int unsigned NUM_VC                = ni_params_pkg::NOC_NUM_VC_DFLT,
    parameter int unsigned FLIT_WIDTH            = ni_params_pkg::NOC_FLIT_WIDTH_DFLT,
    parameter int unsigned SLAVE_VC_BUFFER_DEPTH = ni_params_pkg::NOC_SLAVE_VC_BUFFER_DEPTH_DFLT
) (
    input  logic              clk_i,
    input  logic              rst_ni,
    input  longint unsigned   nmu_ctx_i,
    input  longint unsigned   nsu_ctx_i,
    axi4_intf.slave           master_axi_i,
    axi4_intf.master          slave_axi_o,
    // NMU NoC side
    output ni_signals_pkg::noc_chan_t  noc_req_o,
    input  noc_types_pkg::noc_credit_t noc_req_cred_i,
    input  ni_signals_pkg::noc_chan_t  noc_rsp_i,
    output noc_types_pkg::noc_credit_t noc_rsp_cred_o,
    // NSU NoC side
    input  ni_signals_pkg::noc_chan_t  noc_req_i,
    output noc_types_pkg::noc_credit_t noc_req_cred_o,
    output ni_signals_pkg::noc_chan_t  noc_rsp_o,
    input  noc_types_pkg::noc_credit_t noc_rsp_cred_i
);

    // Elaboration guard: noc_types_pkg::noc_credit_t width must match NUM_VC.
    initial begin
        if ($bits(noc_types_pkg::noc_credit_t) != NUM_VC) begin
            $fatal(1, "%m: noc_credit_t width %0d != NUM_VC %0d; use matching noc_types_pkg_vc{N}.sv",
                   $bits(noc_types_pkg::noc_credit_t), NUM_VC);
        end
    end

    nmu_wrap #(
        .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),
        .NUM_VC(NUM_VC), .FLIT_WIDTH(FLIT_WIDTH), .SLAVE_VC_BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH)
    ) u_nmu (
        .clk_i(clk_i), .rst_ni(rst_ni), .ctx_i(nmu_ctx_i),
        .axi_i(master_axi_i),
        .noc_req_o(noc_req_o), .noc_req_cred_i(noc_req_cred_i),
        .noc_rsp_i(noc_rsp_i), .noc_rsp_cred_o(noc_rsp_cred_o)
    );

    nsu_wrap #(
        .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),
        .NUM_VC(NUM_VC), .FLIT_WIDTH(FLIT_WIDTH), .SLAVE_VC_BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH)
    ) u_nsu (
        .clk_i(clk_i), .rst_ni(rst_ni), .ctx_i(nsu_ctx_i),
        .noc_req_i(noc_req_i), .noc_req_cred_o(noc_req_cred_o),
        .noc_rsp_o(noc_rsp_o), .noc_rsp_cred_i(noc_rsp_cred_i),
        .axi_o(slave_axi_o)
    );

endmodule

`endif  // NI_WRAP_SV
