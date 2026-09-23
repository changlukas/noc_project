// SPDX-License-Identifier: Apache-2.0
`resetall
`timescale 1ns / 1ps
`default_nettype none

// Network injection: packet locks, VC ownership and credit qualification.
module nmu_channel_assign #(
    parameter int unsigned NUM_DAT_VC      = ni_params_pkg::NUM_DAT_VC,
    parameter int unsigned DAT_VC_MODE     = ni_params_pkg::NOC_DAT_VC_MODE,
    parameter int unsigned ROUTER_VC_DEPTH = ni_params_pkg::NOC_ROUTER_VC_DEPTH
) (
    input  wire logic                                                      clk_i,
    input  wire logic                                                      rst_i,
    input  wire ni_flit_pkg::req_flit_t [ni_types_pkg::NUM_NMU_REQ_CH-1:0] s_req_i,
    input  wire logic                   [ni_types_pkg::NUM_NMU_REQ_CH-1:0] s_req_valid_i,
    output wire logic                   [ni_types_pkg::NUM_NMU_REQ_CH-1:0] s_req_ready_o,
    input  wire ni_flit_pkg::dat_flit_t [ni_types_pkg::NUM_NMU_DAT_CH-1:0] s_dat_i,
    input  wire logic                   [ni_types_pkg::NUM_NMU_DAT_CH-1:0] s_dat_valid_i,
    output wire logic                   [ni_types_pkg::NUM_NMU_DAT_CH-1:0] s_dat_ready_o,
    output wire ni_flit_pkg::req_flit_t                                    m_req_o,
    output wire logic                                                      m_req_valid_o,
    input  wire logic                                                      m_req_ready_i,
    output wire ni_flit_pkg::dat_flit_t                                    m_dat_o,
    output wire logic                                                      m_dat_valid_o,
    input  wire logic                                     [NUM_DAT_VC-1:0] dat_credit_return_i
);
    import ni_types_pkg::*;

    import ni_flit_pkg::*;
    localparam int REQ_SEL_W = NUM_NMU_REQ_CH > 1 ? $clog2(NUM_NMU_REQ_CH) : 1;
    localparam int VC_IDX_W  = NUM_DAT_VC > 1 ? $clog2(NUM_DAT_VC) : 1;
    localparam int NUM_IDS   = 1 << AW_AWID_WIDTH;
    localparam int NUM_WR_VC = DAT_VC_MODE == 1 ? NUM_DAT_VC/2 : NUM_DAT_VC;
    if (NUM_DAT_VC < 1 || NUM_DAT_VC > (1 << ni_flit_pkg::VC_ID_WIDTH)) begin : gen_invalid_vc_count
        initial $fatal(0, "Error: NUM_DAT_VC is outside the encoded VC range (instance %m)");
    end
    if (DAT_VC_MODE > 1 ||
        (DAT_VC_MODE == 1 &&
         (NUM_DAT_VC < 2 || NUM_DAT_VC[0]))) begin : gen_invalid_vc_mode
        initial $fatal(0, "Error: split DAT VC mode requires a positive even NUM_DAT_VC (instance %m)");
    end
    if (ROUTER_VC_DEPTH < 2 || (ROUTER_VC_DEPTH & (ROUTER_VC_DEPTH-1)) != 0) begin : gen_invalid_credit_depth
        initial $fatal(0, "Error: ROUTER_VC_DEPTH must be a power of two >= 2 (instance %m)");
    end

    logic                    req_write_lock_reg, req_write_lock_next;
    logic                    dat_write_lock_reg, dat_write_lock_next;
    logic                    req_rr_reg, req_rr_next;
    logic                    req_hold_reg, req_hold_next, req_hold_aw_reg, req_hold_aw_next;
    logic        [VC_IDX_W-1:0] dat_active_vc_reg, dat_active_vc_next;
    logic        [VC_IDX_W-1:0] dat_vc_rr_reg, dat_vc_rr_next;
    logic  [NUM_IDS-1:0] fixed_vc_valid_reg, fixed_vc_valid_next;
    logic [DST_ID_WIDTH-1:0] fixed_vc_dst_reg [NUM_IDS], fixed_vc_dst_next [NUM_IDS];
    logic        [VC_IDX_W-1:0] fixed_vc_id_reg [NUM_IDS], fixed_vc_id_next [NUM_IDS];
    wire    [NUM_DAT_VC-1:0] dat_credit_left;
    wire [AW_AWID_WIDTH-1:0] aw_id = s_dat_i[NMU_DAT_AW_IDX].payload[AW_AWID_LSB +: AW_AWID_WIDTH];
    wire [DST_ID_WIDTH-1:0] aw_dst = s_dat_i[NMU_DAT_AW_IDX].header[DST_ID_LSB +: DST_ID_WIDTH];
    wire aw_reorder = s_dat_i[NMU_DAT_AW_IDX].header[ORDERING_REQ_LSB];
    logic                                req_sel_aw, dat_vc_available;
    logic [VC_IDX_W-1:0]                    dat_sel_vc;
    logic                   [REQ_SEL_W-1:0] req_sel;
    ni_flit_pkg::dat_flit_t              dat_flit;
    wire [VC_IDX_W-1:0] dat_vc = dat_write_lock_reg ? dat_active_vc_reg : dat_sel_vc;
    wire req_transfer = m_req_valid_o && m_req_ready_i;
    wire dat_transfer = m_dat_valid_o;

    for (genvar vc = 0; vc < NUM_DAT_VC; vc++) begin : gen_credit
        cc_credit_counter #(
            .NumCredits (ROUTER_VC_DEPTH)
        ) i_credit (
            .clk_i         (clk_i                                  ),
            .rst_ni        (1'b1                                   ),
            .clr_i         (rst_i                                  ),
            .credit_o      (                                       ),
            .credit_give_i (!rst_i && dat_credit_return_i[vc]      ),
            .credit_take_i (dat_transfer && dat_vc == VC_IDX_W'(vc)),
            .credit_left_o (dat_credit_left[vc]                    ),
            .credit_crit_o (                                       ),
            .credit_full_o (                                       )
        );
    end
    always_comb begin
        req_sel_aw = 1'b0;
        if (req_hold_reg) req_sel_aw = req_hold_aw_reg;
        else if (!req_write_lock_reg)
            req_sel_aw = s_req_valid_i[NMU_REQ_AW_IDX] && s_req_valid_i[NMU_REQ_W_IDX] && (!s_req_valid_i[NMU_REQ_AR_IDX] || !req_rr_reg);
        req_sel = req_write_lock_reg ? REQ_SEL_W'(NMU_REQ_W_IDX) : req_sel_aw ? REQ_SEL_W'(NMU_REQ_AW_IDX) : REQ_SEL_W'(NMU_REQ_AR_IDX);
    end
    assign m_req_valid_o = !rst_i && s_req_valid_i[req_sel];
    assign m_req_o       = m_req_valid_o ? s_req_i[req_sel] : '0;
    assign s_req_ready_o = req_transfer ? (NUM_NMU_REQ_CH'(1) << req_sel) : '0;

    always_comb begin
        int candidate;
        candidate        = 0;
        dat_sel_vc       = dat_vc_rr_reg;
        dat_vc_available = 1'b0;
        if (!aw_reorder && fixed_vc_valid_reg[aw_id] && fixed_vc_dst_reg[aw_id] == aw_dst) begin
            dat_sel_vc       = fixed_vc_id_reg[aw_id];
            dat_vc_available = dat_credit_left[dat_sel_vc] || dat_credit_return_i[dat_sel_vc];
        end else begin
            for (int offset = NUM_WR_VC-1; offset >= 0; offset--) begin
                candidate = (int'(dat_vc_rr_reg) + offset) % NUM_WR_VC;
                if (dat_credit_left[candidate] || dat_credit_return_i[candidate]) begin
                    dat_vc_available = 1'b1;
                    dat_sel_vc       = VC_IDX_W'(candidate);
                end
            end
        end
    end
    assign m_dat_valid_o = !rst_i && (dat_write_lock_reg ?
        (s_dat_valid_i[NMU_DAT_W_IDX] && (dat_credit_left[dat_active_vc_reg] || dat_credit_return_i[dat_active_vc_reg])) :
        (s_dat_valid_i[NMU_DAT_AW_IDX] && s_dat_valid_i[NMU_DAT_W_IDX] && dat_vc_available));
    assign s_dat_ready_o = m_dat_valid_o ? (NUM_NMU_DAT_CH'(1) << (dat_write_lock_reg ? NMU_DAT_W_IDX : NMU_DAT_AW_IDX)) : '0;
    always_comb begin
        dat_flit                                  = s_dat_i[dat_write_lock_reg ? NMU_DAT_W_IDX : NMU_DAT_AW_IDX];
        dat_flit.header[VC_ID_LSB +: VC_ID_WIDTH] = VC_ID_WIDTH'(dat_vc);
    end
    assign m_dat_o = m_dat_valid_o ? dat_flit : '0;

    always_comb begin
        req_write_lock_next = req_write_lock_reg;
        dat_write_lock_next = dat_write_lock_reg;
        req_rr_next         = req_rr_reg;
        req_hold_next       = m_req_valid_o && !m_req_ready_i && !req_write_lock_reg;
        req_hold_aw_next    = req_hold_aw_reg;
        dat_active_vc_next  = dat_active_vc_reg;
        dat_vc_rr_next      = dat_vc_rr_reg;
        fixed_vc_valid_next = fixed_vc_valid_reg;
        for (int id = 0; id < NUM_IDS; id++) begin
            fixed_vc_dst_next[id] = fixed_vc_dst_reg[id];
            fixed_vc_id_next[id]  = fixed_vc_id_reg[id];
        end
        if (req_hold_next && !req_hold_reg) req_hold_aw_next = req_sel_aw;
        if (req_transfer) begin
            if (req_write_lock_reg) begin
                if (m_req_o.header[FLIT_TAIL_LSB]) req_write_lock_next = 1'b0;
            end else if (req_sel_aw) begin
                req_write_lock_next = 1'b1;
                req_rr_next         = 1'b1;
            end else req_rr_next = 1'b0;
        end
        if (dat_transfer) begin
            if (dat_write_lock_reg) begin
                if (m_dat_o.header[FLIT_TAIL_LSB]) dat_write_lock_next = 1'b0;
            end else begin
                dat_active_vc_next  = dat_sel_vc;
                dat_write_lock_next = 1'b1;
                dat_vc_rr_next      = dat_sel_vc == VC_IDX_W'(NUM_WR_VC-1) ? '0 : dat_sel_vc + 1'b1;
                if (!aw_reorder) begin
                    fixed_vc_valid_next[aw_id] = 1'b1;
                    fixed_vc_dst_next[aw_id]   = aw_dst;
                    fixed_vc_id_next[aw_id]    = dat_sel_vc;
                end
            end
        end
    end
    always_ff @(posedge clk_i) begin
        if (rst_i) begin
            req_write_lock_reg <= '0;
            dat_write_lock_reg <= '0;
            req_rr_reg         <= '0;
            req_hold_reg       <= '0;
            req_hold_aw_reg    <= '0;
            dat_active_vc_reg  <= '0;
            dat_vc_rr_reg      <= '0;
            fixed_vc_valid_reg <= '0;
        end else begin
            req_write_lock_reg <= req_write_lock_next;
            dat_write_lock_reg <= dat_write_lock_next;
            req_rr_reg         <= req_rr_next;
            req_hold_reg       <= req_hold_next;
            req_hold_aw_reg    <= req_hold_aw_next;
            dat_active_vc_reg  <= dat_active_vc_next;
            dat_vc_rr_reg      <= dat_vc_rr_next;
            fixed_vc_valid_reg <= fixed_vc_valid_next;
            for (int id = 0; id < NUM_IDS; id++) begin
                fixed_vc_dst_reg[id] <= fixed_vc_dst_next[id];
                fixed_vc_id_reg[id]  <= fixed_vc_id_next[id];
            end
        end
    end
endmodule
`resetall
