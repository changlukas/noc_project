// ni_wrap — Network Interface: bundles nmu_wrap + nsu_wrap for one mesh node.
//
// Ports:
//   master_axi_i  — AXI slave modport; test master drives AW/W/AR into the NMU.
//   slave_axi_o   — AXI master modport; NSU drives AW/W/AR out toward test slave.
//   noc_nmu_o     — noc_intf.mosi: NMU drives req_valid/req_flit + rsp_credit_return
//                   to router; NMU reads req_credit_return + rsp_valid/rsp_flit back.
//   noc_nsu_i     — noc_intf.miso: NSU reads req_valid/req_flit + rsp_credit_return
//                   from router; NSU drives rsp_valid/rsp_flit + req_credit_return.
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
    noc_intf.mosi             noc_nmu_o,
    noc_intf.miso             noc_nsu_i
);

    nmu_wrap #(
        .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),
        .NUM_VC(NUM_VC), .FLIT_WIDTH(FLIT_WIDTH), .SLAVE_VC_BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH)
    ) u_nmu (
        .clk_i(clk_i), .rst_ni(rst_ni), .ctx_i(nmu_ctx_i),
        .axi_i(master_axi_i), .noc_mosi_o(noc_nmu_o)
    );

    nsu_wrap #(
        .ID_WIDTH(ID_WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .DATA_WIDTH(DATA_WIDTH),
        .NUM_VC(NUM_VC), .FLIT_WIDTH(FLIT_WIDTH), .SLAVE_VC_BUFFER_DEPTH(SLAVE_VC_BUFFER_DEPTH)
    ) u_nsu (
        .clk_i(clk_i), .rst_ni(rst_ni), .ctx_i(nsu_ctx_i),
        .noc_miso_i(noc_nsu_i), .axi_o(slave_axi_o)
    );

endmodule

`endif  // NI_WRAP_SV
