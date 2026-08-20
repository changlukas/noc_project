`resetall
`timescale 1ns / 1ps
`default_nettype none

module tb_nmu_request_fifo #(
    parameter int unsigned AXI_FIFO_DEPTH = 8,
    parameter int unsigned AXI_ID_WIDTH = 3
);

    typedef struct packed {
        logic [AXI_ID_WIDTH-1:0] id;
        logic [7:0]              tag;
        logic [7:0]              len;
    } aw_t;

    typedef logic [8:0] w_t;

    typedef struct packed {
        logic [AXI_ID_WIDTH-1:0] id;
        logic [7:0]              tag;
    } ar_t;

    logic axi_clk_i = 1'b0;
    logic axi_rst_ni = 1'b0;
    logic noc_clk_i = 1'b0;
    logic noc_rst_ni = 1'b0;

    logic s_aw_valid;
    logic s_aw_ready;
    aw_t  s_aw_data;
    logic m_aw_valid;
    logic m_aw_ready;
    aw_t  m_aw_data;

    logic s_w_valid;
    logic s_w_ready;
    w_t   s_w_data;
    logic m_w_valid;
    logic m_w_ready;
    w_t   m_w_data;

    logic s_ar_valid;
    logic s_ar_ready;
    ar_t  s_ar_data;
    logic m_ar_valid;
    logic m_ar_ready;
    ar_t  m_ar_data;

    nmu_request_fifo #(
        .AXI_FIFO_DEPTH ( AXI_FIFO_DEPTH ),
        .AXI_ID_WIDTH   ( AXI_ID_WIDTH   ),
        .AW_T           ( aw_t           ),
        .W_T            ( w_t            ),
        .AR_T           ( ar_t           )
    ) dut (
        .axi_clk_i,
        .axi_rst_ni,
        .noc_clk_i,
        .noc_rst_ni,
        .s_aw_valid_i ( s_aw_valid ),
        .s_aw_ready_o ( s_aw_ready ),
        .s_aw_data_i  ( s_aw_data  ),
        .m_aw_valid_o ( m_aw_valid ),
        .m_aw_ready_i ( m_aw_ready ),
        .m_aw_data_o  ( m_aw_data  ),
        .s_w_valid_i  ( s_w_valid  ),
        .s_w_ready_o  ( s_w_ready  ),
        .s_w_data_i   ( s_w_data   ),
        .m_w_valid_o  ( m_w_valid  ),
        .m_w_ready_i  ( m_w_ready  ),
        .m_w_data_o   ( m_w_data   ),
        .s_ar_valid_i ( s_ar_valid ),
        .s_ar_ready_o ( s_ar_ready ),
        .s_ar_data_i  ( s_ar_data  ),
        .m_ar_valid_o ( m_ar_valid ),
        .m_ar_ready_i ( m_ar_ready ),
        .m_ar_data_o  ( m_ar_data  )
    );

    always #5ns axi_clk_i <= ~axi_clk_i;
    always #7ns noc_clk_i <= ~noc_clk_i;

    initial begin
        #100us;
        $fatal(1, "Timed out waiting for NMU request FIFO test completion");
    end

    function automatic w_t make_w(input logic [7:0] tag, input logic last);
        make_w = {tag, last};
    endfunction

    task automatic send_aw(input aw_t data);
        begin
            @(negedge axi_clk_i);
            s_aw_data = data;
            s_aw_valid = 1'b1;
            do @(posedge axi_clk_i); while (!s_aw_ready);
            @(negedge axi_clk_i);
            s_aw_valid = 1'b0;
        end
    endtask

    task automatic send_w(input w_t data);
        begin
            @(negedge axi_clk_i);
            s_w_data = data;
            s_w_valid = 1'b1;
            do @(posedge axi_clk_i); while (!s_w_ready);
            @(negedge axi_clk_i);
            s_w_valid = 1'b0;
        end
    endtask

    task automatic send_ar(input ar_t data);
        begin
            @(negedge axi_clk_i);
            s_ar_data = data;
            s_ar_valid = 1'b1;
            do @(posedge axi_clk_i); while (!s_ar_ready);
            @(negedge axi_clk_i);
            s_ar_valid = 1'b0;
        end
    endtask

    task automatic receive_aw(input aw_t expected);
        begin
            wait (m_aw_valid === 1'b1);
            #1ps;
            assert (m_aw_data == expected)
                else $fatal(1, "AW FIFO expected tag %0h, got %0h", expected.tag, m_aw_data.tag);
            @(negedge noc_clk_i);
            m_aw_ready = 1'b1;
            @(posedge noc_clk_i);
            @(negedge noc_clk_i);
            m_aw_ready = 1'b0;
        end
    endtask

    task automatic receive_w(input w_t expected);
        begin
            wait (m_w_valid === 1'b1);
            #1ps;
            assert (m_w_data == expected)
                else $fatal(1, "W FIFO expected %0h, got %0h", expected, m_w_data);
            @(negedge noc_clk_i);
            m_w_ready = 1'b1;
            @(posedge noc_clk_i);
            @(negedge noc_clk_i);
            m_w_ready = 1'b0;
        end
    endtask

    task automatic receive_ar(input ar_t expected);
        begin
            wait (m_ar_valid === 1'b1);
            #1ps;
            assert (m_ar_data == expected)
                else $fatal(1, "AR FIFO expected tag %0h, got %0h", expected.tag, m_ar_data.tag);
            @(negedge noc_clk_i);
            m_ar_ready = 1'b1;
            @(posedge noc_clk_i);
            @(negedge noc_clk_i);
            m_ar_ready = 1'b0;
        end
    endtask

    initial begin
        aw_t aw_data;
        w_t w_data;
        ar_t ar_data;
        int unsigned aw_fill_count;

        s_aw_valid = 1'b0;
        s_aw_data = '0;
        m_aw_ready = 1'b0;
        s_w_valid = 1'b0;
        s_w_data = '0;
        m_w_ready = 1'b0;
        s_ar_valid = 1'b0;
        s_ar_data = '0;
        m_ar_ready = 1'b0;

        repeat (3) @(posedge axi_clk_i);
        #1ps;
        axi_rst_ni = 1'b1;
        repeat (3) @(posedge noc_clk_i);
        #1ps;
        noc_rst_ni = 1'b1;
        assert (!m_aw_valid && !m_w_valid && !m_ar_valid)
            else $fatal(1, "Request FIFO output valid was not cleared by reset");

        // A full AW FIFO must not backpressure the independent W and AR channels.
        aw_fill_count = 0;
        while (s_aw_ready && aw_fill_count < AXI_FIFO_DEPTH + 4) begin
            int unsigned index;
            index = aw_fill_count;
            aw_data.id = AXI_ID_WIDTH'(index);
            aw_data.tag = 8'h10 + 8'(index);
            aw_data.len = 8'd0;
            send_aw(aw_data);
            aw_fill_count++;
            #1ps;
        end
        assert (!s_aw_ready) else $fatal(1, "AW FIFO did not become full");
        assert (s_w_ready && s_ar_ready)
            else $fatal(1, "Full AW FIFO backpressured W or AR");

        w_data = make_w(8'ha0, 1'b1);
        ar_data.id = AXI_ID_WIDTH'(0);
        ar_data.tag = 8'hb0;
        send_w(w_data);
        send_ar(ar_data);
        receive_w(w_data);
        receive_ar(ar_data);

        // Drain AW in FIFO order and confirm source readiness returns after CDC synchronization.
        for (int unsigned index = 0; index < aw_fill_count; index++) begin
            aw_data.id = AXI_ID_WIDTH'(index);
            aw_data.tag = 8'h10 + 8'(index);
            aw_data.len = 8'd0;
            receive_aw(aw_data);
        end
        repeat (3) @(posedge axi_clk_i);
        assert (s_aw_ready) else $fatal(1, "AW FIFO did not release source backpressure");

        // Preserve AW order and W-beat order: the following metadata stage can associate
        // the two bursts by their accepted AW/W sequence.
        aw_data.id = AXI_ID_WIDTH'(1);
        aw_data.tag = 8'hc0;
        aw_data.len = 8'd1;
        send_aw(aw_data);
        aw_data.id = AXI_ID_WIDTH'(2);
        aw_data.tag = 8'hd0;
        aw_data.len = 8'd0;
        send_aw(aw_data);

        w_data = make_w(8'hc1, 1'b0);
        send_w(w_data);
        w_data = make_w(8'hc2, 1'b1);
        send_w(w_data);
        w_data = make_w(8'hd1, 1'b1);
        send_w(w_data);

        aw_data.id = AXI_ID_WIDTH'(1);
        aw_data.tag = 8'hc0;
        aw_data.len = 8'd1;
        receive_aw(aw_data);
        aw_data.id = AXI_ID_WIDTH'(2);
        aw_data.tag = 8'hd0;
        aw_data.len = 8'd0;
        receive_aw(aw_data);
        w_data = make_w(8'hc1, 1'b0);
        receive_w(w_data);
        w_data = make_w(8'hc2, 1'b1);
        receive_w(w_data);
        w_data = make_w(8'hd1, 1'b1);
        receive_w(w_data);

        $display("PASS: NMU request FIFO");
        $finish;
    end

endmodule

`resetall
