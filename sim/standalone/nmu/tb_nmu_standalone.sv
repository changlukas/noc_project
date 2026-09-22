`timescale 1ns / 1ps
module tb_nmu_standalone #(
    parameter int ID_WIDTH = 8,
    parameter int NOC_HALF_PERIOD = 7,
    parameter int BUFFER_DEPTH = 128,
    parameter bit READ_ROB_ENABLED = 1
);
    import ni_flit_pkg::*;
`ifdef DUMP_WAVE
    initial begin : dump_wave
        string wave_file;
`ifdef VERILATOR
        if (!$value$plusargs("wave_file=%s", wave_file)) wave_file = "nmu.fst";
        $dumpfile(wave_file);
        $dumpvars(0, tb_nmu_standalone);
`else
        if (!$value$plusargs("wave_file=%s", wave_file)) wave_file = "nmu.fsdb";
        $fsdbDumpfile(wave_file);
        $fsdbDumpvars(0, tb_nmu_standalone, "+all");
`endif
    end
`endif
    logic axi_clk = 0, noc_clk = 0, rst_n = 0;
    bit warmup = 1;
    bit block_case = 0;
    string case_name = "legacy";
    int response_order = 1, response_delay = 12, startup_delay = 0;
    int stall_enable = 1, reset_warmup = 1;
    int min_outstanding = 0, min_unique = 0;
    int require_ooo = 0, require_buffered = 0, require_capacity = 0, require_stall = 0;
    int peak_w = 0, peak_r = 0, peak_unique_w = 0, peak_unique_r = 0;
    int blocked_aw = 0, blocked_ar = 0, reordered_b = 0, reordered_r = 0;

    int warm_requests = 0;
    req_flit_t warm_aw_packets[$], warm_ar_packets[$];
    always #5 axi_clk = ~axi_clk;
    always #(NOC_HALF_PERIOD) noc_clk = ~noc_clk;
    AXI_BUS_DV #(.AXI_ADDR_WIDTH(48), .AXI_DATA_WIDTH(512),
        .AXI_ID_WIDTH(ID_WIDTH), .AXI_USER_WIDTH(58)) vip(axi_clk);
    axi_if #(.ADDR_W(48), .DATA_W(512), .ID_W(ID_WIDTH), .AWUSER_W(58)) bus();
    typedef axi_test::axi_file_master #(.AW(48), .DW(512), .IW(ID_WIDTH),
        .UW(58), .TA(1ns), .TT(2ns)) master_t;
    master_t master;
    master_t::ax_beat_t expected_aw[$], expected_ar[$];
    master_t::w_beat_t expected_w[$];
    int expected_b_by_id[256][$], expected_r_by_id[256][$];
    int read_beat[256];
    int pending_b[$], pending_r[$];
    req_flit_t aw_packets[$], ar_packets[$];
    int aw_index = 0, ar_index = 0, w_index = 0, w_beat = 0, active_aw = -1;
    int b_count = 0, r_count = 0, cycles = 0, axi_cycles = 0;
    int b_buffered = 0, r_buffered = 0, reordered_sent = 0;
    int stall_cycles = 0;
    int live_w[256], live_r[256];
    int id_exhaustion_w = 0, id_exhaustion_r = 0;
    int b_full_cycles = 0, r_full_cycles = 0;
    logic req_valid, req_ready, rsp_valid = 0, rsp_ready, dat_valid;
    req_flit_t req;
    rsp_flit_t rsp = '0;
    wire allow_b = !warmup && (stall_enable == 0 || axi_cycles % 23 >= 7);
    wire allow_r = !warmup && (stall_enable == 0 || axi_cycles % 19 >= 6);
    assign bus.awid = vip.aw_id;
    assign bus.awaddr = vip.aw_addr;
    assign bus.awlen = vip.aw_len;
    assign bus.awsize = vip.aw_size;
    assign bus.awburst = vip.aw_burst;
    assign bus.awlock = vip.aw_lock;
    assign bus.awcache = vip.aw_cache;
    assign bus.awprot = vip.aw_prot;
    assign bus.awqos = vip.aw_qos;
    assign bus.awregion = vip.aw_region;
    assign bus.awuser = vip.aw_user;
    assign bus.awvalid = vip.aw_valid;
    assign vip.aw_ready = bus.awready;
    assign bus.wdata = vip.w_data;
    assign bus.wstrb = vip.w_strb;
    assign bus.wlast = vip.w_last;
    assign bus.wvalid = vip.w_valid;
    assign vip.w_ready = bus.wready;
    assign bus.arid = vip.ar_id;
    assign bus.araddr = vip.ar_addr;
    assign bus.arlen = vip.ar_len;
    assign bus.arsize = vip.ar_size;
    assign bus.arburst = vip.ar_burst;
    assign bus.arlock = vip.ar_lock;
    assign bus.arcache = vip.ar_cache;
    assign bus.arprot = vip.ar_prot;
    assign bus.arqos = vip.ar_qos;
    assign bus.arregion = vip.ar_region;
    assign bus.arvalid = vip.ar_valid;
    assign vip.ar_ready = bus.arready;
    assign bus.wuser = '0;
    assign bus.aruser = '0;
    assign bus.bready = vip.b_ready && allow_b;
    assign vip.b_valid = bus.bvalid && allow_b;
    assign vip.b_id = bus.bid;
    assign vip.b_resp = bus.bresp;
    assign vip.b_user = '0;
    assign bus.rready = vip.r_ready && allow_r;
    assign vip.r_valid = bus.rvalid && allow_r;
    assign vip.r_id = bus.rid;
    assign vip.r_data = bus.rdata;
    assign vip.r_resp = bus.rresp;
    assign vip.r_last = bus.rlast;
    assign vip.r_user = '0;
    nmu #(.AXI_ID_WIDTH(ID_WIDTH), .READ_ROB_ENABLED(READ_ROB_ENABLED),
        .NMU_ROB_B_DEPTH(BUFFER_DEPTH), .NMU_ROB_R_DEPTH(BUFFER_DEPTH)) dut (
        .ACLK(axi_clk), .ARESETn(rst_n), .noc_clk(noc_clk), .noc_rst_n(rst_n),
        .axi_wr_i(bus), .axi_rd_i(bus), .tx_req_valid_o(req_valid),
        .tx_req_flit_o(req), .tx_req_ready_i(req_ready),
        .rx_rsp_valid_i(rsp_valid), .rx_rsp_flit_i(rsp), .rx_rsp_ready_o(rsp_ready),
        .tx_dat_valid_o(dat_valid), .tx_dat_flit_o(), .tx_dat_crdvalid_i('0),
        .rx_dat_valid_i(1'b0), .rx_dat_flit_i('0), .rx_dat_ready_o()
    );
    always @(negedge noc_clk) if (rst_n) cycles++;
    always @(posedge axi_clk) if (rst_n) #0.5 axi_cycles++;
    assign req_ready = rst_n && (stall_enable == 0 || cycles % 17 >= 5);
    function automatic logic [63:0] read_pattern(input int txn, input int beat);
        return 64'hcafe123400000000 | (64'(txn) << 16) | 64'(beat);
    endfunction
    function automatic longint unsigned beat_address(input master_t::ax_beat_t ax, input int beat);
        longint unsigned step_size, span, address;
        step_size = 64'd1 << ax.ax_size;
        span = (64'(ax.ax_len)+1)*step_size;
        address = 64'(ax.ax_addr);
        if (beat != 0 && ax.ax_burst != 0) begin
            address = (address & ~(step_size-1)) + 64'(beat)*step_size;
            if (ax.ax_burst == 2)
                address = (64'(ax.ax_addr) & ~(span-1)) | (address & (span-1));
        end
        return address;
    endfunction
    task automatic check_address(input req_flit_t packet, input master_t::ax_beat_t ax);
        bit hit;
        hit = 0;
        if (packet.payload[AW_AWADDR_LSB +: AW_AWADDR_WIDTH] !== ax.ax_addr ||
            packet.payload[AW_AWLEN_LSB +: AW_AWLEN_WIDTH] !== ax.ax_len ||
            packet.payload[AW_AWSIZE_LSB +: AW_AWSIZE_WIDTH] !== ax.ax_size ||
            packet.payload[AW_AWBURST_LSB +: AW_AWBURST_WIDTH] !== ax.ax_burst)
            $fatal(1, "REQ mismatch AW=%0d AR=%0d: got addr=%h len=%h size=%h burst=%h expected addr=%h len=%h size=%h burst=%h",
                aw_index, ar_index,
                packet.payload[AW_AWADDR_LSB +: AW_AWADDR_WIDTH],
                packet.payload[AW_AWLEN_LSB +: AW_AWLEN_WIDTH],
                packet.payload[AW_AWSIZE_LSB +: AW_AWSIZE_WIDTH],
                packet.payload[AW_AWBURST_LSB +: AW_AWBURST_WIDTH],
                ax.ax_addr, ax.ax_len, ax.ax_size, ax.ax_burst);
        if (packet.payload[AW_AWCACHE_LSB +: AW_AWCACHE_WIDTH] !== ax.ax_cache ||
            packet.payload[AW_AWLOCK_LSB] !== ax.ax_lock ||
            packet.payload[AW_AWPROT_LSB +: AW_AWPROT_WIDTH] !== ax.ax_prot ||
            packet.payload[AW_AWQOS_LSB +: AW_AWQOS_WIDTH] !== ax.ax_qos ||
            packet.payload[AW_AWREGION_LSB +: AW_AWREGION_WIDTH] !== ax.ax_region ||
            packet.header[SRC_ID_LSB +: SRC_ID_WIDTH] !== 0 ||
            packet.header[SRC_PORT_ID_LSB +: SRC_PORT_ID_WIDTH] !== 0)
            $fatal(1, "REQ attributes/source identity mismatch");
        for (int i = 0; i < topology_pkg::SAM_NUM_RULES; i++) begin
            if (!hit && ax.ax_addr >= topology_pkg::SAM[i].start_addr &&
                ax.ax_addr < topology_pkg::SAM[i].end_addr) begin
                hit = 1;
                if (packet.header[DST_ID_LSB +: DST_ID_WIDTH] !== topology_pkg::SAM[i].idx.dst_id ||
                    packet.header[DST_PORT_ID_LSB +: DST_PORT_ID_WIDTH] !== topology_pkg::SAM[i].idx.dst_port_id)
                    $fatal(1, "REQ SAM destination mismatch");
            end
        end
        if (!hit) $fatal(1, "stimulus address outside SAM");
    endtask
    always @(posedge noc_clk) begin : monitor_req
        int channel, lane;
        if (rst_n && warmup && req_valid && req_ready) begin
            warm_requests++;
            if (req.header[AXI_CH_LSB +: AXI_CH_WIDTH] == AXI_CH_WIDTH'(AXI_CH_NarrowAw)) warm_aw_packets.push_back(req);
            if (req.header[AXI_CH_LSB +: AXI_CH_WIDTH] == AXI_CH_WIDTH'(AXI_CH_NarrowAr)) warm_ar_packets.push_back(req);
        end
        if (rst_n && !warmup) begin

            if (dut.i_response_path.i_ordering.b_free_count == 0) b_full_cycles++;
            if (READ_ROB_ENABLED && dut.i_response_path.i_ordering.r_free_count == 0) r_full_cycles++;
            if (dat_valid) $fatal(1, "control-plane test unexpectedly used DAT");
            if (dut.i_response_path.i_ordering.s_b_valid_i && dut.i_response_path.i_ordering.s_b_ready_o && !dut.i_response_path.i_ordering.b_direct)
                b_buffered = b_buffered + 1;
            if (dut.i_response_path.i_ordering.s_r_valid_i && dut.i_response_path.i_ordering.s_r_ready_o && !dut.i_response_path.i_ordering.r_direct)
                r_buffered = r_buffered + 1;
            if (req_valid && req_ready) begin
                channel = int'(req.header[AXI_CH_LSB +: AXI_CH_WIDTH]);
                case (channel)
                    AXI_CH_NarrowAw: begin
                        if (active_aw != -1 || aw_index >= expected_aw.size()) $fatal(1, "unexpected AW");
                        check_address(req, expected_aw[aw_index]);
                        if (req.payload[AW_AWUSER_LSB +: AW_AWUSER_WIDTH] !== expected_aw[aw_index].ax_user[7:0])
                            $fatal(1, "AWUSER mismatch");
                        aw_packets.push_back(req);
                        active_aw = aw_index;
                        aw_index++;
                        w_beat = 0;
                    end
                    AXI_CH_NarrowW: begin
                        if (active_aw == -1 || w_index >= expected_w.size()) $fatal(1, "orphan W");
                        lane = int'((beat_address(expected_aw[active_aw], w_beat) % 64) / 8);
                        if (req.payload[NARROW_W_WDATA_LSB +: 64] !== expected_w[w_index].w_data[lane*64 +: 64] ||
                            req.payload[NARROW_W_WSTRB_LSB +: 8] !== expected_w[w_index].w_strb[lane*8 +: 8] ||
                            req.payload[NARROW_W_WLAST_LSB] !== expected_w[w_index].w_last ||
                            req.header[ORDERING_TAG_LSB +: ORDERING_TAG_WIDTH] !== aw_packets[active_aw].header[ORDERING_TAG_LSB +: ORDERING_TAG_WIDTH] ||
                            req.header[DST_ID_LSB +: DST_ID_WIDTH] !== aw_packets[active_aw].header[DST_ID_LSB +: DST_ID_WIDTH])
                            $fatal(1, "W payload/ownership mismatch");
                        w_index++; w_beat++;
                        if (req.payload[NARROW_W_WLAST_LSB]) begin
                            if (w_beat != int'(expected_aw[active_aw].ax_len)+1) $fatal(1, "W length mismatch");
                            pending_b.push_back(active_aw);
                            active_aw = -1;
                        end
                    end
                    AXI_CH_NarrowAr: begin
                        if (active_aw != -1 || ar_index >= expected_ar.size()) $fatal(1, "unexpected/interleaved AR");
                        check_address(req, expected_ar[ar_index]);
                        ar_packets.push_back(req);
                        pending_r.push_back(ar_index);
                        ar_index++;
                    end
                    default: $fatal(1, "non-narrow request");
                endcase
            end
        end
    end
    always @(posedge axi_clk) begin : monitor_response
        int id, txn, lane, popped, unique_w, unique_r, total_w, total_r;
        logic [511:0] data;
        if (rst_n && !warmup) begin
            unique_w = 0; unique_r = 0; total_w = 0; total_r = 0;
            for (int i = 0; i < 256; i++) begin
                total_w += live_w[i]; total_r += live_r[i];
                if (live_w[i] != 0) unique_w++;
                if (live_r[i] != 0) unique_r++;
            end
            if (total_w > peak_w) peak_w = total_w;
            if (total_r > peak_r) peak_r = total_r;
            if (unique_w > peak_unique_w) peak_unique_w = unique_w;
            if (unique_r > peak_unique_r) peak_unique_r = unique_r;
            if (bus.awvalid && !bus.awready) blocked_aw++;
            if (bus.arvalid && !bus.arready) blocked_ar++;
            if (unique_w == 8 && bus.awvalid && !bus.awready && live_w[int'(bus.awid)] == 0)
                id_exhaustion_w++;
            if (unique_r == 8 && bus.arvalid && !bus.arready && live_r[int'(bus.arid)] == 0)
                id_exhaustion_r++;
            if (bus.awvalid && bus.awready) live_w[int'(bus.awid)]++;
            if (bus.arvalid && bus.arready) live_r[int'(bus.arid)]++;
            if ((bus.bvalid && !bus.bready) || (bus.rvalid && !bus.rready)) stall_cycles++;
            if (bus.bvalid && bus.bready) begin
                id = int'(bus.bid);
                if (expected_b_by_id[id].size() == 0) $fatal(1, "unexpected B ID");
                txn = expected_b_by_id[id].pop_front();
                if (bus.bresp !== 2'(txn % 3)) $fatal(1, "B response/order mismatch");
                live_w[id]--;
                b_count++;
            end
            if (bus.rvalid && bus.rready) begin
                id = int'(bus.rid);
                if (expected_r_by_id[id].size() == 0) $fatal(1, "unexpected R ID");
                txn = expected_r_by_id[id][0];
                lane = int'((beat_address(expected_ar[txn], read_beat[id]) % 64) / 8);
                data = 512'(read_pattern(txn, read_beat[id])) << (lane*64);
                if (bus.rdata !== data || bus.rresp !== 0 ||
                    bus.rlast !== (read_beat[id] == int'(expected_ar[txn].ax_len)))
                    $fatal(1, "R data/lane/order/last mismatch txn=%0d beat=%0d got=%h expected=%h", txn, read_beat[id], bus.rdata, data);
                read_beat[id]++;
                if (bus.rlast) begin
                    popped = expected_r_by_id[id].pop_front();
                    read_beat[id] = 0;
                    live_r[id]--;
                    r_count++;
                end
            end
        end
    end
    task automatic send_rsp(input rsp_flit_t value);
        @(negedge noc_clk); rsp = value; rsp_valid = 1;
        do @(posedge noc_clk); while (!rsp_ready);
        @(negedge noc_clk); rsp_valid = 0;
    endtask
    // Select the latest tagged response first to exercise reorder storage.
    // Untagged same-ID responses retain their request order.
    initial begin : response_stimulus
        int index, txn;
        bit eligible;
        rsp_flit_t value;
        req_flit_t request;
        wait(rst_n && !warmup);
        repeat (startup_delay) @(negedge noc_clk);
        forever begin
            repeat (response_delay) @(negedge noc_clk);
            if (pending_b.size() != 0) begin
                index = 0;
                for (int i = 1; i < pending_b.size(); i++) begin
                    eligible = response_order == 2 ||
                        (response_order == 1 && aw_packets[pending_b[i]].header[ORDERING_REQ_LSB]);
                    // Preserve same-ID order within each destination. Cross-ID
                    // scheduling preserves all same-ID order, regardless of route.
                    if (block_case) begin
                        for (int j = 0; j < i; j++) begin
                            if (expected_aw[pending_b[j]].ax_id == expected_aw[pending_b[i]].ax_id &&
                                (response_order == 2 ||
                                 aw_packets[pending_b[j]].header[DST_ID_LSB +: DST_ID_WIDTH] ==
                                 aw_packets[pending_b[i]].header[DST_ID_LSB +: DST_ID_WIDTH])) eligible = 0;
                        end
                    end
                    if (eligible) index = i;
                end
                txn = pending_b[index]; pending_b.delete(index);
                if (index != 0) reordered_b++;
                if (index != 0) reordered_sent++;
                request = aw_packets[txn];
                value = '0;
                value.header = request.header;
                value.header[DST_ID_LSB +: DST_ID_WIDTH] = request.header[SRC_ID_LSB +: SRC_ID_WIDTH];
                value.header[SRC_ID_LSB +: SRC_ID_WIDTH] = request.header[DST_ID_LSB +: DST_ID_WIDTH];
                value.header[DST_PORT_ID_LSB +: DST_PORT_ID_WIDTH] = request.header[SRC_PORT_ID_LSB +: SRC_PORT_ID_WIDTH];
                value.header[SRC_PORT_ID_LSB +: SRC_PORT_ID_WIDTH] = request.header[DST_PORT_ID_LSB +: DST_PORT_ID_WIDTH];
                value.header[AXI_CH_LSB +: AXI_CH_WIDTH] = AXI_CH_WIDTH'(AXI_CH_NarrowB);
                value.header[FLIT_TAIL_LSB] = 1;
                value.payload[B_BID_LSB +: B_BID_WIDTH] = request.payload[AW_AWID_LSB +: AW_AWID_WIDTH];
                value.payload[B_BRESP_LSB +: B_BRESP_WIDTH] = 2'(txn % 3);
                send_rsp(value);
            end
            if (pending_r.size() != 0) begin
                index = 0;
                for (int i = 1; i < pending_r.size(); i++) begin
                    eligible = response_order == 2 ||
                        (response_order == 1 && ar_packets[pending_r[i]].header[ORDERING_REQ_LSB]);
                    // Preserve same-ID order within each destination. Cross-ID
                    // scheduling preserves all same-ID order, regardless of route.
                    if (block_case) begin
                        for (int j = 0; j < i; j++) begin
                            if (expected_ar[pending_r[j]].ax_id == expected_ar[pending_r[i]].ax_id &&
                                (response_order == 2 ||
                                 ar_packets[pending_r[j]].header[DST_ID_LSB +: DST_ID_WIDTH] ==
                                 ar_packets[pending_r[i]].header[DST_ID_LSB +: DST_ID_WIDTH])) eligible = 0;
                        end
                    end
                    if (eligible) index = i;
                end
                txn = pending_r[index]; pending_r.delete(index);
                if (index != 0) reordered_r++;
                if (index != 0) reordered_sent++;
                request = ar_packets[txn];
                for (int beat = 0; beat <= int'(expected_ar[txn].ax_len); beat++) begin
                    value = '0;
                    value.header = request.header;
                value.header[DST_ID_LSB +: DST_ID_WIDTH] = request.header[SRC_ID_LSB +: SRC_ID_WIDTH];
                value.header[SRC_ID_LSB +: SRC_ID_WIDTH] = request.header[DST_ID_LSB +: DST_ID_WIDTH];
                value.header[DST_PORT_ID_LSB +: DST_PORT_ID_WIDTH] = request.header[SRC_PORT_ID_LSB +: SRC_PORT_ID_WIDTH];
                value.header[SRC_PORT_ID_LSB +: SRC_PORT_ID_WIDTH] = request.header[DST_PORT_ID_LSB +: DST_PORT_ID_WIDTH];
                    value.header[AXI_CH_LSB +: AXI_CH_WIDTH] = AXI_CH_WIDTH'(AXI_CH_NarrowR);
                    value.header[FLIT_TAIL_LSB] = 1;
                    value.payload[NARROW_R_RLAST_LSB] = beat == int'(expected_ar[txn].ax_len);
                    value.payload[NARROW_R_RID_LSB +: NARROW_R_RID_WIDTH] = request.payload[AR_ARID_LSB +: AR_ARID_WIDTH];
                    value.payload[NARROW_R_RDATA_LSB +: 64] = read_pattern(txn, beat);
                    if ($test$plusargs("corrupt_rsp") && txn == 0 && beat == 0)
                        value.payload[NARROW_R_RDATA_LSB] = ~value.payload[NARROW_R_RDATA_LSB];
                    send_rsp(value);
                end
            end
        end
    end
    initial begin : run
        string stim_dir;
        int pattern_id_width, probe;
        master_t::ax_beat_t warm_aw, warm_ar;
        master_t::w_beat_t warm_w;
        rsp_flit_t warm_rsp;
        master = new(vip);
        if (!$value$plusargs("stim_dir=%s", stim_dir)) $fatal(1, "missing stim_dir");
        block_case = $test$plusargs("block_case");
        if (block_case) begin
            if (!$value$plusargs("case_id_width=%d", pattern_id_width) || pattern_id_width != ID_WIDTH)
                $fatal(1, "pattern ID width does not match DUT");
            void'($value$plusargs("case_name=%s", case_name));
            if (!$value$plusargs("response_order=%d", response_order)) $fatal(1, "missing response_order");
            if (!$value$plusargs("response_delay=%d", response_delay)) $fatal(1, "missing response_delay");
            if (!$value$plusargs("startup_delay=%d", startup_delay)) $fatal(1, "missing startup_delay");
            if (!$value$plusargs("stall_enable=%d", stall_enable)) $fatal(1, "missing stall_enable");
            if (!$value$plusargs("reset_warmup=%d", reset_warmup)) $fatal(1, "missing reset_warmup");
            if (!$value$plusargs("min_outstanding=%d", min_outstanding)) $fatal(1, "missing min_outstanding");
            if (!$value$plusargs("min_unique=%d", min_unique)) $fatal(1, "missing min_unique");
            if (!$value$plusargs("require_ooo=%d", require_ooo)) $fatal(1, "missing require_ooo");
            if (!$value$plusargs("require_buffered=%d", require_buffered)) $fatal(1, "missing require_buffered");
            if (!$value$plusargs("require_capacity=%d", require_capacity)) $fatal(1, "missing require_capacity");
            if (!$value$plusargs("require_stall=%d", require_stall)) $fatal(1, "missing require_stall");
            // The upstream file parser assumes nonempty input. Probe empty
            // directions before using its existing parse functions.
            master.read_fd = $fopen({stim_dir,"/read.txt"}, "r");
            master.write_fd = $fopen({stim_dir,"/write.txt"}, "r");
            if (master.read_fd == 0 || master.write_fd == 0) $fatal(1, "missing AXI input file");
            probe = $fgetc(master.read_fd);
            if (probe != -1) begin
                probe = $ungetc(probe, master.read_fd);
                master.parse_read();
            end
            probe = $fgetc(master.write_fd);
            if (probe != -1) begin
                probe = $ungetc(probe, master.write_fd);
                master.parse_write();
            end
            $fclose(master.read_fd);
            $fclose(master.write_fd);
        end else begin
            master.load_files({stim_dir,"/read.txt"}, {stim_dir,"/write.txt"});
        end
        expected_aw = master.aw_queue;
        expected_ar = master.ar_queue;
        expected_w = master.w_queue;
        foreach (expected_aw[i]) expected_b_by_id[int'(expected_aw[i].ax_id)].push_back(i);
        foreach (expected_ar[i]) expected_r_by_id[int'(expected_ar[i].ax_id)].push_back(i);
        repeat (5) @(negedge axi_clk);
        rst_n = 1;
        if (reset_warmup != 0) begin
        // Populate remap, CDC and order-list state, then flush before the real run.
        fork
            master.drv.send_aw(expected_aw[0]);
            begin
                for (int i = 0; i <= int'(expected_aw[0].ax_len); i++) master.drv.send_w(expected_w[i]);
            end
            master.drv.send_ar(expected_ar[0]);
        join
        warm_aw = new;
        warm_ar = new;
        warm_w = new;
        warm_aw.ax_id = expected_aw[0].ax_id;
        warm_ar.ax_id = expected_ar[0].ax_id;
        // A second destination forces tagged responses in enabled mode.
        warm_aw.ax_addr = expected_aw[0].ax_addr ^ 48'h100000000;
        warm_ar.ax_addr = expected_ar[0].ax_addr ^ 48'h100000000;
        warm_aw.ax_size = 3; warm_aw.ax_burst = 1;
        warm_ar.ax_size = 3; warm_ar.ax_burst = 1;
        warm_w.w_last = 1;
        if (READ_ROB_ENABLED) begin
            fork
                master.drv.send_aw(warm_aw);
                master.drv.send_w(warm_w);
                master.drv.send_ar(warm_ar);
            join
            wait(warm_aw_packets.size() == 2 && warm_ar_packets.size() == 2);
            repeat (10) @(negedge noc_clk);
            warm_rsp = '0;
            warm_rsp.header = warm_aw_packets[1].header;
            warm_rsp.header[AXI_CH_LSB +: AXI_CH_WIDTH] = AXI_CH_WIDTH'(AXI_CH_NarrowB);
            warm_rsp.header[FLIT_TAIL_LSB] = 1;
            warm_rsp.payload[B_BID_LSB +: B_BID_WIDTH] = warm_aw_packets[1].payload[AW_AWID_LSB +: AW_AWID_WIDTH];
            send_rsp(warm_rsp);
            warm_rsp = '0;
            warm_rsp.header = warm_ar_packets[1].header;
            warm_rsp.header[AXI_CH_LSB +: AXI_CH_WIDTH] = AXI_CH_WIDTH'(AXI_CH_NarrowR);
            warm_rsp.header[FLIT_TAIL_LSB] = 1;
            warm_rsp.payload[NARROW_R_RID_LSB +: NARROW_R_RID_WIDTH] = warm_ar_packets[1].payload[AR_ARID_LSB +: AR_ARID_WIDTH];
            warm_rsp.payload[NARROW_R_RLAST_LSB] = 1;
            send_rsp(warm_rsp);
        end
        repeat (40) @(negedge noc_clk);
        if (READ_ROB_ENABLED &&
            (dut.i_response_path.i_ordering.b_complete == '0 || dut.i_response_path.i_ordering.r_complete == '0))
            $fatal(1, "reset did not cover occupied B/R reorder storage");
        if (warm_requests == 0) $fatal(1, "reset warmup did not reach NMU egress");
        @(negedge axi_clk); rst_n = 0; master.reset();
        repeat (10) @(negedge noc_clk);
        @(negedge axi_clk); rst_n = 1; warmup = 0;
        end else begin
            warmup = 0;
        end
        master.run();
        repeat (20) @(negedge axi_clk);
        if (b_count != expected_aw.size() || r_count != expected_ar.size() ||
            aw_index != expected_aw.size() || ar_index != expected_ar.size() ||
            w_index != expected_w.size() || pending_b.size() != 0 || pending_r.size() != 0)
            $fatal(1, "incomplete transaction drain");
        if ($test$plusargs("require_reorder") &&
            (b_buffered == 0 || (READ_ROB_ENABLED && r_buffered == 0)))
            $fatal(1, "reorder coverage was vacuous B=%0d R=%0d", b_buffered, r_buffered);
        for (int i = 0; i < 256; i++)
            if (live_w[i] != 0 || live_r[i] != 0) $fatal(1, "live ID leaked");
        if ($test$plusargs("require_reorder") && ID_WIDTH == 8 &&
            (id_exhaustion_w == 0 || id_exhaustion_r == 0)) $fatal(1, "ID exhaustion coverage missing");
        if ($test$plusargs("require_pressure") && (b_full_cycles == 0 || r_full_cycles == 0))
            $fatal(1, "buffer pressure coverage missing");
        if (block_case) begin
            if ((expected_aw.size() != 0 && (peak_w < min_outstanding || peak_unique_w < min_unique)) ||
                (expected_ar.size() != 0 && (peak_r < min_outstanding || peak_unique_r < min_unique)))
                $fatal(1, "outstanding coverage missing");
            if (require_ooo != 0 && (reordered_b == 0 || reordered_r == 0))
                $fatal(1, "cross-ID out-of-order coverage missing");
            if (require_buffered != 0 && (b_buffered == 0 || (READ_ROB_ENABLED && r_buffered == 0)))
                $fatal(1, "B/R reorder coverage missing");
            if (require_stall != 0 && stall_cycles == 0) $fatal(1, "response stall coverage missing");
            if (require_capacity != 0) begin
                if (blocked_aw == 0 || blocked_ar == 0) $fatal(1, "admission pressure missing");
                if (ID_WIDTH == 8 && (id_exhaustion_w == 0 || id_exhaustion_r == 0))
                    $fatal(1, "ID exhaustion/recovery coverage missing");
                if (BUFFER_DEPTH == 8 && READ_ROB_ENABLED && (b_full_cycles == 0 || r_full_cycles == 0))
                    $fatal(1, "B/R pool capacity coverage missing");
            end
            $display("COVER case=%s peak_W=%0d peak_R=%0d unique_W=%0d unique_R=%0d blocked_AW=%0d blocked_AR=%0d ooo_B=%0d ooo_R=%0d",
                case_name,peak_w,peak_r,peak_unique_w,peak_unique_r,blocked_aw,blocked_ar,reordered_b,reordered_r);
        end
        $display("COVER buffer full B=%0d R=%0d", b_full_cycles, r_full_cycles);
        $display("COVER ID exhaustion write=%0d read=%0d", id_exhaustion_w, id_exhaustion_r);
        $display("PASS NMU standalone ID=%0d B=%0d R=%0d buffered_B=%0d buffered_R=%0d reordered=%0d stall=%0d",
            ID_WIDTH,b_count,r_count,b_buffered,r_buffered,reordered_sent,stall_cycles);
        $finish;
    end
    assert property (@(posedge noc_clk) disable iff (!rst_n)
        req_valid && !req_ready |=> req_valid && $stable(req))
        else $fatal(1, "REQ changed while stalled");
    assert property (@(posedge axi_clk) disable iff (!rst_n)
        bus.bvalid && !bus.bready |=> bus.bvalid && $stable({bus.bid,bus.bresp}))
        else $fatal(1, "B changed while stalled");
    assert property (@(posedge axi_clk) disable iff (!rst_n)
        bus.rvalid && !bus.rready |=> bus.rvalid && $stable({bus.rid,bus.rresp,bus.rdata,bus.rlast}))
        else $fatal(1, "R changed while stalled");
    initial begin #2000000; $fatal(1, "NMU standalone timeout AW=%0d AR=%0d B=%0d R=%0d",aw_index,ar_index,b_count,r_count); end
endmodule
