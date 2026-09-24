`timescale 1ns / 1ps

module tb_nmu_ordering_stall;
    localparam int unsigned ID_W = ni_params_pkg::AXI_ID_WIDTH;
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
    ni_signals_pkg::noc_axi_b_t                       retired_b [128];
    ni_signals_pkg::noc_axi_r_t                       retired_r [128];
    int unsigned                                  retired_b_cycle [128], retired_r_cycle [128];
    int unsigned b_retire_count = 0, r_retire_count = 0, cycle_count = 0;

    nmu_ordering #(
        .B_ROB_DEPTH            (8   ),
        .R_ROB_DEPTH            (16  ),
        .MAX_OUTSTANDING_PER_ID (4   ),
        .R_ROB_EN               (1'b1)
    ) dut (.*);

    always #5ns clk_i = !clk_i;

    ni_types_pkg::nmu_aw_request_t prev_aw;
    ni_types_pkg::nmu_ar_request_t prev_ar;
    ni_signals_pkg::noc_axi_b_t        prev_b;
    ni_signals_pkg::noc_axi_r_t        prev_r;
    logic aw_stalled = 0, ar_stalled = 0, b_stalled = 0, r_stalled = 0;
    always @(posedge clk_i or negedge rst_n_i) begin
        if (~rst_n_i) begin
            b_retire_count <= 0;
            r_retire_count <= 0;
            cycle_count    <= 0;
            aw_stalled     <= 0;
            ar_stalled     <= 0;
            b_stalled      <= 0;
            r_stalled      <= 0;
        end else begin
            if (aw_stalled && (!m_aw_valid_o || m_aw_o !== prev_aw))
                $fatal(1, "AW changed while stalled");
            if (ar_stalled && (!m_ar_valid_o || m_ar_o !== prev_ar))
                $fatal(1, "AR changed while stalled");
            if (b_stalled && (!m_b_valid_o || m_b_o !== prev_b))
                $fatal(1, "B changed while stalled");
            if (r_stalled && (!m_r_valid_o || m_r_o !== prev_r))
                $fatal(1, "R changed while stalled");
            aw_stalled  <= m_aw_valid_o && !m_aw_ready_i;
            ar_stalled  <= m_ar_valid_o && !m_ar_ready_i;
            b_stalled   <= m_b_valid_o && !m_b_ready_i;
            r_stalled   <= m_r_valid_o && !m_r_ready_i;
            prev_aw     <= m_aw_o;
            prev_ar     <= m_ar_o;
            prev_b      <= m_b_o;
            prev_r      <= m_r_o;
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
    end

    task automatic send_aw(input int id, input int dst);
        @(negedge clk_i);
        s_aw_i                           = '0;
        s_aw_i.axi.awid                  = ID_W'(id);
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
        s_b_i.axi.bid           = ID_W'(id);
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
        s_ar_i.axi.arid            = ID_W'(id);
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
        s_r_i.axi.rid           = ID_W'(id);
        s_r_i.axi.rdata         = ni_params_pkg::AXI_DATA_WIDTH'(data);
        s_r_i.axi.rlast         = last;
        s_r_i.meta.ordering_req = ordered;
        s_r_i.meta.ordering_tag = TAG_W'(tag);
        s_r_valid_i             = 1;
        do @(posedge clk_i); while (!s_r_ready_o);
        @(negedge clk_i);
        s_r_valid_i = 0;
    endtask

    task automatic reset_dut;
        @(negedge clk_i);
        rst_n_i      = 0;
        s_aw_i       = '0; s_w_i = '0; s_ar_i = '0; s_b_i = '0; s_r_i = '0;
        s_aw_valid_i = 0; s_w_valid_i = 0; s_ar_valid_i = 0;
        s_b_valid_i  = 0; s_r_valid_i = 0;
        m_aw_ready_i = 1; m_w_ready_i = 1; m_ar_ready_i = 1;
        m_b_ready_i  = 1; m_r_ready_i = 1;
        repeat (3) @(negedge clk_i);
        rst_n_i = 1;
    endtask

    initial begin
        reset_dut();
        // Make slot zero live, then offer slot one on a stalled channel.
        send_aw(0, 1); send_aw(0, 2); send_aw(1, 1);
        send_ar(0, 1, 0); send_ar(0, 2, 0); send_ar(1, 1, 0);
        @(negedge clk_i);
        m_aw_ready_i                     = 0; m_ar_ready_i = 0;
        s_aw_i                           = '0; s_aw_i.axi.awid = ID_W'(1);
        s_aw_i.route.route.domain.dst_id = ni_flit_pkg::DST_ID_WIDTH'(2);
        s_ar_i                           = '0; s_ar_i.axi.arid = ID_W'(1); s_ar_i.axi.arlen = 1;
        s_ar_i.route.domain.dst_id       = ni_flit_pkg::DST_ID_WIDTH'(2);
        s_aw_valid_i                     = 1; s_ar_valid_i = 1;
        #1;
        if (!m_aw_valid_o || !m_ar_valid_o || !m_aw_o.meta.ordering_req ||
                !m_ar_o.meta.ordering_req || m_aw_o.meta.ordering_tag != 1 ||
                m_ar_o.meta.ordering_tag != 1) $fatal(1, "offered tag setup failed");
        send_b(0, 0, 0, 0); send_b(0, 1, 0, 1); send_b(1, 0, 0, 0);
        send_r(0, 0, 0, 32'h10, 1); send_r(0, 1, 0, 32'h20, 1);
        send_r(1, 0, 0, 32'h30, 1);
        #1;
        if (m_aw_o.meta.ordering_tag != 1 || m_ar_o.meta.ordering_tag != 1 ||
                !m_aw_o.meta.ordering_req || !m_ar_o.meta.ordering_req)
            $fatal(1, "offered metadata changed after earlier retirement");
        m_aw_ready_i = 1; m_ar_ready_i = 1;
        @(posedge clk_i); @(negedge clk_i);
        s_aw_valid_i = 0; s_ar_valid_i = 0;
        send_b(1, 1, 1, 2);
        send_r(1, 1, 1, 32'h41, 0); send_r(1, 1, 1, 32'h42, 1);
        if (b_retire_count != 4 || r_retire_count != 5 ||
                retired_b[3].bresp != 2 || retired_r[3].rdata != ni_params_pkg::AXI_DATA_WIDTH'(32'h41) ||
                retired_r[4].rdata != ni_params_pkg::AXI_DATA_WIDTH'(32'h42))
            $fatal(1, "held tag did not reserve and retire its range");
        send_aw(1, 3);
        if (last_aw_ordering_req) $fatal(1, "idle write ID did not return to bypass");
        send_aw(1, 4);
        if (!last_aw_ordering_req || last_aw_ordering_tag != 0)
            $fatal(1, "write slots were not reclaimed");
        send_ar(1, 3, 0);
        if (last_ar_ordering_req) $fatal(1, "idle read ID did not return to bypass");
        send_ar(1, 4, 0);
        if (!last_ar_ordering_req || last_ar_ordering_tag != 0)
            $fatal(1, "read slots were not reclaimed");

        reset_dut();
        // A later completed ID must not displace a stalled buffered winner.
        send_aw(1, 1); send_aw(1, 2); send_aw(2, 1); send_aw(2, 2);
        send_b(1, 1, 0, 1);
        send_b(2, 0, 0, 0);
        send_b(1, 0, 0, 0);
        m_b_ready_i = 0;
        #1;
        if (!m_b_valid_o || m_b_o.bid != ID_W'(1)) $fatal(1, "B winner setup failed");
        send_b(2, 1, 1, 2);
        repeat (3) @(negedge clk_i);
        m_b_ready_i = 1;
        repeat (2) @(posedge clk_i);
        @(negedge clk_i);
        if (b_retire_count != 4 || retired_b[2].bid != ID_W'(1) ||
                retired_b[3].bid != ID_W'(2) || retired_b[3].bresp != 2 ||
                retired_b_cycle[3] != retired_b_cycle[2] + 1)
            $fatal(1, "B selection hold lost ordering or full throughput");

        // First beat forwards directly, second fills while another ID is held.
        send_ar(1, 1, 0); send_ar(1, 2, 0);
        send_ar(2, 1, 0); send_ar(2, 2, 1);
        send_r(1, 1, 0, 32'h100, 1);
        send_r(2, 0, 0, 32'h200, 1);
        send_r(2, 1, 1, 32'h201, 0);
        send_r(1, 0, 0, 32'h101, 1);
        m_r_ready_i = 0;
        #1;
        if (!m_r_valid_o || m_r_o.rid != ID_W'(1)) $fatal(1, "R winner setup failed");
        send_r(2, 1, 1, 32'h202, 1);
        repeat (3) @(negedge clk_i);
        m_r_ready_i = 1;
        repeat (2) @(posedge clk_i);
        @(negedge clk_i);
        if (r_retire_count != 5 || retired_r[3].rid != ID_W'(1) ||
                retired_r[4].rid != ID_W'(2) || retired_r[4].rdata != ni_params_pkg::AXI_DATA_WIDTH'(32'h202) ||
                !retired_r[4].rlast || retired_r_cycle[4] != retired_r_cycle[3] + 1)
            $fatal(1, "mixed direct/buffered R burst did not retire correctly");

        // Reset cancels both pending offers and response selection locks.
        send_aw(3, 1);
        @(negedge clk_i);
        m_aw_ready_i                     = 0;
        s_aw_i.route.route.domain.dst_id = ni_flit_pkg::DST_ID_WIDTH'(2);
        s_aw_valid_i                     = 1;
        m_b_ready_i                      = 0;
        s_b_i                            = '0; s_b_i.axi.bid = ID_W'(3); s_b_valid_i = 1;
        repeat (2) @(negedge clk_i);
        reset_dut();
        send_aw(3, 2);
        if (last_aw_ordering_req) $fatal(1, "reset retained held ordering state");
        send_b(3, 0, 0, 0);
        if (b_retire_count != 1) $fatal(1, "reset replayed a stale response");
        $display("PASS: request metadata, tag reservation, B/R stalls, mixed R paths and reset");
        $finish;
    end
    initial begin #20us; $fatal(1, "ordering stall test timeout"); end
endmodule
