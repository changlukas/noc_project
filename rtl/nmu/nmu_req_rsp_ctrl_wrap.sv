// SPDX-License-Identifier: Apache-2.0

`resetall
`timescale 1ns / 1ps
`default_nettype none

// Boundary-only wrapper. Storage and ordering policy belong to the next issue.
module nmu_req_rsp_ctrl_wrap #(
    parameter int unsigned AXI_ID_WIDTH = ni_params_pkg::AXI_ID_WIDTH_DFLT,
    parameter int unsigned MAX_TXNS_PER_ID = ni_params_pkg::NMU_MAX_TXNS_PER_ID_DFLT
) (
    nmu_req_rsp_ctrl_if.response_buffer ctrl
);

    initial begin
        if (AXI_ID_WIDTH < 1 || AXI_ID_WIDTH > 8) begin
            $fatal(0, "Error: AXI_ID_WIDTH must be between 1 and 8 (instance %m)");
        end
        if (MAX_TXNS_PER_ID < 1) begin
            $fatal(0, "Error: MAX_TXNS_PER_ID must be at least 1 (instance %m)");
        end
    end

endmodule

`resetall
