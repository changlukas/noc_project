// SPDX-License-Identifier: Apache-2.0

`resetall
`timescale 1ns / 1ps
`default_nettype none

/* Shared NMU request-ordering and response-reordering subsystem. */
module nmu_ordering #(
    parameter int unsigned NMU_ROB_B_DEPTH = ni_params_pkg::NMU_ROB_B_DEPTH_DFLT,
    parameter int unsigned NMU_ROB_R_DEPTH = ni_params_pkg::NMU_ROB_R_DEPTH_DFLT,
    parameter int unsigned NMU_MAX_TXNS_PER_ID = ni_params_pkg::NMU_MAX_TXNS_PER_ID_DFLT,
    parameter bit READ_ROB_ENABLED = bit'(ni_params_pkg::NMU_READ_ROB_ENABLED_DFLT)
) (
    input  wire logic                                      clk_i,
    input  wire logic                                      rst_i,
    input  wire ni_child_types_pkg::nmu_sam_aw_result_t   s_aw_i,
    input  wire logic                                      s_aw_valid_i,
    output wire logic                                      s_aw_ready_o,
    output wire ni_child_types_pkg::nmu_aw_request_t       m_aw_o,
    output wire logic                                      m_aw_valid_o,
    input  wire logic                                      m_aw_ready_i,
    input  wire ni_signals_pkg::axi_w_t                   s_w_i,
    input  wire logic                                      s_w_valid_i,
    output wire logic                                      s_w_ready_o,
    output wire ni_signals_pkg::axi_w_t                   m_w_o,
    output wire logic                                      m_w_valid_o,
    input  wire logic                                      m_w_ready_i,
    input  wire ni_child_types_pkg::nmu_sam_ar_result_t   s_ar_i,
    input  wire logic                                      s_ar_valid_i,
    output wire logic                                      s_ar_ready_o,
    output wire ni_child_types_pkg::nmu_ar_request_t       m_ar_o,
    output wire logic                                      m_ar_valid_o,
    input  wire logic                                      m_ar_ready_i,
    input  wire ni_child_types_pkg::nmu_b_response_t       s_b_i,
    input  wire logic                                      s_b_valid_i,
    output wire logic                                      s_b_ready_o,
    output wire ni_signals_pkg::axi_b_t                   m_b_o,
    output wire logic                                      m_b_valid_o,
    input  wire logic                                      m_b_ready_i,
    input  wire ni_child_types_pkg::nmu_r_response_t       s_r_i,
    input  wire logic                                      s_r_valid_i,
    output wire logic                                      s_r_ready_o,
    output wire ni_signals_pkg::axi_r_t                   m_r_o,
    output wire logic                                      m_r_valid_o,
    input  wire logic                                      m_r_ready_i
);

    localparam int unsigned ID_W = $bits(s_aw_i.axi.awid);
    localparam int unsigned NUM_IDS = 1 << ID_W;
    localparam int unsigned TAG_SPACE = 1 << ni_flit_pkg::ORDERING_TAG_WIDTH;
    localparam int unsigned BEAT_COUNT_W = ni_flit_pkg::AXI_LEN_WIDTH + 1;
    localparam int unsigned CL_MAX_TXNS = $clog2(NMU_MAX_TXNS_PER_ID + 1);
    localparam int unsigned CL_NUM_IDS = $clog2(NUM_IDS);

    if (NMU_ROB_B_DEPTH < 1 || NMU_ROB_B_DEPTH > TAG_SPACE) begin : gen_invalid_b_depth
        $fatal(0, "Error: NMU_ROB_B_DEPTH must be in [1, TAG_SPACE] (instance %m)");
    end
    if (NMU_ROB_R_DEPTH < 1 || NMU_ROB_R_DEPTH > TAG_SPACE) begin : gen_invalid_r_depth
        $fatal(0, "Error: NMU_ROB_R_DEPTH must be in [1, TAG_SPACE] (instance %m)");
    end
    if (NMU_MAX_TXNS_PER_ID < 1 || NMU_MAX_TXNS_PER_ID > TAG_SPACE) begin : gen_invalid_max_txns
        $fatal(0, "Error: NMU_MAX_TXNS_PER_ID must be in [1, TAG_SPACE] (instance %m)");
    end
    if ($bits(s_ar_i.axi.arid) != ID_W || $bits(s_b_i.axi.bid) != ID_W ||
        $bits(s_r_i.axi.rid) != ID_W) begin : gen_mismatched_id_width
        $fatal(0, "Error: AW, AR, B, and R ID widths must match (instance %m)");
    end

    typedef ni_child_types_pkg::nmu_rob_order_entry_t order_entry_t;
    typedef ni_child_types_pkg::nmu_ordering_domain_t domain_t;

    order_entry_t write_order_reg [NUM_IDS][NMU_MAX_TXNS_PER_ID];
    order_entry_t read_order_reg [NUM_IDS][NMU_MAX_TXNS_PER_ID];
    logic [CL_MAX_TXNS-1:0] write_head_reg [NUM_IDS], write_tail_reg [NUM_IDS];
    logic [CL_MAX_TXNS-1:0] read_head_reg [NUM_IDS], read_tail_reg [NUM_IDS];
    logic [CL_MAX_TXNS-1:0] write_usage_reg [NUM_IDS], read_usage_reg [NUM_IDS];
    domain_t write_domain_reg [NUM_IDS], read_domain_reg [NUM_IDS];
    logic [NUM_IDS-1:0] write_sticky_reg = '0, read_sticky_reg = '0;
    logic [NUM_IDS-1:0] write_collective_reg = '0;

    logic [ni_flit_pkg::AXI_LEN_WIDTH:0] r_retire_offset_reg [NUM_IDS];

    logic [CL_NUM_IDS-1:0] b_rr_reg = '0, r_rr_reg = '0;
    logic [ni_flit_pkg::ORDERING_TAG_WIDTH:0] b_free_count, r_free_count;
    logic [ni_flit_pkg::ORDERING_TAG_WIDTH-1:0] b_next_base, r_next_base;
    logic aw_reorder, ar_reorder, aw_can_accept, ar_can_accept;
    order_entry_t write_head [NUM_IDS], read_head [NUM_IDS];
    logic [NUM_IDS-1:0] b_buffer_ready, r_buffer_ready;
    logic b_select_valid, r_select_valid;
    logic [CL_NUM_IDS-1:0] b_select_id, r_select_id;
    logic b_direct, r_direct;
    logic [CL_NUM_IDS-1:0] b_retire_id, r_retire_id;
    logic [NMU_ROB_B_DEPTH-1:0] b_complete;
    logic [NMU_ROB_R_DEPTH-1:0] r_complete;
    logic [ni_flit_pkg::ORDERING_TAG_WIDTH-1:0] b_peek_addr, r_peek_addr;
    logic [ni_flit_pkg::ORDERING_TAG_WIDTH-1:0] b_release_addr, r_release_addr;
    ni_signals_pkg::axi_b_t b_peek_data;
    ni_signals_pkg::axi_r_t r_peek_data;
    logic b_fill_ready, r_fill_ready;
    wire logic [BEAT_COUNT_W-1:0] ar_beat_count =
        BEAT_COUNT_W'(s_ar_i.axi.arlen) + BEAT_COUNT_W'(1);
    wire logic aw_accept = s_aw_valid_i && s_aw_ready_o;
    wire logic ar_accept = s_ar_valid_i && s_ar_ready_o;
    wire logic b_retire = m_b_valid_o && m_b_ready_i;
    wire logic r_retire = m_r_valid_o && m_r_ready_i;

    always_comb begin
        for (int n = 0; n < NUM_IDS; n++) begin
            write_head[n] = write_order_reg[n][write_head_reg[n]];
            read_head[n] = read_order_reg[n][read_head_reg[n]];
            b_buffer_ready[n] = write_usage_reg[n] != 0 && write_head[n].ordering_req &&
                b_complete[write_head[n].base];
            r_buffer_ready[n] = READ_ROB_ENABLED && read_usage_reg[n] != 0 &&
                read_head[n].ordering_req &&
                r_complete[read_head[n].base + r_retire_offset_reg[n]];
        end
    end

    always_comb begin
        aw_reorder = 1'b0;
        if (write_usage_reg[s_aw_i.axi.awid] != 0) begin
            aw_reorder = write_sticky_reg[s_aw_i.axi.awid] ||
                write_domain_reg[s_aw_i.axi.awid] != s_aw_i.route.route.domain;
        end
        aw_can_accept = write_usage_reg[s_aw_i.axi.awid] < NMU_MAX_TXNS_PER_ID &&
            !write_collective_reg[s_aw_i.axi.awid] &&
            (s_aw_i.route.collective_op == 0 || write_usage_reg[s_aw_i.axi.awid] == 0) &&
            (!aw_reorder || b_free_count >= 1);

        ar_reorder = 1'b0;
        if (read_usage_reg[s_ar_i.axi.arid] != 0) begin
            ar_reorder = read_sticky_reg[s_ar_i.axi.arid] ||
                read_domain_reg[s_ar_i.axi.arid] != s_ar_i.route.domain;
        end
        ar_can_accept = read_usage_reg[s_ar_i.axi.arid] < NMU_MAX_TXNS_PER_ID &&
            (READ_ROB_ENABLED ?
                (!ar_reorder || r_free_count >= ar_beat_count) :
                !ar_reorder);
    end

    assign m_aw_o.axi = s_aw_i.axi;
    assign m_aw_o.meta.route = s_aw_i.route.route;
    assign m_aw_o.meta.ordering_req = aw_reorder;
    assign m_aw_o.meta.ordering_tag = aw_reorder ? b_next_base : '0;
    assign m_aw_o.user = s_aw_i.route.user;
    assign m_aw_o.collective_op = s_aw_i.route.collective_op;
    assign m_aw_o.collective_mask = s_aw_i.route.collective_mask;
    assign m_aw_valid_o = s_aw_valid_i && aw_can_accept;
    assign s_aw_ready_o = m_aw_ready_i && aw_can_accept;

    assign m_w_o = s_w_i;
    assign m_w_valid_o = s_w_valid_i;
    assign s_w_ready_o = m_w_ready_i;

    assign m_ar_o.axi = s_ar_i.axi;
    assign m_ar_o.meta.route = s_ar_i.route;
    assign m_ar_o.meta.ordering_req = READ_ROB_ENABLED && ar_reorder;
    assign m_ar_o.meta.ordering_tag = READ_ROB_ENABLED && ar_reorder ? r_next_base : '0;
    assign m_ar_valid_o = s_ar_valid_i && ar_can_accept;
    assign s_ar_ready_o = m_ar_ready_i && ar_can_accept;

    always_comb begin
        b_select_valid = 1'b0;
        b_select_id = b_rr_reg;
        for (int offset = NUM_IDS-1; offset >= 0; offset--) begin
            int candidate;
            candidate = (b_rr_reg + offset) % NUM_IDS;
            if (b_buffer_ready[candidate]) begin
                b_select_valid = 1'b1;
                b_select_id = CL_NUM_IDS'(candidate);
            end
        end
        b_direct = !b_select_valid && s_b_valid_i &&
            write_usage_reg[s_b_i.axi.bid] != 0 &&
            ((!write_head[s_b_i.axi.bid].ordering_req && !s_b_i.meta.ordering_req) ||
             (write_head[s_b_i.axi.bid].ordering_req && s_b_i.meta.ordering_req &&
              write_head[s_b_i.axi.bid].base == s_b_i.meta.ordering_tag));
    end

    assign m_b_valid_o = b_select_valid || b_direct;
    assign b_peek_addr = write_head[b_select_id].base;
    assign b_release_addr = write_head[b_retire_id].base;
    assign m_b_o = b_select_valid ? b_peek_data : s_b_i.axi;
    assign s_b_ready_o = b_direct ? m_b_ready_i :
        (s_b_i.meta.ordering_req && b_fill_ready);
    assign b_retire_id = b_select_valid ? b_select_id : s_b_i.axi.bid;

    always_comb begin
        r_select_valid = 1'b0;
        r_select_id = r_rr_reg;
        for (int offset = NUM_IDS-1; offset >= 0; offset--) begin
            int candidate;
            candidate = (r_rr_reg + offset) % NUM_IDS;
            if (r_buffer_ready[candidate]) begin
                r_select_valid = 1'b1;
                r_select_id = CL_NUM_IDS'(candidate);
            end
        end
        r_direct = !r_select_valid && s_r_valid_i && read_usage_reg[s_r_i.axi.rid] != 0 &&
            ((!read_head[s_r_i.axi.rid].ordering_req && !s_r_i.meta.ordering_req) ||
             (read_head[s_r_i.axi.rid].ordering_req && s_r_i.meta.ordering_req &&
              read_head[s_r_i.axi.rid].base == s_r_i.meta.ordering_tag));
    end

    assign m_r_valid_o = r_select_valid || r_direct;
    assign r_peek_addr = read_head[r_select_id].base + r_retire_offset_reg[r_select_id];
    assign r_release_addr = read_head[r_retire_id].base + r_retire_offset_reg[r_retire_id];
    assign m_r_o = r_select_valid ? r_peek_data : s_r_i.axi;
    assign s_r_ready_o = r_direct ? m_r_ready_i :
        (READ_ROB_ENABLED && s_r_i.meta.ordering_req && r_fill_ready);
    assign r_retire_id = r_select_valid ? r_select_id : s_r_i.axi.rid;

    always_ff @(posedge clk_i) begin
        if (aw_accept) begin
            int id;
            id = s_aw_i.axi.awid;
            write_order_reg[id][write_tail_reg[id]] <= '{
                base: aw_reorder ? b_next_base : '0,
                beat_count: 1,
                ordering_req: aw_reorder,
                collective: s_aw_i.route.collective_op != 0
            };
            write_tail_reg[id] <= write_tail_reg[id] == NMU_MAX_TXNS_PER_ID-1 ? '0 : write_tail_reg[id] + 1'b1;
            write_domain_reg[id] <= s_aw_i.route.route.domain;
            if (s_aw_i.route.collective_op != 0) write_collective_reg[id] <= 1'b1;
        end

        if (ar_accept) begin
            int id;
            id = s_ar_i.axi.arid;
            read_order_reg[id][read_tail_reg[id]] <= '{
                base: READ_ROB_ENABLED && ar_reorder ? r_next_base : '0,
                beat_count: ar_beat_count,
                ordering_req: READ_ROB_ENABLED && ar_reorder,
                collective: 1'b0
            };
            read_tail_reg[id] <= read_tail_reg[id] == NMU_MAX_TXNS_PER_ID-1 ? '0 : read_tail_reg[id] + 1'b1;
            read_domain_reg[id] <= s_ar_i.route.domain;
        end

        if (b_retire) begin
            int id;
            id = b_retire_id;
            write_head_reg[id] <= write_head_reg[id] == NMU_MAX_TXNS_PER_ID-1 ? '0 : write_head_reg[id] + 1'b1;
            if (write_head[id].collective) write_collective_reg[id] <= 1'b0;
            b_rr_reg <= CL_NUM_IDS'(id + 1);
        end

        if (r_retire) begin
            int id;
            id = r_retire_id;
            if (read_head[id].beat_count == 1 || m_r_o.rlast) begin
                read_head_reg[id] <= read_head_reg[id] == NMU_MAX_TXNS_PER_ID-1 ? '0 : read_head_reg[id] + 1'b1;
                r_retire_offset_reg[id] <= '0;
            end else begin
                read_order_reg[id][read_head_reg[id]].beat_count <= read_head[id].beat_count - 1'b1;
                r_retire_offset_reg[id] <= r_retire_offset_reg[id] + 1'b1;
            end
            r_rr_reg <= CL_NUM_IDS'(id + 1);
        end

        // Request-side state is evaluated before same-cycle retirement, matching
        // the specified NMU tick order.  Combined updates prevent lost counts.
        for (int id = 0; id < NUM_IDS; id++) begin
            case ({aw_accept && s_aw_i.axi.awid == id, b_retire && b_retire_id == id})
                2'b10: write_usage_reg[id] <= write_usage_reg[id] + 1'b1;
                2'b01: write_usage_reg[id] <= write_usage_reg[id] - 1'b1;
                default: write_usage_reg[id] <= write_usage_reg[id];
            endcase
            if (aw_accept && s_aw_i.axi.awid == id) begin
                if (write_usage_reg[id] == 0) write_sticky_reg[id] <= 1'b0;
                else if (aw_reorder) write_sticky_reg[id] <= 1'b1;
            end else if (b_retire && b_retire_id == id && write_usage_reg[id] == 1) begin
                write_sticky_reg[id] <= 1'b0;
            end

            case ({ar_accept && s_ar_i.axi.arid == id,
                   r_retire && r_retire_id == id && (read_head[id].beat_count == 1 || m_r_o.rlast)})
                2'b10: read_usage_reg[id] <= read_usage_reg[id] + 1'b1;
                2'b01: read_usage_reg[id] <= read_usage_reg[id] - 1'b1;
                default: read_usage_reg[id] <= read_usage_reg[id];
            endcase
            if (ar_accept && s_ar_i.axi.arid == id) begin
                if (read_usage_reg[id] == 0) read_sticky_reg[id] <= 1'b0;
                else if (READ_ROB_ENABLED && ar_reorder) read_sticky_reg[id] <= 1'b1;
            end else if (r_retire && r_retire_id == id && read_usage_reg[id] == 1 &&
                         (read_head[id].beat_count == 1 || m_r_o.rlast)) begin
                read_sticky_reg[id] <= 1'b0;
            end
        end

        if (rst_i) begin
            write_sticky_reg <= '0;
            read_sticky_reg <= '0;
            write_collective_reg <= '0;
            b_rr_reg <= '0;
            r_rr_reg <= '0;
            for (int id = 0; id < NUM_IDS; id++) begin
                write_head_reg[id] <= '0; write_tail_reg[id] <= '0; write_usage_reg[id] <= '0;
                read_head_reg[id] <= '0; read_tail_reg[id] <= '0; read_usage_reg[id] <= '0;
                write_domain_reg[id] <= '0; read_domain_reg[id] <= '0;
                r_retire_offset_reg[id] <= '0;
            end
        end
    end

    nmu_reorder_storage #(
        .DEPTH (NMU_ROB_B_DEPTH), .T (ni_signals_pkg::axi_b_t)
    ) i_b_storage (
        .clk_i, .rst_i,
        .reserve_valid_i (aw_accept && aw_reorder), .reserve_count_i (1),
        .next_base_o (b_next_base), .free_count_o (b_free_count),
        .fill_valid_i (s_b_valid_i && !b_direct && s_b_i.meta.ordering_req),
        .fill_ready_o (b_fill_ready), .fill_base_i (s_b_i.meta.ordering_tag),
        .fill_last_i (1'b1), .fill_data_i (s_b_i.axi),
        .peek_addr_i (b_peek_addr), .peek_complete_o (), .peek_data_o (b_peek_data),
        .release_valid_i (b_retire && write_head[b_retire_id].ordering_req),
        .release_addr_i (b_release_addr), .complete_o (b_complete)
    );

    if (READ_ROB_ENABLED) begin : gen_read_reorder_storage
        nmu_reorder_storage #(
            .DEPTH (NMU_ROB_R_DEPTH), .T (ni_signals_pkg::axi_r_t)
        ) i_r_storage (
            .clk_i, .rst_i,
            .reserve_valid_i (ar_accept && ar_reorder),
            .reserve_count_i (ar_beat_count),
            .next_base_o (r_next_base), .free_count_o (r_free_count),
            .fill_valid_i (s_r_valid_i && !r_direct && s_r_i.meta.ordering_req),
            .fill_ready_o (r_fill_ready), .fill_base_i (s_r_i.meta.ordering_tag),
            .fill_last_i (s_r_i.axi.rlast), .fill_data_i (s_r_i.axi),
            .peek_addr_i (r_peek_addr), .peek_complete_o (), .peek_data_o (r_peek_data),
            .release_valid_i (r_retire && read_head[r_retire_id].ordering_req),
            .release_addr_i (r_release_addr), .complete_o (r_complete)
        );
    end else begin : gen_no_read_reorder_storage
        assign r_next_base = '0;
        assign r_free_count = '0;
        assign r_fill_ready = 1'b0;
        assign r_peek_data = '0;
        assign r_complete = '0;
    end

endmodule

`resetall
