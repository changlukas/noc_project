`timescale 1ns / 1ps
`default_nettype none

module tb_nmu_request_packetize_stress #(
    parameter int unsigned FIFO_DEPTH = 2,
    parameter int unsigned NUM_DAT_VC = 2,
    parameter int unsigned DAT_VC_MODE = 0,
    parameter int unsigned REG_TYPE = 0,
    parameter int unsigned AW_REG_TYPE = REG_TYPE,
    parameter int unsigned W_REG_TYPE = REG_TYPE,
    parameter int unsigned AR_REG_TYPE = REG_TYPE
);
    localparam int unsigned WRITES = 24;
    localparam int unsigned READS = 24;
    localparam int unsigned BEATS = 3;
    localparam int unsigned CREDIT_DEPTH = 2;
    localparam int unsigned WRITE_VCS = DAT_VC_MODE == 1 ? NUM_DAT_VC/2 : NUM_DAT_VC;
    logic clk_i = 0, rst_n_i = 0;
    ni_types_pkg::nmu_aw_request_t                  s_aw_i;
    ni_signals_pkg::axi_w_t                         s_w_i;
    ni_types_pkg::nmu_ar_request_t                  s_ar_i;
    ni_flit_pkg::req_flit_t                         m_req_o, previous_req;
    ni_flit_pkg::dat_flit_t                         m_dat_o;
    logic                                           s_aw_valid_i, s_aw_ready_o, s_w_valid_i, s_w_ready_o;
    logic                                           s_ar_valid_i, s_ar_ready_o, m_req_valid_o, m_req_ready_i, m_dat_valid_o;
    logic                          [NUM_DAT_VC-1:0] dat_credit_return_i;
    logic                          [NUM_DAT_VC-1:0] credit_delay [3];
    logic [31:0] random_reg = 32'hb7251309;
    logic req_stalled = 0;
    int narrow_count = 0, data_count = 0, read_count = 0;
    int narrow_beat = 0, data_beat = 0;
    int data_txn [NUM_DAT_VC], data_beat_vc [NUM_DAT_VC];
    bit data_active_vc [NUM_DAT_VC];
    bit data_seen [WRITES];
    int cycle = 0, parallel_count = 0;
    bit narrow_active = 0, data_active = 0;
    int credit [NUM_DAT_VC];
    int pending_credit [NUM_DAT_VC];
    int vc_bypass_count = 0;
    bit writes_done = 0, reads_done = 0;

    nmu_request_inject_tb_dut #(
        .REQ_AW_REG_TYPE (AW_REG_TYPE),
        .REQ_W_REG_TYPE (W_REG_TYPE),
        .REQ_AR_REG_TYPE (AR_REG_TYPE),
        .DAT_AW_REG_TYPE (AW_REG_TYPE),
        .DAT_W_REG_TYPE (W_REG_TYPE),
        .FIFO_DEPTH      (FIFO_DEPTH  ),
        .NUM_DAT_VC      (NUM_DAT_VC  ),
        .DAT_VC_MODE     (DAT_VC_MODE ),
        .ROUTER_VC_DEPTH (CREDIT_DEPTH)
    ) dut (.*);

    always #5ns clk_i = !clk_i;

    function automatic logic [47:0] address(input int transaction, input bit wide);
        return 48'h1000 + 48'(transaction * 256 + (wide ? 0 : 8));
    endfunction
    function automatic logic [63:0] payload(input int transaction, input int beat);
        return 64'habc00000 + 64'(transaction * 16 + beat);
    endfunction

    // Delayed credit returns model real occupied downstream slots.
    always @(negedge clk_i) begin
        if (~rst_n_i) begin
            m_req_ready_i       = 0;
            dat_credit_return_i = '0;
            random_reg          = 32'hb7251309;
        end else begin
            random_reg          = {random_reg[30:0], random_reg[31] ^ random_reg[21] ^ random_reg[1] ^ random_reg[0]};
            m_req_ready_i       = cycle > 12 && random_reg[0];
            for (int vc = 0; vc < NUM_DAT_VC; vc++)
                dat_credit_return_i[vc] = pending_credit[vc] > 0 && (vc != 0 || cycle > 150);
        end
    end

    always @(posedge clk_i or negedge rst_n_i) begin
        if (~rst_n_i) begin
            for (int vc = 0; vc < NUM_DAT_VC; vc++) begin
                credit[vc] = CREDIT_DEPTH;
                pending_credit[vc] = 0;
            end
            for (int n = 0; n < 3; n++) credit_delay[n] = '0;
            req_stalled = 0;
        end else begin
            cycle++;
            if (req_stalled && (!m_req_valid_o || m_req_o !== previous_req))
                $fatal(1, "REQ payload changed under randomized stalls");
            req_stalled     = m_req_valid_o && !m_req_ready_i;
            previous_req    = m_req_o;
            credit_delay[2] = credit_delay[1];
            credit_delay[1] = credit_delay[0];
            credit_delay[0] = '0;
            for (int vc = 0; vc < NUM_DAT_VC; vc++) begin
                if (dat_credit_return_i[vc]) begin
                    credit[vc]++;
                    pending_credit[vc]--;
                end
                if (credit[vc] > CREDIT_DEPTH) $fatal(1, "credit overflow");
            end
            if (m_req_valid_o && m_req_ready_i) begin
                case (int'(m_req_o.header[ni_flit_pkg::AXI_CH_MSB:ni_flit_pkg::AXI_CH_LSB]))
                    ni_flit_pkg::AXI_CH_NarrowAw: begin
                        if (narrow_active || narrow_count >= WRITES/2 ||
                                m_req_o.payload[ni_flit_pkg::AW_AWADDR_MSB:ni_flit_pkg::AW_AWADDR_LSB] != address(narrow_count*2, 0))
                            $fatal(1, "narrow AW lost ownership or order");
                        narrow_active = 1; narrow_beat = 0;
                    end
                    ni_flit_pkg::AXI_CH_NarrowW: begin
                        if (!narrow_active ||
                                m_req_o.payload[ni_flit_pkg::NARROW_W_WDATA_MSB:ni_flit_pkg::NARROW_W_WDATA_LSB] != payload(narrow_count*2, narrow_beat) ||
                                m_req_o.header[ni_flit_pkg::FLIT_TAIL_LSB] != (narrow_beat == BEATS-1))
                            $fatal(1, "narrow W payload or burst lock mismatch");
                        narrow_beat++;
                        if (narrow_beat == BEATS) begin narrow_active = 0; narrow_count++; end
                    end
                    ni_flit_pkg::AXI_CH_DataAr: begin
                        if (narrow_active || read_count >= READS ||
                                m_req_o.payload[ni_flit_pkg::AR_ARADDR_MSB:ni_flit_pkg::AR_ARADDR_LSB] != address(read_count, 1))
                            $fatal(1, "AR lost order or interrupted write packet");
                        read_count++;
                    end
                    default: $fatal(1, "unexpected REQ channel");
                endcase
            end
            if (m_dat_valid_o) begin
                int vc;
                vc = int'(m_dat_o.header[ni_flit_pkg::VC_ID_MSB:ni_flit_pkg::VC_ID_LSB]);
                if (vc >= WRITE_VCS || credit[vc] == 0) $fatal(1, "illegal or empty DAT VC");
                credit[vc]--;
                pending_credit[vc]++;
                if (vc != 0 && credit[0] == 0 && !dut.i_tx_buffer.dat_empty[0])
                    vc_bypass_count++;
                case (int'(m_dat_o.header[ni_flit_pkg::AXI_CH_MSB:ni_flit_pkg::AXI_CH_LSB]))
                    ni_flit_pkg::AXI_CH_DataAw: begin
                        int txn;
                        txn = int'((m_dat_o.payload[ni_flit_pkg::AW_AWADDR_MSB:ni_flit_pkg::AW_AWADDR_LSB] - 48'h1000) >> 8);
                        if (txn < 0 || txn >= WRITES || !txn[0] || data_active_vc[vc])
                            $fatal(1, "data AW lost ownership or VC order");
                        if (data_seen[txn]) $fatal(1, "duplicate data AW");
                        data_seen[txn] = 1;
                        data_active_vc[vc] = 1;
                        data_txn[vc] = txn;
                        data_beat_vc[vc] = 0;
                    end
                    ni_flit_pkg::AXI_CH_DataW: begin
                        if (!data_active_vc[vc] ||
                                m_dat_o.payload[ni_flit_pkg::DATA_W_WDATA_MSB:ni_flit_pkg::DATA_W_WDATA_LSB] != ni_params_pkg::AXI_DATA_WIDTH'(payload(data_txn[vc], data_beat_vc[vc])) ||
                                m_dat_o.header[ni_flit_pkg::FLIT_TAIL_LSB] != (data_beat_vc[vc] == BEATS-1))
                            $fatal(1, "data W payload, VC, or lock mismatch");
                        data_beat_vc[vc]++;
                        if (data_beat_vc[vc] == BEATS) begin
                            data_active_vc[vc] = 0;
                            data_count++;
                        end
                    end
                    default: $fatal(1, "unexpected DAT channel");
                endcase
            end
            if (m_req_valid_o && m_req_ready_i && m_dat_valid_o) parallel_count++;
        end
    end

    initial begin
        s_aw_i              = '0; s_w_i = '0; s_ar_i = '0;
        s_aw_valid_i        = 0; s_w_valid_i = 0; s_ar_valid_i = 0;
        dat_credit_return_i = '0; m_req_ready_i = 0;
        repeat (4) @(negedge clk_i);
        #1; rst_n_i = 1;
        fork
            begin
                for (int txn = 0; txn < WRITES; txn++) begin
                    @(negedge clk_i); #1;
                    s_aw_i                           = '0;
                    s_aw_i.axi.awid                  = ni_params_pkg::NOC_ID_WIDTH'(txn % 8);
                    s_aw_i.axi.awaddr                = address(txn, txn[0]);
                    s_aw_i.axi.awlen                 = ni_flit_pkg::AXI_LEN_WIDTH'(BEATS-1);
                    s_aw_i.axi.awsize                = txn[0] ? 3'd6 : 3'd3;
                    s_aw_i.axi.awburst               = 2'b01;
                    s_aw_i.meta.route.domain.is_data = txn[0];
                    s_aw_i.meta.route.domain.dst_id  = ni_flit_pkg::DST_ID_WIDTH'(txn % 4);
                    s_aw_valid_i                     = 1;
                    do @(posedge clk_i); while (!s_aw_ready_o);
                    @(negedge clk_i); #1; s_aw_valid_i = 0;
                    for (int beat = 0; beat < BEATS; beat++) begin
                        s_w_i = '0;
                        if (txn[0]) s_w_i.wdata = ni_params_pkg::AXI_DATA_WIDTH'(payload(txn, beat));
                        else s_w_i.wdata[(beat+1)*64 +: 64] = payload(txn, beat);
                        s_w_i.wstrb = '1;
                        s_w_i.wlast = beat == BEATS-1;
                        s_w_valid_i = 1;
                        do @(posedge clk_i); while (!s_w_ready_o);
                        @(negedge clk_i); #1; s_w_valid_i = 0;
                    end
                end
                writes_done = 1;
            end
            begin
                for (int txn = 0; txn < READS; txn++) begin
                    @(negedge clk_i); #1;
                    s_ar_i                           = '0;
                    s_ar_i.axi.arid                  = ni_params_pkg::NOC_ID_WIDTH'(txn % 8);
                    s_ar_i.axi.araddr                = address(txn, 1);
                    s_ar_i.meta.route.domain.is_data = 1;
                    s_ar_valid_i                     = 1;
                    do @(posedge clk_i); while (!s_ar_ready_o);
                    @(negedge clk_i); #1; s_ar_valid_i = 0;
                end
                reads_done = 1;
            end
        join
        wait (narrow_count == WRITES/2 && data_count == WRITES/2 && read_count == READS);
        repeat (8) @(negedge clk_i);
        if (!writes_done || !reads_done || parallel_count == 0 || m_req_valid_o || m_dat_valid_o)
            $fatal(1, "incomplete drain or no parallel progress");
        for (int vc = 0; vc < NUM_DAT_VC; vc++) begin
            if (credit[vc] != CREDIT_DEPTH) $fatal(1, "credit not conserved after drain");
        end
        if (WRITE_VCS > 1 && vc_bypass_count == 0) $fatal(1, "no progress past credit-starved VC");
        $display("VC_BYPASS count=%0d", vc_bypass_count);
        $display("PASS: packet stress depth=%0d vcs=%0d mode=%0d writes=%0d reads=%0d parallel=%0d", FIFO_DEPTH, NUM_DAT_VC, DAT_VC_MODE, WRITES, READS, parallel_count);
        $finish;
    end
    initial begin #100us; $fatal(1, "packet stress timeout"); end
endmodule

`resetall
