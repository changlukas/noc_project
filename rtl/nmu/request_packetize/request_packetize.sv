// SPDX-License-Identifier: Apache-2.0

`resetall
`timescale 1ns / 1ps
`default_nettype none

// Combinational flit encoding with optional per-channel output registers.
module nmu_request_packetize #(
    parameter int unsigned REQ_AW_REG_TYPE                           = 0,
    parameter int unsigned REQ_W_REG_TYPE                            = 0,
    parameter int unsigned REQ_AR_REG_TYPE                           = 0,
    parameter int unsigned DAT_AW_REG_TYPE                           = 0,
    parameter int unsigned DAT_W_REG_TYPE                            = 0,
    parameter logic [ni_flit_pkg::SRC_ID_WIDTH-1:0]      SRC_ID      = '0,
    parameter logic [ni_flit_pkg::SRC_PORT_ID_WIDTH-1:0] SRC_PORT_ID = '0
) (
    input  wire logic                                                             clk_i,
    input  wire logic                                                             rst_n_i,
    input  wire ni_types_pkg::nmu_aw_request_t                                    s_aw_i,
    input  wire logic                                                             s_aw_valid_i,
    output wire logic                                                             s_aw_ready_o,
    input  wire ni_types_pkg::nmu_aw_request_t                                    s_w_aw_i,
    input  wire logic                            [ni_flit_pkg::AXI_LEN_WIDTH-1:0] s_w_beat_i,
    input  wire ni_signals_pkg::noc_axi_w_t                                       s_w_i,
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
    typedef struct packed {
        ni_signals_pkg::noc_axi_w_t                                         axi;
        ni_types_pkg::nmu_aw_request_t                                  owner;
        logic                          [ni_flit_pkg::AXI_LEN_WIDTH-1:0] beat_index;
    } write_beat_t;

    wire write_beat_t write_beat = '{axi: s_w_i, owner: s_w_aw_i, beat_index: s_w_beat_i};
    wire aw_is_data = s_aw_i.meta.route.domain.is_data;
    wire w_is_data = s_w_aw_i.meta.route.domain.is_data;
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
        logic    [ni_flit_pkg::PAYLOAD_WIDTH-1:0] value;
        logic [ni_params_pkg::AXI_ADDR_WIDTH-1:0] beat_addr;
        logic   [ni_params_pkg::AXI_ADDR_WIDTH:0] beat_bytes, wrap_bytes;
        logic [ni_params_pkg::AXI_ADDR_WIDTH-1:0] wrap_base;
        logic      [ni_flit_pkg::AXI_LEN_WIDTH:0] burst_beats;
        logic [$clog2(ni_params_pkg::AXI_DATA_WIDTH /
                      ni_flit_pkg::NOC_NARROW_DATA_WIDTH)-1:0] lane;
        value = '0;
        if (narrow) begin
            beat_bytes = (ni_params_pkg::AXI_ADDR_WIDTH+1)'(1) << beat.owner.axi.awsize;
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

    ni_flit_pkg::req_flit_t [NUM_NMU_REQ_CH-1:0] req_flit;
    ni_flit_pkg::dat_flit_t [NUM_NMU_DAT_CH-1:0] dat_flit;
    wire ni_flit_pkg::req_flit_t [NUM_NMU_REQ_CH-1:0] req_output;
    wire ni_flit_pkg::dat_flit_t [NUM_NMU_DAT_CH-1:0] dat_output;
    for (genvar n = 0; n < NUM_NMU_REQ_CH; n++) begin : gen_req_output
        assign m_req_o[n] = m_req_valid_o[n] ? req_output[n] : '0;
    end
    for (genvar n = 0; n < NUM_NMU_DAT_CH; n++) begin : gen_dat_output
        assign m_dat_o[n] = m_dat_valid_o[n] ? dat_output[n] : '0;
    end
    wire [NUM_NMU_REQ_CH-1:0] req_valid, req_ready;
    wire [NUM_NMU_DAT_CH-1:0] dat_valid, dat_ready;
    assign req_valid[NMU_REQ_AW_IDX] = rst_n_i && s_aw_valid_i && !aw_is_data;
    assign req_valid[NMU_REQ_W_IDX]  = rst_n_i && s_w_valid_i && !w_is_data;
    assign req_valid[NMU_REQ_AR_IDX] = rst_n_i && s_ar_valid_i;
    assign dat_valid[NMU_DAT_AW_IDX] = rst_n_i && s_aw_valid_i && aw_is_data;
    assign dat_valid[NMU_DAT_W_IDX]  = rst_n_i && s_w_valid_i && w_is_data;
    assign s_aw_ready_o              = rst_n_i && (aw_is_data ? dat_ready[NMU_DAT_AW_IDX] : req_ready[NMU_REQ_AW_IDX]);
    assign s_w_ready_o               = rst_n_i && (w_is_data ? dat_ready[NMU_DAT_W_IDX] : req_ready[NMU_REQ_W_IDX]);
    assign s_ar_ready_o              = rst_n_i && req_ready[NMU_REQ_AR_IDX];
    always_comb begin
        req_flit = '0;
        dat_flit = '0;
        req_flit[NMU_REQ_AW_IDX].header = make_header(ni_flit_pkg::AXI_CH_WIDTH'(ni_flit_pkg::AXI_CH_NarrowAw),
            s_aw_i.meta, s_aw_i.collective_op, s_aw_i.collective_mask,
            '0, !s_aw_i.meta.ordering_req, 1'b0);
        req_flit[NMU_REQ_AW_IDX].payload = ni_flit_pkg::AW_WIDTH'(pack_aw(s_aw_i));
        req_flit[NMU_REQ_W_IDX].header   = make_header(ni_flit_pkg::AXI_CH_WIDTH'(ni_flit_pkg::AXI_CH_NarrowW),
            write_beat.owner.meta, write_beat.owner.collective_op, write_beat.owner.collective_mask,
            '0, !write_beat.owner.meta.ordering_req, write_beat.axi.wlast);
        req_flit[NMU_REQ_W_IDX].payload = ni_flit_pkg::AW_WIDTH'(pack_w(write_beat, 1'b1));
        req_flit[NMU_REQ_AR_IDX].header = make_header(ni_flit_pkg::AXI_CH_WIDTH'(
            s_ar_i.meta.route.domain.is_data ? ni_flit_pkg::AXI_CH_DataAr : ni_flit_pkg::AXI_CH_NarrowAr),
            s_ar_i.meta, '0, '0, '0, 1'b0, 1'b1);
        req_flit[NMU_REQ_AR_IDX].payload = ni_flit_pkg::AW_WIDTH'(pack_ar(s_ar_i));
        dat_flit[NMU_DAT_AW_IDX].header  = make_header(ni_flit_pkg::AXI_CH_WIDTH'(ni_flit_pkg::AXI_CH_DataAw),
            s_aw_i.meta, s_aw_i.collective_op, s_aw_i.collective_mask,
            '0, !s_aw_i.meta.ordering_req, 1'b0);
        dat_flit[NMU_DAT_AW_IDX].payload = pack_aw(s_aw_i);
        dat_flit[NMU_DAT_W_IDX].header   = make_header(ni_flit_pkg::AXI_CH_WIDTH'(ni_flit_pkg::AXI_CH_DataW),
            write_beat.owner.meta, write_beat.owner.collective_op, write_beat.owner.collective_mask,
            '0, !write_beat.owner.meta.ordering_req, write_beat.axi.wlast);
        dat_flit[NMU_DAT_W_IDX].payload = pack_w(write_beat, 1'b0);
    end
    stream_register #(
        .REG_TYPE (REQ_AW_REG_TYPE        ),
        .data_t   (ni_flit_pkg::req_flit_t)
    ) i_req_aw_reg (
        .clk_i     (clk_i                                                    ),
        .rst_n_i   (rst_n_i                                                  ),
        .s_data_i  (req_valid[NMU_REQ_AW_IDX] ? req_flit[NMU_REQ_AW_IDX] : '0),
        .s_valid_i (req_valid[NMU_REQ_AW_IDX]                                ),
        .s_ready_o (req_ready[NMU_REQ_AW_IDX]                                ),
        .m_data_o  (req_output[NMU_REQ_AW_IDX]                               ),
        .m_valid_o (m_req_valid_o[NMU_REQ_AW_IDX]                            ),
        .m_ready_i (m_req_ready_i[NMU_REQ_AW_IDX]                            )
    );
    stream_register #(
        .REG_TYPE (REQ_W_REG_TYPE         ),
        .data_t   (ni_flit_pkg::req_flit_t)
    ) i_req_w_reg (
        .clk_i     (clk_i                                                  ),
        .rst_n_i   (rst_n_i                                                ),
        .s_data_i  (req_valid[NMU_REQ_W_IDX] ? req_flit[NMU_REQ_W_IDX] : '0),
        .s_valid_i (req_valid[NMU_REQ_W_IDX]                               ),
        .s_ready_o (req_ready[NMU_REQ_W_IDX]                               ),
        .m_data_o  (req_output[NMU_REQ_W_IDX]                              ),
        .m_valid_o (m_req_valid_o[NMU_REQ_W_IDX]                           ),
        .m_ready_i (m_req_ready_i[NMU_REQ_W_IDX]                           )
    );
    stream_register #(
        .REG_TYPE (REQ_AR_REG_TYPE        ),
        .data_t   (ni_flit_pkg::req_flit_t)
    ) i_req_ar_reg (
        .clk_i     (clk_i                                                    ),
        .rst_n_i   (rst_n_i                                                  ),
        .s_data_i  (req_valid[NMU_REQ_AR_IDX] ? req_flit[NMU_REQ_AR_IDX] : '0),
        .s_valid_i (req_valid[NMU_REQ_AR_IDX]                                ),
        .s_ready_o (req_ready[NMU_REQ_AR_IDX]                                ),
        .m_data_o  (req_output[NMU_REQ_AR_IDX]                               ),
        .m_valid_o (m_req_valid_o[NMU_REQ_AR_IDX]                            ),
        .m_ready_i (m_req_ready_i[NMU_REQ_AR_IDX]                            )
    );
    stream_register #(
        .REG_TYPE (DAT_AW_REG_TYPE        ),
        .data_t   (ni_flit_pkg::dat_flit_t)
    ) i_dat_aw_reg (
        .clk_i     (clk_i                                                    ),
        .rst_n_i   (rst_n_i                                                  ),
        .s_data_i  (dat_valid[NMU_DAT_AW_IDX] ? dat_flit[NMU_DAT_AW_IDX] : '0),
        .s_valid_i (dat_valid[NMU_DAT_AW_IDX]                                ),
        .s_ready_o (dat_ready[NMU_DAT_AW_IDX]                                ),
        .m_data_o  (dat_output[NMU_DAT_AW_IDX]                               ),
        .m_valid_o (m_dat_valid_o[NMU_DAT_AW_IDX]                            ),
        .m_ready_i (m_dat_ready_i[NMU_DAT_AW_IDX]                            )
    );
    stream_register #(
        .REG_TYPE (DAT_W_REG_TYPE         ),
        .data_t   (ni_flit_pkg::dat_flit_t)
    ) i_dat_w_reg (
        .clk_i     (clk_i                                                  ),
        .rst_n_i   (rst_n_i                                                ),
        .s_data_i  (dat_valid[NMU_DAT_W_IDX] ? dat_flit[NMU_DAT_W_IDX] : '0),
        .s_valid_i (dat_valid[NMU_DAT_W_IDX]                               ),
        .s_ready_o (dat_ready[NMU_DAT_W_IDX]                               ),
        .m_data_o  (dat_output[NMU_DAT_W_IDX]                              ),
        .m_valid_o (m_dat_valid_o[NMU_DAT_W_IDX]                           ),
        .m_ready_i (m_dat_ready_i[NMU_DAT_W_IDX]                           )
    );
endmodule
`resetall
