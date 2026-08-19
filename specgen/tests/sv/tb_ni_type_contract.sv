`resetall
`timescale 1ns / 1ps
`default_nettype none

module tb_ni_type_contract;

    ni_signals_pkg::axi_aw_t axi_aw;
    ni_signals_pkg::axi_w_t  axi_w;
    ni_signals_pkg::axi_b_t  axi_b;
    ni_signals_pkg::axi_ar_t axi_ar;
    ni_signals_pkg::axi_r_t  axi_r;

    ni_flit_pkg::req_flit_t req_flit;
    ni_flit_pkg::rsp_flit_t rsp_flit;
    ni_flit_pkg::dat_flit_t dat_flit;

    ni_child_types_pkg::nmu_ordering_domain_t nmu_domain;
    ni_child_types_pkg::nmu_route_t nmu_route;
    ni_child_types_pkg::nmu_aw_route_t nmu_aw_route;
    ni_child_types_pkg::nmu_request_t nmu_request;
    ni_child_types_pkg::nmu_response_t nmu_response;
    ni_child_types_pkg::nmu_sam_aw_t nmu_sam_aw;
    ni_child_types_pkg::nmu_sam_aw_result_t nmu_sam_aw_result;
    ni_child_types_pkg::nmu_sam_ar_result_t nmu_sam_ar_result;
    ni_child_types_pkg::nmu_aw_request_t nmu_aw_request;
    ni_child_types_pkg::nmu_ar_request_t nmu_ar_request;
    ni_child_types_pkg::nmu_b_response_t nmu_b_response;
    ni_child_types_pkg::nmu_r_response_t nmu_r_response;
    ni_child_types_pkg::nmu_rob_order_entry_t nmu_order_entry;
    ni_child_types_pkg::nmu_b_rob_entry_t nmu_b_entry;
    ni_child_types_pkg::nmu_r_rob_entry_t nmu_r_entry;
    ni_child_types_pkg::nmu_read_context_t nmu_read_context;
    ni_child_types_pkg::response_entry_t response_entry;
    ni_child_types_pkg::nsu_aw_request_t nsu_aw_request;
    ni_child_types_pkg::nsu_ar_request_t nsu_ar_request;
    ni_child_types_pkg::nsu_b_response_t nsu_b_response;
    ni_child_types_pkg::nsu_r_response_t nsu_r_response;

    // One independent valid/ready bit per stream in the rtl/README.md table:
    // nmu_sam 2 inputs + 2 outputs, nmu_rob 5 + 5,
    // nsu_depacketize 2 + 3, and nsu_response_queue 4 + 4.
    logic [3:0] nmu_sam_valid;
    logic [3:0] nmu_sam_ready;
    logic [9:0] nmu_rob_valid;
    logic [9:0] nmu_rob_ready;
    logic [4:0] nsu_depacketize_valid;
    logic [4:0] nsu_depacketize_ready;
    logic [7:0] nsu_response_queue_valid;
    logic [7:0] nsu_response_queue_ready;

    initial begin
        if ($bits(axi_aw) != 80)  $fatal(1, "axi_aw_t width");
        if ($bits(axi_w)  != 577) $fatal(1, "axi_w_t width");
        if ($bits(axi_b)  != 5)   $fatal(1, "axi_b_t width");
        if ($bits(axi_ar) != 80)  $fatal(1, "axi_ar_t width");
        if ($bits(axi_r)  != 518) $fatal(1, "axi_r_t width");

        if ($bits(req_flit) != ni_params_pkg::NOC_REQ_FLIT_WIDTH_DFLT)
            $fatal(1, "req_flit_t width");
        if ($bits(rsp_flit) != ni_params_pkg::NOC_RSP_FLIT_WIDTH_DFLT)
            $fatal(1, "rsp_flit_t width");
        if ($bits(dat_flit) != ni_params_pkg::NOC_DAT_FLIT_WIDTH_DFLT)
            $fatal(1, "dat_flit_t width");
        if ($bits(nmu_domain) != 11) $fatal(1, "nmu_ordering_domain_t width");
        if ($bits(nmu_route) != 11) $fatal(1, "nmu_route_t width");
        if ($bits(nmu_aw_route) != 29) $fatal(1, "nmu_aw_route_t width");
        if ($bits(nmu_request) != 20) $fatal(1, "nmu_request_t width");
        if ($bits(nmu_response) != 10) $fatal(1, "nmu_response_t width");
        if ($bits(nmu_sam_aw) != 138) $fatal(1, "nmu_sam_aw_t width");
        if ($bits(nmu_sam_aw_result) != 109) $fatal(1, "nmu_sam_aw_result_t width");
        if ($bits(nmu_sam_ar_result) != 91) $fatal(1, "nmu_sam_ar_result_t width");
        if ($bits(nmu_aw_request) != 118) $fatal(1, "nmu_aw_request_t width");
        if ($bits(nmu_ar_request) != 100) $fatal(1, "nmu_ar_request_t width");
        if ($bits(nmu_b_response) != 15) $fatal(1, "nmu_b_response_t width");
        if ($bits(nmu_r_response) != 528) $fatal(1, "nmu_r_response_t width");
        if ($bits(nmu_order_entry) != 19) $fatal(1, "nmu_rob_order_entry_t width");
        if ($bits(nmu_b_entry) != 7) $fatal(1, "nmu_b_rob_entry_t width");
        if ($bits(nmu_r_entry) != 523) $fatal(1, "nmu_r_rob_entry_t width");
        if ($bits(nmu_read_context) != 69) $fatal(1, "nmu_read_context_t width");
        if ($bits(response_entry) != 94) $fatal(1, "response_entry_t width");
        if ($bits(nsu_aw_request) != 174) $fatal(1, "nsu_aw_request_t width");
        if ($bits(nsu_ar_request) != 174) $fatal(1, "nsu_ar_request_t width");
        if ($bits(nsu_b_response) != 99) $fatal(1, "nsu_b_response_t width");
        if ($bits(nsu_r_response) != 612) $fatal(1, "nsu_r_response_t width");

        // The four leaf-boundary payloads elaborate independently of their
        // valid/ready wires; handshake is never embedded in a packed record.
        nmu_sam_aw = '0;
        nmu_sam_aw_result = '0;
        nmu_sam_ar_result = '0;
        nmu_aw_request = '0;
        nmu_ar_request = '0;
        nmu_b_response = '0;
        nmu_r_response = '0;
        nsu_aw_request = '0;
        nsu_ar_request = '0;
        nsu_b_response = '0;
        nsu_r_response = '0;
        nmu_sam_valid = '0;
        nmu_sam_ready = '0;
        nmu_rob_valid = '0;
        nmu_rob_ready = '0;
        nsu_depacketize_valid = '0;
        nsu_depacketize_ready = '0;
        nsu_response_queue_valid = '0;
        nsu_response_queue_ready = '0;

        req_flit = '0;
        req_flit.header = '1;
        if (req_flit[ni_flit_pkg::HEADER_WIDTH-1:0] != '1)
            $fatal(1, "REQ header is not in the canonical LSB position");
        req_flit = '0;
        req_flit.payload = '1;
        if (req_flit[ni_params_pkg::NOC_REQ_FLIT_WIDTH_DFLT-1:
                     ni_flit_pkg::HEADER_WIDTH] != '1)
            $fatal(1, "REQ payload is not above the header");

        rsp_flit = '0;
        rsp_flit.header = '1;
        if (rsp_flit[ni_flit_pkg::HEADER_WIDTH-1:0] != '1)
            $fatal(1, "RSP header is not in the canonical LSB position");
        rsp_flit = '0;
        rsp_flit.payload = '1;
        if (rsp_flit[ni_params_pkg::NOC_RSP_FLIT_WIDTH_DFLT-1:
                     ni_flit_pkg::HEADER_WIDTH] != '1)
            $fatal(1, "RSP payload is not above the header");

        dat_flit = '0;
        dat_flit.header = '1;
        if (dat_flit[ni_flit_pkg::HEADER_WIDTH-1:0] != '1)
            $fatal(1, "DAT header is not in the canonical LSB position");
        dat_flit = '0;
        dat_flit.payload = '1;
        if (dat_flit[ni_params_pkg::NOC_DAT_FLIT_WIDTH_DFLT-1:
                     ni_flit_pkg::HEADER_WIDTH] != '1)
            $fatal(1, "DAT payload is not above the header");

        $display("PASS: generated NI type contract");
        $finish;
    end

endmodule

`resetall
