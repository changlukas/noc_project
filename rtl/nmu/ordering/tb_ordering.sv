`timescale 1ns / 1ps

module tb_nmu_ordering #(
    parameter int unsigned MAX_ACTIVE_IDS = 1 << ni_params_pkg::NOC_ID_WIDTH
);
    localparam int unsigned ID_W = ni_params_pkg::NOC_ID_WIDTH;
    localparam int unsigned LEN_W = 8;
    localparam int unsigned TAG_W = ni_flit_pkg::ORDERING_TAG_WIDTH;
    localparam int unsigned COLLECTIVE_OP_W = ni_flit_pkg::COLLECTIVE_OP_WIDTH;

    logic clk_i = 0, rst_n_i = 0;
    ni_types_pkg::nmu_sam_aw_result_t             s_aw_i;
    ni_types_pkg::nmu_aw_request_t                m_aw_o;
    ni_signals_pkg::noc_axi_w_t                       s_w_i, m_w_o;
    ni_types_pkg::nmu_sam_ar_result_t             s_ar_i;
    ni_types_pkg::nmu_ar_request_t                m_ar_o;
    ni_types_pkg::nmu_b_response_t                s_b_i;
    ni_signals_pkg::noc_axi_b_t                       m_b_o;
    ni_types_pkg::nmu_r_response_t                s_r_i;
    ni_signals_pkg::noc_axi_r_t                       m_r_o;
    logic                                         s_aw_valid_i, s_aw_ready_o, m_aw_valid_o, m_aw_ready_i;
    logic                                         s_w_valid_i, s_w_ready_o, m_w_valid_o, m_w_ready_i;
    logic                                         s_ar_valid_i, s_ar_ready_o, m_ar_valid_o, m_ar_ready_i;
    logic                                         s_b_valid_i, s_b_ready_o, m_b_valid_o, m_b_ready_i;
    logic                                         s_r_valid_i, s_r_ready_o, m_r_valid_o, m_r_ready_i;
    logic                                         last_aw_ordering_req, last_ar_ordering_req;
    logic                             [TAG_W-1:0] last_aw_ordering_tag, last_ar_ordering_tag;
    ni_signals_pkg::noc_axi_b_t                       retired_b [8];
    ni_signals_pkg::noc_axi_r_t                       retired_r [8];
    int unsigned                                  retired_b_cycle [8], retired_r_cycle [8];
    int unsigned b_retire_count = 0, r_retire_count = 0, cycle_count = 0;

    nmu_ordering #(
        .MAX_ACTIVE_IDS         (MAX_ACTIVE_IDS),
        .B_ROB_DEPTH            (8   ),
        .R_ROB_DEPTH            (16  ),
        .MAX_OUTSTANDING_PER_ID (4   ),
        .R_ROB_EN               (1'b1)
    ) dut (.*);

    always #5ns clk_i = !clk_i;

    always @(posedge clk_i) begin
        cycle_count <= cycle_count + 1;
        if (m_b_valid_o && m_b_ready_i) begin
            retired_b[b_retire_count]       <= m_b_o;
            retired_b_cycle[b_retire_count] <= cycle_count;
            b_retire_count                  <= b_retire_count + 1;
        end
        if (m_r_valid_o && m_r_ready_i) begin
            retired_r[r_retire_count]       <= m_r_o;
            retired_r_cycle[r_retire_count] <= cycle_count;
            r_retire_count                  <= r_retire_count + 1;
        end
    end

    task automatic send_aw(input int id, input int dst);
        @(negedge clk_i);
        s_aw_i                           = '0;
        s_aw_i.axi.awid                  = ID_W'(id % MAX_ACTIVE_IDS);
        s_aw_i.route.route.domain.dst_id = ni_flit_pkg::DST_ID_WIDTH'(dst);
        s_aw_valid_i                     = 1;
        do @(posedge clk_i); while (!s_aw_ready_o);
        last_aw_ordering_req = m_aw_o.meta.ordering_req;
        last_aw_ordering_tag = m_aw_o.meta.ordering_tag;
        @(negedge clk_i);
        s_aw_valid_i = 0;
    endtask

    task automatic send_b(input int id, input bit ordered, input int tag, input logic [1:0] resp);
        @(negedge clk_i);
        s_b_i                   = '0;
        s_b_i.axi.bid           = ID_W'(id % MAX_ACTIVE_IDS);
        s_b_i.axi.bresp         = resp;
        s_b_i.meta.ordering_req = ordered;
        s_b_i.meta.ordering_tag = TAG_W'(tag);
        s_b_valid_i             = 1;
        do @(posedge clk_i); while (!s_b_ready_o);
        @(negedge clk_i);
        s_b_valid_i = 0;
    endtask

    task automatic send_ar(input int id, input int dst, input int len);
        @(negedge clk_i);
        s_ar_i                     = '0;
        s_ar_i.axi.arid            = ID_W'(id % MAX_ACTIVE_IDS);
        s_ar_i.axi.arlen           = LEN_W'(len);
        s_ar_i.route.domain.dst_id = ni_flit_pkg::DST_ID_WIDTH'(dst);
        s_ar_valid_i               = 1;
        do @(posedge clk_i); while (!s_ar_ready_o);
        last_ar_ordering_req = m_ar_o.meta.ordering_req;
        last_ar_ordering_tag = m_ar_o.meta.ordering_tag;
        @(negedge clk_i);
        s_ar_valid_i = 0;
    endtask

    task automatic send_r(
        input int id, input bit ordered, input int tag,
        input logic [31:0] data, input bit last
    );
        @(negedge clk_i);
        s_r_i                   = '0;
        s_r_i.axi.rid           = ID_W'(id % MAX_ACTIVE_IDS);
        s_r_i.axi.rdata         = ni_params_pkg::AXI_DATA_WIDTH'(data);
        s_r_i.axi.rlast         = last;
        s_r_i.meta.ordering_req = ordered;
        s_r_i.meta.ordering_tag = TAG_W'(tag);
        s_r_valid_i             = 1;
        do @(posedge clk_i); while (!s_r_ready_o);
        @(negedge clk_i);
        s_r_valid_i = 0;
    endtask

    initial begin
        s_aw_i       = '0; s_w_i = '0; s_ar_i = '0; s_b_i = '0; s_r_i = '0;
        s_aw_valid_i = 0; s_w_valid_i = 0; s_ar_valid_i = 0;
        s_b_valid_i  = 0; s_r_valid_i = 0;
        m_aw_ready_i = 1; m_w_ready_i = 1; m_ar_ready_i = 1;
        m_b_ready_i  = 0; m_r_ready_i = 1;
        repeat (3) @(posedge clk_i);
        @(negedge clk_i); rst_n_i = 1;

        // Same ID changes destination: second request must reserve a B slot.
        send_aw(1, 1);
        send_aw(1, 2);
        if (!last_aw_ordering_req || last_aw_ordering_tag != 0)
            $fatal(1, "second AW did not receive reorder tag zero");

        // The later tagged response can fill while the bypassed head is absent.
        send_b(1, 1, 0, 2'b10);

        // Different ID bypass response progresses although ID 1 has a blocked head.
        send_aw(2, 3);
        m_b_ready_i = 1;
        send_b(2, 0, 0, 2'b00);
        if (b_retire_count != 1 || retired_b[0].bid != ID_W'(2))
            $fatal(1, "different ID did not progress");

        // Retire ID 1 bypassed head, then buffered successor.
        send_b(1, 0, 0, 2'b00);
        while (b_retire_count < 3) @(negedge clk_i);
        if (retired_b[2].bresp != 2'b10) $fatal(1, "buffered B did not retire after head");

        // A two-beat tagged R burst fills behind a bypassed head, then retires
        // one beat per cycle using a separate retirement offset.
        send_ar(3, 1, 0);
        send_ar(3, 2, 1);
        if (!last_ar_ordering_req || last_ar_ordering_tag != 0)
            $fatal(1, "second AR did not receive reorder tag zero");
        send_r(3, 1, 0, 32'ha5a5_0001, 0);
        send_r(3, 1, 0, 32'ha5a5_0002, 1);
        send_r(3, 0, 0, 32'h1111_0000, 1);
        while (r_retire_count < 3) @(negedge clk_i);
        if (retired_r[1].rdata[31:0] != 32'ha5a5_0001)
            $fatal(1, "first buffered R beat mismatch");
        if (retired_r[2].rdata[31:0] != 32'ha5a5_0002 ||
            retired_r_cycle[2] != retired_r_cycle[1] + 1)
            $fatal(1, "R retirement did not sustain one beat per cycle");

        // A collective request is an ordering barrier for its ID.  A later AW
        // remains blocked until the collective response retires.
        @(negedge clk_i);
        s_aw_i                           = '0;
        s_aw_i.axi.awid                  = ID_W'(4 % MAX_ACTIVE_IDS);
        s_aw_i.route.route.domain.dst_id = ni_flit_pkg::DST_ID_WIDTH'(1);
        s_aw_i.route.collective_op       = COLLECTIVE_OP_W'(ni_flit_pkg::COLLECTIVE_OP_MULTICAST);
        s_aw_valid_i                     = 1;
        do @(posedge clk_i); while (!s_aw_ready_o);
        @(negedge clk_i);
        s_aw_valid_i = 0;

        @(negedge clk_i);
        s_aw_i                           = '0;
        s_aw_i.axi.awid                  = ID_W'(4 % MAX_ACTIVE_IDS);
        s_aw_i.route.route.domain.dst_id = ni_flit_pkg::DST_ID_WIDTH'(1);
        s_aw_valid_i                     = 1;
        repeat (3) begin
            @(posedge clk_i);
            if (s_aw_ready_o) $fatal(1, "AW crossed an outstanding collective request");
        end
        @(negedge clk_i);
        s_aw_valid_i = 0;

        send_b(4, 0, 0, 2'b00);
        send_aw(4, 1);
        send_b(4, 0, 0, 2'b00);

        $display("ORDERING_CAPACITY_PASS active_ids=%0d", MAX_ACTIVE_IDS);
        $finish;
    end

    initial begin #20us; $fatal(1, "ordering timeout"); end
endmodule
