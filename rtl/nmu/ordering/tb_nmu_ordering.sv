`timescale 1ns / 1ps

module tb_nmu_ordering;
    logic clk_i = 0, rst_i = 1;
    ni_child_types_pkg::nmu_sam_aw_result_t s_aw_i;
    ni_child_types_pkg::nmu_aw_request_t m_aw_o;
    ni_signals_pkg::axi_w_t s_w_i, m_w_o;
    ni_child_types_pkg::nmu_sam_ar_result_t s_ar_i;
    ni_child_types_pkg::nmu_ar_request_t m_ar_o;
    ni_child_types_pkg::nmu_b_response_t s_b_i;
    ni_signals_pkg::axi_b_t m_b_o;
    ni_child_types_pkg::nmu_r_response_t s_r_i;
    ni_signals_pkg::axi_r_t m_r_o;
    logic s_aw_valid_i, s_aw_ready_o, m_aw_valid_o, m_aw_ready_i;
    logic s_w_valid_i, s_w_ready_o, m_w_valid_o, m_w_ready_i;
    logic s_ar_valid_i, s_ar_ready_o, m_ar_valid_o, m_ar_ready_i;
    logic s_b_valid_i, s_b_ready_o, m_b_valid_o, m_b_ready_i;
    logic s_r_valid_i, s_r_ready_o, m_r_valid_o, m_r_ready_i;

    nmu_ordering #(
        .NMU_ROB_B_DEPTH (8), .NMU_ROB_R_DEPTH (16),
        .NMU_MAX_TXNS_PER_ID (4), .READ_ROB_ENABLED (1'b1)
    ) dut (.*);

    always #5ns clk_i = !clk_i;

    task automatic send_aw(input int id, input int dst);
        s_aw_i = '0;
        s_aw_i.axi.awid = 3'(id);
        s_aw_i.route.route.domain.dst_id = ni_flit_pkg::DST_ID_WIDTH'(dst);
        s_aw_valid_i = 1;
        do @(posedge clk_i); while (!s_aw_ready_o);
        s_aw_valid_i = 0;
    endtask

    task automatic send_b(input int id, input bit ordered, input int tag, input logic [1:0] resp);
        s_b_i = '0;
        s_b_i.axi.bid = 3'(id);
        s_b_i.axi.bresp = resp;
        s_b_i.meta.ordering_req = ordered;
        s_b_i.meta.ordering_tag = 8'(tag);
        s_b_valid_i = 1;
        do @(posedge clk_i); while (!s_b_ready_o);
        s_b_valid_i = 0;
    endtask

    task automatic send_ar(input int id, input int dst, input int len);
        s_ar_i = '0;
        s_ar_i.axi.arid = 3'(id);
        s_ar_i.axi.arlen = 8'(len);
        s_ar_i.route.domain.dst_id = ni_flit_pkg::DST_ID_WIDTH'(dst);
        s_ar_valid_i = 1;
        do @(posedge clk_i); while (!s_ar_ready_o);
        s_ar_valid_i = 0;
    endtask

    task automatic send_r(
        input int id, input bit ordered, input int tag,
        input logic [31:0] data, input bit last
    );
        s_r_i = '0;
        s_r_i.axi.rid = 3'(id);
        s_r_i.axi.rdata = data;
        s_r_i.axi.rlast = last;
        s_r_i.meta.ordering_req = ordered;
        s_r_i.meta.ordering_tag = 8'(tag);
        s_r_valid_i = 1;
        do @(posedge clk_i); while (!s_r_ready_o);
        s_r_valid_i = 0;
    endtask

    initial begin
        s_aw_i = '0; s_w_i = '0; s_ar_i = '0; s_b_i = '0; s_r_i = '0;
        s_aw_valid_i = 0; s_w_valid_i = 0; s_ar_valid_i = 0;
        s_b_valid_i = 0; s_r_valid_i = 0;
        m_aw_ready_i = 1; m_w_ready_i = 1; m_ar_ready_i = 1;
        m_b_ready_i = 0; m_r_ready_i = 1;
        repeat (3) @(posedge clk_i); rst_i = 0;

        // Same ID changes destination: second request must reserve a B slot.
        send_aw(1, 1);
        send_aw(1, 2);
        if (!m_aw_o.meta.ordering_req || m_aw_o.meta.ordering_tag != 0)
            $fatal(1, "second AW did not receive reorder tag zero");

        // The later tagged response can fill while the bypassed head is absent.
        send_b(1, 1, 0, 2'b10);

        // Different ID bypass response progresses although ID 1 has a blocked head.
        send_aw(2, 3);
        s_b_i = '0; s_b_i.axi.bid = 3'd2; s_b_valid_i = 1; m_b_ready_i = 1;
        do @(posedge clk_i); while (!s_b_ready_o);
        if (!m_b_valid_o || m_b_o.bid != 3'd2) $fatal(1, "different ID did not progress");
        s_b_valid_i = 0;

        // Retire ID 1 bypassed head, then buffered successor.
        s_b_i = '0; s_b_i.axi.bid = 3'd1; s_b_valid_i = 1;
        do @(posedge clk_i); while (!s_b_ready_o);
        s_b_valid_i = 0;
        do @(posedge clk_i); while (!m_b_valid_o);
        if (m_b_o.bresp != 2'b10) $fatal(1, "buffered B did not retire after head");
        @(posedge clk_i);

        // A two-beat tagged R burst fills behind a bypassed head, then retires
        // one beat per cycle using a separate retirement offset.
        send_ar(3, 1, 0);
        send_ar(3, 2, 1);
        if (!m_ar_o.meta.ordering_req || m_ar_o.meta.ordering_tag != 0)
            $fatal(1, "second AR did not receive reorder tag zero");
        send_r(3, 1, 0, 32'ha5a5_0001, 0);
        send_r(3, 1, 0, 32'ha5a5_0002, 1);
        send_r(3, 0, 0, 32'h1111_0000, 1);
        do @(posedge clk_i); while (!m_r_valid_o);
        if (m_r_o.rdata[31:0] != 32'ha5a5_0001) $fatal(1, "first buffered R beat mismatch");
        @(posedge clk_i);
        if (!m_r_valid_o || m_r_o.rdata[31:0] != 32'ha5a5_0002)
            $fatal(1, "R retirement did not sustain one beat per cycle");
        @(posedge clk_i);

        // A collective request is an ordering barrier for its ID.  A later AW
        // remains blocked until the collective response retires.
        s_aw_i = '0;
        s_aw_i.axi.awid = 3'd4;
        s_aw_i.route.route.domain.dst_id = ni_flit_pkg::DST_ID_WIDTH'(1);
        s_aw_i.route.collective_op = 2'd1;
        s_aw_valid_i = 1;
        do @(posedge clk_i); while (!s_aw_ready_o);
        s_aw_valid_i = 0;

        s_aw_i = '0;
        s_aw_i.axi.awid = 3'd4;
        s_aw_i.route.route.domain.dst_id = ni_flit_pkg::DST_ID_WIDTH'(1);
        s_aw_valid_i = 1;
        repeat (3) begin
            @(posedge clk_i);
            if (s_aw_ready_o) $fatal(1, "AW crossed an outstanding collective request");
        end
        s_aw_valid_i = 0;

        send_b(4, 0, 0, 2'b00);
        send_aw(4, 1);
        send_b(4, 0, 0, 2'b00);

        $finish;
    end

    initial begin #20us; $fatal(1, "ordering timeout"); end
endmodule
