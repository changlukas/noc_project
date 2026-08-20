`timescale 1ns/1ps

`ifndef NI_CHILD_TYPES_PKG_SVH
`define NI_CHILD_TYPES_PKG_SVH

package ni_child_types_pkg;

    // NMU request classification.  This is the complete ordering-domain key.
    typedef struct packed {
        logic [ni_flit_pkg::DST_ID_WIDTH-1:0]      dst_id;
        logic [ni_flit_pkg::DST_PORT_ID_WIDTH-1:0] dst_port_id;
        logic                                      is_data;
    } nmu_ordering_domain_t;

    // SAM-resolved request metadata.  The SAM child consumes the generated
    // topology_pkg::sam_idx_t directly; only the ordering-domain projection
    // crosses every request stream.
    typedef struct packed {
        nmu_ordering_domain_t domain;
    } nmu_route_t;

    // AW-only state: AR has no collective surface or exposed user sideband.
    typedef struct packed {
        nmu_route_t                                     route;
        logic [ni_flit_pkg::AXI_USER_WIDTH-1:0]      user;
        logic [ni_flit_pkg::COLLECTIVE_OP_WIDTH-1:0] collective_op;
        logic [ni_flit_pkg::COLLECTIVE_MASK_WIDTH-1:0] collective_mask;
    } nmu_aw_route_t;

    // RoB-admitted request metadata carried beside axi_aw_t/axi_w_t/axi_ar_t.
    typedef struct packed {
        nmu_route_t                                  route;
        logic                                        ordering_req;
        logic [ni_flit_pkg::ORDERING_TAG_WIDTH-1:0] ordering_tag;
    } nmu_request_t;

    // Decoded response metadata carried beside axi_b_t or axi_r_t.
    typedef struct packed {
        logic                                        is_data;
        logic                                        ordering_req;
        logic [ni_flit_pkg::ORDERING_TAG_WIDTH-1:0] ordering_tag;
    } nmu_response_t;

    typedef struct packed {
        ni_signals_pkg::axi_aw_t axi;
        nmu_aw_route_t            route;
    } nmu_sam_aw_result_t;

    typedef struct packed {
        ni_signals_pkg::axi_ar_t axi;
        nmu_route_t               route;
    } nmu_sam_ar_result_t;

    // nmu_rob ordered request and decoded response stream payloads.
    typedef struct packed {
        ni_signals_pkg::axi_aw_t axi;
        nmu_request_t            meta;
        logic [ni_flit_pkg::AXI_USER_WIDTH-1:0]      user;
        logic [ni_flit_pkg::COLLECTIVE_OP_WIDTH-1:0] collective_op;
        logic [ni_flit_pkg::COLLECTIVE_MASK_WIDTH-1:0] collective_mask;
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
        logic [ni_flit_pkg::AXI_LEN_WIDTH:0]        beat_count;
        logic                                        ordering_req;
        logic                                        collective;
    } nmu_rob_order_entry_t;

    // Enabled-mode B and R slot records.  Allocation and completion are
    // separate bits because an allocated response may not have arrived yet.
    typedef struct packed {
        logic                       occupied;
        logic                       complete;
        ni_signals_pkg::axi_b_t     beat;
    } nmu_b_rob_entry_t;

    typedef struct packed {
        logic                       occupied;
        logic                       complete;
        logic [$clog2(ni_params_pkg::AXI_DATA_WIDTH_DFLT /
                      ni_flit_pkg::NOC_NARROW_DATA_WIDTH)-1:0] narrow_lane;
        ni_signals_pkg::axi_r_t     beat;
    } nmu_r_rob_entry_t;

    // Narrow-read address basis shared by enabled bypass and structural
    // READ_ROB_ENABLED=0 paths.  beat_index advances on each accepted R beat.
    typedef struct packed {
        logic [ni_params_pkg::AXI_ADDR_WIDTH_DFLT-1:0] local_addr;
        logic [ni_flit_pkg::AXI_LEN_WIDTH-1:0]          len;
        logic [ni_flit_pkg::AXI_SIZE_WIDTH-1:0]         size;
        logic [ni_flit_pkg::AXI_BURST_WIDTH-1:0]        burst;
        logic [ni_flit_pkg::AXI_LEN_WIDTH-1:0]          beat_index;
    } nmu_read_context_t;

    // NSU Response Queue transaction record.  Write entries zero the read
    // context; read entries zero the collective fields.
    typedef struct packed {
        logic [ni_flit_pkg::SRC_ID_WIDTH-1:0]          src_id;
        logic [ni_flit_pkg::SRC_PORT_ID_WIDTH-1:0]     src_port_id;
        logic [ni_params_pkg::NOC_ID_WIDTH_DFLT-1:0]   noc_id;
        logic                                           ordering_req;
        logic [ni_flit_pkg::ORDERING_TAG_WIDTH-1:0]    ordering_tag;
        logic                                           is_data;
        logic [ni_params_pkg::AXI_ADDR_WIDTH_DFLT-1:0] local_addr;
        logic [ni_flit_pkg::AXI_LEN_WIDTH-1:0]         len;
        logic [ni_flit_pkg::AXI_SIZE_WIDTH-1:0]        size;
        logic [ni_flit_pkg::AXI_BURST_WIDTH-1:0]       burst;
        logic [ni_flit_pkg::COLLECTIVE_OP_WIDTH-1:0]   collective_op;
        logic [ni_flit_pkg::COLLECTIVE_MASK_WIDTH-1:0] collective_mask;
    } response_entry_t;

    // nsu_depacketize request outputs and nsu_response_queue response outputs.
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

`endif // NI_CHILD_TYPES_PKG_SVH
