`timescale 1ns / 1ps

module tb_nmu_response_depacketize #(
    parameter int NUM_DAT_VC = 2,
    parameter int DAT_VC_MODE = 0,
    parameter int DAT_RX_VC_DEPTH = 2,
    parameter int REG_TYPE = 0
);
    import ni_flit_pkg::*;
    import ni_types_pkg::*;
    localparam int FIRST_VC = DAT_VC_MODE == 1 ? NUM_DAT_VC/2 : 0;
    logic clk = 0, rst_n_i = 0;
    always #5 clk = ~clk;
    rsp_flit_t rsp;
    dat_flit_t dat;
    logic rsp_valid = 0, rsp_ready, dat_valid = 0;
    logic            [NUM_DAT_VC-1:0] credit_return;
    nmu_b_response_t                  b;
    nmu_r_response_t                  r;
    logic b_valid, r_valid, b_ready = 1, r_ready = 0;
    int wr_cnt [NUM_DAT_VC+1], rd_cnt [NUM_DAT_VC+1];
    int credit [NUM_DAT_VC];
    int fifo_rd_cnt [NUM_DAT_VC];
    int total_r = 0, total_b = 0, recoveries = 0, parallel_ingress = 0;
    logic [NUM_DAT_VC-1:0] expected_credit = '0;
    nmu_r_response_t held_r;
    bit held = 0;
    int fault = 0;
    initial void'($value$plusargs("fault=%d", fault));

    rsp_flit_t buffered_b;
    dat_flit_t buffered_r;
    wire buffered_b_valid, buffered_b_ready, buffered_r_valid, buffered_r_ready;
    nmu_response_buffer #(
        .RSP_FIFO_DEPTH  (2              ),
        .NUM_DAT_VC      (NUM_DAT_VC     ),
        .DAT_VC_MODE     (DAT_VC_MODE    ),
        .DAT_RX_VC_DEPTH (DAT_RX_VC_DEPTH)
    ) i_buffer (
        .clk_i               (clk          ),
        .rst_n_i             (rst_n_i      ),
        .s_rsp_i             (rsp          ),
        .s_rsp_valid_i       (rsp_valid    ),
        .s_rsp_ready_o       (rsp_ready    ),
        .s_dat_i             (dat          ),
        .s_dat_valid_i       (dat_valid    ),
        .dat_credit_return_o (credit_return),
        .m_b_o               (buffered_b            ),
        .m_b_valid_o         (buffered_b_valid      ),
        .m_b_ready_i         (buffered_b_ready      ),
        .m_r_o               (buffered_r            ),
        .m_r_valid_o         (buffered_r_valid      ),
        .m_r_ready_i         (buffered_r_ready      )
    );
    nmu_response_depacketize #(
        .B_REG_TYPE (REG_TYPE),
        .R_REG_TYPE (REG_TYPE)
    ) dut (
        .clk_i (clk), .rst_n_i (rst_n_i),
        .s_b_i (buffered_b), .s_b_valid_i (buffered_b_valid), .s_b_ready_o (buffered_b_ready),
        .s_r_i (buffered_r), .s_r_valid_i (buffered_r_valid), .s_r_ready_o (buffered_r_ready),
        .m_b_o (b), .m_b_valid_o (b_valid), .m_b_ready_i (b_ready),
        .m_r_o (r), .m_r_valid_o (r_valid), .m_r_ready_i (r_ready)
    );

    function automatic nmu_r_response_t expected(input int vc, input int seq);
        nmu_r_response_t value;
        value           = '0;
        value.axi.rid   = DATA_R_RID_WIDTH'(vc);
        value.axi.rresp = 2'(seq % 3);
        value.axi.rlast = seq % 3 == 2;
        value.axi.rdata = '0;
        for (int lane = 0; lane < (vc == NUM_DAT_VC ? 2 : 16); lane++)
            value.axi.rdata[lane*32 +: 32] = 32'(seq + lane*1024 + vc*65536);
        value.meta.is_data      = vc != NUM_DAT_VC;
        value.meta.ordering_req = 1;
        value.meta.ordering_tag = ORDERING_TAG_WIDTH'(seq);
        return value;
    endfunction

    task automatic set_dat(input int vc);
        nmu_r_response_t value;
        value                                               = expected(vc, wr_cnt[vc]);
        dat                                                 = '0;
        dat.header[AXI_CH_LSB +: AXI_CH_WIDTH]              = AXI_CH_WIDTH'(AXI_CH_DataR);
        dat.header[VC_ID_LSB +: VC_ID_WIDTH]                = VC_ID_WIDTH'(vc);
        dat.header[FLIT_TAIL_LSB]                           = 1;
        dat.header[ORDERING_REQ_LSB]                        = value.meta.ordering_req;
        dat.header[ORDERING_TAG_LSB +: ORDERING_TAG_WIDTH]  = value.meta.ordering_tag;
        dat.payload[DATA_R_RID_LSB +: DATA_R_RID_WIDTH]     = value.axi.rid;
        dat.payload[DATA_R_RRESP_LSB +: DATA_R_RRESP_WIDTH] = value.axi.rresp;
        dat.payload[DATA_R_RLAST_LSB]                       = value.axi.rlast;
        dat.payload[DATA_R_RDATA_LSB +: DATA_R_RDATA_WIDTH] = value.axi.rdata;
        dat_valid                                           = 1;
    endtask

    task automatic set_rsp(input bit read_rsp);
        nmu_r_response_t value;
        value                                              = expected(NUM_DAT_VC, wr_cnt[NUM_DAT_VC]);
        rsp                                                = '0;
        rsp.header[AXI_CH_LSB +: AXI_CH_WIDTH]             = AXI_CH_WIDTH'(read_rsp ? AXI_CH_NarrowR : AXI_CH_DataB);
        rsp.header[ORDERING_REQ_LSB]                       = 1;
        rsp.header[ORDERING_TAG_LSB +: ORDERING_TAG_WIDTH] = value.meta.ordering_tag;
        if (read_rsp) begin
            rsp.payload[NARROW_R_RID_LSB +: NARROW_R_RID_WIDTH]     = value.axi.rid;
            rsp.payload[NARROW_R_RRESP_LSB +: NARROW_R_RRESP_WIDTH] = value.axi.rresp;
            rsp.payload[NARROW_R_RLAST_LSB]                         = value.axi.rlast;
            rsp.payload[NARROW_R_RDATA_LSB +: NARROW_R_RDATA_WIDTH] = value.axi.rdata[63:0];
        end else begin
            rsp.payload[B_BID_LSB +: B_BID_WIDTH]     = 3;
            rsp.payload[B_BRESP_LSB +: B_BRESP_WIDTH] = 2;
        end
        rsp_valid = 1;
    endtask

    always @(posedge clk) begin : check
        int vc;
        if (~rst_n_i) begin
            held            = 0;
            expected_credit = '0;
            for (int n = 0; n <= NUM_DAT_VC; n++) begin
                wr_cnt[n] = 0; rd_cnt[n] = 0;
            end
            for (int n = 0; n < NUM_DAT_VC; n++) begin
                credit[n] = DAT_RX_VC_DEPTH;
                fifo_rd_cnt[n] = 0;
            end
            if (credit_return !== '0) $fatal(1, "credit pulse during reset");
        end else if (fault == 0) begin
            if (buffered_r_ready && |i_buffer.r_valid && !buffered_r_valid) $fatal(1, "avoidable R output bubble");
            if (held && (!r_valid || r !== held_r)) $fatal(1, "stalled R changed");
            held = r_valid && !r_ready; held_r = r;
            if (!$onehot0(credit_return)) $fatal(1, "multiple DAT pops");
            if (r_valid && r_ready) begin
                vc = r.meta.is_data ? int'(r.axi.rid) : NUM_DAT_VC;
                if (rd_cnt[vc] >= wr_cnt[vc] || r !== expected(vc, rd_cnt[vc]))
                    $fatal(1, "response mismatch vc=%0d seq=%0d", vc, rd_cnt[vc]);
                rd_cnt[vc]++; total_r++;
            end
            if (credit_return !== expected_credit) $fatal(1, "registered credit mismatch");
            expected_credit = '0;
            if (buffered_r_valid && buffered_r_ready &&
                    buffered_r.header[AXI_CH_LSB +: AXI_CH_WIDTH] == AXI_CH_WIDTH'(AXI_CH_DataR)) begin
                vc = int'(buffered_r.header[VC_ID_LSB +: VC_ID_WIDTH]);
                expected_credit[vc] = 1'b1;
                fifo_rd_cnt[vc]++;
            end
            for (int n = 0; n < NUM_DAT_VC; n++) credit[n] += int'(credit_return[n]);
            if (dat_valid) begin
                vc = int'(dat.header[VC_ID_LSB +: VC_ID_WIDTH]);
                if (credit[vc] <= 0) $fatal(1, "sender violated credits");
                if (credit[vc] == 1 && credit_return[vc]) recoveries++;
                credit[vc]--; wr_cnt[vc]++;
                if (rsp_valid && rsp_ready) parallel_ingress++;
            end
            if (rsp_valid && rsp_ready && rsp.header[AXI_CH_LSB +: AXI_CH_WIDTH] == AXI_CH_WIDTH'(AXI_CH_NarrowR))
                wr_cnt[NUM_DAT_VC]++;
            for (int n = FIRST_VC; n < NUM_DAT_VC; n++)
                if (credit[n] + wr_cnt[n] - fifo_rd_cnt[n] + int'(expected_credit[n]) != DAT_RX_VC_DEPTH)
                    $fatal(1, "per-VC conservation mismatch");
            if (b_valid && b_ready) begin
                if (b.axi.bid != 3 || b.axi.bresp != 2 || !b.meta.is_data || !b.meta.ordering_req)
                    $fatal(1, "B decode mismatch");
                total_b++;
            end
        end
    end

    always @(negedge rst_n_i) begin
        #1ps;
        if (i_buffer.credit_return_reg !== '0 ||
            i_buffer.b_empty !== 1'b1 || i_buffer.r_empty !== 1'b1 ||
            i_buffer.dat_empty !== '1)
            $fatal(1, "Response FIFO/credit reset waited for a clock edge");
    end

    initial begin : stimulus
        int sel, before_r;
        bit drained;
        rsp = '0; dat = '0;
        repeat (3) @(negedge clk); rst_n_i = 1;
        if (fault != 0) begin
            set_dat(FIRST_VC);
            if (fault == 1) dat.header[AXI_CH_LSB +: AXI_CH_WIDTH] = AXI_CH_WIDTH'(AXI_CH_DataW);
            if (fault == 2) dat.header[VC_ID_LSB +: VC_ID_WIDTH] = VC_ID_WIDTH'(NUM_DAT_VC);
            if (fault == 3) dat.header[VC_ID_LSB +: VC_ID_WIDTH] = '0;
            repeat (DAT_RX_VC_DEPTH+3) @(negedge clk);
            $fatal(1, "fault escaped ingress checks");
        end
        // Fill every VC while the selected output is stalled. RLAST stays low
        // on initial beats, so progress to other VCs proves beat arbitration.
        for (int vc = FIRST_VC; vc < NUM_DAT_VC; vc++) begin
            for (int beat = 0; beat < DAT_RX_VC_DEPTH; beat++) begin
                set_dat(vc);
                if (vc == FIRST_VC && beat == 0) set_rsp(0);
                @(negedge clk); rsp_valid = 0;
            end
        end
        dat_valid = 0;
        repeat (3) @(negedge clk);
        if (total_b != 1) $fatal(1, "B blocked behind DAT");
        set_rsp(1); @(negedge clk); rsp_valid = 0;
        r_ready = 1;
        // A full FIFO pops first; the registered credit permits refill next cycle.
        @(negedge clk);
        #1ps;
        sel = -1;
        for (int vc = FIRST_VC; vc < NUM_DAT_VC; vc++) if (credit_return[vc]) sel = vc;
        if (sel < 0) $fatal(1, "no available DAT head");
        set_dat(sel);
        @(negedge clk); dat_valid = 0;
        before_r = total_r;
        repeat (NUM_DAT_VC+2) @(negedge clk);
        for (int vc = FIRST_VC; vc < NUM_DAT_VC; vc++)
            if (rd_cnt[vc] == 0) $fatal(1, "VC starved while other VC RLAST was low");
        if (total_r == before_r) $fatal(1, "arbiter did not progress");
        do begin
            drained = 1;
            for (int vc = 0; vc <= NUM_DAT_VC; vc++) if (rd_cnt[vc] != wr_cnt[vc]) drained = 0;
            @(negedge clk);
        end while (!drained);
        if (recoveries == 0 || parallel_ingress == 0) $fatal(1, "vacuous full/parallel coverage");
        begin : throughput
            int sent, cycles, start_r;
            sent = 0; cycles = 0; start_r = total_r;
            while (sent < 64) begin
                if (credit[FIRST_VC] + int'(credit_return[FIRST_VC]) > 0) begin
                    set_dat(FIRST_VC); sent++;
                end else dat_valid = 0;
                @(negedge clk); cycles++;
            end
            dat_valid = 0;
            while (rd_cnt[FIRST_VC] != wr_cnt[FIRST_VC]) begin
                @(negedge clk); cycles++;
            end
            repeat (2) @(negedge clk);
            if (total_r-start_r != 64) $fatal(1, "throughput lost response");
            $display("PERF depth=%0d beats=64 cycles=%0d", DAT_RX_VC_DEPTH, cycles);
            if (cycles > 66) $fatal(1, "avoidable throughput bubble with one-cycle credits");
        end
        // Flush occupied queues and a held arbitration decision, then reseed.
        r_ready = 0; set_dat(FIRST_VC); @(negedge clk); dat_valid = 0;
        repeat (2) @(negedge clk); rst_n_i = 0;
        repeat (2) @(negedge clk); rst_n_i = 1; r_ready = 1;
        if (r_valid) $fatal(1, "reset retained response");
        set_dat(FIRST_VC); @(negedge clk); dat_valid = 0;
        repeat (5) @(negedge clk);
        if (rd_cnt[FIRST_VC] != 1 || credit[FIRST_VC] != DAT_RX_VC_DEPTH)
            $fatal(1, "reset credit reseed/drain failed");
        $display("PASS depacketize vcs=%0d mode=%0d depth=%0d R=%0d credit_recovery=%0d parallel=%0d",
            NUM_DAT_VC, DAT_VC_MODE, DAT_RX_VC_DEPTH, total_r, recoveries, parallel_ingress);
        $finish;
    end
`ifdef DUMP_WAVE
    initial begin
        string wave_file;
        if (!$value$plusargs("wave_file=%s", wave_file)) wave_file = "dat_ingress.fsdb";
`ifdef VERILATOR
        $dumpfile(wave_file); $dumpvars(0, tb_nmu_response_depacketize);
`else
        $fsdbDumpfile(wave_file); $fsdbDumpvars(0, tb_nmu_response_depacketize, "+all");
`endif
    end
`endif
    initial begin #100us; $fatal(1, "depacketize timeout"); end
endmodule
