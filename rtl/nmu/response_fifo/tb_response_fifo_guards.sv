`timescale 1ns / 1ps

module tb_nmu_response_fifo_guards;
    logic                   clk, rst_n_i;
    ni_signals_pkg::axi_b_t b;
    ni_signals_pkg::axi_r_t r;
    logic                   b_valid, b_ready, r_valid, r_ready;
    nmu_response_fifo #(.AXI_FIFO_DEPTH (3)) dut (
        .noc_clk_i   (clk    ),
        .noc_rst_n_i (rst_n_i),
        .axi_clk_i   (clk    ),
        .axi_rst_n_i (rst_n_i),
        .s_b_data_i  (b      ),
        .s_b_valid_i (b_valid),
        .s_b_ready_o (       ),
        .m_b_data_o  (       ),
        .m_b_valid_o (       ),
        .m_b_ready_i (b_ready),
        .s_r_data_i  (r      ),
        .s_r_valid_i (r_valid),
        .s_r_ready_o (       ),
        .m_r_data_o  (       ),
        .m_r_valid_o (       ),
        .m_r_ready_i (r_ready)
    );
    initial begin clk = 0; rst_n_i = 0; b = '0; r = '0; b_valid = 0; r_valid = 0; b_ready = 0; r_ready = 0; #1ns; $finish; end
endmodule
