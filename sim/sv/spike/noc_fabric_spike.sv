`timescale 1ns/1ps
// SPIKE: mesh_2x1_vc8 struct-port elaboration de-risk (throwaway).
//
// Purpose: prove ni_signals_pkg::noc_chan_t / noc_credit_t work as module
// ports in unpacked arrays [NUM_NODES] wired by genvar generate.
// NOC_NUM_VC_DFLT = 1 in ni_params_pkg; the struct credit field is
// therefore [0:0] (1 bit).  See task-2-report.md §vc8-width for the
// finding on parameterized-VC struct design.
//
// No DPI, no real NMU/NSU — stub modules only.
// Handshake: node0 fires one req flit; node1 echoes a credit; node0
// sets cred_seen_o on receipt.

`ifndef NOC_FABRIC_SPIKE_SV
`define NOC_FABRIC_SPIKE_SV

// ---------------------------------------------------------------------------
// Stub: req driver (instantiated at node 0)
// ---------------------------------------------------------------------------
module node_req_driver (
    input  logic                          clk_i,
    input  logic                          rst_ni,
    output ni_signals_pkg::noc_chan_t     req_o,
    input  ni_signals_pkg::noc_credit_t  cred_i,
    output logic                          cred_seen_o
);
    logic fired;
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            req_o.valid  <= 1'b0;
            req_o.flit   <= '0;
            fired        <= 1'b0;
            cred_seen_o  <= 1'b0;
        end else begin
            if (!fired) begin
                req_o.valid <= 1'b1;
                req_o.flit  <= {{(ni_params_pkg::NOC_FLIT_WIDTH_DFLT-8){1'b0}}, 8'hA5};
                fired       <= 1'b1;
            end else begin
                req_o.valid <= 1'b0;
            end
            if (|cred_i.credit)
                cred_seen_o <= 1'b1;
        end
    end
endmodule

// ---------------------------------------------------------------------------
// Stub: credit echo (instantiated at node 1)
// Returns a credit for one cycle when it sees a valid req.
// ---------------------------------------------------------------------------
module node_credit_echo (
    input  logic                          clk_i,
    input  logic                          rst_ni,
    input  ni_signals_pkg::noc_chan_t     req_i,
    output ni_signals_pkg::noc_credit_t  cred_o
);
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            cred_o.credit <= '0;
        end else begin
            cred_o.credit <= req_i.valid ? '1 : '0;
        end
    end
endmodule

// ---------------------------------------------------------------------------
// Fabric: 2-node spike, struct arrays as ports, generate wiring
//
// Port arrays are the KEY de-risk target:
//   node_req      [NUM_NODES]  - ni_signals_pkg::noc_chan_t   (output)
//   node_cred     [NUM_NODES]  - ni_signals_pkg::noc_credit_t (output)
//   cred_seen     [NUM_NODES]  - logic                        (output)
// ---------------------------------------------------------------------------
module noc_fabric_spike #(
    parameter int unsigned NUM_NODES = 2
) (
    input  logic clk_i,
    input  logic rst_ni,
    // Struct-typed unpacked array ports - this is what we are elaborating.
    output ni_signals_pkg::noc_chan_t    node_req  [NUM_NODES],
    output ni_signals_pkg::noc_credit_t node_cred [NUM_NODES],
    output logic                          cred_seen [NUM_NODES]
);
    // Internal link wires: node0 req drives into node1, node1 credit returns to node0.
    ni_signals_pkg::noc_chan_t    w_req  [NUM_NODES];
    ni_signals_pkg::noc_credit_t w_cred [NUM_NODES];

    // node 0: fires a req flit, waits for credit back from node 1.
    node_req_driver u_node0 (
        .clk_i      (clk_i),
        .rst_ni     (rst_ni),
        .req_o      (w_req[0]),
        .cred_i     (w_cred[0]),
        .cred_seen_o(cred_seen[0])
    );

    // node 1: echoes a credit when it sees a valid req from node 0.
    node_credit_echo u_node1 (
        .clk_i  (clk_i),
        .rst_ni (rst_ni),
        .req_i  (w_req[0]),
        .cred_o (w_cred[0])
    );

    // node 1 is a pure sink in this spike; no outgoing req or cred_seen.
    assign cred_seen[1] = 1'b0;

    // Wire internal arrays to output ports via generate loop.
    // This exercises "genvar wiring of struct-array ports" — the de-risk target.
    genvar i;
    generate
        for (i = 0; i < NUM_NODES; i++) begin : gen_port_wire
            assign node_req[i]  = w_req[i];
            assign node_cred[i] = w_cred[i];
        end
    endgenerate

    // Tie off unused internal arrays (node1 side).
    assign w_req[1]  = '0;
    assign w_cred[1] = '0;

endmodule

`endif // NOC_FABRIC_SPIKE_SV
