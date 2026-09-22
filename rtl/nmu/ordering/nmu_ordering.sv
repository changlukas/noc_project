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

    ni_signals_pkg::axi_r_t retire_response;
    assign m_r_o = retire_response;

    localparam int unsigned ID_W = $bits(s_aw_i.axi.awid);
    localparam int unsigned NUM_IDS = 1 << ID_W;
    localparam int unsigned TAG_W = ni_flit_pkg::ORDERING_TAG_WIDTH;
    localparam int unsigned TAG_SPACE = 1 << TAG_W;
    localparam int unsigned BEAT_COUNT_W = ni_flit_pkg::AXI_LEN_WIDTH + 1;
    localparam int unsigned CL_MAX_TXNS = $clog2(NMU_MAX_TXNS_PER_ID + 1);
    localparam int unsigned CL_ORDER = NMU_MAX_TXNS_PER_ID > 1 ? $clog2(NMU_MAX_TXNS_PER_ID) : 1;
    localparam int unsigned CL_NUM_IDS = $clog2(NUM_IDS);
    localparam int unsigned CL_B_DEPTH = NMU_ROB_B_DEPTH > 1 ? $clog2(NMU_ROB_B_DEPTH) : 1;
    localparam int unsigned CL_R_DEPTH = NMU_ROB_R_DEPTH > 1 ? $clog2(NMU_ROB_R_DEPTH) : 1;

    if (NMU_ROB_B_DEPTH < 1 || NMU_ROB_B_DEPTH > TAG_SPACE) begin : gen_invalid_b_depth
        initial $fatal(0, "Error: NMU_ROB_B_DEPTH must be in [1, TAG_SPACE] (instance %m)");
    end
    if (NMU_ROB_R_DEPTH < 1 || NMU_ROB_R_DEPTH > TAG_SPACE) begin : gen_invalid_r_depth
        initial $fatal(0, "Error: NMU_ROB_R_DEPTH must be in [1, TAG_SPACE] (instance %m)");
    end
    if (NMU_MAX_TXNS_PER_ID < 1 || NMU_MAX_TXNS_PER_ID > TAG_SPACE) begin : gen_invalid_max_txns
        initial $fatal(0, "Error: NMU_MAX_TXNS_PER_ID must be in [1, TAG_SPACE] (instance %m)");
    end
    if ($bits(s_ar_i.axi.arid) != ID_W || $bits(s_b_i.axi.bid) != ID_W ||
            $bits(s_r_i.axi.rid) != ID_W) begin : gen_mismatched_id_width
        initial $fatal(0, "Error: AW, AR, B, and R ID widths must match (instance %m)");
    end

    typedef ni_child_types_pkg::nmu_rob_order_entry_t order_entry_t;
    typedef ni_child_types_pkg::nmu_ordering_domain_t domain_t;

    localparam int unsigned BYTE_OFFSET_W = $clog2(ni_params_pkg::AXI_DATA_WIDTH_DFLT/8);
    typedef struct packed {
        logic [BYTE_OFFSET_W-1:0] addr;
        logic [7:0] len;
        logic [2:0] size;
        logic [1:0] burst;
        logic is_data;
    } read_lane_context_t;
    read_lane_context_t rd_lane_context_reg [NUM_IDS][NMU_MAX_TXNS_PER_ID];
    read_lane_context_t retire_context;
    ni_signals_pkg::axi_r_t retire_r;
    int unsigned retire_byte_addr, retire_step, retire_span, retire_lane;

    order_entry_t wr_order_reg [NUM_IDS][NMU_MAX_TXNS_PER_ID];
    order_entry_t rd_order_reg [NUM_IDS][NMU_MAX_TXNS_PER_ID];
    logic [CL_ORDER-1:0] wr_order_rd_ptr_reg [NUM_IDS], wr_order_rd_ptr_next [NUM_IDS];
    logic [CL_ORDER-1:0] wr_order_wr_ptr_reg [NUM_IDS], wr_order_wr_ptr_next [NUM_IDS];
    logic [CL_ORDER-1:0] rd_order_rd_ptr_reg [NUM_IDS], rd_order_rd_ptr_next [NUM_IDS];
    logic [CL_ORDER-1:0] rd_order_wr_ptr_reg [NUM_IDS], rd_order_wr_ptr_next [NUM_IDS];
    logic [CL_MAX_TXNS-1:0] wr_outstanding_cnt_reg [NUM_IDS], wr_outstanding_cnt_next [NUM_IDS];
    logic [CL_MAX_TXNS-1:0] rd_outstanding_cnt_reg [NUM_IDS], rd_outstanding_cnt_next [NUM_IDS];
    domain_t wr_domain_reg [NUM_IDS], wr_domain_next [NUM_IDS];
    domain_t rd_domain_reg [NUM_IDS], rd_domain_next [NUM_IDS];
    logic [BEAT_COUNT_W-1:0] r_retire_offset_reg [NUM_IDS], r_retire_offset_next [NUM_IDS];
    logic [NUM_IDS-1:0] wr_reorder_active_reg, wr_reorder_active_next;
    logic [NUM_IDS-1:0] rd_reorder_active_reg, rd_reorder_active_next;
    logic [NUM_IDS-1:0] wr_collective_active_reg, wr_collective_active_next;
    logic [CL_NUM_IDS-1:0] b_rr_reg, b_rr_next;
    logic [CL_NUM_IDS-1:0] r_rr_reg, r_rr_next;
    logic aw_hold_reg, aw_hold_next;
    logic ar_hold_reg, ar_hold_next;
    logic aw_hold_reorder_reg, aw_hold_reorder_next;
    logic ar_hold_reorder_reg, ar_hold_reorder_next;
    logic [TAG_W-1:0] aw_hold_tag_reg, aw_hold_tag_next;
    logic [TAG_W-1:0] ar_hold_tag_reg, ar_hold_tag_next;
    logic b_hold_reg, b_hold_next;
    logic r_hold_reg, r_hold_next;
    logic b_hold_direct_reg, b_hold_direct_next;
    logic r_hold_direct_reg, r_hold_direct_next;
    logic [CL_NUM_IDS-1:0] b_hold_id_reg, b_hold_id_next;
    logic [CL_NUM_IDS-1:0] r_hold_id_reg, r_hold_id_next;

    logic [TAG_W:0] b_free_cnt, r_free_cnt;
    logic [TAG_W-1:0] b_next_base, r_next_base, aw_tag, ar_tag;
    logic aw_reorder, ar_reorder, aw_can_accept, ar_can_accept;
    logic aw_reorder_required, ar_reorder_required;
    order_entry_t wr_order_head [NUM_IDS], rd_order_head [NUM_IDS];
    logic [NUM_IDS-1:0] b_buffer_ready, r_buffer_ready;
    logic b_sel_valid, r_sel_valid;
    logic [CL_NUM_IDS-1:0] b_sel_id, r_sel_id;
    logic b_direct, r_direct;
    logic [CL_NUM_IDS-1:0] b_retire_id, r_retire_id;
    logic [NMU_ROB_B_DEPTH-1:0] b_complete;
    logic [NMU_ROB_R_DEPTH-1:0] r_complete;
    logic [TAG_W-1:0] b_storage_rd_addr, r_storage_rd_addr, b_storage_free_addr, r_storage_free_addr;
    ni_signals_pkg::axi_b_t b_storage_rd_data;
    ni_signals_pkg::axi_r_t r_storage_rd_data;
    logic b_storage_wr_ready, r_storage_wr_ready;
    wire logic [BEAT_COUNT_W-1:0] ar_beat_cnt =
        BEAT_COUNT_W'(s_ar_i.axi.arlen) + BEAT_COUNT_W'(1);
    wire logic aw_accept = s_aw_valid_i && s_aw_ready_o;
    wire logic ar_accept = s_ar_valid_i && s_ar_ready_o;
    wire logic b_retire = m_b_valid_o && m_b_ready_i;
    wire logic r_retire = m_r_valid_o && m_r_ready_i;
    wire logic r_retire_last = rd_order_head[r_retire_id].beat_count == 1 || m_r_o.rlast;

    always_comb begin
        for (int n = 0; n < NUM_IDS; n++) begin
            wr_order_head[n] = wr_order_reg[n][wr_order_rd_ptr_reg[n]];
            rd_order_head[n] = rd_order_reg[n][rd_order_rd_ptr_reg[n]];
            b_buffer_ready[n] = wr_outstanding_cnt_reg[n] != 0 && wr_order_head[n].ordering_req &&
                int'(wr_order_head[n].base) < NMU_ROB_B_DEPTH &&
                b_complete[CL_B_DEPTH'(wr_order_head[n].base)];
            r_buffer_ready[n] = READ_ROB_ENABLED && rd_outstanding_cnt_reg[n] != 0 &&
                rd_order_head[n].ordering_req &&
                int'(rd_order_head[n].base) + int'(r_retire_offset_reg[n]) < NMU_ROB_R_DEPTH &&
                r_complete[CL_R_DEPTH'($unsigned(int'(rd_order_head[n].base) + int'(r_retire_offset_reg[n])))];
        end
    end

    always_comb begin
        aw_reorder_required = wr_outstanding_cnt_reg[s_aw_i.axi.awid] != 0 &&
            (wr_reorder_active_reg[s_aw_i.axi.awid] ||
             wr_domain_reg[s_aw_i.axi.awid] != s_aw_i.route.route.domain);
        ar_reorder_required = rd_outstanding_cnt_reg[s_ar_i.axi.arid] != 0 &&
            (rd_reorder_active_reg[s_ar_i.axi.arid] ||
             rd_domain_reg[s_ar_i.axi.arid] != s_ar_i.route.domain);
        aw_reorder = aw_hold_reg ? aw_hold_reorder_reg : aw_reorder_required;
        ar_reorder = ar_hold_reg ? ar_hold_reorder_reg :
            READ_ROB_ENABLED && ar_reorder_required;
        aw_tag = aw_hold_reg ? aw_hold_tag_reg : (aw_reorder ? b_next_base : '0);
        ar_tag = ar_hold_reg ? ar_hold_tag_reg : (ar_reorder ? r_next_base : '0);
        aw_can_accept = aw_hold_reg ||
            (wr_outstanding_cnt_reg[s_aw_i.axi.awid] < CL_MAX_TXNS'(NMU_MAX_TXNS_PER_ID) &&
             !wr_collective_active_reg[s_aw_i.axi.awid] &&
             (s_aw_i.route.collective_op == 0 || wr_outstanding_cnt_reg[s_aw_i.axi.awid] == 0) &&
             (!aw_reorder || b_free_cnt != 0));
        ar_can_accept = ar_hold_reg ||
            (rd_outstanding_cnt_reg[s_ar_i.axi.arid] < CL_MAX_TXNS'(NMU_MAX_TXNS_PER_ID) &&
             (READ_ROB_ENABLED ? (!ar_reorder || r_free_cnt >= (TAG_W+1)'(ar_beat_cnt)) :
              !ar_reorder_required));
    end

    assign m_aw_o.axi = s_aw_i.axi;
    assign m_aw_o.meta.route = s_aw_i.route.route;
    assign m_aw_o.meta.ordering_req = aw_reorder;
    assign m_aw_o.meta.ordering_tag = aw_tag;
    assign m_aw_o.user = s_aw_i.route.user;
    assign m_aw_o.collective_op = s_aw_i.route.collective_op;
    assign m_aw_o.collective_mask = s_aw_i.route.collective_mask;
    assign m_aw_valid_o = !rst_i && s_aw_valid_i && aw_can_accept;
    assign s_aw_ready_o = !rst_i && m_aw_ready_i && aw_can_accept;
    assign m_w_o = s_w_i;
    assign m_w_valid_o = !rst_i && s_w_valid_i;
    assign s_w_ready_o = !rst_i && m_w_ready_i;
    assign m_ar_o.axi = s_ar_i.axi;
    assign m_ar_o.meta.route = s_ar_i.route;
    assign m_ar_o.meta.ordering_req = ar_reorder;
    assign m_ar_o.meta.ordering_tag = ar_tag;
    assign m_ar_valid_o = !rst_i && s_ar_valid_i && ar_can_accept;
    assign s_ar_ready_o = !rst_i && m_ar_ready_i && ar_can_accept;

    always_comb begin
        b_sel_valid = 1'b0;
        b_sel_id = b_rr_reg;
        for (int offset = NUM_IDS-1; offset >= 0; offset--) begin
            if (b_buffer_ready[(int'(b_rr_reg) + offset) % NUM_IDS]) begin
                b_sel_valid = 1'b1;
                b_sel_id = CL_NUM_IDS'(int'(b_rr_reg) + offset);
            end
        end
        b_direct = !b_sel_valid && s_b_valid_i &&
            wr_outstanding_cnt_reg[s_b_i.axi.bid] != 0 &&
            ((!wr_order_head[s_b_i.axi.bid].ordering_req && !s_b_i.meta.ordering_req) ||
             (wr_order_head[s_b_i.axi.bid].ordering_req && s_b_i.meta.ordering_req &&
              wr_order_head[s_b_i.axi.bid].base == s_b_i.meta.ordering_tag));
        if (b_hold_reg) begin
            b_sel_valid = !b_hold_direct_reg;
            b_sel_id = b_hold_id_reg;
            b_direct = b_hold_direct_reg && s_b_valid_i;
        end
    end

    assign m_b_valid_o = !rst_i && (b_sel_valid || b_direct);
    assign b_storage_rd_addr = !rst_i && b_sel_valid ? wr_order_head[b_sel_id].base : '0;
    assign b_storage_free_addr = wr_order_head[b_retire_id].base;
    assign m_b_o = b_sel_valid ? b_storage_rd_data : s_b_i.axi;
    assign s_b_ready_o = !rst_i && (b_direct ? m_b_ready_i :
        (s_b_i.meta.ordering_req && b_storage_wr_ready));
    assign b_retire_id = b_sel_valid ? b_sel_id : s_b_i.axi.bid;

    always_comb begin
        r_sel_valid = 1'b0;
        r_sel_id = r_rr_reg;
        for (int offset = NUM_IDS-1; offset >= 0; offset--) begin
            if (r_buffer_ready[(int'(r_rr_reg) + offset) % NUM_IDS]) begin
                r_sel_valid = 1'b1;
                r_sel_id = CL_NUM_IDS'(int'(r_rr_reg) + offset);
            end
        end
        r_direct = !r_sel_valid && s_r_valid_i && rd_outstanding_cnt_reg[s_r_i.axi.rid] != 0 &&
            ((!rd_order_head[s_r_i.axi.rid].ordering_req && !s_r_i.meta.ordering_req) ||
             (rd_order_head[s_r_i.axi.rid].ordering_req && s_r_i.meta.ordering_req &&
              rd_order_head[s_r_i.axi.rid].base == s_r_i.meta.ordering_tag));
        if (r_hold_reg) begin
            r_sel_valid = !r_hold_direct_reg;
            r_sel_id = r_hold_id_reg;
            r_direct = r_hold_direct_reg && s_r_valid_i;
        end
    end

    assign m_r_valid_o = !rst_i && (r_sel_valid || r_direct);
    assign r_storage_rd_addr = !rst_i && r_sel_valid ?
        TAG_W'(int'(rd_order_head[r_sel_id].base) + int'(r_retire_offset_reg[r_sel_id])) : '0;
    assign r_storage_free_addr = TAG_W'(int'(rd_order_head[r_retire_id].base) + int'(r_retire_offset_reg[r_retire_id]));
    // NarrowR payload contains one 64-bit lane; position it using the issuing AR.
    // Context is selected only at retirement, after any out-of-order buffering.
    always_comb begin
        retire_r = r_sel_valid ? r_storage_rd_data : s_r_i.axi;
        retire_context = rd_lane_context_reg[r_retire_id][rd_order_rd_ptr_reg[r_retire_id]];
        retire_step = 1 << retire_context.size;
        retire_span = (int'(retire_context.len) + 1) * retire_step;
        retire_byte_addr = int'(retire_context.addr);
        if (retire_context.burst != 0 && r_retire_offset_reg[r_retire_id] != 0) begin
            retire_byte_addr = (int'(retire_context.addr) & ~(retire_step-1)) +
                int'(r_retire_offset_reg[r_retire_id]) * retire_step;
            if (retire_context.burst == 2)
                retire_byte_addr = (int'(retire_context.addr) & ~(retire_span-1)) |
                    (retire_byte_addr & (retire_span-1));
        end
        retire_lane = (retire_byte_addr % (ni_params_pkg::AXI_DATA_WIDTH_DFLT/8)) / 8;
        retire_response = retire_r;
        if (!retire_context.is_data)
            retire_response.rdata = ni_params_pkg::AXI_DATA_WIDTH_DFLT'(retire_r.rdata[63:0]) << (retire_lane*64);
    end
    assign s_r_ready_o = !rst_i && (r_direct ? m_r_ready_i :
        (READ_ROB_ENABLED && s_r_i.meta.ordering_req && r_storage_wr_ready));
    assign r_retire_id = r_sel_valid ? r_sel_id : s_r_i.axi.rid;

    always_comb begin
        wr_reorder_active_next = wr_reorder_active_reg;
        rd_reorder_active_next = rd_reorder_active_reg;
        wr_collective_active_next = wr_collective_active_reg;
        b_rr_next = b_rr_reg;
        r_rr_next = r_rr_reg;
        aw_hold_next = aw_hold_reg;
        ar_hold_next = ar_hold_reg;
        aw_hold_reorder_next = aw_hold_reorder_reg;
        ar_hold_reorder_next = ar_hold_reorder_reg;
        aw_hold_tag_next = aw_hold_tag_reg;
        ar_hold_tag_next = ar_hold_tag_reg;
        b_hold_next = b_hold_reg;
        r_hold_next = r_hold_reg;
        b_hold_direct_next = b_hold_direct_reg;
        r_hold_direct_next = r_hold_direct_reg;
        b_hold_id_next = b_hold_id_reg;
        r_hold_id_next = r_hold_id_reg;
        for (int id = 0; id < NUM_IDS; id++) begin
            wr_order_rd_ptr_next[id] = wr_order_rd_ptr_reg[id];
            wr_order_wr_ptr_next[id] = wr_order_wr_ptr_reg[id];
            rd_order_rd_ptr_next[id] = rd_order_rd_ptr_reg[id];
            rd_order_wr_ptr_next[id] = rd_order_wr_ptr_reg[id];
            wr_outstanding_cnt_next[id] = wr_outstanding_cnt_reg[id];
            rd_outstanding_cnt_next[id] = rd_outstanding_cnt_reg[id];
            wr_domain_next[id] = wr_domain_reg[id];
            rd_domain_next[id] = rd_domain_reg[id];
            r_retire_offset_next[id] = r_retire_offset_reg[id];
        end

        aw_hold_next = m_aw_valid_o && !m_aw_ready_i;
        ar_hold_next = m_ar_valid_o && !m_ar_ready_i;
        if (aw_hold_next && !aw_hold_reg) begin
            aw_hold_reorder_next = aw_reorder;
            aw_hold_tag_next = aw_tag;
        end
        if (ar_hold_next && !ar_hold_reg) begin
            ar_hold_reorder_next = ar_reorder;
            ar_hold_tag_next = ar_tag;
        end
        b_hold_next = m_b_valid_o && !m_b_ready_i;
        r_hold_next = m_r_valid_o && !m_r_ready_i;
        if (b_hold_next && !b_hold_reg) begin
            b_hold_direct_next = b_direct;
            b_hold_id_next = b_sel_id;
        end
        if (r_hold_next && !r_hold_reg) begin
            r_hold_direct_next = r_direct;
            r_hold_id_next = r_sel_id;
        end

        if (aw_accept) begin
            wr_order_wr_ptr_next[s_aw_i.axi.awid] =
                wr_order_wr_ptr_reg[s_aw_i.axi.awid] == CL_ORDER'(NMU_MAX_TXNS_PER_ID-1) ? '0 :
                wr_order_wr_ptr_reg[s_aw_i.axi.awid] + 1'b1;
            wr_domain_next[s_aw_i.axi.awid] = s_aw_i.route.route.domain;
            if (s_aw_i.route.collective_op != 0) begin
                wr_collective_active_next[s_aw_i.axi.awid] = 1'b1;
            end
        end
        if (ar_accept) begin
            rd_order_wr_ptr_next[s_ar_i.axi.arid] =
                rd_order_wr_ptr_reg[s_ar_i.axi.arid] == CL_ORDER'(NMU_MAX_TXNS_PER_ID-1) ? '0 :
                rd_order_wr_ptr_reg[s_ar_i.axi.arid] + 1'b1;
            rd_domain_next[s_ar_i.axi.arid] = s_ar_i.route.domain;
        end
        if (b_retire) begin
            wr_order_rd_ptr_next[b_retire_id] =
                wr_order_rd_ptr_reg[b_retire_id] == CL_ORDER'(NMU_MAX_TXNS_PER_ID-1) ? '0 :
                wr_order_rd_ptr_reg[b_retire_id] + 1'b1;
            if (wr_order_head[b_retire_id].collective) begin
                wr_collective_active_next[b_retire_id] = 1'b0;
            end
            b_rr_next = b_retire_id + 1'b1;
        end
        if (r_retire) begin
            if (r_retire_last) begin
                rd_order_rd_ptr_next[r_retire_id] =
                    rd_order_rd_ptr_reg[r_retire_id] == CL_ORDER'(NMU_MAX_TXNS_PER_ID-1) ? '0 :
                    rd_order_rd_ptr_reg[r_retire_id] + 1'b1;
                r_retire_offset_next[r_retire_id] = '0;
            end else begin
                r_retire_offset_next[r_retire_id] = r_retire_offset_reg[r_retire_id] + 1'b1;
            end
            r_rr_next = r_retire_id + 1'b1;
        end

        for (int id = 0; id < NUM_IDS; id++) begin
            case ({aw_accept && s_aw_i.axi.awid == ID_W'(id),
                   b_retire && b_retire_id == CL_NUM_IDS'(id)})
                2'b10: wr_outstanding_cnt_next[id] = wr_outstanding_cnt_reg[id] + 1'b1;
                2'b01: wr_outstanding_cnt_next[id] = wr_outstanding_cnt_reg[id] - 1'b1;
                default: begin end
            endcase
            if (aw_accept && s_aw_i.axi.awid == ID_W'(id)) begin
                if (wr_outstanding_cnt_reg[id] == 0) begin
                    wr_reorder_active_next[id] = aw_reorder;
                end else if (aw_reorder) begin
                    wr_reorder_active_next[id] = 1'b1;
                end
            end else if (b_retire && b_retire_id == CL_NUM_IDS'(id) && wr_outstanding_cnt_reg[id] == 1) begin
                wr_reorder_active_next[id] = 1'b0;
            end
            case ({ar_accept && s_ar_i.axi.arid == ID_W'(id),
                   r_retire && r_retire_id == CL_NUM_IDS'(id) && r_retire_last})
                2'b10: rd_outstanding_cnt_next[id] = rd_outstanding_cnt_reg[id] + 1'b1;
                2'b01: rd_outstanding_cnt_next[id] = rd_outstanding_cnt_reg[id] - 1'b1;
                default: begin end
            endcase
            if (ar_accept && s_ar_i.axi.arid == ID_W'(id)) begin
                if (rd_outstanding_cnt_reg[id] == 0) begin
                    rd_reorder_active_next[id] = ar_reorder;
                end else if (ar_reorder) begin
                    rd_reorder_active_next[id] = 1'b1;
                end
            end else if (r_retire && r_retire_id == CL_NUM_IDS'(id) &&
                    rd_outstanding_cnt_reg[id] == 1 && r_retire_last) begin
                rd_reorder_active_next[id] = 1'b0;
            end
        end
    end

    always_ff @(posedge clk_i) begin
        if (rst_i) begin
            wr_reorder_active_reg <= '0;
            rd_reorder_active_reg <= '0;
            wr_collective_active_reg <= '0;
            b_rr_reg <= '0;
            r_rr_reg <= '0;
            aw_hold_reg <= '0;
            ar_hold_reg <= '0;
            aw_hold_reorder_reg <= '0;
            ar_hold_reorder_reg <= '0;
            aw_hold_tag_reg <= '0;
            ar_hold_tag_reg <= '0;
            b_hold_reg <= '0;
            r_hold_reg <= '0;
            b_hold_direct_reg <= '0;
            r_hold_direct_reg <= '0;
            b_hold_id_reg <= '0;
            r_hold_id_reg <= '0;
            for (int id = 0; id < NUM_IDS; id++) begin
                wr_order_rd_ptr_reg[id] <= '0;
                wr_order_wr_ptr_reg[id] <= '0;
                rd_order_rd_ptr_reg[id] <= '0;
                rd_order_wr_ptr_reg[id] <= '0;
                wr_outstanding_cnt_reg[id] <= '0;
                rd_outstanding_cnt_reg[id] <= '0;
                wr_domain_reg[id] <= '0;
                rd_domain_reg[id] <= '0;
                r_retire_offset_reg[id] <= '0;
            end
        end else begin
            wr_reorder_active_reg <= wr_reorder_active_next;
            rd_reorder_active_reg <= rd_reorder_active_next;
            wr_collective_active_reg <= wr_collective_active_next;
            b_rr_reg <= b_rr_next;
            r_rr_reg <= r_rr_next;
            aw_hold_reg <= aw_hold_next;
            ar_hold_reg <= ar_hold_next;
            aw_hold_reorder_reg <= aw_hold_reorder_next;
            ar_hold_reorder_reg <= ar_hold_reorder_next;
            aw_hold_tag_reg <= aw_hold_tag_next;
            ar_hold_tag_reg <= ar_hold_tag_next;
            b_hold_reg <= b_hold_next;
            r_hold_reg <= r_hold_next;
            b_hold_direct_reg <= b_hold_direct_next;
            r_hold_direct_reg <= r_hold_direct_next;
            b_hold_id_reg <= b_hold_id_next;
            r_hold_id_reg <= r_hold_id_next;
            for (int id = 0; id < NUM_IDS; id++) begin
                wr_order_rd_ptr_reg[id] <= wr_order_rd_ptr_next[id];
                wr_order_wr_ptr_reg[id] <= wr_order_wr_ptr_next[id];
                rd_order_rd_ptr_reg[id] <= rd_order_rd_ptr_next[id];
                rd_order_wr_ptr_reg[id] <= rd_order_wr_ptr_next[id];
                wr_outstanding_cnt_reg[id] <= wr_outstanding_cnt_next[id];
                rd_outstanding_cnt_reg[id] <= rd_outstanding_cnt_next[id];
                wr_domain_reg[id] <= wr_domain_next[id];
                rd_domain_reg[id] <= rd_domain_next[id];
                r_retire_offset_reg[id] <= r_retire_offset_next[id];
            end
            if (aw_accept) begin
                wr_order_reg[s_aw_i.axi.awid][wr_order_wr_ptr_reg[s_aw_i.axi.awid]] <= '{
                    base: aw_tag, beat_count: BEAT_COUNT_W'(1),
                    ordering_req: aw_reorder, collective: s_aw_i.route.collective_op != 0
                };
            end
            if (ar_accept) begin
                rd_lane_context_reg[s_ar_i.axi.arid][rd_order_wr_ptr_reg[s_ar_i.axi.arid]] <= '{
                    addr: BYTE_OFFSET_W'(s_ar_i.axi.araddr), len: s_ar_i.axi.arlen,
                    size: s_ar_i.axi.arsize, burst: s_ar_i.axi.arburst,
                    is_data: s_ar_i.route.domain.is_data
                };
                rd_order_reg[s_ar_i.axi.arid][rd_order_wr_ptr_reg[s_ar_i.axi.arid]] <= '{
                    base: ar_tag, beat_count: ar_beat_cnt,
                    ordering_req: ar_reorder, collective: 1'b0
                };
            end
            if (r_retire && !r_retire_last) begin
                rd_order_reg[r_retire_id][rd_order_rd_ptr_reg[r_retire_id]].beat_count <=
                    rd_order_head[r_retire_id].beat_count - 1'b1;
            end
        end
    end

    nmu_reorder_storage #(
        .DEPTH (NMU_ROB_B_DEPTH),
        .T (ni_signals_pkg::axi_b_t)
    ) i_b_storage (
        .clk_i,
        .rst_i,
        .alloc_valid_i (aw_accept && aw_reorder),
        .alloc_base_i (aw_tag),
        .alloc_cnt_i ((TAG_W+1)'(1)),
        .next_base_o (b_next_base),
        .free_cnt_o (b_free_cnt),
        .wr_valid_i (!rst_i && s_b_valid_i && s_b_i.meta.ordering_req && (!b_direct || m_b_ready_i)),
        .wr_bypass_i (b_direct),
        .wr_ready_o (b_storage_wr_ready),
        .wr_base_i (!rst_i && s_b_valid_i && s_b_i.meta.ordering_req ?
            s_b_i.meta.ordering_tag : '0),
        .wr_last_i (1'b1),
        .wr_data_i (s_b_i.axi),
        .rd_en_i (b_sel_valid),
        .rd_addr_i (b_storage_rd_addr),
        .rd_entry_complete_o (),
        .rd_data_o (b_storage_rd_data),
        .free_valid_i (b_retire && wr_order_head[b_retire_id].ordering_req),
        .free_addr_i (b_storage_free_addr),
        .complete_o (b_complete)
    );

    if (READ_ROB_ENABLED) begin : gen_read_reorder_storage
        nmu_reorder_storage #(
            .DEPTH (NMU_ROB_R_DEPTH),
            .T (ni_signals_pkg::axi_r_t)
        ) i_r_storage (
            .clk_i,
            .rst_i,
            .alloc_valid_i (ar_accept && ar_reorder),
            .alloc_base_i (ar_tag),
            .alloc_cnt_i ((TAG_W+1)'(ar_beat_cnt)),
            .next_base_o (r_next_base),
            .free_cnt_o (r_free_cnt),
            .wr_valid_i (!rst_i && s_r_valid_i && s_r_i.meta.ordering_req && (!r_direct || m_r_ready_i)),
            .wr_bypass_i (r_direct),
            .wr_ready_o (r_storage_wr_ready),
            .wr_base_i (!rst_i && s_r_valid_i && s_r_i.meta.ordering_req ?
                s_r_i.meta.ordering_tag : '0),
            .wr_last_i (!rst_i && s_r_valid_i && s_r_i.meta.ordering_req && s_r_i.axi.rlast),
            .wr_data_i (s_r_i.axi),
            .rd_en_i (r_sel_valid),
            .rd_addr_i (r_storage_rd_addr),
            .rd_entry_complete_o (),
            .rd_data_o (r_storage_rd_data),
            .free_valid_i (r_retire && rd_order_head[r_retire_id].ordering_req),
            .free_addr_i (r_storage_free_addr),
            .complete_o (r_complete)
        );
    end else begin : gen_no_read_reorder_storage
        assign r_next_base = '0;
        assign r_free_cnt = '0;
        assign r_storage_wr_ready = 1'b0;
        assign r_storage_rd_data = '0;
        assign r_complete = '0;
    end
endmodule

`resetall
