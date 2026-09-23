// SPDX-License-Identifier: Apache-2.0

`resetall
`timescale 1ns / 1ps
`default_nettype none

// Packetize REQ/DAT independently; accepted AW order determines W ownership.
module nmu_request_packetize #(
    parameter int unsigned                               FIFO_DEPTH = ni_params_pkg::NOC_FIFO_DEPTH,
    parameter logic [ni_flit_pkg::SRC_ID_WIDTH-1:0]      SRC_ID      = '0,
    parameter logic [ni_flit_pkg::SRC_PORT_ID_WIDTH-1:0] SRC_PORT_ID = '0
) (
    input  wire logic                                                             clk_i,
    input  wire logic                                                             rst_i,
    input  wire ni_types_pkg::nmu_aw_request_t                                    s_aw_i,
    input  wire logic                                                             s_aw_valid_i,
    output wire logic                                                             s_aw_ready_o,
    input  wire ni_signals_pkg::axi_w_t                                           s_w_i,
    input  wire logic                                                             s_w_valid_i,
    output wire logic                                                             s_w_ready_o,
    input  wire ni_types_pkg::nmu_ar_request_t                                    s_ar_i,
    input  wire logic                                                             s_ar_valid_i,
    output wire logic                                                             s_ar_ready_o,
    output wire ni_flit_pkg::req_flit_t        [ni_types_pkg::NUM_NMU_REQ_CH-1:0] m_req_o,
    output wire logic                          [ni_types_pkg::NUM_NMU_REQ_CH-1:0] m_req_valid_o,
    input  wire logic                          [ni_types_pkg::NUM_NMU_REQ_CH-1:0] m_req_ready_i,
    output wire ni_flit_pkg::dat_flit_t        [ni_types_pkg::NUM_NMU_DAT_CH-1:0] m_dat_o,
    output wire logic                          [ni_types_pkg::NUM_NMU_DAT_CH-1:0] m_dat_valid_o,
    input  wire logic                          [ni_types_pkg::NUM_NMU_DAT_CH-1:0] m_dat_ready_i
);
    import ni_types_pkg::*;

    localparam logic [ni_flit_pkg::AXI_BURST_WIDTH-1:0] AXI_BURST_INCR = 2'b01;
    localparam logic [ni_flit_pkg::AXI_BURST_WIDTH-1:0] AXI_BURST_WRAP = 2'b10;
    if (FIFO_DEPTH < 2 || (FIFO_DEPTH & (FIFO_DEPTH-1)) != 0) begin : gen_invalid_fifo_depth
        initial $fatal(0, "Error: FIFO_DEPTH must be a power of two >= 2 (instance %m)");
    end
    typedef struct packed {
        ni_signals_pkg::axi_w_t                                               axi;
        ni_types_pkg::nmu_aw_request_t                                  owner;
        logic                                [ni_flit_pkg::AXI_LEN_WIDTH-1:0] beat_index;
    } write_beat_t;

    logic                                     [ni_flit_pkg::AXI_LEN_WIDTH-1:0] owner_beat_index_reg, owner_beat_index_next;
    wire ni_types_pkg::nmu_aw_request_t                                  owner_head;
    wire logic                                                                 owner_full, owner_empty;
    wire ni_types_pkg::nmu_aw_request_t                                  narrow_aw_head;
    wire logic                                                                 narrow_aw_full, narrow_aw_empty;
    wire ni_types_pkg::nmu_aw_request_t                                  data_aw_head;
    wire logic                                                                 data_aw_full, data_aw_empty;
    wire ni_types_pkg::nmu_ar_request_t                                  ar_head;
    wire logic                                                                 ar_full, ar_empty;
    wire write_beat_t                                                          narrow_w_head;
    wire logic                                                                 narrow_w_full, narrow_w_empty;
    wire write_beat_t                                                          data_w_head;
    wire logic                                                                 data_w_full, data_w_empty;
    wire write_beat_t write_beat = '{
        axi: s_w_i, owner: owner_head, beat_index: owner_beat_index_reg
    };
    wire logic aw_is_data = s_aw_i.meta.route.domain.is_data;
    wire logic aw_accept = s_aw_valid_i && s_aw_ready_o;
    wire logic w_accept = s_w_valid_i && s_w_ready_o;
    wire logic ar_accept = s_ar_valid_i && s_ar_ready_o;
    function automatic logic [ni_flit_pkg::HEADER_WIDTH-1:0] make_header(
        input logic [ni_flit_pkg::AXI_CH_WIDTH-1:0] axi_ch,
        input ni_types_pkg::nmu_request_t meta,
        input logic [ni_flit_pkg::COLLECTIVE_OP_WIDTH-1:0] collective_op,
        input logic [ni_flit_pkg::COLLECTIVE_MASK_WIDTH-1:0] collective_mask,
        input logic [ni_flit_pkg::VC_ID_WIDTH-1:0] vc_id,
        input logic fixed_vc,
        input logic tail
    );
        logic [ni_flit_pkg::HEADER_WIDTH-1:0] header;
        header                                                                    = '0;
        header[ni_flit_pkg::AXI_CH_MSB:ni_flit_pkg::AXI_CH_LSB]                   = axi_ch;
        header[ni_flit_pkg::SRC_ID_MSB:ni_flit_pkg::SRC_ID_LSB]                   = SRC_ID;
        header[ni_flit_pkg::DST_ID_MSB:ni_flit_pkg::DST_ID_LSB]                   = meta.route.domain.dst_id;
        header[ni_flit_pkg::FIXED_VC_LSB]                                         = fixed_vc;
        header[ni_flit_pkg::VC_ID_MSB:ni_flit_pkg::VC_ID_LSB]                     = ni_flit_pkg::VC_ID_WIDTH'(vc_id);
        header[ni_flit_pkg::FLIT_TAIL_LSB]                                        = tail;
        header[ni_flit_pkg::ORDERING_REQ_LSB]                                     = meta.ordering_req;
        header[ni_flit_pkg::ORDERING_TAG_MSB:ni_flit_pkg::ORDERING_TAG_LSB]       = meta.ordering_tag;
        header[ni_flit_pkg::COLLECTIVE_OP_MSB:ni_flit_pkg::COLLECTIVE_OP_LSB]     = collective_op;
        header[ni_flit_pkg::COLLECTIVE_MASK_MSB:ni_flit_pkg::COLLECTIVE_MASK_LSB] = collective_mask;
        header[ni_flit_pkg::DST_PORT_ID_MSB:ni_flit_pkg::DST_PORT_ID_LSB]         = meta.route.domain.dst_port_id;
        header[ni_flit_pkg::SRC_PORT_ID_MSB:ni_flit_pkg::SRC_PORT_ID_LSB]         = SRC_PORT_ID;
        return header;
    endfunction

    function automatic logic [ni_flit_pkg::PAYLOAD_WIDTH-1:0] pack_aw(
        input ni_types_pkg::nmu_aw_request_t request
    );
        logic [ni_flit_pkg::PAYLOAD_WIDTH-1:0] value;
        value                                                            = '0;
        value[ni_flit_pkg::AW_AWID_MSB:ni_flit_pkg::AW_AWID_LSB]         = request.axi.awid;
        value[ni_flit_pkg::AW_AWADDR_MSB:ni_flit_pkg::AW_AWADDR_LSB]     = request.axi.awaddr;
        value[ni_flit_pkg::AW_AWLEN_MSB:ni_flit_pkg::AW_AWLEN_LSB]       = request.axi.awlen;
        value[ni_flit_pkg::AW_AWSIZE_MSB:ni_flit_pkg::AW_AWSIZE_LSB]     = request.axi.awsize;
        value[ni_flit_pkg::AW_AWBURST_MSB:ni_flit_pkg::AW_AWBURST_LSB]   = request.axi.awburst;
        value[ni_flit_pkg::AW_AWCACHE_MSB:ni_flit_pkg::AW_AWCACHE_LSB]   = request.axi.awcache;
        value[ni_flit_pkg::AW_AWLOCK_LSB]                                = request.axi.awlock;
        value[ni_flit_pkg::AW_AWPROT_MSB:ni_flit_pkg::AW_AWPROT_LSB]     = request.axi.awprot;
        value[ni_flit_pkg::AW_AWREGION_MSB:ni_flit_pkg::AW_AWREGION_LSB] = request.axi.awregion;
        value[ni_flit_pkg::AW_AWQOS_MSB:ni_flit_pkg::AW_AWQOS_LSB]       = request.axi.awqos;
        value[ni_flit_pkg::AW_AWUSER_MSB:ni_flit_pkg::AW_AWUSER_LSB]     = request.user;
        return value;
    endfunction

    function automatic logic [ni_flit_pkg::PAYLOAD_WIDTH-1:0] pack_ar(
        input ni_types_pkg::nmu_ar_request_t request
    );
        logic [ni_flit_pkg::PAYLOAD_WIDTH-1:0] value;
        value                                                            = '0;
        value[ni_flit_pkg::AR_ARID_MSB:ni_flit_pkg::AR_ARID_LSB]         = request.axi.arid;
        value[ni_flit_pkg::AR_ARADDR_MSB:ni_flit_pkg::AR_ARADDR_LSB]     = request.axi.araddr;
        value[ni_flit_pkg::AR_ARLEN_MSB:ni_flit_pkg::AR_ARLEN_LSB]       = request.axi.arlen;
        value[ni_flit_pkg::AR_ARSIZE_MSB:ni_flit_pkg::AR_ARSIZE_LSB]     = request.axi.arsize;
        value[ni_flit_pkg::AR_ARBURST_MSB:ni_flit_pkg::AR_ARBURST_LSB]   = request.axi.arburst;
        value[ni_flit_pkg::AR_ARCACHE_MSB:ni_flit_pkg::AR_ARCACHE_LSB]   = request.axi.arcache;
        value[ni_flit_pkg::AR_ARLOCK_LSB]                                = request.axi.arlock;
        value[ni_flit_pkg::AR_ARPROT_MSB:ni_flit_pkg::AR_ARPROT_LSB]     = request.axi.arprot;
        value[ni_flit_pkg::AR_ARREGION_MSB:ni_flit_pkg::AR_ARREGION_LSB] = request.axi.arregion;
        value[ni_flit_pkg::AR_ARQOS_MSB:ni_flit_pkg::AR_ARQOS_LSB]       = request.axi.arqos;
        return value;
    endfunction

    function automatic logic [ni_flit_pkg::PAYLOAD_WIDTH-1:0] pack_w(
        input write_beat_t beat,
        input logic narrow
    );
        logic         [ni_flit_pkg::PAYLOAD_WIDTH-1:0] value;
        logic [ni_params_pkg::AXI_ADDR_WIDTH-1:0] beat_addr;
        logic   [ni_params_pkg::AXI_ADDR_WIDTH:0] beat_bytes, wrap_bytes;
        logic [ni_params_pkg::AXI_ADDR_WIDTH-1:0] wrap_base;
        logic           [ni_flit_pkg::AXI_LEN_WIDTH:0] burst_beats;
        logic [$clog2(ni_params_pkg::AXI_DATA_WIDTH /
                      ni_flit_pkg::NOC_NARROW_DATA_WIDTH)-1:0] lane;
        value = '0;
        if (narrow) begin
            beat_bytes  = (ni_params_pkg::AXI_ADDR_WIDTH+1)'(1) << beat.owner.axi.awsize;
            burst_beats = (ni_flit_pkg::AXI_LEN_WIDTH+1)'(beat.owner.axi.awlen) +
                (ni_flit_pkg::AXI_LEN_WIDTH+1)'(1);
            wrap_bytes = beat_bytes * burst_beats;
            beat_addr  = beat.owner.axi.awaddr;
            if (beat.owner.axi.awburst == AXI_BURST_INCR) begin
                beat_addr = ni_params_pkg::AXI_ADDR_WIDTH'({1'b0, beat.owner.axi.awaddr} + beat.beat_index * beat_bytes);
            end else if (beat.owner.axi.awburst == AXI_BURST_WRAP) begin
                wrap_base = ni_params_pkg::AXI_ADDR_WIDTH'({1'b0, beat.owner.axi.awaddr} & ~(wrap_bytes-1'b1));
                beat_addr = ni_params_pkg::AXI_ADDR_WIDTH'({1'b0, wrap_base} +
                    (({1'b0, beat.owner.axi.awaddr} - {1'b0, wrap_base} + beat.beat_index * beat_bytes) &
                     (wrap_bytes-1'b1)));
            end
            lane = beat_addr[$clog2(ni_params_pkg::AXI_DATA_WIDTH/8)-1:
                             $clog2(ni_flit_pkg::NOC_NARROW_DATA_WIDTH/8)];
            value[ni_flit_pkg::NARROW_W_WLAST_LSB] = beat.axi.wlast;
            value[ni_flit_pkg::NARROW_W_WSTRB_MSB:ni_flit_pkg::NARROW_W_WSTRB_LSB] =
                beat.axi.wstrb[lane*ni_flit_pkg::NARROW_WSTRB_WIDTH +:
                               ni_flit_pkg::NARROW_WSTRB_WIDTH];
            value[ni_flit_pkg::NARROW_W_WDATA_MSB:ni_flit_pkg::NARROW_W_WDATA_LSB] =
                beat.axi.wdata[lane*ni_flit_pkg::NOC_NARROW_DATA_WIDTH +:
                               ni_flit_pkg::NOC_NARROW_DATA_WIDTH];
        end else begin
            value[ni_flit_pkg::DATA_W_WLAST_LSB]                               = beat.axi.wlast;
            value[ni_flit_pkg::DATA_W_WSTRB_MSB:ni_flit_pkg::DATA_W_WSTRB_LSB] = beat.axi.wstrb;
            value[ni_flit_pkg::DATA_W_WDATA_MSB:ni_flit_pkg::DATA_W_WDATA_LSB] = beat.axi.wdata;
        end
        return value;
    endfunction

    assign s_aw_ready_o = !rst_i && !owner_full &&
        (aw_is_data ? !data_aw_full : !narrow_aw_full);
    assign s_w_ready_o = !rst_i && !owner_empty &&
        (owner_head.meta.route.domain.is_data ? !data_w_full : !narrow_w_full);
    assign s_ar_ready_o = !rst_i && !ar_full;

    // Candidate order: REQ AW/W/AR and DAT AW/W.
    ni_flit_pkg::req_flit_t [NUM_NMU_REQ_CH-1:0] req_flit;
    ni_flit_pkg::dat_flit_t [NUM_NMU_DAT_CH-1:0] dat_flit;
    assign m_req_valid_o[NMU_REQ_AW_IDX] = !rst_i && !narrow_aw_empty;
    assign m_req_valid_o[NMU_REQ_W_IDX]  = !rst_i && !narrow_w_empty;
    assign m_req_valid_o[NMU_REQ_AR_IDX] = !rst_i && !ar_empty;
    assign m_dat_valid_o[NMU_DAT_AW_IDX] = !rst_i && !data_aw_empty;
    assign m_dat_valid_o[NMU_DAT_W_IDX]  = !rst_i && !data_w_empty;
    for (genvar i = 0; i < NUM_NMU_REQ_CH; i++) begin : gen_req
        assign m_req_o[i] = m_req_valid_o[i] ? req_flit[i] : '0;
    end
    for (genvar i = 0; i < NUM_NMU_DAT_CH; i++) begin : gen_dat
        assign m_dat_o[i] = m_dat_valid_o[i] ? dat_flit[i] : '0;
    end
    always_comb begin
        req_flit                    = '0;
        dat_flit                    = '0;
        req_flit[NMU_REQ_AW_IDX].header = make_header(ni_flit_pkg::AXI_CH_WIDTH'(ni_flit_pkg::AXI_CH_NarrowAw),
            narrow_aw_head.meta, narrow_aw_head.collective_op, narrow_aw_head.collective_mask,
            '0, !narrow_aw_head.meta.ordering_req, 1'b0);
        req_flit[NMU_REQ_AW_IDX].payload = ni_flit_pkg::AW_WIDTH'(pack_aw(narrow_aw_head));
        req_flit[NMU_REQ_W_IDX].header   = make_header(ni_flit_pkg::AXI_CH_WIDTH'(ni_flit_pkg::AXI_CH_NarrowW),
            narrow_w_head.owner.meta, narrow_w_head.owner.collective_op, narrow_w_head.owner.collective_mask,
            '0, !narrow_w_head.owner.meta.ordering_req, narrow_w_head.axi.wlast);
        req_flit[NMU_REQ_W_IDX].payload = ni_flit_pkg::AW_WIDTH'(pack_w(narrow_w_head, 1'b1));
        req_flit[NMU_REQ_AR_IDX].header = make_header(ni_flit_pkg::AXI_CH_WIDTH'(
            ar_head.meta.route.domain.is_data ? ni_flit_pkg::AXI_CH_DataAr : ni_flit_pkg::AXI_CH_NarrowAr),
            ar_head.meta, '0, '0, '0, 1'b0, 1'b1);
        req_flit[NMU_REQ_AR_IDX].payload = ni_flit_pkg::AW_WIDTH'(pack_ar(ar_head));
        dat_flit[NMU_DAT_AW_IDX].header  = make_header(ni_flit_pkg::AXI_CH_WIDTH'(ni_flit_pkg::AXI_CH_DataAw),
            data_aw_head.meta, data_aw_head.collective_op, data_aw_head.collective_mask,
            '0, !data_aw_head.meta.ordering_req, 1'b0);
        dat_flit[NMU_DAT_AW_IDX].payload = pack_aw(data_aw_head);
        dat_flit[NMU_DAT_W_IDX].header   = make_header(ni_flit_pkg::AXI_CH_WIDTH'(ni_flit_pkg::AXI_CH_DataW),
            data_w_head.owner.meta, data_w_head.owner.collective_op, data_w_head.owner.collective_mask,
            '0, !data_w_head.owner.meta.ordering_req, data_w_head.axi.wlast);
        dat_flit[NMU_DAT_W_IDX].payload = pack_w(data_w_head, 1'b0);
    end
    cc_fifo #(
        .Depth       (FIFO_DEPTH                    ),
        .FallThrough (1'b0                          ),
        .data_t      (ni_types_pkg::nmu_aw_request_t)
    ) i_owner_fifo (
        .clk_i   (clk_i                  ),
        .rst_ni  (1'b1                   ),
        .clr_i   (1'b0                   ),
        .flush_i (rst_i                  ),
        .full_o  (owner_full             ),
        .empty_o (owner_empty            ),
        .usage_o (                       ),
        .data_i  (s_aw_i                 ),
        .push_i  (aw_accept              ),
        .data_o  (owner_head             ),
        .pop_i   (w_accept && s_w_i.wlast)
    );

    cc_fifo #(
        .Depth       (FIFO_DEPTH                    ),
        .FallThrough (1'b0                          ),
        .data_t      (ni_types_pkg::nmu_aw_request_t)
    ) i_narrow_aw_fifo (
        .clk_i   (clk_i                                                         ),
        .rst_ni  (1'b1                                                          ),
        .clr_i   (1'b0                                                          ),
        .flush_i (rst_i                                                         ),
        .full_o  (narrow_aw_full                                                ),
        .empty_o (narrow_aw_empty                                               ),
        .usage_o (                                                              ),
        .data_i  (s_aw_i                                                        ),
        .push_i  (aw_accept && !aw_is_data                                      ),
        .data_o  (narrow_aw_head                                                ),
        .pop_i   (m_req_valid_o[NMU_REQ_AW_IDX] && m_req_ready_i[NMU_REQ_AW_IDX])
    );

    cc_fifo #(
        .Depth       (FIFO_DEPTH                    ),
        .FallThrough (1'b0                          ),
        .data_t      (ni_types_pkg::nmu_aw_request_t)
    ) i_data_aw_fifo (
        .clk_i   (clk_i                                                         ),
        .rst_ni  (1'b1                                                          ),
        .clr_i   (1'b0                                                          ),
        .flush_i (rst_i                                                         ),
        .full_o  (data_aw_full                                                  ),
        .empty_o (data_aw_empty                                                 ),
        .usage_o (                                                              ),
        .data_i  (s_aw_i                                                        ),
        .push_i  (aw_accept && aw_is_data                                       ),
        .data_o  (data_aw_head                                                  ),
        .pop_i   (m_dat_valid_o[NMU_DAT_AW_IDX] && m_dat_ready_i[NMU_DAT_AW_IDX])
    );

    cc_fifo #(
        .Depth       (FIFO_DEPTH                    ),
        .FallThrough (1'b0                          ),
        .data_t      (ni_types_pkg::nmu_ar_request_t)
    ) i_ar_fifo (
        .clk_i   (clk_i                                                         ),
        .rst_ni  (1'b1                                                          ),
        .clr_i   (1'b0                                                          ),
        .flush_i (rst_i                                                         ),
        .full_o  (ar_full                                                       ),
        .empty_o (ar_empty                                                      ),
        .usage_o (                                                              ),
        .data_i  (s_ar_i                                                        ),
        .push_i  (ar_accept                                                     ),
        .data_o  (ar_head                                                       ),
        .pop_i   (m_req_valid_o[NMU_REQ_AR_IDX] && m_req_ready_i[NMU_REQ_AR_IDX])
    );

    cc_fifo #(
        .Depth       (FIFO_DEPTH  ),
        .FallThrough (1'b0        ),
        .data_t      (write_beat_t)
    ) i_narrow_w_fifo (
        .clk_i   (clk_i                                                       ),
        .rst_ni  (1'b1                                                        ),
        .clr_i   (1'b0                                                        ),
        .flush_i (rst_i                                                       ),
        .full_o  (narrow_w_full                                               ),
        .empty_o (narrow_w_empty                                              ),
        .usage_o (                                                            ),
        .data_i  (write_beat                                                  ),
        .push_i  (w_accept && !owner_head.meta.route.domain.is_data           ),
        .data_o  (narrow_w_head                                               ),
        .pop_i   (m_req_valid_o[NMU_REQ_W_IDX] && m_req_ready_i[NMU_REQ_W_IDX])
    );

    cc_fifo #(
        .Depth       (FIFO_DEPTH  ),
        .FallThrough (1'b0        ),
        .data_t      (write_beat_t)
    ) i_data_w_fifo (
        .clk_i   (clk_i                                                       ),
        .rst_ni  (1'b1                                                        ),
        .clr_i   (1'b0                                                        ),
        .flush_i (rst_i                                                       ),
        .full_o  (data_w_full                                                 ),
        .empty_o (data_w_empty                                                ),
        .usage_o (                                                            ),
        .data_i  (write_beat                                                  ),
        .push_i  (w_accept && owner_head.meta.route.domain.is_data            ),
        .data_o  (data_w_head                                                 ),
        .pop_i   (m_dat_valid_o[NMU_DAT_W_IDX] && m_dat_ready_i[NMU_DAT_W_IDX])
    );

    always_comb begin
        owner_beat_index_next = owner_beat_index_reg;
        if (w_accept)
            owner_beat_index_next = s_w_i.wlast ? '0 : owner_beat_index_reg + 1'b1;
    end
    always_ff @(posedge clk_i) begin
        if (rst_i) owner_beat_index_reg <= '0;
        else owner_beat_index_reg <= owner_beat_index_next;
    end
endmodule
`resetall
