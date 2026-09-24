`timescale 1ns / 1ps
`include "axi/assign.svh"
module tb_nmu_cosim #(
    parameter int unsigned AXI_ID_WIDTH = ni_params_pkg::AXI_ID_WIDTH,
    parameter int unsigned MAX_ACTIVE_IDS = 1 << (AXI_ID_WIDTH < ni_params_pkg::NOC_ID_WIDTH ?
        AXI_ID_WIDTH : ni_params_pkg::NOC_ID_WIDTH),
    parameter int unsigned MAX_OUTSTANDING_PER_ID = ni_params_pkg::NMU_MAX_OUTSTANDING_PER_ID,
    parameter int unsigned RSP_DELAY_CYCLES = 0,
    parameter int unsigned OUTPUT_REG_TYPE = 0,
    parameter int unsigned IO_FIFO_DEPTH = 32
);
    import ni_params_pkg::*;
    localparam int NUM_PORTS = 5;
    localparam int NMU_PORT = 0;
    localparam int NUM_NSUS = NUM_PORTS - 1;
    localparam int ROUTER_X = 1;
    localparam int ROUTER_Y = 1;
    localparam int MESH_DIM = 4;
    localparam int NMU_ID   = (ROUTER_Y << ni_flit_pkg::X_WIDTH) | ROUTER_X;
    localparam int NUM_WR_VC = NOC_DAT_VC_MODE == 1 ? NUM_DAT_VC/2 : NUM_DAT_VC;
    initial begin
        $display("TX_STORAGE req_bits=%0d dat_bits=%0d context_bits=%0d",
            IO_FIFO_DEPTH*$bits(ni_flit_pkg::req_flit_t),
            NUM_WR_VC*IO_FIFO_DEPTH*$bits(ni_flit_pkg::dat_flit_t),
            $bits(ni_types_pkg::nmu_aw_request_t)+ni_flit_pkg::AXI_LEN_WIDTH+1);
        $display("TX_STORAGE_OLD equal_depth_bits=%0d prior_depth_bits=%0d",
            IO_FIFO_DEPTH*(3*$bits(ni_types_pkg::nmu_aw_request_t)+$bits(ni_types_pkg::nmu_ar_request_t)+
                2*($bits(ni_signals_pkg::axi_w_t)+$bits(ni_types_pkg::nmu_aw_request_t)+ni_flit_pkg::AXI_LEN_WIDTH)),
            NOC_FIFO_DEPTH*(3*$bits(ni_types_pkg::nmu_aw_request_t)+$bits(ni_types_pkg::nmu_ar_request_t)+
                2*($bits(ni_signals_pkg::axi_w_t)+$bits(ni_types_pkg::nmu_aw_request_t)+ni_flit_pkg::AXI_LEN_WIDTH)));
    end
    int reorder_test = 0;
    int dst_wr_cnt[NUM_NSUS] = '{default:0};
    int dst_rd_cnt[NUM_NSUS] = '{default:0};
    function automatic int nsu_id(input int port);
        case (port)
            1: return ((ROUTER_Y + 1) << ni_flit_pkg::X_WIDTH) | ROUTER_X;
            2: return (ROUTER_Y << ni_flit_pkg::X_WIDTH) | (ROUTER_X + 1);
            3: return ((ROUTER_Y - 1) << ni_flit_pkg::X_WIDTH) | ROUTER_X;
            4: return (ROUTER_Y << ni_flit_pkg::X_WIDTH) | (ROUTER_X - 1);
            default: return NMU_ID;
        endcase
    endfunction
    bit corrupt_rsp = 0;
    logic clk = 0, rst_n = 0;
    wire axi_rst_n, noc_rst_n;
    always #5 clk = ~clk;
    cc_rstgen_bypass #(.NumRegs(2)) i_axi_reset_sync (
        .clk_i            (clk),
        .rst_ni           (rst_n),
        .rst_test_mode_ni (rst_n),
        .test_mode_i      (1'b0),
        .rst_no           (axi_rst_n),
        .init_no          ()
    );
    cc_rstgen_bypass #(.NumRegs(2)) i_noc_reset_sync (
        .clk_i            (clk),
        .rst_ni           (rst_n),
        .rst_test_mode_ni (rst_n),
        .test_mode_i      (1'b0),
        .rst_no           (noc_rst_n),
        .init_no          ()
    );
    AXI_BUS_DV #(.AXI_ADDR_WIDTH(AXI_ADDR_WIDTH), .AXI_DATA_WIDTH(AXI_DATA_WIDTH),
        .AXI_ID_WIDTH   (AXI_ID_WIDTH),
        .AXI_USER_WIDTH (AXI_AWUSER_WIDTH)) vip(clk);
    axi_if #(.ADDR_W(AXI_ADDR_WIDTH), .DATA_W(AXI_DATA_WIDTH),
        .ID_W     (AXI_ID_WIDTH),
        .AWUSER_W (AXI_AWUSER_WIDTH)) bus();
    longint unsigned router_ctx, nsu_ctx[NUM_NSUS];
    wire [NUM_PORTS-1:0] tx_req_valid, rx_req_valid;
    wire [NOC_REQ_FLIT_WIDTH-1:0] tx_req_flit [NUM_PORTS], rx_req_flit [NUM_PORTS];
    wire [NUM_PORTS-1:0] tx_rsp_valid, rx_rsp_valid;
    wire [NOC_RSP_FLIT_WIDTH-1:0] tx_rsp_flit [NUM_PORTS], rx_rsp_flit [NUM_PORTS];
    wire [NUM_PORTS-1:0] tx_dat_valid, rx_dat_valid;
    wire [NOC_DAT_FLIT_WIDTH-1:0] tx_dat_flit [NUM_PORTS], rx_dat_flit [NUM_PORTS];
    wire [NUM_PORTS-1:0] tx_req_ready, rx_req_ready, tx_rsp_ready, rx_rsp_ready;
    wire [NUM_DAT_VC-1:0] tx_dat_credit [NUM_PORTS], rx_dat_credit [NUM_PORTS];
    assign bus.awid     = vip.aw_id;
    assign bus.awaddr   = vip.aw_addr;
    assign bus.awlen    = vip.aw_len;
    assign bus.awsize   = vip.aw_size;
    assign bus.awburst  = vip.aw_burst;
    assign bus.awlock   = vip.aw_lock;
    assign bus.awcache  = vip.aw_cache;
    assign bus.awprot   = vip.aw_prot;
    assign bus.awqos    = vip.aw_qos;
    assign bus.awregion = vip.aw_region;
    assign bus.awuser   = vip.aw_user;
    assign bus.awvalid  = vip.aw_valid;
    assign vip.aw_ready = bus.awready;
    assign bus.wdata    = vip.w_data;
    assign bus.wstrb    = vip.w_strb;
    assign bus.wlast    = vip.w_last;
    assign bus.wvalid   = vip.w_valid;
    assign vip.w_ready  = bus.wready;
    assign bus.arid     = vip.ar_id;
    assign bus.araddr   = vip.ar_addr;
    assign bus.arlen    = vip.ar_len;
    assign bus.arsize   = vip.ar_size;
    assign bus.arburst  = vip.ar_burst;
    assign bus.arlock   = vip.ar_lock;
    assign bus.arcache  = vip.ar_cache;
    assign bus.arprot   = vip.ar_prot;
    assign bus.arqos    = vip.ar_qos;
    assign bus.arregion = vip.ar_region;
    assign bus.arvalid  = vip.ar_valid;
    assign vip.ar_ready = bus.arready;
    assign bus.wuser    = '0;
    assign bus.aruser   = '0;
    assign bus.bready   = vip.b_ready;
    assign vip.b_valid  = bus.bvalid;
    assign vip.b_id     = bus.bid;
    assign vip.b_resp   = bus.bresp;
    assign vip.b_user   = '0;
    assign bus.rready   = vip.r_ready;
    assign vip.r_valid  = bus.rvalid;
    assign vip.r_id     = bus.rid;
    assign vip.r_data   = bus.rdata ^ (corrupt_rsp ? AXI_DATA_WIDTH'(1) : '0);
    assign vip.r_resp   = bus.rresp;
    assign vip.r_last   = bus.rlast;
    assign vip.r_user   = '0;
    nmu #(
        .REQ_AW_REG_TYPE (OUTPUT_REG_TYPE),
        .REQ_W_REG_TYPE (OUTPUT_REG_TYPE),
        .REQ_AR_REG_TYPE (OUTPUT_REG_TYPE),
        .DAT_AW_REG_TYPE (OUTPUT_REG_TYPE),
        .DAT_W_REG_TYPE (OUTPUT_REG_TYPE),
        .B_REG_TYPE (OUTPUT_REG_TYPE),
        .R_REG_TYPE (OUTPUT_REG_TYPE),
        .AXI_ID_WIDTH (AXI_ID_WIDTH),
        .MAX_ACTIVE_IDS (MAX_ACTIVE_IDS),
        .MAX_OUTSTANDING_PER_ID (MAX_OUTSTANDING_PER_ID),
        .AXI_FIFO_DEPTH (IO_FIFO_DEPTH),
        .REQ_FIFO_DEPTH (IO_FIFO_DEPTH),
        .DAT_TX_FIFO_DEPTH (IO_FIFO_DEPTH),
        .RSP_RX_FIFO_DEPTH (IO_FIFO_DEPTH),
        .SRC_ID (ni_flit_pkg::SRC_ID_WIDTH'(NMU_ID))
    ) dut (
        .ACLK              (clk),
        .ARESETn           (axi_rst_n),
        .noc_clk           (clk),
        .noc_rst_n         (noc_rst_n),
        .axi_wr_i          (bus),
        .axi_rd_i          (bus),
        .tx_req_valid_o    (rx_req_valid[NMU_PORT]),
        .tx_req_flit_o     (rx_req_flit[NMU_PORT]),
        .tx_req_ready_i    (rx_req_ready[NMU_PORT]),
        .rx_rsp_valid_i    (tx_rsp_valid[NMU_PORT]),
        .rx_rsp_flit_i     (tx_rsp_flit[NMU_PORT]),
        .rx_rsp_ready_o    (tx_rsp_ready[NMU_PORT]),
        .tx_dat_valid_o    (rx_dat_valid[NMU_PORT]),
        .tx_dat_flit_o     (rx_dat_flit[NMU_PORT]),
        .tx_dat_crdvalid_i (rx_dat_credit[NMU_PORT]),
        .rx_dat_valid_i    (tx_dat_valid[NMU_PORT]),
        .rx_dat_flit_i     (tx_dat_flit[NMU_PORT]),
        .rx_dat_crdvalid_o (tx_dat_credit[NMU_PORT])
    );
    router_wrap i_router (
        .clk_i           (clk),
        .rst_n_i         (noc_rst_n),
        .ctx_i           (router_ctx),
        .tx_req_valid    (tx_req_valid),
        .tx_req_flit     (tx_req_flit),
        .tx_req_ready    (tx_req_ready),
        .rx_req_valid    (rx_req_valid),
        .rx_req_flit     (rx_req_flit),
        .rx_req_ready    (rx_req_ready),
        .tx_rsp_valid    (tx_rsp_valid),
        .tx_rsp_flit     (tx_rsp_flit),
        .tx_rsp_ready    (tx_rsp_ready),
        .rx_rsp_valid    (rx_rsp_valid),
        .rx_rsp_flit     (rx_rsp_flit),
        .rx_rsp_ready    (rx_rsp_ready),
        .tx_dat_valid    (tx_dat_valid),
        .tx_dat_flit     (tx_dat_flit),
        .tx_dat_crdvalid (tx_dat_credit),
        .rx_dat_valid    (rx_dat_valid),
        .rx_dat_flit     (rx_dat_flit),
        .rx_dat_crdvalid (rx_dat_credit)
    );
    for (genvar n = 0; n < NUM_NSUS; n++) begin : gen_nsu
        localparam int PORT = n + 1;
        AXI_BUS #(.AXI_ADDR_WIDTH(AXI_ADDR_WIDTH), .AXI_DATA_WIDTH(AXI_DATA_WIDTH),
            .AXI_ID_WIDTH   (NSU_AXI_ID_WIDTH),
            .AXI_USER_WIDTH (AXI_AWUSER_WIDTH)) mem_bus();
        ni_signals_pkg::axi_req_t mem_req;
        ni_signals_pkg::axi_rsp_t mem_rsp;
        AXI_BUS #(.AXI_ADDR_WIDTH(AXI_ADDR_WIDTH), .AXI_DATA_WIDTH(AXI_DATA_WIDTH),
            .AXI_ID_WIDTH   (NSU_AXI_ID_WIDTH),
            .AXI_USER_WIDTH (AXI_AWUSER_WIDTH)) delayed_bus();
        int b_wait_start = -1, r_wait_start = -1;
        int sample_cycle = 0;
        always @(posedge clk) begin
            if (noc_rst_n) begin
                sample_cycle++;
                if (mem_bus.aw_valid && mem_bus.aw_ready) dst_wr_cnt[n]++;
                if (mem_bus.ar_valid && mem_bus.ar_ready) dst_rd_cnt[n]++;
                if (reorder_test != 0 && PORT == 4) begin
                    if (delayed_bus.b_valid && b_wait_start < 0) b_wait_start = sample_cycle;
                    if (mem_bus.b_valid && mem_bus.b_ready && b_wait_start >= 0) begin
                        $display("DELAY_SAMPLE channel=B cycles=%0d", sample_cycle - b_wait_start);
                        b_wait_start = -1;
                    end
                    if (delayed_bus.r_valid && r_wait_start < 0) r_wait_start = sample_cycle;
                    if (mem_bus.r_valid && mem_bus.r_ready && r_wait_start >= 0) begin
                        $display("DELAY_SAMPLE channel=R cycles=%0d", sample_cycle - r_wait_start);
                        r_wait_start = -1;
                    end
                end
            end
        end
        nsu_wrap i_nsu (
            .clk_i             (clk),
            .rst_n_i           (noc_rst_n),
            .ctx_i             (nsu_ctx[n]),
            .rx_req_valid_i    (tx_req_valid[PORT]),
            .rx_req_flit_i     (tx_req_flit[PORT]),
            .rx_req_ready_o    (tx_req_ready[PORT]),
            .tx_rsp_valid_o    (rx_rsp_valid[PORT]),
            .tx_rsp_flit_o     (rx_rsp_flit[PORT]),
            .tx_rsp_ready_i    (rx_rsp_ready[PORT]),
            .tx_dat_valid_o    (rx_dat_valid[PORT]),
            .tx_dat_flit_o     (rx_dat_flit[PORT]),
            .tx_dat_crdvalid_i (rx_dat_credit[PORT]),
            .rx_dat_valid_i    (tx_dat_valid[PORT]),
            .rx_dat_flit_i     (tx_dat_flit[PORT]),
            .rx_dat_crdvalid_o (tx_dat_credit[PORT]),
            .axi_req_o         (mem_req),
            .axi_rsp_i         (mem_rsp)
        );
        wire delay_en = reorder_test != 0 && PORT == 4;
        if (RSP_DELAY_CYCLES == 0) begin : gen_no_delay
            `AXI_ASSIGN(delayed_bus, mem_bus)
        end else begin : gen_rsp_delay
            AXI_BUS #(
                .AXI_ADDR_WIDTH (AXI_ADDR_WIDTH),
                .AXI_DATA_WIDTH (AXI_DATA_WIDTH),
                .AXI_ID_WIDTH   (NSU_AXI_ID_WIDTH),
                .AXI_USER_WIDTH (AXI_AWUSER_WIDTH)
            ) delay_bus[RSP_DELAY_CYCLES+1]();
            `AXI_ASSIGN(delay_bus[0], mem_bus)
            `AXI_ASSIGN(delayed_bus, delay_bus[RSP_DELAY_CYCLES])
            // One-cycle upstream cells make the sweep include every integer delay.
            for (genvar stage = 0; stage < RSP_DELAY_CYCLES; stage++) begin : gen_stage
                axi_delayer_intf #(
                    .AXI_ID_WIDTH        (NSU_AXI_ID_WIDTH),
                    .AXI_ADDR_WIDTH      (AXI_ADDR_WIDTH),
                    .AXI_DATA_WIDTH      (AXI_DATA_WIDTH),
                    .AXI_USER_WIDTH      (AXI_AWUSER_WIDTH),
                    .STALL_RANDOM_INPUT  (1'b0),
                    .STALL_RANDOM_OUTPUT (1'b0),
                    .FIXED_DELAY_INPUT   (0),
                    .FIXED_DELAY_OUTPUT  (1)
                ) i_rsp_delay (
                    .clk_i    (clk),
                    .rst_ni   (axi_rst_n),
                    .bypass_i (!delay_en),
                    .slv      (delay_bus[stage]),
                    .mst      (delay_bus[stage+1])
                );
            end
        end
        assign mem_bus.aw_id = mem_req.awid;
        assign mem_bus.aw_addr = mem_req.awaddr;
        assign mem_bus.aw_len = mem_req.awlen;
        assign mem_bus.aw_size = mem_req.awsize;
        assign mem_bus.aw_burst = mem_req.awburst;
        assign mem_bus.aw_lock = mem_req.awlock;
        assign mem_bus.aw_cache = mem_req.awcache;
        assign mem_bus.aw_prot = mem_req.awprot;
        assign mem_bus.aw_qos = mem_req.awqos;
        assign mem_bus.aw_region = mem_req.awregion;
        assign mem_bus.aw_valid = mem_req.awvalid;
        assign mem_bus.w_data = mem_req.wdata;
        assign mem_bus.w_strb = mem_req.wstrb;
        assign mem_bus.w_last = mem_req.wlast;
        assign mem_bus.w_valid = mem_req.wvalid;
        assign mem_bus.ar_id = mem_req.arid;
        assign mem_bus.ar_addr = mem_req.araddr;
        assign mem_bus.ar_len = mem_req.arlen;
        assign mem_bus.ar_size = mem_req.arsize;
        assign mem_bus.ar_burst = mem_req.arburst;
        assign mem_bus.ar_lock = mem_req.arlock;
        assign mem_bus.ar_cache = mem_req.arcache;
        assign mem_bus.ar_prot = mem_req.arprot;
        assign mem_bus.ar_qos = mem_req.arqos;
        assign mem_bus.ar_region = mem_req.arregion;
        assign mem_bus.ar_valid = mem_req.arvalid;
        assign mem_bus.aw_user = '0;
        assign mem_bus.aw_atop = '0;
        assign mem_bus.w_user = '0;
        assign mem_bus.ar_user = '0;
        assign mem_bus.b_ready = mem_req.bready;
        assign mem_bus.r_ready = mem_req.rready;
        assign mem_rsp.awready = mem_bus.aw_ready;
        assign mem_rsp.wready = mem_bus.w_ready;
        assign mem_rsp.arready = mem_bus.ar_ready;
        assign mem_rsp.bid = mem_bus.b_id;
        assign mem_rsp.bresp = mem_bus.b_resp;
        assign mem_rsp.bvalid = mem_bus.b_valid;
        assign mem_rsp.rid = mem_bus.r_id;
        assign mem_rsp.rdata = mem_bus.r_data;
        assign mem_rsp.rresp = mem_bus.r_resp;
        assign mem_rsp.rlast = mem_bus.r_last;
        assign mem_rsp.rvalid = mem_bus.r_valid;
        axi_sim_mem_intf #(
            .AXI_ADDR_WIDTH     (AXI_ADDR_WIDTH),
            .AXI_DATA_WIDTH     (AXI_DATA_WIDTH),
            .AXI_ID_WIDTH       (NSU_AXI_ID_WIDTH),
            .AXI_USER_WIDTH     (AXI_AWUSER_WIDTH),
            .WARN_UNINITIALIZED (1'b1),
            .UNINITIALIZED_DATA ("undefined"),
            .APPL_DELAY         (1ns),
            .ACQ_DELAY          (2ns)
        ) i_memory (
            .clk_i              (clk),
            .rst_ni             (axi_rst_n),
            .axi_slv            (delayed_bus),
            .mon_w_valid_o      (),
            .mon_w_addr_o       (),
            .mon_w_data_o       (),
            .mon_w_id_o         (),
            .mon_w_user_o       (),
            .mon_w_beat_count_o (),
            .mon_w_last_o       (),
            .mon_r_valid_o      (),
            .mon_r_addr_o       (),
            .mon_r_data_o       (),
            .mon_r_id_o         (),
            .mon_r_user_o       (),
            .mon_r_beat_count_o (),
            .mon_r_last_o       ()
        );
    end
    assign rx_rsp_valid[NMU_PORT] = 1'b0;
    assign rx_rsp_flit[NMU_PORT]  = '0;
    assign tx_req_ready[NMU_PORT] = 1'b0;
    for (genvar port = 1; port < NUM_PORTS; port++) begin : gen_tieoff
        assign rx_req_valid[port] = 1'b0;
        assign rx_req_flit[port]  = '0;
        assign tx_rsp_ready[port] = 1'b0;
        always @(posedge clk) begin
            if (noc_rst_n && tx_rsp_valid[port])
                $fatal(1, "Response routed away from LOCAL");
        end
    end
    always @(posedge clk) begin
        if (noc_rst_n && tx_req_valid[NMU_PORT])
            $fatal(1, "Request routed back to LOCAL");
    end
    typedef axi_test::axi_file_master #(
        .AW (AXI_ADDR_WIDTH),
        .DW (AXI_DATA_WIDTH),
        .IW (AXI_ID_WIDTH),
        .UW (AXI_AWUSER_WIDTH),
        .TA (1ns),
        .TT (2ns)
    ) master_t;
    typedef axi_test::axi_scoreboard #(
        .AW (AXI_ADDR_WIDTH),
        .DW (AXI_DATA_WIDTH),
        .IW (AXI_ID_WIDTH),
        .UW (AXI_AWUSER_WIDTH),
        .TT (2ns)
    ) scoreboard_t;
    import "DPI-C" context function int cmodel_check_error(output string message);
    always @(negedge clk) begin : check_model_error
        string message;
        if (noc_rst_n && cmodel_check_error(message) != 0)
            $fatal(1, "C++ model error: %s", message);
    end
    int b_count = 0, r_count = 0, r_beats = 0, checked_bytes = 0;
    int expected_writes, expected_reads, expected_beats;
    int live_w[2**AXI_ID_WIDTH] = '{default:0};
    int live_r[2**AXI_ID_WIDTH] = '{default:0};
    int peak_w = 0, peak_r = 0, peak_unique_w = 0, peak_unique_r = 0;
    int min_outstanding = 1, min_unique = 1;
    master_t::ax_beat_t expected_ar[2**AXI_ID_WIDTH][$];
    int read_beat[2**AXI_ID_WIDTH] = '{default:0};
    master_t master, init_master, verify_master;
    scoreboard_t scoreboard;
    int init_phase = 0, concurrent_rw = 0, stall_cycles = 0, hold_cycles = 0;
    int capacity_test = 0, data_case = 0;
    int b_stall_cnt = 0, r_stall_cnt = 0, aw_stall_cnt = 0, ar_stall_cnt = 0;
    int tx_req_peak = 0;
    int tx_dat_peak[NUM_DAT_VC] = '{default:0};
    int tx_req_beats = 0, tx_dat_beats = 0;
    always @(posedge clk) begin
        if (noc_rst_n) begin
            if (int'(dut.i_request_path.i_tx_buffer.i_req_fifo.usage_o) > tx_req_peak)
                tx_req_peak = int'(dut.i_request_path.i_tx_buffer.i_req_fifo.usage_o);
            if (rx_req_valid[NMU_PORT] && rx_req_ready[NMU_PORT]) tx_req_beats++;
            if (rx_dat_valid[NMU_PORT]) tx_dat_beats++;
        end
    end
    for (genvar vc = 0; vc < NUM_DAT_VC; vc++) begin : gen_tx_occupancy
        if (NOC_DAT_VC_MODE == 0 || vc < NUM_DAT_VC/2) begin : gen_write
            always @(posedge clk) begin
                if (noc_rst_n && int'(dut.i_request_path.i_tx_buffer.gen_dat_vc[vc].gen_write.i_fifo.usage_o) > tx_dat_peak[vc])
                    tx_dat_peak[vc] = int'(dut.i_request_path.i_tx_buffer.gen_dat_vc[vc].gen_write.i_fifo.usage_o);
            end
        end
    end
    int b_full_cnt = 0, r_full_cnt = 0, dat_full_cnt = 0;
    int wr_limit_cnt = 0, rd_limit_cnt = 0;
    int overlap_cnt = 0, w_during_read_cnt = 0, r_during_write_cnt = 0;
    bit concurrent_active = 0;

    typedef struct packed {
        int id;
        int tag;
        int dst;
        int seq;
        bit reorder;
    } order_txn_t;
    order_txn_t pending_b[$], pending_r[$];
    order_txn_t expected_b[2**NOC_ID_WIDTH][$];
    bit b_arrived[int];
    int issue_b_cnt = 0, issue_r_cnt = 0;
    int ingress_b_ooo = 0, ingress_r_ooo = 0;
    int ingress_b_same_id_ooo = 0, ingress_r_same_id_ooo = 0;
    int buffered_b_cnt = 0, buffered_r_cnt = 0;
    ni_types_pkg::nmu_aw_request_t issued_aw;
    ni_types_pkg::nmu_ar_request_t issued_ar;
    ni_flit_pkg::rsp_flit_t ingress_rsp;
    ni_flit_pkg::dat_flit_t ingress_dat;
    assign issued_aw = dut.i_response_path.i_ordering.m_aw_o;
    assign issued_ar = dut.i_response_path.i_ordering.m_ar_o;
    assign ingress_rsp = tx_rsp_flit[NMU_PORT];
    assign ingress_dat = tx_dat_flit[NMU_PORT];

    task automatic check_arrival(input bit is_read, input int id, tag, dst,
                                 input bit reorder, last);
        order_txn_t txn;
        int found;
        found = -1;
        if (is_read) begin
            foreach (pending_r[i]) begin
                if (found < 0 && pending_r[i].id == id && pending_r[i].dst == dst &&
                    pending_r[i].reorder == reorder && (!reorder || pending_r[i].tag == tag))
                    found = i;
            end
            if (found < 0) $fatal(1, "Unmatched R at NMU ingress");
            if (last) begin
                if (found != 0) ingress_r_ooo++;
                for (int i = 0; i < found; i++) begin
                    if (pending_r[i].id == id) begin
                        ingress_r_same_id_ooo++;
                        break;
                    end
                end
                txn = pending_r[found];
                pending_r.delete(found);
            end
        end else begin
            foreach (pending_b[i]) begin
                if (found < 0 && pending_b[i].id == id && pending_b[i].dst == dst &&
                    pending_b[i].reorder == reorder && (!reorder || pending_b[i].tag == tag))
                    found = i;
            end
            if (found < 0) $fatal(1, "Unmatched B at NMU ingress");
            if (found != 0) ingress_b_ooo++;
            for (int i = 0; i < found; i++) begin
                if (pending_b[i].id == id) begin
                    ingress_b_same_id_ooo++;
                    break;
                end
            end
            txn = pending_b[found];
            b_arrived[txn.seq] = 1'b1;
            pending_b.delete(found);
        end
        if (last) $display("INGRESS_ORDER channel=%s seq=%0d id=%0d src=%0h tag=%0d time=%0t",
            is_read ? "R" : "B", txn.seq, id, dst, tag, $time);
    endtask

    always @(posedge clk) begin : check_order
        order_txn_t txn;
        int ch, id, tag, dst;
        bit reorder, last;
        if (noc_rst_n && reorder_test != 0) begin
            if (dut.i_response_path.i_ordering.m_aw_valid_o && dut.i_response_path.i_ordering.m_aw_ready_i) begin
                txn = '{int'(issued_aw.axi.awid), int'(issued_aw.meta.ordering_tag),
                    int'(issued_aw.meta.route.domain.dst_id), issue_b_cnt++, issued_aw.meta.ordering_req};
                pending_b.push_back(txn);
                expected_b[txn.id].push_back(txn);
            end
            if (dut.i_response_path.i_ordering.m_ar_valid_o && dut.i_response_path.i_ordering.m_ar_ready_i) begin
                txn = '{int'(issued_ar.axi.arid), int'(issued_ar.meta.ordering_tag),
                    int'(issued_ar.meta.route.domain.dst_id), issue_r_cnt++, issued_ar.meta.ordering_req};
                pending_r.push_back(txn);
            end
            if (tx_rsp_valid[NMU_PORT] && tx_rsp_ready[NMU_PORT]) begin
                ch = ingress_rsp.header[ni_flit_pkg::AXI_CH_LSB +: ni_flit_pkg::AXI_CH_WIDTH];
                reorder = ingress_rsp.header[ni_flit_pkg::ORDERING_REQ_LSB];
                tag = ingress_rsp.header[ni_flit_pkg::ORDERING_TAG_LSB +: ni_flit_pkg::ORDERING_TAG_WIDTH];
                dst = ingress_rsp.header[ni_flit_pkg::SRC_ID_LSB +: ni_flit_pkg::SRC_ID_WIDTH];
                if (ch == ni_flit_pkg::AXI_CH_NarrowR) begin
                    id = ingress_rsp.payload[ni_flit_pkg::NARROW_R_RID_LSB +: ni_flit_pkg::NARROW_R_RID_WIDTH];
                    last = ingress_rsp.payload[ni_flit_pkg::NARROW_R_RLAST_LSB];
                    check_arrival(1'b1, id, tag, dst, reorder, last);
                end else begin
                    id = ingress_rsp.payload[ni_flit_pkg::B_BID_LSB +: ni_flit_pkg::B_BID_WIDTH];
                    check_arrival(1'b0, id, tag, dst, reorder, 1'b1);
                end
            end
            if (tx_dat_valid[NMU_PORT]) begin
                id = ingress_dat.payload[ni_flit_pkg::DATA_R_RID_LSB +: ni_flit_pkg::DATA_R_RID_WIDTH];
                tag = ingress_dat.header[ni_flit_pkg::ORDERING_TAG_LSB +: ni_flit_pkg::ORDERING_TAG_WIDTH];
                dst = ingress_dat.header[ni_flit_pkg::SRC_ID_LSB +: ni_flit_pkg::SRC_ID_WIDTH];
                reorder = ingress_dat.header[ni_flit_pkg::ORDERING_REQ_LSB];
                last = ingress_dat.payload[ni_flit_pkg::DATA_R_RLAST_LSB];
                check_arrival(1'b1, id, tag, dst, reorder, last);
            end
            if (dut.i_response_path.i_ordering.b_retire) begin
                id = dut.i_response_path.i_ordering.m_b_o.bid;
                if (expected_b[id].size() == 0) $fatal(1, "Unsolicited B retirement");
                txn = expected_b[id].pop_front();
                if (!b_arrived.exists(txn.seq)) $fatal(1, "B retired before its response arrived");
                if (dut.i_response_path.i_ordering.b_direct) begin
                    if (txn.reorder != dut.i_response_path.i_ordering.s_b_i.meta.ordering_req ||
                        (txn.reorder && txn.tag != dut.i_response_path.i_ordering.s_b_i.meta.ordering_tag))
                        $fatal(1, "B direct retirement order mismatch");
                end else begin
                    if (!txn.reorder || txn.tag != dut.i_response_path.i_ordering.b_storage_rd_addr)
                        $fatal(1, "B buffered retirement order mismatch");
                    buffered_b_cnt++;
                end
                b_arrived.delete(txn.seq);
            end
            if (dut.i_response_path.i_ordering.r_retire && !dut.i_response_path.i_ordering.r_direct)
                buffered_r_cnt++;
        end
    end

    function automatic void expect_reads(input master_t source);
        foreach (source.ar_queue[i]) begin
            expected_ar[source.ar_queue[i].ax_id].push_back(source.ar_queue[i]);
            expected_beats += int'(source.ar_queue[i].ax_len) + 1;
        end
    endfunction

    task automatic receive_b();
        master_t::b_beat_t beat;
        if (hold_cycles != 0) begin
            wait (vip.b_valid);
            repeat (hold_cycles) @(posedge clk);
        end
        if (stall_cycles == 0) begin
            master.wait_b();
        end else begin
            while (master.b_outst.size() != 0) begin
                wait (vip.b_valid);
                repeat (stall_cycles) @(posedge clk);
                master.drv.recv_b(beat);
                void'(master.b_outst.pop_front());
            end
        end
    endtask

    task automatic receive_r();
        master_t::r_beat_t beat;
        if (hold_cycles != 0) begin
            wait (vip.r_valid);
            repeat (hold_cycles) @(posedge clk);
        end
        if (stall_cycles == 0) begin
            master.wait_r();
        end else begin
            while (master.r_outst.size() != 0) begin
                wait (vip.r_valid);
                repeat (stall_cycles) @(posedge clk);
                master.drv.recv_r(beat);
                if (beat.r_last) void'(master.r_outst.pop_front());
            end
        end
    endtask

    assert property (@(posedge clk) disable iff (!axi_rst_n)
        bus.bvalid && !bus.bready |=> bus.bvalid && $stable({bus.bid, bus.bresp}))
        else $fatal(1, "B response changed under backpressure");
    assert property (@(posedge clk) disable iff (!axi_rst_n)
        bus.rvalid && !bus.rready |=> bus.rvalid &&
        $stable({bus.rid, bus.rdata, bus.rresp, bus.rlast}))
        else $fatal(1, "R response changed under backpressure");
    import "DPI-C" context function void cmodel_init();
    import "DPI-C" context function void cmodel_finalize();
    import "DPI-C" context function longint unsigned cmodel_router_create(
        input string name, input int x, y, mesh_x, mesh_y, num_vc);
    import "DPI-C" context function longint unsigned cmodel_nsu_create(
        input string name, input int src_id, num_vc, max_ids, max_outstanding,
        port_id, input string config_path);
    import "DPI-C" context function void cmodel_nsu_set_dat_credit_depth(
        input longint unsigned ctx, input int depth);
    initial begin : run
        string stim_dir;
        cmodel_init();
        router_ctx = cmodel_router_create("router", ROUTER_X, ROUTER_Y,
            MESH_DIM, MESH_DIM, NUM_DAT_VC);
        for (int n = 0; n < NUM_NSUS; n++) begin
            nsu_ctx[n] = cmodel_nsu_create($sformatf("nsu_%0d", n + 1), nsu_id(n + 1),
                NUM_DAT_VC, NSU_META_BUFFER_MAX_UNIQUE_IDS,
                NSU_META_BUFFER_MAX_OUTSTANDING, 0, "");
            cmodel_nsu_set_dat_credit_depth(nsu_ctx[n], NOC_ROUTER_VC_DEPTH);
        end
        void'($value$plusargs("reorder_test=%d", reorder_test));
        $display("RESPONSE_DELAY enabled=%0d west_setting=%0d", reorder_test != 0, RSP_DELAY_CYCLES);
        $display("DAT_CREDIT_DEPTH router=%0d nmu_rx=%0d nsu_rx=%0d",
            NOC_ROUTER_VC_DEPTH, NOC_ROUTER_VC_DEPTH, NOC_NI_DAT_RX_VC_DEPTH);
        master = new(vip);
        scoreboard = new(vip);
        if (!$value$plusargs("stim_dir=%s", stim_dir)) $fatal(1, "Missing stim_dir");
        master.load_files({stim_dir, "/read.txt"}, {stim_dir, "/write.txt"});
        corrupt_rsp = $test$plusargs("corrupt_rsp");
        void'($value$plusargs("min_outstanding=%d", min_outstanding));
        void'($value$plusargs("min_unique=%d", min_unique));
        expected_writes = master.num_writes;
        expected_reads = master.num_reads;
        expected_beats = 0;
        expect_reads(master);
        void'($value$plusargs("init_phase=%d", init_phase));
        void'($value$plusargs("concurrent_rw=%d", concurrent_rw));
        void'($value$plusargs("stall_cycles=%d", stall_cycles));
        void'($value$plusargs("hold_cycles=%d", hold_cycles));
        void'($value$plusargs("capacity_test=%d", capacity_test));
        void'($value$plusargs("data_case=%d", data_case));
        if (init_phase) begin
            init_master = new(vip);
            init_master.write_fd = $fopen({stim_dir, "/init_write.txt"}, "r");
            if (!init_master.write_fd) $fatal(1, "Missing initialization writes");
            init_master.parse_write();
            $fclose(init_master.write_fd);
            init_master.num_writes = init_master.aw_queue.size();
            expected_writes += init_master.num_writes;
        end
        if (concurrent_rw) begin
            verify_master = new(vip);
            verify_master.read_fd = $fopen({stim_dir, "/verify_read.txt"}, "r");
            if (!verify_master.read_fd) $fatal(1, "Missing verification reads");
            verify_master.parse_read();
            $fclose(verify_master.read_fd);
            verify_master.num_reads = verify_master.ar_queue.size();
            expected_reads += verify_master.num_reads;
            expect_reads(verify_master);
        end
        if (expected_writes == 0 || expected_reads == 0)
            $fatal(1, "Empty memory test");
        repeat (5) @(negedge clk);
        rst_n = 1;
        wait (axi_rst_n && noc_rst_n);
        @(posedge clk);
        scoreboard.enable_all_checks();
        scoreboard.monitor();
        if (init_phase) begin
            fork init_master.run_aw(); init_master.run_w(); init_master.wait_b(); join
        end
        if (concurrent_rw) begin
            concurrent_active = 1'b1;
            master.run();
            concurrent_active = 1'b0;
            fork verify_master.run_ar(); verify_master.wait_r(); join
        end else begin
            fork master.run_aw(); master.run_w(); receive_b(); join
            fork master.run_ar(); receive_r(); join
        end
        repeat (10) @(posedge clk);
        if (b_count != expected_writes || r_count != expected_reads ||
            r_beats != expected_beats || checked_bytes == 0)
            $fatal(1, "Transaction count mismatch");
        foreach (expected_ar[id])
            if (expected_ar[id].size() != 0) $fatal(1, "Unreturned read ID %0d", id);
        if (peak_w < min_outstanding || peak_r < min_outstanding ||
            peak_unique_w < min_unique || peak_unique_r < min_unique)
            $fatal(1, "Outstanding/ID coverage not reached");
        $display("TX_BUFFER req_peak=%0d req_beats=%0d dat_beats=%0d", tx_req_peak, tx_req_beats, tx_dat_beats);
        for (int vc = 0; vc < NUM_DAT_VC; vc++) $display("TX_DAT_BUFFER vc=%0d peak=%0d", vc, tx_dat_peak[vc]);
        $display("COVERAGE peak_w=%0d peak_r=%0d unique_w=%0d unique_r=%0d",
            peak_w, peak_r, peak_unique_w, peak_unique_r);
        $display("STALL b=%0d r=%0d aw=%0d ar=%0d", b_stall_cnt, r_stall_cnt, aw_stall_cnt, ar_stall_cnt);
        $display("CAPACITY b_full=%0d r_full=%0d dat_full=%0d wr_limit=%0d rd_limit=%0d",
            b_full_cnt, r_full_cnt, dat_full_cnt, wr_limit_cnt, rd_limit_cnt);
        $display("CONCURRENT live=%0d w_during_read=%0d r_during_write=%0d",
            overlap_cnt, w_during_read_cnt, r_during_write_cnt);
        if ((stall_cycles != 0 || hold_cycles != 0) &&
                (b_stall_cnt == 0 || r_stall_cnt == 0))
            $fatal(1, "Response backpressure was not exercised");
        if (capacity_test && (b_full_cnt == 0 || wr_limit_cnt == 0 || rd_limit_cnt == 0 ||
                aw_stall_cnt == 0 || ar_stall_cnt == 0 ||
                (data_case ? dat_full_cnt == 0 : r_full_cnt == 0)))
            $fatal(1, "Required capacity saturation was not reached");
        if (concurrent_rw && (overlap_cnt == 0 || w_during_read_cnt == 0 || r_during_write_cnt == 0))
            $fatal(1, "Read/write concurrency was not exercised");
        $display("REORDER_COVERAGE b_ooo=%0d r_ooo=%0d b_same_id=%0d r_same_id=%0d b_buffered=%0d r_buffered=%0d",
            ingress_b_ooo, ingress_r_ooo, ingress_b_same_id_ooo, ingress_r_same_id_ooo,
            buffered_b_cnt, buffered_r_cnt);
        for (int n = 0; n < NUM_NSUS; n++) begin
            $display("DESTINATION port=%0d writes=%0d reads=%0d", n + 1, dst_wr_cnt[n], dst_rd_cnt[n]);
            if (reorder_test != 0 && (dst_wr_cnt[n] == 0 || dst_rd_cnt[n] == 0))
                $fatal(1, "Ordering case did not exercise every destination");
        end
        if (reorder_test != 0 && (pending_b.size() != 0 || pending_r.size() != 0))
            $fatal(1, "Pending ingress responses remain");
        if (reorder_test != 0 && (ingress_b_ooo == 0 || ingress_r_ooo == 0))
            $fatal(1, "Required response disorder was not reached");
        if (reorder_test == 2 && (ingress_b_same_id_ooo == 0 || ingress_r_same_id_ooo == 0 ||
                buffered_b_cnt == 0 || buffered_r_cnt == 0))
            $fatal(1, "Required same-ID reordering was not reached");
        scoreboard.reset();
        $display("NMU_COSIM_COUNTS writes=%0d reads=%0d r_beats=%0d checked_bytes=%0d",
            b_count, r_count, r_beats, checked_bytes);
        cmodel_finalize();
        $finish;
    end
    initial begin
        repeat (100000) @(posedge clk);
        $fatal(1, "NMU co-simulation timeout");
    end
    always @(posedge clk) begin : check_responses
        master_t::ax_beat_t request;
        logic [AXI_ADDR_WIDTH-1:0] address;
        logic [7:0] expected_byte;
        int lane, total_w, total_r, unique_w, unique_r;
        #2ns;
        if (axi_rst_n && vip.aw_valid && vip.aw_ready) live_w[vip.aw_id]++;
        if (axi_rst_n && vip.ar_valid && vip.ar_ready) live_r[vip.ar_id]++;
        if (axi_rst_n && vip.b_valid && vip.b_ready) begin
            if ($isunknown(vip.b_id) || vip.b_resp !== axi_pkg::RESP_OKAY)
                $fatal(1, "Invalid B response");
            if (live_w[vip.b_id] == 0) $fatal(1, "Unsolicited B response");
            live_w[vip.b_id]--;
            b_count++;
        end
        if (axi_rst_n && vip.r_valid && vip.r_ready) begin
            if ($isunknown(vip.r_id) || expected_ar[vip.r_id].size() == 0)
                $fatal(1, "Unexpected read ID");
            request = expected_ar[vip.r_id][0];
            if (vip.r_resp !== axi_pkg::RESP_OKAY ||
                vip.r_last !== (read_beat[vip.r_id] == int'(request.ax_len)))
                $fatal(1, "Invalid RRESP/RLAST");
            address = request.ax_addr + AXI_ADDR_WIDTH'(read_beat[vip.r_id] << request.ax_size);
            for (int byte_idx = 0; byte_idx < (1 << request.ax_size); byte_idx++) begin
                lane = int'(address % (AXI_DATA_WIDTH/8)) + byte_idx;
                scoreboard.get_byte(address + AXI_ADDR_WIDTH'(byte_idx), expected_byte);
                if ($isunknown(expected_byte) || $isunknown(vip.r_data[lane*8 +: 8]))
                    $fatal(1, "Read comparison contains uninitialized data");
                checked_bytes++;
            end
            r_beats++;
            if (vip.r_last) begin
                void'(expected_ar[vip.r_id].pop_front());
                read_beat[vip.r_id] = 0;
                if (live_r[vip.r_id] == 0) $fatal(1, "Unsolicited R response");
                live_r[vip.r_id]--;
                r_count++;
            end else read_beat[vip.r_id]++;
        end
        total_w = 0; total_r = 0; unique_w = 0; unique_r = 0;
        foreach (live_w[id]) begin
            total_w += live_w[id]; total_r += live_r[id];
            if (live_w[id] != 0) unique_w++;
            if (live_r[id] != 0) unique_r++;
        end
        if (axi_rst_n) begin
            if (vip.b_valid && !vip.b_ready) b_stall_cnt++;
            if (vip.r_valid && !vip.r_ready) r_stall_cnt++;
            if (vip.aw_valid && !vip.aw_ready) aw_stall_cnt++;
            if (vip.ar_valid && !vip.ar_ready) ar_stall_cnt++;
            if ((dut.i_response_path.i_rx_buffer.rsp_full && dut.i_response_path.i_rx_channel_assign.is_b)) b_full_cnt++;
            if ((dut.i_response_path.i_rx_buffer.rsp_full && dut.i_response_path.i_rx_channel_assign.is_r)) r_full_cnt++;
            if (|dut.i_response_path.i_rx_buffer.dat_full) dat_full_cnt++;
            if (dut.i_request_path.i_id_remap.wr_exists_full) wr_limit_cnt++;
            if (dut.i_request_path.i_id_remap.rd_exists_full) rd_limit_cnt++;
            if (concurrent_active) begin
                if (total_w != 0 && total_r != 0) overlap_cnt++;
                if (total_r != 0 && vip.w_valid && vip.w_ready) w_during_read_cnt++;
                if (total_w != 0 && vip.r_valid && vip.r_ready) r_during_write_cnt++;
            end
        end
        if (total_w > peak_w) peak_w = total_w;
        if (total_r > peak_r) peak_r = total_r;
        if (unique_w > peak_unique_w) peak_unique_w = unique_w;
        if (unique_r > peak_unique_r) peak_unique_r = unique_r;
    end
`ifdef DUMP_WAVE
    initial begin : dump_wave
        string wave_file;
        if (!$value$plusargs("wave_file=%s", wave_file)) wave_file = "nmu_cosim.fsdb";
        $fsdbDumpfile(wave_file);
        $fsdbDumpvars(0, tb_nmu_cosim, "+all");
    end
`endif
    int perf_cycle = 0, perf_start = -1, perf_end = -1;
    int wr_id_stall = 0, rd_id_stall = 0, wr_txn_stall = 0, rd_txn_stall = 0;
    always @(posedge clk) begin
        if (axi_rst_n) begin
            perf_cycle++;
            if (perf_start < 0 && ((vip.aw_valid && vip.aw_ready) || (vip.ar_valid && vip.ar_ready)))
                perf_start = perf_cycle;
            if ((vip.b_valid && vip.b_ready) || (vip.r_valid && vip.r_ready && vip.r_last))
                perf_end = perf_cycle;
            if (vip.aw_valid && !vip.aw_ready && !dut.i_request_path.i_id_remap.aw_hold_reg) begin
                if (!dut.i_request_path.i_id_remap.wr_exists && dut.i_request_path.i_id_remap.wr_full) wr_id_stall++;
                if (dut.i_request_path.i_id_remap.wr_exists_full) wr_txn_stall++;
            end
            if (vip.ar_valid && !vip.ar_ready && !dut.i_request_path.i_id_remap.ar_hold_reg) begin
                if (!dut.i_request_path.i_id_remap.rd_exists && dut.i_request_path.i_id_remap.rd_full) rd_id_stall++;
                if (dut.i_request_path.i_id_remap.rd_exists_full) rd_txn_stall++;
            end
        end
    end
    final begin
        $display("CAPACITY_PERF active_ids=%0d per_id=%0d cycles=%0d wr_id_stall=%0d rd_id_stall=%0d wr_txn_stall=%0d rd_txn_stall=%0d",
            MAX_ACTIVE_IDS, MAX_OUTSTANDING_PER_ID, perf_end-perf_start+1,
            wr_id_stall, rd_id_stall, wr_txn_stall, rd_txn_stall);
    end
endmodule
