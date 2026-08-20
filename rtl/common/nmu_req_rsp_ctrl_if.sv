// SPDX-License-Identifier: Apache-2.0

`resetall
`timescale 1ns / 1ps
`default_nettype none

interface nmu_req_rsp_ctrl_if #(
    parameter int unsigned AXI_ID_WIDTH = ni_params_pkg::AXI_ID_WIDTH_DFLT,
    parameter int unsigned MAX_TXNS_PER_ID = ni_params_pkg::NMU_MAX_TXNS_PER_ID_DFLT
);

    localparam int unsigned AXI_LEN_WIDTH = ni_flit_pkg::AXI_LEN_WIDTH;
    localparam int unsigned DST_ID_WIDTH = ni_flit_pkg::DST_ID_WIDTH;
    localparam int unsigned DST_PORT_ID_WIDTH = ni_flit_pkg::DST_PORT_ID_WIDTH;
    localparam int unsigned ORDERING_TAG_WIDTH = ni_flit_pkg::ORDERING_TAG_WIDTH;

    if (AXI_ID_WIDTH < 1 || AXI_ID_WIDTH > 8) begin : gen_invalid_axi_id_width
        initial $fatal(0, "Error: AXI_ID_WIDTH must be between 1 and 8 (instance %m)");
    end
    if (MAX_TXNS_PER_ID < 1) begin : gen_invalid_max_txns
        initial $fatal(0, "Error: MAX_TXNS_PER_ID must be at least 1 (instance %m)");
    end

    // Request path -> ResponseBuffer: independent AW and AR admission lanes.
    logic aw_req_valid, aw_req_ready;
    logic [AXI_ID_WIDTH-1:0] aw_req_axi_id;
    logic [AXI_LEN_WIDTH-1:0] aw_req_burst_len;
    logic [DST_ID_WIDTH-1:0] aw_req_dst_id;
    logic [DST_PORT_ID_WIDTH-1:0] aw_req_dst_port_id;
    logic aw_req_is_data;
    logic ar_req_valid, ar_req_ready;
    logic [AXI_ID_WIDTH-1:0] ar_req_axi_id;
    logic [AXI_LEN_WIDTH-1:0] ar_req_burst_len;
    logic [DST_ID_WIDTH-1:0] ar_req_dst_id;
    logic [DST_PORT_ID_WIDTH-1:0] ar_req_dst_port_id;
    logic ar_req_is_data;

    // ResponseBuffer -> Request path: ordering metadata stamped on requests.
    logic aw_rsp_valid, aw_rsp_ready, aw_rsp_ordering_req;
    logic [ORDERING_TAG_WIDTH-1:0] aw_rsp_ordering_tag;
    logic ar_rsp_valid, ar_rsp_ready, ar_rsp_ordering_req;
    logic [ORDERING_TAG_WIDTH-1:0] ar_rsp_ordering_tag;

    // Response path -> ResponseBuffer: independent B and R retirement lanes.
    logic b_retire_valid, b_retire_ready, b_retire_ordering_req, b_retire_last;
    logic [AXI_ID_WIDTH-1:0] b_retire_axi_id;
    logic [ORDERING_TAG_WIDTH-1:0] b_retire_ordering_tag;
    logic r_retire_valid, r_retire_ready, r_retire_ordering_req, r_retire_last;
    logic [AXI_ID_WIDTH-1:0] r_retire_axi_id;
    logic [ORDERING_TAG_WIDTH-1:0] r_retire_ordering_tag;

    modport request_path (
        output aw_req_valid, output aw_req_axi_id, output aw_req_burst_len,
        output aw_req_dst_id, output aw_req_dst_port_id, output aw_req_is_data,
        input aw_req_ready,
        output ar_req_valid, output ar_req_axi_id, output ar_req_burst_len,
        output ar_req_dst_id, output ar_req_dst_port_id, output ar_req_is_data,
        input ar_req_ready,
        input aw_rsp_valid, input aw_rsp_ordering_req, input aw_rsp_ordering_tag,
        output aw_rsp_ready,
        input ar_rsp_valid, input ar_rsp_ordering_req, input ar_rsp_ordering_tag,
        output ar_rsp_ready,
        input b_retire_valid, input b_retire_axi_id, input b_retire_ordering_req,
        input b_retire_ordering_tag, input b_retire_last, output b_retire_ready,
        input r_retire_valid, input r_retire_axi_id, input r_retire_ordering_req,
        input r_retire_ordering_tag, input r_retire_last, output r_retire_ready
    );

    modport response_buffer (
        input aw_req_valid, input aw_req_axi_id, input aw_req_burst_len,
        input aw_req_dst_id, input aw_req_dst_port_id, input aw_req_is_data,
        output aw_req_ready,
        input ar_req_valid, input ar_req_axi_id, input ar_req_burst_len,
        input ar_req_dst_id, input ar_req_dst_port_id, input ar_req_is_data,
        output ar_req_ready,
        output aw_rsp_valid, output aw_rsp_ordering_req, output aw_rsp_ordering_tag,
        input aw_rsp_ready,
        output ar_rsp_valid, output ar_rsp_ordering_req, output ar_rsp_ordering_tag,
        input ar_rsp_ready,
        output b_retire_valid, output b_retire_axi_id, output b_retire_ordering_req,
        output b_retire_ordering_tag, output b_retire_last, input b_retire_ready,
        output r_retire_valid, output r_retire_axi_id, output r_retire_ordering_req,
        output r_retire_ordering_tag, output r_retire_last, input r_retire_ready
    );

endinterface

`resetall
