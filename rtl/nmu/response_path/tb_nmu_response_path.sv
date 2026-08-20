`timescale 1ns / 1ps

module tb_nmu_response_path;

    ni_flit_pkg::rsp_flit_t s_rsp;
    logic s_rsp_valid, s_rsp_ready;
    ni_flit_pkg::dat_flit_t s_dat;
    logic s_dat_valid, s_dat_ready;
    ni_signals_pkg::axi_b_t m_b;
    logic m_b_valid, m_b_ready;
    ni_signals_pkg::axi_r_t m_r;
    logic m_r_valid, m_r_ready;

    nmu_response_path dut (
        .s_rsp_i       (s_rsp),
        .s_rsp_valid_i (s_rsp_valid),
        .s_rsp_ready_o (s_rsp_ready),
        .s_dat_i       (s_dat),
        .s_dat_valid_i (s_dat_valid),
        .s_dat_ready_o (s_dat_ready),
        .m_b_o         (m_b),
        .m_b_valid_o   (m_b_valid),
        .m_b_ready_i   (m_b_ready),
        .m_r_o         (m_r),
        .m_r_valid_o   (m_r_valid),
        .m_r_ready_i   (m_r_ready)
    );

    initial $finish;

endmodule
