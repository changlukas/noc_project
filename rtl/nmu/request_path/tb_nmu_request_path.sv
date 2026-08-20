`timescale 1ns / 1ps

module tb_nmu_request_path;

    ni_signals_pkg::axi_aw_t s_aw;
    logic s_aw_valid, s_aw_ready;
    ni_signals_pkg::axi_w_t s_w;
    logic s_w_valid, s_w_ready;
    ni_signals_pkg::axi_ar_t s_ar;
    logic s_ar_valid, s_ar_ready;
    ni_flit_pkg::req_flit_t m_req;
    logic m_req_valid, m_req_ready;
    ni_flit_pkg::dat_flit_t m_dat;
    logic m_dat_valid;
    logic [ni_params_pkg::NOC_DAT_NUM_VC_DFLT-1:0] m_dat_crdvalid;

    nmu_request_path dut (
        .s_aw_i           (s_aw),
        .s_aw_valid_i     (s_aw_valid),
        .s_aw_ready_o     (s_aw_ready),
        .s_w_i            (s_w),
        .s_w_valid_i      (s_w_valid),
        .s_w_ready_o      (s_w_ready),
        .s_ar_i           (s_ar),
        .s_ar_valid_i     (s_ar_valid),
        .s_ar_ready_o     (s_ar_ready),
        .m_req_o          (m_req),
        .m_req_valid_o    (m_req_valid),
        .m_req_ready_i    (m_req_ready),
        .m_dat_o          (m_dat),
        .m_dat_valid_o    (m_dat_valid),
        .m_dat_crdvalid_i (m_dat_crdvalid)
    );

    initial $finish;

endmodule
