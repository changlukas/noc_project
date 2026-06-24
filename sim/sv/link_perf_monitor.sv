// Passive inter-router link monitor. Counts flits (valid high) and credit-deficit
// stall cycles per VC. Credit is a single-cycle pulse per VC; valid is gated on
// credit upstream (per the VC the flit belongs to), so backpressure is observable
// only as credit[vc_id]==0 (downstream VC buffer full for that VC).

`ifndef LINK_PERF_MONITOR_SV
`define LINK_PERF_MONITOR_SV

module link_perf_monitor #(
    parameter string LINK_NAME    = "link",
    parameter int    BUFFER_DEPTH = 4,
    parameter int    NUM_VC       = 1
) (
    input logic clk_i,
    input logic rst_ni,
    input logic valid,                                           // a flit is on the wire this cycle
    input logic [$clog2(NUM_VC < 2 ? 2 : NUM_VC)-1:0] vc_id,  // VC of the flit this cycle
    input logic [NUM_VC-1:0] credit_pulse                       // per-VC downstream slot freed
);
    import "DPI-C" context function void cmodel_perf_link(
        input string name, input longint flit_count, input longint stall_cyc);

    longint      flit_count, stall_cyc;
    int unsigned credit [NUM_VC];

    // Combinational helper: any VC has credit==0 this cycle.
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
            automatic longint next_flit  = flit_count + (valid ? 1 : 0);
            // stall: cycles where no flit is moving but at least one VC buffer is full.
            automatic longint next_stall = stall_cyc  + ((!valid && any_vc_zero) ? 1 : 0);
            flit_count <= next_flit;
            stall_cyc  <= next_stall;
            // Per-VC credit update: replenish returned slots; consume on valid flit.
            for (int v = 0; v < NUM_VC; v++) begin
                automatic int delta = 0;
                if (credit_pulse[v]) delta = delta + 1;
                if (valid && (int'(vc_id) == v)) delta = delta - 1;
                credit[v] <= credit[v] + delta;
            end
            // live push (last-write-wins): final cycle's call carries the total.
            cmodel_perf_link(LINK_NAME, next_flit, next_stall);
        end
    end

    // Per-VC credit must never underflow: valid && credit[vc_id]==0 means the
    // upstream sender violated the credit protocol (or a mis-wire). Assert loudly.
    assert property (@(posedge clk_i) disable iff (!rst_ni)
        !(valid && credit[vc_id] == 0))
        else $error("[%s] credit underflow on VC%0d: valid asserted with zero credit",
                    LINK_NAME, vc_id);
endmodule

`endif  // LINK_PERF_MONITOR_SV
