// Passive AXI slot monitor (PG037-style). Reads one AXI interface's wires;
// correlates latency by per-(id,dir) in-order FIFO; counts idle/outstanding;
// reports each completion + end-of-run backpressure via DPI. No drives.
module axi_perf_monitor #(
    parameter string SLOT_NAME = "slot",
    parameter int    ID_W = 8,
    parameter int    MAX_OUTSTANDING = 64
) (
    input logic clk_i,
    input logic rst_ni,
    input logic                awvalid, input logic awready,
    input logic [ID_W-1:0]     awid,
    input logic [63:0]         awaddr,
    input logic [7:0]          awlen,  input logic [2:0] awsize,
    input logic                wvalid, input logic wready,
    input logic                bvalid, input logic bready, input logic [ID_W-1:0] bid,
    input logic                arvalid, input logic arready,
    input logic [ID_W-1:0]     arid,
    input logic [63:0]         araddr,
    input logic [7:0]          arlen,  input logic [2:0] arsize,
    input logic                rvalid, input logic rready, input logic rlast,
    input logic [ID_W-1:0]     rid
);
    import "DPI-C" context function void cmodel_perf_axi_txn(
        input string slot, input int id, input int is_write,
        input longint addr, input int len, input int size,
        input longint accept_cyc, input longint complete_cyc);
    import "DPI-C" context function void cmodel_perf_axi_backpressure(
        input string slot, input longint slave_write_idle_cyc,
        input longint master_read_idle_cyc, input longint outstanding_max);

    localparam int NID = 1 << ID_W;
    longint cyc;
    longint slave_write_idle, master_read_idle, outstanding_max;
    int     outstanding;
    // per-id in-order issue queues (separate write/read), holding accept cycle +
    // the addr/len/size needed to reconstruct the completion record.
    longint w_acc [NID][$]; longint w_addr [NID][$]; int w_len [NID][$]; int w_sz [NID][$];
    longint r_acc [NID][$]; longint r_addr [NID][$]; int r_len [NID][$]; int r_sz [NID][$];

    always_ff @(posedge clk_i) begin
        if (!rst_ni) begin
            cyc <= 0; slave_write_idle <= 0; master_read_idle <= 0;
            outstanding_max <= 0; outstanding <= 0;
        end else begin
            cyc <= cyc + 1;
            if (wvalid && !wready) slave_write_idle  <= slave_write_idle  + 1;
            if (rvalid && !rready) master_read_idle   <= master_read_idle  + 1;

            if (awvalid && awready) begin
                if (w_acc[awid].size() >= MAX_OUTSTANDING)
                    $fatal(1, "%s: write id=%0d exceeds MAX_OUTSTANDING", SLOT_NAME, awid);
                w_acc[awid].push_back(cyc);  w_addr[awid].push_back(awaddr);
                w_len[awid].push_back(int'(awlen)); w_sz[awid].push_back(int'(awsize));
                outstanding++;
            end
            if (arvalid && arready) begin
                if (r_acc[arid].size() >= MAX_OUTSTANDING)
                    $fatal(1, "%s: read id=%0d exceeds MAX_OUTSTANDING", SLOT_NAME, arid);
                r_acc[arid].push_back(cyc);  r_addr[arid].push_back(araddr);
                r_len[arid].push_back(int'(arlen)); r_sz[arid].push_back(int'(arsize));
                outstanding++;
            end
            if (outstanding > outstanding_max) outstanding_max <= outstanding;

            if (bvalid && bready) begin
                if (w_acc[bid].size() == 0)
                    $fatal(1, "%s: B with no outstanding write id=%0d", SLOT_NAME, bid);
                cmodel_perf_axi_txn(SLOT_NAME, bid, 1, w_addr[bid][0],
                                    w_len[bid][0], w_sz[bid][0], w_acc[bid][0], cyc);
                void'(w_acc[bid].pop_front());  void'(w_addr[bid].pop_front());
                void'(w_len[bid].pop_front());  void'(w_sz[bid].pop_front());
                outstanding--;
            end
            if (rvalid && rready && rlast) begin
                if (r_acc[rid].size() == 0)
                    $fatal(1, "%s: R(last) with no outstanding read id=%0d", SLOT_NAME, rid);
                cmodel_perf_axi_txn(SLOT_NAME, rid, 0, r_addr[rid][0],
                                    r_len[rid][0], r_sz[rid][0], r_acc[rid][0], cyc);
                void'(r_acc[rid].pop_front());  void'(r_addr[rid].pop_front());
                void'(r_len[rid].pop_front());  void'(r_sz[rid].pop_front());
                outstanding--;
            end
        end
    end

    final cmodel_perf_axi_backpressure(SLOT_NAME, slave_write_idle,
                                       master_read_idle, outstanding_max);
endmodule
