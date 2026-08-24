`timescale 1ns / 1ps

module tb_nmu_response_path;
    logic noc_clk_i = 0, axi_clk_i = 0;
    logic noc_rst_ni = 0, axi_rst_ni = 0;
    ni_signals_pkg::axi_b_t s_b_i, m_b_o;
    ni_signals_pkg::axi_r_t s_r_i, m_r_o;
    logic s_b_valid_i, s_b_ready_o, s_r_valid_i, s_r_ready_o;
    logic m_b_valid_o, m_b_ready_i, m_r_valid_o, m_r_ready_i;

    nmu_response_path #(.AXI_FIFO_DEPTH (4)) dut (.*);

    always #5ns noc_clk_i = !noc_clk_i;
    always #3ns axi_clk_i = !axi_clk_i;

    initial begin
        s_b_i = '0; s_r_i = '0; s_b_valid_i = 0; s_r_valid_i = 0;
        m_b_ready_i = 0; m_r_ready_i = 1;
        repeat (3) @(posedge noc_clk_i); noc_rst_ni = 1;
        repeat (3) @(posedge axi_clk_i); axi_rst_ni = 1;
        s_b_i.bid = 'h2; s_b_i.bresp = 2'b01;
        s_r_i.rid = 'h3; s_r_i.rdata = 'h1234; s_r_i.rlast = 1;
        s_b_valid_i = 1; s_r_valid_i = 1;
        fork
            begin do @(posedge noc_clk_i); while (!s_b_ready_o); s_b_valid_i = 0; end
            begin do @(posedge noc_clk_i); while (!s_r_ready_o); s_r_valid_i = 0; end
        join
        do @(posedge axi_clk_i); while (!m_r_valid_o);
        if (m_r_o != s_r_i) $fatal(1, "R changed across response path");
        if (m_b_valid_o !== 1'b1) $fatal(1, "B did not progress independently to its output FIFO");
        m_b_ready_i = 1;
        do @(posedge axi_clk_i); while (!m_b_valid_o);
        if (m_b_o != s_b_i) $fatal(1, "B changed across response path");

        // A destination-domain reset must flush a response already buffered.
        m_b_ready_i = 0;
        s_b_i.bid = 'h1;
        s_b_valid_i = 1;
        do @(posedge noc_clk_i); while (!s_b_ready_o);
        s_b_valid_i = 0;
        do @(posedge axi_clk_i); while (!m_b_valid_o);
        axi_rst_ni = 0;
        repeat (2) @(posedge axi_clk_i);
        if (m_b_valid_o !== 1'b0) $fatal(1, "AXI reset did not flush B FIFO output");
        axi_rst_ni = 1;
        $finish;
    end

    initial begin #10us; $fatal(1, "response path integration timeout"); end
endmodule
