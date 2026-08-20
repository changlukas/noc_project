`timescale 1ns / 1ps

module tb_nmu_request_path;

    ni_child_types_pkg::nmu_sam_aw_t s_aw;
    logic                             s_aw_valid;
    logic                             s_aw_ready;
    ni_signals_pkg::axi_w_t           s_w;
    logic                             s_w_valid;
    logic                             s_w_ready;
    ni_signals_pkg::axi_ar_t          s_ar;
    logic                             s_ar_valid;
    logic                             s_ar_ready;
    ni_flit_pkg::req_flit_t           m_req;
    logic                             m_req_valid;
    logic                             m_req_ready;
    ni_flit_pkg::dat_flit_t           m_dat;
    logic                             m_dat_valid;
    logic [ni_params_pkg::NOC_DAT_NUM_VC_DFLT-1:0] m_dat_crdvalid;

    nmu_request_path dut (
        .s_aw_i          (s_aw),
        .s_aw_valid_i    (s_aw_valid),
        .s_aw_ready_o    (s_aw_ready),
        .s_w_i           (s_w),
        .s_w_valid_i     (s_w_valid),
        .s_w_ready_o     (s_w_ready),
        .s_ar_i          (s_ar),
        .s_ar_valid_i    (s_ar_valid),
        .s_ar_ready_o    (s_ar_ready),
        .m_req_o         (m_req),
        .m_req_valid_o   (m_req_valid),
        .m_req_ready_i   (m_req_ready),
        .m_dat_o         (m_dat),
        .m_dat_valid_o   (m_dat_valid),
        .m_dat_crdvalid_i(m_dat_crdvalid)
    );

    initial $finish;

endmodule

module tb_nmu_response_path;

    ni_flit_pkg::rsp_flit_t s_rsp;
    logic                   s_rsp_valid;
    logic                   s_rsp_ready;
    ni_flit_pkg::dat_flit_t s_dat;
    logic                   s_dat_valid;
    logic                   s_dat_ready;
    ni_signals_pkg::axi_b_t m_b;
    logic                   m_b_valid;
    logic                   m_b_ready;
    ni_signals_pkg::axi_r_t m_r;
    logic                   m_r_valid;
    logic                   m_r_ready;

    nmu_response_path dut (
        .s_rsp_i        (s_rsp),
        .s_rsp_valid_i  (s_rsp_valid),
        .s_rsp_ready_o  (s_rsp_ready),
        .s_dat_i        (s_dat),
        .s_dat_valid_i  (s_dat_valid),
        .s_dat_ready_o  (s_dat_ready),
        .m_b_o          (m_b),
        .m_b_valid_o    (m_b_valid),
        .m_b_ready_i    (m_b_ready),
        .m_r_o          (m_r),
        .m_r_valid_o    (m_r_valid),
        .m_r_ready_i    (m_r_ready)
    );

    initial $finish;

endmodule
