// Passive inter-router link monitor. Counts flits and stall cycles, per FLOW
// (S3a T5 §8):
//   "credit" (DAT): valid is gated on credit upstream (per the VC the flit
//     belongs to), so every valid flit is a real transfer; a stall cycle is
//     "not moving, but the credit VC buffer is full" (credit[vc_id]==0).
//     Credit is a single-cycle pulse per VC.
//   "ready_valid" (REQ/RSP): a transfer only happens on valid && ready; a
//     stall cycle is "presented but not accepted" (valid && !ready). No
//     credit array (REQ/RSP are single-VC, ready/valid per spec §4.3).

`ifndef LINK_PERF_MONITOR_SV
`define LINK_PERF_MONITOR_SV

module link_perf_monitor #(
    parameter string LINK_NAME    = "link",
    parameter string FLOW         = "credit",  // "credit" | "ready_valid"
    parameter int    BUFFER_DEPTH = 4,
    parameter int    NUM_VC       = 1,
    parameter int    VC_ID_WIDTH  = 3  // full flit-header VC_ID field width (ni_flit_pkg::VC_ID_WIDTH)
) (
    input logic clk_i,
    input logic rst_ni,
    input logic valid,                    // a flit is presented on the wire this cycle
    input logic ready,                    // ready_valid flow only: downstream accepts this cycle
    input logic [VC_ID_WIDTH-1:0] vc_id,  // credit flow only: full VC_ID field from flit header
    input logic [NUM_VC-1:0] credit_pulse // credit flow only: per-VC downstream slot freed
);
    import "DPI-C" context function void cmodel_perf_link(
        input string name, input longint flit_count, input longint stall_cyc);

    longint      flit_count, stall_cyc;
    int unsigned credit [NUM_VC];

    // Combinational helper: any VC has credit==0 this cycle (credit flow only).
    logic any_vc_zero;
    always_comb begin
        any_vc_zero = 1'b0;
        for (int v = 0; v < NUM_VC; v++) begin
            if (credit[v] == 0) any_vc_zero = 1'b1;
        end
    end

    always_ff @(posedge clk_i) begin
        if (!rst_ni) begin
            flit_count <= 0;
            stall_cyc  <= 0;
            for (int v = 0; v < NUM_VC; v++) credit[v] <= BUFFER_DEPTH;
        end else begin
            automatic longint next_flit;
            automatic longint next_stall;
            if (FLOW == "ready_valid") begin
                // A transfer needs both sides; a presented-but-not-accepted
                // cycle is the stall.
                next_flit  = flit_count + ((valid && ready) ? 1 : 0);
                next_stall = stall_cyc  + ((valid && !ready) ? 1 : 0);
            end else begin
                // Credit reserves the slot before valid asserts, so every
                // valid flit is a real transfer; the stall is "idle, but the
                // credit VC buffer is full."
                next_flit  = flit_count + (valid ? 1 : 0);
                next_stall = stall_cyc  + ((!valid && any_vc_zero) ? 1 : 0);
                for (int v = 0; v < NUM_VC; v++) begin
                    automatic int delta = 0;
                    if (credit_pulse[v]) delta = delta + 1;
                    if (valid && (int'(vc_id) == v)) delta = delta - 1;
                    credit[v] <= credit[v] + delta;
                end
            end
            flit_count <= next_flit;
            stall_cyc  <= next_stall;
            // live push (last-write-wins): final cycle's call carries the total.
            cmodel_perf_link(LINK_NAME, next_flit, next_stall);
        end
    end

    // Credit-flow-only assertions: no analogous protocol violation exists on
    // the ready_valid side (the sender-side WormholeArbiter/SimpleRouter
    // already only presents valid when it has a flit to send, and the wire
    // itself enforces "accept iff ready" -- there is no separate credit pool
    // to desync from).
    if (FLOW != "ready_valid") begin : g_credit_asserts
        // Per-VC credit must never underflow: valid && credit[vc_id]==0 means the
        // upstream sender violated the credit protocol (or a mis-wire). Assert loudly.
        assert property (@(posedge clk_i) disable iff (!rst_ni)
            !(valid && credit[vc_id] == 0))
            else $error("[%s] credit underflow on VC%0d: valid asserted with zero credit",
                        LINK_NAME, vc_id);

        // vc_id in flit header must be a valid VC index. The flit field is VC_ID_WIDTH
        // bits wide (fixed by the packet spec) but NUM_VC may be < 2^VC_ID_WIDTH; a
        // mis-configured encoder would otherwise silently alias into a valid credit[].
        assert property (@(posedge clk_i) disable iff (!rst_ni)
            !(valid && (int'(vc_id) >= NUM_VC)))
            else $error("[%s] out-of-range vc_id=%0d on valid flit (NUM_VC=%0d)",
                        LINK_NAME, vc_id, NUM_VC);
    end
endmodule

`endif  // LINK_PERF_MONITOR_SV
