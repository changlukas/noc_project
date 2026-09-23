`timescale 1ns/1ps

`ifndef NI_TYPES_PKG_SVH
`define NI_TYPES_PKG_SVH

package ni_types_pkg;

    // Fixed packetizer slots, not configurable VC or source counts.
    localparam int NMU_REQ_AW_IDX = 0;
    localparam int NMU_REQ_W_IDX  = 1;
    localparam int NMU_REQ_AR_IDX = 2;
    localparam int NUM_NMU_REQ_CH = 3;
    localparam int NMU_DAT_AW_IDX = 0;
    localparam int NMU_DAT_W_IDX  = 1;
    localparam int NUM_NMU_DAT_CH = 2;

    // NMU request classification.  This is the complete ordering-domain key.
    typedef struct packed {
        logic      [ni_flit_pkg::DST_ID_WIDTH-1:0] dst_id;
        logic [ni_flit_pkg::DST_PORT_ID_WIDTH-1:0] dst_port_id;
        logic                                      is_data;
    } nmu_ordering_domain_t;

    typedef struct packed {
        nmu_ordering_domain_t domain;
    } nmu_route_t;

    // AW-only state: AR has no collective surface or exposed user sideband.
    typedef struct packed {
        nmu_route_t                                          route;
        logic              [ni_flit_pkg::AXI_USER_WIDTH-1:0] user;
        logic         [ni_flit_pkg::COLLECTIVE_OP_WIDTH-1:0] collective_op;
        logic       [ni_flit_pkg::COLLECTIVE_MASK_WIDTH-1:0] collective_mask;
    } nmu_aw_route_t;

    typedef struct packed {
        nmu_route_t                                       route;
        logic                                             ordering_req;
        logic       [ni_flit_pkg::ORDERING_TAG_WIDTH-1:0] ordering_tag;
    } nmu_request_t;

    typedef struct packed {
        logic                                       is_data;
        logic                                       ordering_req;
        logic [ni_flit_pkg::ORDERING_TAG_WIDTH-1:0] ordering_tag;
    } nmu_response_t;

    typedef struct packed {
        ni_signals_pkg::axi_aw_t axi;
        nmu_aw_route_t           route;
    } nmu_sam_aw_result_t;

    typedef struct packed {
        ni_signals_pkg::axi_ar_t axi;
        nmu_route_t              route;
    } nmu_sam_ar_result_t;

    typedef struct packed {
        ni_signals_pkg::axi_aw_t                                          axi;
        nmu_request_t                                                     meta;
        logic                           [ni_flit_pkg::AXI_USER_WIDTH-1:0] user;
        logic                      [ni_flit_pkg::COLLECTIVE_OP_WIDTH-1:0] collective_op;
        logic                    [ni_flit_pkg::COLLECTIVE_MASK_WIDTH-1:0] collective_mask;
    } nmu_aw_request_t;

    typedef struct packed {
        ni_signals_pkg::axi_ar_t axi;
        nmu_request_t            meta;
    } nmu_ar_request_t;

    typedef struct packed {
        ni_signals_pkg::axi_b_t axi;
        nmu_response_t          meta;
    } nmu_b_response_t;

    typedef struct packed {
        ni_signals_pkg::axi_r_t axi;
        nmu_response_t          meta;
    } nmu_r_response_t;

    // Per-ID issue-order record.  beat_count represents one through 256 beats.
    typedef struct packed {
        logic [ni_flit_pkg::ORDERING_TAG_WIDTH-1:0] base;
        logic        [ni_flit_pkg::AXI_LEN_WIDTH:0] beat_count;
        logic                                       ordering_req;
        logic                                       collective;
    } nmu_rob_order_entry_t;

    // Enabled-mode B and R slot records.  Allocation and completion are
    // separate bits because an allocated response may not have arrived yet.
    typedef struct packed {
        logic                   occupied;
        logic                   complete;
        ni_signals_pkg::axi_b_t beat;
    } nmu_b_rob_entry_t;

    typedef struct packed {
        logic occupied;
        logic complete;
        logic [$clog2(ni_params_pkg::AXI_DATA_WIDTH /
                      ni_flit_pkg::NOC_NARROW_DATA_WIDTH)-1:0] narrow_lane;
        ni_signals_pkg::axi_r_t beat;
    } nmu_r_rob_entry_t;

    // Narrow-read address basis shared by enabled bypass and structural
    // R_ROB_EN=0 paths.  beat_index advances on each accepted R beat.
    typedef struct packed {
        logic [ni_params_pkg::AXI_ADDR_WIDTH-1:0] local_addr;
        logic    [ni_flit_pkg::AXI_LEN_WIDTH-1:0] len;
        logic   [ni_flit_pkg::AXI_SIZE_WIDTH-1:0] size;
        logic  [ni_flit_pkg::AXI_BURST_WIDTH-1:0] burst;
        logic    [ni_flit_pkg::AXI_LEN_WIDTH-1:0] beat_index;
    } nmu_read_context_t;

    // NSU Response Queue transaction record.  Write entries zero the read
    // context; read entries zero the collective fields.
    typedef struct packed {
        logic          [ni_flit_pkg::SRC_ID_WIDTH-1:0] src_id;
        logic     [ni_flit_pkg::SRC_PORT_ID_WIDTH-1:0] src_port_id;
        logic        [ni_params_pkg::NOC_ID_WIDTH-1:0] noc_id;
        logic                                          ordering_req;
        logic    [ni_flit_pkg::ORDERING_TAG_WIDTH-1:0] ordering_tag;
        logic                                          is_data;
        logic      [ni_params_pkg::AXI_ADDR_WIDTH-1:0] local_addr;
        logic         [ni_flit_pkg::AXI_LEN_WIDTH-1:0] len;
        logic        [ni_flit_pkg::AXI_SIZE_WIDTH-1:0] size;
        logic       [ni_flit_pkg::AXI_BURST_WIDTH-1:0] burst;
        logic   [ni_flit_pkg::COLLECTIVE_OP_WIDTH-1:0] collective_op;
        logic [ni_flit_pkg::COLLECTIVE_MASK_WIDTH-1:0] collective_mask;
    } response_entry_t;

    typedef struct packed {
        ni_signals_pkg::axi_aw_t axi;
        response_entry_t         response;
    } nsu_aw_request_t;

    typedef struct packed {
        ni_signals_pkg::axi_ar_t axi;
        response_entry_t         response;
    } nsu_ar_request_t;

    typedef struct packed {
        ni_signals_pkg::axi_b_t axi;
        response_entry_t        response;
    } nsu_b_response_t;

    typedef struct packed {
        ni_signals_pkg::axi_r_t axi;
        response_entry_t        response;
    } nsu_r_response_t;

endpackage

`endif // NI_TYPES_PKG_SVH
