// SPDX-License-Identifier: Apache-2.0

`resetall
`timescale 1ns / 1ps
`default_nettype none

/* Packetize ordered NMU requests and independently schedule the REQ and DAT
 * NoC faces.  Accepted AWs form the global ownership order used to associate
 * the AXI W stream with its route, traffic class, ordering metadata, and VC.
 */
module nmu_request_packetize #(
    parameter int unsigned FIFO_DEPTH = ni_params_pkg::NOC_FIFO_DEPTH_DFLT,
    parameter int unsigned DAT_NUM_VC = ni_params_pkg::NOC_DAT_NUM_VC_DFLT,
    parameter int unsigned DAT_VC_MODE = ni_params_pkg::NOC_DAT_VC_MODE_DFLT,
    parameter int unsigned ROUTER_VC_DEPTH = ni_params_pkg::NOC_ROUTER_VC_DEPTH_DFLT,
    parameter logic [ni_flit_pkg::SRC_ID_WIDTH-1:0] SRC_ID = '0,
    parameter logic [ni_flit_pkg::SRC_PORT_ID_WIDTH-1:0] SRC_PORT_ID = '0
) (
    input  wire logic                                    clk_i,
    input  wire logic                                    rst_i,
    input  wire ni_child_types_pkg::nmu_aw_request_t     s_aw_i,
    input  wire logic                                    s_aw_valid_i,
    output wire logic                                    s_aw_ready_o,
    input  wire ni_signals_pkg::axi_w_t                  s_w_i,
    input  wire logic                                    s_w_valid_i,
    output wire logic                                    s_w_ready_o,
    input  wire ni_child_types_pkg::nmu_ar_request_t     s_ar_i,
    input  wire logic                                    s_ar_valid_i,
    output wire logic                                    s_ar_ready_o,
    output wire ni_flit_pkg::req_flit_t                  m_req_o,
    output wire logic                                    m_req_valid_o,
    input  wire logic                                    m_req_ready_i,
    output wire ni_flit_pkg::dat_flit_t                  m_dat_o,
    output wire logic                                    m_dat_valid_o,
    input  wire logic [DAT_NUM_VC-1:0]                   dat_credit_return_i
);

    localparam int unsigned PTR_W = FIFO_DEPTH > 1 ? $clog2(FIFO_DEPTH) : 1;
    localparam int unsigned USE_W = $clog2(FIFO_DEPTH + 1);
    localparam int unsigned VC_W = DAT_NUM_VC > 1 ? $clog2(DAT_NUM_VC) : 1;
    localparam int unsigned CREDIT_W = $clog2(ROUTER_VC_DEPTH + 1);
    localparam int unsigned NUM_AXI_IDS = 1 << $bits(s_aw_i.axi.awid);
    localparam int unsigned DAT_VC_MODE_READ_WRITE_SPLIT = 1;
    localparam logic [ni_flit_pkg::AXI_BURST_WIDTH-1:0] AXI_BURST_INCR = 2'b01;
    localparam logic [ni_flit_pkg::AXI_BURST_WIDTH-1:0] AXI_BURST_WRAP = 2'b10;
    localparam int unsigned WRITE_VC_COUNT =
        DAT_VC_MODE == DAT_VC_MODE_READ_WRITE_SPLIT ? DAT_NUM_VC / 2 : DAT_NUM_VC;

    typedef struct packed {
        ni_signals_pkg::axi_w_t                 axi;
        ni_child_types_pkg::nmu_aw_request_t    owner;
        logic [ni_flit_pkg::AXI_LEN_WIDTH-1:0]  beat_index;
    } write_beat_t;

    ni_child_types_pkg::nmu_aw_request_t owner_mem [FIFO_DEPTH];
    ni_child_types_pkg::nmu_aw_request_t narrow_aw_mem [FIFO_DEPTH];
    ni_child_types_pkg::nmu_aw_request_t data_aw_mem [FIFO_DEPTH];
    ni_child_types_pkg::nmu_ar_request_t ar_mem [FIFO_DEPTH];
    write_beat_t narrow_w_mem [FIFO_DEPTH];
    write_beat_t data_w_mem [FIFO_DEPTH];

    logic [PTR_W-1:0] owner_wr_ptr_reg, owner_rd_ptr_reg;
    logic [PTR_W-1:0] narrow_aw_wr_ptr_reg, narrow_aw_rd_ptr_reg;
    logic [PTR_W-1:0] data_aw_wr_ptr_reg, data_aw_rd_ptr_reg;
    logic [PTR_W-1:0] ar_wr_ptr_reg, ar_rd_ptr_reg;
    logic [PTR_W-1:0] narrow_w_wr_ptr_reg, narrow_w_rd_ptr_reg;
    logic [PTR_W-1:0] data_w_wr_ptr_reg, data_w_rd_ptr_reg;
    logic [USE_W-1:0] owner_usage_reg, narrow_aw_usage_reg, data_aw_usage_reg;
    logic [USE_W-1:0] ar_usage_reg, narrow_w_usage_reg, data_w_usage_reg;
    logic [ni_flit_pkg::AXI_LEN_WIDTH-1:0] owner_beat_index_reg;

    logic req_write_lock_reg, dat_write_lock_reg;
    logic req_rr_reg;
    ni_child_types_pkg::nmu_aw_request_t req_active_aw_reg, dat_active_aw_reg;
    logic [VC_W-1:0] dat_active_vc_reg, dat_vc_rr_reg;
    logic [CREDIT_W-1:0] dat_credit_reg [DAT_NUM_VC];
    logic [NUM_AXI_IDS-1:0] fixed_vc_valid_reg;
    logic [ni_flit_pkg::DST_ID_WIDTH-1:0] fixed_vc_dst_reg [NUM_AXI_IDS];
    logic [VC_W-1:0] fixed_vc_id_reg [NUM_AXI_IDS];

    wire ni_child_types_pkg::nmu_aw_request_t owner_head = owner_mem[owner_rd_ptr_reg];
    wire ni_child_types_pkg::nmu_aw_request_t narrow_aw_head = narrow_aw_mem[narrow_aw_rd_ptr_reg];
    wire ni_child_types_pkg::nmu_aw_request_t data_aw_head = data_aw_mem[data_aw_rd_ptr_reg];
    wire ni_child_types_pkg::nmu_ar_request_t ar_head = ar_mem[ar_rd_ptr_reg];
    wire write_beat_t narrow_w_head = narrow_w_mem[narrow_w_rd_ptr_reg];
    wire write_beat_t data_w_head = data_w_mem[data_w_rd_ptr_reg];

    wire logic aw_is_data = s_aw_i.meta.route.domain.is_data;
    wire logic aw_accept = s_aw_valid_i && s_aw_ready_o;
    wire logic w_accept = s_w_valid_i && s_w_ready_o;
    wire logic ar_accept = s_ar_valid_i && s_ar_ready_o;
    wire logic req_transfer = m_req_valid_o && m_req_ready_i;
    wire logic dat_transfer = m_dat_valid_o;

    logic req_select_aw;
    logic dat_vc_available;
    logic [VC_W-1:0] dat_selected_vc;
    logic [ni_flit_pkg::HEADER_WIDTH-1:0] req_header, dat_header;
    logic [ni_flit_pkg::PAYLOAD_WIDTH-1:0] req_payload, dat_payload;

    function automatic logic [PTR_W-1:0] ptr_next(input logic [PTR_W-1:0] ptr);
        ptr_next = ptr == FIFO_DEPTH-1 ? '0 : ptr + 1'b1;
    endfunction

    function automatic logic [ni_flit_pkg::HEADER_WIDTH-1:0] make_header(
        input logic [ni_flit_pkg::AXI_CH_WIDTH-1:0] axi_ch,
        input ni_child_types_pkg::nmu_request_t meta,
        input logic [ni_flit_pkg::COLLECTIVE_OP_WIDTH-1:0] collective_op,
        input logic [ni_flit_pkg::COLLECTIVE_MASK_WIDTH-1:0] collective_mask,
        input logic [VC_W-1:0] vc_id,
        input logic fixed_vc,
        input logic tail
    );
        logic [ni_flit_pkg::HEADER_WIDTH-1:0] header;
        header = '0;
        header[ni_flit_pkg::AXI_CH_MSB:ni_flit_pkg::AXI_CH_LSB] = axi_ch;
        header[ni_flit_pkg::SRC_ID_MSB:ni_flit_pkg::SRC_ID_LSB] = SRC_ID;
        header[ni_flit_pkg::DST_ID_MSB:ni_flit_pkg::DST_ID_LSB] = meta.route.domain.dst_id;
        header[ni_flit_pkg::FIXED_VC_LSB] = fixed_vc;
        header[ni_flit_pkg::VC_ID_MSB:ni_flit_pkg::VC_ID_LSB] = ni_flit_pkg::VC_ID_WIDTH'(vc_id);
        header[ni_flit_pkg::FLIT_TAIL_LSB] = tail;
        header[ni_flit_pkg::ORDERING_REQ_LSB] = meta.ordering_req;
        header[ni_flit_pkg::ORDERING_TAG_MSB:ni_flit_pkg::ORDERING_TAG_LSB] = meta.ordering_tag;
        header[ni_flit_pkg::COLLECTIVE_OP_MSB:ni_flit_pkg::COLLECTIVE_OP_LSB] = collective_op;
        header[ni_flit_pkg::COLLECTIVE_MASK_MSB:ni_flit_pkg::COLLECTIVE_MASK_LSB] = collective_mask;
        header[ni_flit_pkg::DST_PORT_ID_MSB:ni_flit_pkg::DST_PORT_ID_LSB] = meta.route.domain.dst_port_id;
        header[ni_flit_pkg::SRC_PORT_ID_MSB:ni_flit_pkg::SRC_PORT_ID_LSB] = SRC_PORT_ID;
        return header;
    endfunction

    function automatic logic [ni_flit_pkg::PAYLOAD_WIDTH-1:0] pack_aw(
        input ni_child_types_pkg::nmu_aw_request_t request
    );
        logic [ni_flit_pkg::PAYLOAD_WIDTH-1:0] value;
        value = '0;
        value[ni_flit_pkg::AW_AWID_MSB:ni_flit_pkg::AW_AWID_LSB] = request.axi.awid;
        value[ni_flit_pkg::AW_AWADDR_MSB:ni_flit_pkg::AW_AWADDR_LSB] = request.axi.awaddr;
        value[ni_flit_pkg::AW_AWLEN_MSB:ni_flit_pkg::AW_AWLEN_LSB] = request.axi.awlen;
        value[ni_flit_pkg::AW_AWSIZE_MSB:ni_flit_pkg::AW_AWSIZE_LSB] = request.axi.awsize;
        value[ni_flit_pkg::AW_AWBURST_MSB:ni_flit_pkg::AW_AWBURST_LSB] = request.axi.awburst;
        value[ni_flit_pkg::AW_AWCACHE_MSB:ni_flit_pkg::AW_AWCACHE_LSB] = request.axi.awcache;
        value[ni_flit_pkg::AW_AWLOCK_LSB] = request.axi.awlock;
        value[ni_flit_pkg::AW_AWPROT_MSB:ni_flit_pkg::AW_AWPROT_LSB] = request.axi.awprot;
        value[ni_flit_pkg::AW_AWREGION_MSB:ni_flit_pkg::AW_AWREGION_LSB] = request.axi.awregion;
        value[ni_flit_pkg::AW_AWQOS_MSB:ni_flit_pkg::AW_AWQOS_LSB] = request.axi.awqos;
        value[ni_flit_pkg::AW_AWUSER_MSB:ni_flit_pkg::AW_AWUSER_LSB] = request.user;
        return value;
    endfunction

    function automatic logic [ni_flit_pkg::PAYLOAD_WIDTH-1:0] pack_ar(
        input ni_child_types_pkg::nmu_ar_request_t request
    );
        logic [ni_flit_pkg::PAYLOAD_WIDTH-1:0] value;
        value = '0;
        value[ni_flit_pkg::AR_ARID_MSB:ni_flit_pkg::AR_ARID_LSB] = request.axi.arid;
        value[ni_flit_pkg::AR_ARADDR_MSB:ni_flit_pkg::AR_ARADDR_LSB] = request.axi.araddr;
        value[ni_flit_pkg::AR_ARLEN_MSB:ni_flit_pkg::AR_ARLEN_LSB] = request.axi.arlen;
        value[ni_flit_pkg::AR_ARSIZE_MSB:ni_flit_pkg::AR_ARSIZE_LSB] = request.axi.arsize;
        value[ni_flit_pkg::AR_ARBURST_MSB:ni_flit_pkg::AR_ARBURST_LSB] = request.axi.arburst;
        value[ni_flit_pkg::AR_ARCACHE_MSB:ni_flit_pkg::AR_ARCACHE_LSB] = request.axi.arcache;
        value[ni_flit_pkg::AR_ARLOCK_LSB] = request.axi.arlock;
        value[ni_flit_pkg::AR_ARPROT_MSB:ni_flit_pkg::AR_ARPROT_LSB] = request.axi.arprot;
        value[ni_flit_pkg::AR_ARREGION_MSB:ni_flit_pkg::AR_ARREGION_LSB] = request.axi.arregion;
        value[ni_flit_pkg::AR_ARQOS_MSB:ni_flit_pkg::AR_ARQOS_LSB] = request.axi.arqos;
        return value;
    endfunction

    function automatic logic [ni_flit_pkg::PAYLOAD_WIDTH-1:0] pack_w(
        input write_beat_t beat,
        input logic narrow
    );
        logic [ni_flit_pkg::PAYLOAD_WIDTH-1:0] value;
        logic [ni_params_pkg::AXI_ADDR_WIDTH_DFLT-1:0] beat_addr;
        logic [ni_params_pkg::AXI_ADDR_WIDTH_DFLT:0] beat_bytes, wrap_bytes;
        logic [ni_params_pkg::AXI_ADDR_WIDTH_DFLT-1:0] wrap_base;
        logic [ni_flit_pkg::AXI_LEN_WIDTH:0] burst_beats;
        logic [$clog2(ni_params_pkg::AXI_DATA_WIDTH_DFLT /
                      ni_flit_pkg::NOC_NARROW_DATA_WIDTH)-1:0] lane;
        value = '0;
        if (narrow) begin
            beat_bytes = (ni_params_pkg::AXI_ADDR_WIDTH_DFLT+1)'(1) << beat.owner.axi.awsize;
            burst_beats = (ni_flit_pkg::AXI_LEN_WIDTH+1)'(beat.owner.axi.awlen) +
                (ni_flit_pkg::AXI_LEN_WIDTH+1)'(1);
            wrap_bytes = beat_bytes * burst_beats;
            beat_addr = beat.owner.axi.awaddr;
            if (beat.owner.axi.awburst == AXI_BURST_INCR) begin
                beat_addr = beat.owner.axi.awaddr + beat.beat_index * beat_bytes;
            end else if (beat.owner.axi.awburst == AXI_BURST_WRAP) begin
                wrap_base = beat.owner.axi.awaddr & ~(wrap_bytes-1'b1);
                beat_addr = wrap_base +
                    ((beat.owner.axi.awaddr - wrap_base + beat.beat_index * beat_bytes) &
                     (wrap_bytes-1'b1));
            end
            lane = beat_addr[$clog2(ni_params_pkg::AXI_DATA_WIDTH_DFLT/8)-1:
                             $clog2(ni_flit_pkg::NOC_NARROW_DATA_WIDTH/8)];
            value[ni_flit_pkg::NARROW_W_WLAST_LSB] = beat.axi.wlast;
            value[ni_flit_pkg::NARROW_W_WSTRB_MSB:ni_flit_pkg::NARROW_W_WSTRB_LSB] =
                beat.axi.wstrb[lane*ni_flit_pkg::NARROW_WSTRB_WIDTH +:
                               ni_flit_pkg::NARROW_WSTRB_WIDTH];
            value[ni_flit_pkg::NARROW_W_WDATA_MSB:ni_flit_pkg::NARROW_W_WDATA_LSB] =
                beat.axi.wdata[lane*ni_flit_pkg::NOC_NARROW_DATA_WIDTH +:
                               ni_flit_pkg::NOC_NARROW_DATA_WIDTH];
        end else begin
            value[ni_flit_pkg::DATA_W_WLAST_LSB] = beat.axi.wlast;
            value[ni_flit_pkg::DATA_W_WSTRB_MSB:ni_flit_pkg::DATA_W_WSTRB_LSB] = beat.axi.wstrb;
            value[ni_flit_pkg::DATA_W_WDATA_MSB:ni_flit_pkg::DATA_W_WDATA_LSB] = beat.axi.wdata;
        end
        return value;
    endfunction

    assign s_aw_ready_o = owner_usage_reg < FIFO_DEPTH &&
        (aw_is_data ? data_aw_usage_reg < FIFO_DEPTH : narrow_aw_usage_reg < FIFO_DEPTH);
    assign s_w_ready_o = owner_usage_reg != 0 &&
        (owner_head.meta.route.domain.is_data ? data_w_usage_reg < FIFO_DEPTH :
                                                narrow_w_usage_reg < FIFO_DEPTH);
    assign s_ar_ready_o = ar_usage_reg < FIFO_DEPTH;

    always_comb begin
        req_select_aw = 1'b0;
        if (!req_write_lock_reg) begin
            if (narrow_aw_usage_reg != 0 && narrow_w_usage_reg != 0 &&
                (ar_usage_reg == 0 || !req_rr_reg)) begin
                req_select_aw = 1'b1;
            end
        end
    end

    always_comb begin
        int candidate;
        dat_vc_available = 1'b0;
        dat_selected_vc = dat_vc_rr_reg;
        candidate = 0;
        if (!data_aw_head.meta.ordering_req &&
            fixed_vc_valid_reg[data_aw_head.axi.awid] &&
            fixed_vc_dst_reg[data_aw_head.axi.awid] == data_aw_head.meta.route.domain.dst_id) begin
            dat_selected_vc = fixed_vc_id_reg[data_aw_head.axi.awid];
            dat_vc_available = dat_credit_reg[dat_selected_vc] != 0 ||
                dat_credit_return_i[dat_selected_vc];
        end else begin
            for (int offset = WRITE_VC_COUNT-1; offset >= 0; offset--) begin
                candidate = (dat_vc_rr_reg + offset) % WRITE_VC_COUNT;
                if (dat_credit_reg[candidate] != 0 || dat_credit_return_i[candidate]) begin
                    dat_vc_available = 1'b1;
                    dat_selected_vc = VC_W'(candidate);
                end
            end
        end
    end

    always_comb begin
        req_header = '0;
        req_payload = '0;
        m_req_valid_o = 1'b0;
        if (req_write_lock_reg && narrow_w_usage_reg != 0) begin
            req_header = make_header(ni_flit_pkg::AXI_CH_WIDTH'(ni_flit_pkg::AXI_CH_NarrowW),
                req_active_aw_reg.meta, req_active_aw_reg.collective_op,
                req_active_aw_reg.collective_mask, '0, !req_active_aw_reg.meta.ordering_req,
                narrow_w_head.axi.wlast);
            req_payload = pack_w(narrow_w_head, 1'b1);
            m_req_valid_o = 1'b1;
        end else if (req_select_aw) begin
            req_header = make_header(ni_flit_pkg::AXI_CH_WIDTH'(ni_flit_pkg::AXI_CH_NarrowAw),
                narrow_aw_head.meta, narrow_aw_head.collective_op,
                narrow_aw_head.collective_mask, '0, !narrow_aw_head.meta.ordering_req, 1'b0);
            req_payload = pack_aw(narrow_aw_head);
            m_req_valid_o = 1'b1;
        end else if (!req_write_lock_reg && ar_usage_reg != 0) begin
            req_header = make_header(ni_flit_pkg::AXI_CH_WIDTH'(
                ar_head.meta.route.domain.is_data ? ni_flit_pkg::AXI_CH_DataAr :
                                                    ni_flit_pkg::AXI_CH_NarrowAr),
                ar_head.meta, '0, '0, '0, 1'b0, 1'b1);
            req_payload = pack_ar(ar_head);
            m_req_valid_o = 1'b1;
        end
        m_req_o.header = req_header;
        m_req_o.payload = req_payload[ni_flit_pkg::AW_WIDTH-1:0];
    end

    always_comb begin
        dat_header = '0;
        dat_payload = '0;
        m_dat_valid_o = 1'b0;
        if (dat_write_lock_reg && data_w_usage_reg != 0 &&
            (dat_credit_reg[dat_active_vc_reg] != 0 || dat_credit_return_i[dat_active_vc_reg])) begin
            dat_header = make_header(ni_flit_pkg::AXI_CH_WIDTH'(ni_flit_pkg::AXI_CH_DataW),
                dat_active_aw_reg.meta, dat_active_aw_reg.collective_op,
                dat_active_aw_reg.collective_mask, dat_active_vc_reg,
                !dat_active_aw_reg.meta.ordering_req, data_w_head.axi.wlast);
            dat_payload = pack_w(data_w_head, 1'b0);
            m_dat_valid_o = 1'b1;
        end else if (!dat_write_lock_reg && data_aw_usage_reg != 0 && data_w_usage_reg != 0 &&
                     dat_vc_available) begin
            dat_header = make_header(ni_flit_pkg::AXI_CH_WIDTH'(ni_flit_pkg::AXI_CH_DataAw),
                data_aw_head.meta, data_aw_head.collective_op, data_aw_head.collective_mask,
                dat_selected_vc, !data_aw_head.meta.ordering_req, 1'b0);
            dat_payload = pack_aw(data_aw_head);
            m_dat_valid_o = 1'b1;
        end
        m_dat_o.header = dat_header;
        m_dat_o.payload = dat_payload;
    end

    always_ff @(posedge clk_i) begin
        if (rst_i) begin
            owner_wr_ptr_reg <= '0;
            owner_rd_ptr_reg <= '0;
            narrow_aw_wr_ptr_reg <= '0;
            narrow_aw_rd_ptr_reg <= '0;
            data_aw_wr_ptr_reg <= '0;
            data_aw_rd_ptr_reg <= '0;
            ar_wr_ptr_reg <= '0;
            ar_rd_ptr_reg <= '0;
            narrow_w_wr_ptr_reg <= '0;
            narrow_w_rd_ptr_reg <= '0;
            data_w_wr_ptr_reg <= '0;
            data_w_rd_ptr_reg <= '0;
            owner_usage_reg <= '0;
            narrow_aw_usage_reg <= '0;
            data_aw_usage_reg <= '0;
            ar_usage_reg <= '0;
            narrow_w_usage_reg <= '0;
            data_w_usage_reg <= '0;
            owner_beat_index_reg <= '0;
            req_write_lock_reg <= 1'b0;
            dat_write_lock_reg <= 1'b0;
            req_rr_reg <= 1'b0;
            dat_vc_rr_reg <= '0;
            fixed_vc_valid_reg <= '0;
            for (int vc = 0; vc < DAT_NUM_VC; vc++) begin
                dat_credit_reg[vc] <= CREDIT_W'(ROUTER_VC_DEPTH);
            end
        end else begin
            if (aw_accept) begin
                owner_mem[owner_wr_ptr_reg] <= s_aw_i;
                owner_wr_ptr_reg <= ptr_next(owner_wr_ptr_reg);
                if (aw_is_data) begin
                    data_aw_mem[data_aw_wr_ptr_reg] <= s_aw_i;
                    data_aw_wr_ptr_reg <= ptr_next(data_aw_wr_ptr_reg);
                end else begin
                    narrow_aw_mem[narrow_aw_wr_ptr_reg] <= s_aw_i;
                    narrow_aw_wr_ptr_reg <= ptr_next(narrow_aw_wr_ptr_reg);
                end
            end
            if (w_accept) begin
                if (owner_head.meta.route.domain.is_data) begin
                    data_w_mem[data_w_wr_ptr_reg] <= '{axi: s_w_i, owner: owner_head,
                                                       beat_index: owner_beat_index_reg};
                    data_w_wr_ptr_reg <= ptr_next(data_w_wr_ptr_reg);
                end else begin
                    narrow_w_mem[narrow_w_wr_ptr_reg] <= '{axi: s_w_i, owner: owner_head,
                                                           beat_index: owner_beat_index_reg};
                    narrow_w_wr_ptr_reg <= ptr_next(narrow_w_wr_ptr_reg);
                end
                if (s_w_i.wlast) begin
                    owner_rd_ptr_reg <= ptr_next(owner_rd_ptr_reg);
                    owner_beat_index_reg <= '0;
                end else begin
                    owner_beat_index_reg <= owner_beat_index_reg + 1'b1;
                end
            end
            if (ar_accept) begin
                ar_mem[ar_wr_ptr_reg] <= s_ar_i;
                ar_wr_ptr_reg <= ptr_next(ar_wr_ptr_reg);
            end

            if (req_transfer) begin
                if (req_write_lock_reg) begin
                    narrow_w_rd_ptr_reg <= ptr_next(narrow_w_rd_ptr_reg);
                    if (narrow_w_head.axi.wlast) req_write_lock_reg <= 1'b0;
                end else if (req_select_aw) begin
                    req_active_aw_reg <= narrow_aw_head;
                    narrow_aw_rd_ptr_reg <= ptr_next(narrow_aw_rd_ptr_reg);
                    req_write_lock_reg <= 1'b1;
                    req_rr_reg <= 1'b1;
                end else begin
                    ar_rd_ptr_reg <= ptr_next(ar_rd_ptr_reg);
                    req_rr_reg <= 1'b0;
                end
            end

            if (dat_transfer) begin
                if (dat_write_lock_reg) begin
                    data_w_rd_ptr_reg <= ptr_next(data_w_rd_ptr_reg);
                    if (data_w_head.axi.wlast) dat_write_lock_reg <= 1'b0;
                end else begin
                    dat_active_aw_reg <= data_aw_head;
                    dat_active_vc_reg <= dat_selected_vc;
                    data_aw_rd_ptr_reg <= ptr_next(data_aw_rd_ptr_reg);
                    dat_write_lock_reg <= 1'b1;
                    dat_vc_rr_reg <= dat_selected_vc == WRITE_VC_COUNT-1 ? '0 : dat_selected_vc + 1'b1;
                    if (!data_aw_head.meta.ordering_req) begin
                        fixed_vc_valid_reg[data_aw_head.axi.awid] <= 1'b1;
                        fixed_vc_dst_reg[data_aw_head.axi.awid] <= data_aw_head.meta.route.domain.dst_id;
                        fixed_vc_id_reg[data_aw_head.axi.awid] <= dat_selected_vc;
                    end
                end
            end

            case ({aw_accept, w_accept && s_w_i.wlast})
                2'b10: owner_usage_reg <= owner_usage_reg + 1'b1;
                2'b01: owner_usage_reg <= owner_usage_reg - 1'b1;
                default: owner_usage_reg <= owner_usage_reg;
            endcase
            case ({aw_accept && !aw_is_data, req_transfer && req_select_aw})
                2'b10: narrow_aw_usage_reg <= narrow_aw_usage_reg + 1'b1;
                2'b01: narrow_aw_usage_reg <= narrow_aw_usage_reg - 1'b1;
                default: narrow_aw_usage_reg <= narrow_aw_usage_reg;
            endcase
            case ({aw_accept && aw_is_data, dat_transfer && !dat_write_lock_reg})
                2'b10: data_aw_usage_reg <= data_aw_usage_reg + 1'b1;
                2'b01: data_aw_usage_reg <= data_aw_usage_reg - 1'b1;
                default: data_aw_usage_reg <= data_aw_usage_reg;
            endcase
            case ({ar_accept, req_transfer && !req_write_lock_reg && !req_select_aw})
                2'b10: ar_usage_reg <= ar_usage_reg + 1'b1;
                2'b01: ar_usage_reg <= ar_usage_reg - 1'b1;
                default: ar_usage_reg <= ar_usage_reg;
            endcase
            case ({w_accept && !owner_head.meta.route.domain.is_data,
                   req_transfer && req_write_lock_reg})
                2'b10: narrow_w_usage_reg <= narrow_w_usage_reg + 1'b1;
                2'b01: narrow_w_usage_reg <= narrow_w_usage_reg - 1'b1;
                default: narrow_w_usage_reg <= narrow_w_usage_reg;
            endcase
            case ({w_accept && owner_head.meta.route.domain.is_data,
                   dat_transfer && dat_write_lock_reg})
                2'b10: data_w_usage_reg <= data_w_usage_reg + 1'b1;
                2'b01: data_w_usage_reg <= data_w_usage_reg - 1'b1;
                default: data_w_usage_reg <= data_w_usage_reg;
            endcase

            for (int vc = 0; vc < DAT_NUM_VC; vc++) begin
                case ({dat_credit_return_i[vc], dat_transfer &&
                       (dat_write_lock_reg ? dat_active_vc_reg : dat_selected_vc) == VC_W'(vc)})
                    2'b10: dat_credit_reg[vc] <= dat_credit_reg[vc] + 1'b1;
                    2'b01: dat_credit_reg[vc] <= dat_credit_reg[vc] - 1'b1;
                    default: dat_credit_reg[vc] <= dat_credit_reg[vc];
                endcase
            end
        end
    end

    if (FIFO_DEPTH < 2 || (FIFO_DEPTH & (FIFO_DEPTH-1)) != 0) begin : gen_invalid_fifo_depth
        $fatal(0, "Error: FIFO_DEPTH must be a power of two >= 2 (instance %m)");
    end
    if (DAT_NUM_VC < 1 || DAT_NUM_VC > (1 << ni_flit_pkg::VC_ID_WIDTH)) begin : gen_invalid_vc_count
        $fatal(0, "Error: DAT_NUM_VC is outside the encoded VC range (instance %m)");
    end
    if (DAT_VC_MODE > DAT_VC_MODE_READ_WRITE_SPLIT ||
        (DAT_VC_MODE == DAT_VC_MODE_READ_WRITE_SPLIT &&
         (DAT_NUM_VC < 2 || DAT_NUM_VC[0]))) begin : gen_invalid_vc_mode
        $fatal(0, "Error: split DAT VC mode requires a positive even DAT_NUM_VC (instance %m)");
    end
    if (ROUTER_VC_DEPTH < 2 || (ROUTER_VC_DEPTH & (ROUTER_VC_DEPTH-1)) != 0) begin : gen_invalid_credit_depth
        $fatal(0, "Error: ROUTER_VC_DEPTH must be a power of two >= 2 (instance %m)");
    end

endmodule

`resetall
