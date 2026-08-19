`timescale 1ns / 1ps

`include "axi/typedef.svh"

module axi_id_remap_case #(
    parameter int unsigned AXI_ID_WIDTH = 3,
    parameter int unsigned NOC_ID_WIDTH = 3
) (
    input  wire logic clk_i,
    input  wire logic rst_ni,
    output wire logic done_o
);

    localparam int unsigned MAX_UNIQ_IDS =
        1 << ((AXI_ID_WIDTH < NOC_ID_WIDTH) ? AXI_ID_WIDTH : NOC_ID_WIDTH);

    typedef logic [AXI_ID_WIDTH-1:0] axi_id_t;
    typedef logic [NOC_ID_WIDTH-1:0] noc_id_t;
    typedef logic [31:0]             axi_addr_t;
    typedef logic [31:0]             axi_data_t;
    typedef logic [3:0]              axi_strb_t;
    typedef logic                    axi_user_t;

    `AXI_TYPEDEF_AW_CHAN_T(slv_aw_t, axi_addr_t, axi_id_t, axi_user_t)
    `AXI_TYPEDEF_W_CHAN_T(slv_w_t, axi_data_t, axi_strb_t, axi_user_t)
    `AXI_TYPEDEF_B_CHAN_T(slv_b_t, axi_id_t, axi_user_t)
    `AXI_TYPEDEF_AR_CHAN_T(slv_ar_t, axi_addr_t, axi_id_t, axi_user_t)
    `AXI_TYPEDEF_R_CHAN_T(slv_r_t, axi_data_t, axi_id_t, axi_user_t)
    `AXI_TYPEDEF_REQ_T(slv_req_t, slv_aw_t, slv_w_t, slv_ar_t)
    `AXI_TYPEDEF_RESP_T(slv_rsp_t, slv_b_t, slv_r_t)

    `AXI_TYPEDEF_AW_CHAN_T(mst_aw_t, axi_addr_t, noc_id_t, axi_user_t)
    `AXI_TYPEDEF_W_CHAN_T(mst_w_t, axi_data_t, axi_strb_t, axi_user_t)
    `AXI_TYPEDEF_B_CHAN_T(mst_b_t, noc_id_t, axi_user_t)
    `AXI_TYPEDEF_AR_CHAN_T(mst_ar_t, axi_addr_t, noc_id_t, axi_user_t)
    `AXI_TYPEDEF_R_CHAN_T(mst_r_t, axi_data_t, noc_id_t, axi_user_t)
    `AXI_TYPEDEF_REQ_T(mst_req_t, mst_aw_t, mst_w_t, mst_ar_t)
    `AXI_TYPEDEF_RESP_T(mst_rsp_t, mst_b_t, mst_r_t)

    slv_req_t slv_req = '0;
    slv_rsp_t slv_rsp;
    mst_req_t mst_req;
    mst_rsp_t mst_rsp = '0;
    logic done_reg = 1'b0;

    assign done_o = done_reg;

    axi_id_remap #(
        .AxiSlvPortIdWidth    (AXI_ID_WIDTH),
        .AxiSlvPortMaxUniqIds (MAX_UNIQ_IDS),
        .AxiMaxTxnsPerId      (4),
        .AxiMstPortIdWidth    (NOC_ID_WIDTH),
        .slv_req_t            (slv_req_t),
        .slv_resp_t           (slv_rsp_t),
        .mst_req_t            (mst_req_t),
        .mst_resp_t           (mst_rsp_t)
    ) dut (
        .clk_i,
        .rst_ni,
        .slv_req_i  (slv_req),
        .slv_resp_o (slv_rsp),
        .mst_req_o  (mst_req),
        .mst_resp_i (mst_rsp)
    );

    task automatic issue_aw(input axi_id_t axi_id, output noc_id_t noc_id);
        @(negedge clk_i);
        slv_req.aw.id = axi_id;
        slv_req.aw_valid = 1'b1;
        @(posedge clk_i);
        #1;
        if (!mst_req.aw_valid || !slv_rsp.aw_ready) begin
            $fatal(1, "AXI_ID_WIDTH=%0d: AW %0h was unexpectedly backpressured", AXI_ID_WIDTH,
                   axi_id);
        end
        noc_id = mst_req.aw.id;
        @(negedge clk_i);
        slv_req.aw_valid = 1'b0;
    endtask

    task automatic return_b(input axi_id_t axi_id, input noc_id_t noc_id);
        @(negedge clk_i);
        mst_rsp.b.id = noc_id;
        mst_rsp.b_valid = 1'b1;
        @(posedge clk_i);
        #1;
        if (!slv_rsp.b_valid || slv_rsp.b.id != axi_id) begin
            $fatal(1, "AXI_ID_WIDTH=%0d: B restore failed: expected %0h, got %0h",
                   AXI_ID_WIDTH, axi_id, slv_rsp.b.id);
        end
        @(negedge clk_i);
        mst_rsp.b_valid = 1'b0;
    endtask

    task automatic issue_ar(input axi_id_t axi_id, output noc_id_t noc_id);
        @(negedge clk_i);
        slv_req.ar.id = axi_id;
        slv_req.ar_valid = 1'b1;
        @(posedge clk_i);
        #1;
        if (!mst_req.ar_valid || !slv_rsp.ar_ready) begin
            $fatal(1, "AXI_ID_WIDTH=%0d: AR %0h was unexpectedly backpressured", AXI_ID_WIDTH,
                   axi_id);
        end
        noc_id = mst_req.ar.id;
        @(negedge clk_i);
        slv_req.ar_valid = 1'b0;
    endtask

    task automatic return_r(input axi_id_t axi_id, input noc_id_t noc_id);
        @(negedge clk_i);
        mst_rsp.r.id = noc_id;
        mst_rsp.r.last = 1'b1;
        mst_rsp.r_valid = 1'b1;
        @(posedge clk_i);
        #1;
        if (!slv_rsp.r_valid || slv_rsp.r.id != axi_id) begin
            $fatal(1, "AXI_ID_WIDTH=%0d: R restore failed: expected %0h, got %0h",
                   AXI_ID_WIDTH, axi_id, slv_rsp.r.id);
        end
        @(negedge clk_i);
        mst_rsp.r_valid = 1'b0;
    endtask

    initial begin : run_case
        axi_id_t axi_id;
        noc_id_t noc_id;
        noc_id_t allocated_wr_ids [0:7];
        noc_id_t allocated_rd_ids [0:7];

        if (AXI_ID_WIDTH < 1 || AXI_ID_WIDTH > 8 || NOC_ID_WIDTH != 3) begin
            $fatal(1, "illegal AXI/NoC ID-width contract");
        end

        slv_req.b_ready = 1'b1;
        slv_req.r_ready = 1'b1;
        mst_rsp.aw_ready = 1'b1;
        mst_rsp.w_ready = 1'b1;
        mst_rsp.ar_ready = 1'b1;

        wait (rst_ni);
        issue_aw(AXI_ID_WIDTH'(1), noc_id);
        return_b(AXI_ID_WIDTH'(1), noc_id);
        issue_ar(AXI_ID_WIDTH'(1), noc_id);
        return_r(AXI_ID_WIDTH'(1), noc_id);

        if (AXI_ID_WIDTH == 8) begin
            for (int unsigned index = 0; index < 8; index++) begin
                issue_aw(AXI_ID_WIDTH'(index), allocated_wr_ids[index]);
                if (allocated_wr_ids[index] != NOC_ID_WIDTH'(index)) begin
                    $fatal(1, "duplicate or non-lowest write NoC ID allocation at %0d", index);
                end
            end

            @(negedge clk_i);
            slv_req.aw.id = AXI_ID_WIDTH'(8);
            slv_req.aw_valid = 1'b1;
            @(posedge clk_i);
            #1;
            if (mst_req.aw_valid || slv_rsp.aw_ready) begin
                $fatal(1, "NoC ID exhaustion did not backpressure a new AXI ID");
            end

            @(negedge clk_i);
            mst_rsp.b.id = allocated_wr_ids[0];
            mst_rsp.b_valid = 1'b1;
            @(posedge clk_i);
            #1;
            if (!slv_rsp.b_valid || slv_rsp.b.id != AXI_ID_WIDTH'(0)) begin
                $fatal(1, "exhaustion response did not restore AXI ID 0");
            end
            @(negedge clk_i);
            mst_rsp.b_valid = 1'b0;
            @(posedge clk_i);
            #1;
            if (!mst_req.aw_valid || mst_req.aw.id != NOC_ID_WIDTH'(0)) begin
                $fatal(1, "released NoC ID was not reused for blocked AXI ID");
            end
            @(negedge clk_i);
            slv_req.aw_valid = 1'b0;

            for (int unsigned index = 0; index < 8; index++) begin
                issue_ar(AXI_ID_WIDTH'(index), allocated_rd_ids[index]);
                if (allocated_rd_ids[index] != NOC_ID_WIDTH'(index)) begin
                    $fatal(1, "duplicate or non-lowest read NoC ID allocation at %0d", index);
                end
            end

            @(negedge clk_i);
            slv_req.ar.id = AXI_ID_WIDTH'(8);
            slv_req.ar_valid = 1'b1;
            @(posedge clk_i);
            #1;
            if (mst_req.ar_valid || slv_rsp.ar_ready) begin
                $fatal(1, "NoC ID exhaustion did not backpressure a new read AXI ID");
            end

            @(negedge clk_i);
            mst_rsp.r.id = allocated_rd_ids[0];
            mst_rsp.r.last = 1'b1;
            mst_rsp.r_valid = 1'b1;
            @(posedge clk_i);
            #1;
            if (!slv_rsp.r_valid || slv_rsp.r.id != AXI_ID_WIDTH'(0)) begin
                $fatal(1, "read exhaustion response did not restore AXI ID 0");
            end
            @(negedge clk_i);
            mst_rsp.r_valid = 1'b0;
            @(posedge clk_i);
            #1;
            if (!mst_req.ar_valid || mst_req.ar.id != NOC_ID_WIDTH'(0)) begin
                $fatal(1, "released read NoC ID was not reused for blocked AXI ID");
            end
            @(negedge clk_i);
            slv_req.ar_valid = 1'b0;
        end

        done_reg = 1'b1;
    end

endmodule

module tb_axi_id_remap;

    logic clk = 1'b0;
    logic rst_n = 1'b0;
    logic done_1, done_3, done_8;

    always #5 clk = ~clk;

    axi_id_remap_case #(.AXI_ID_WIDTH(1)) case_1 (.clk_i(clk), .rst_ni(rst_n), .done_o(done_1));
    axi_id_remap_case #(.AXI_ID_WIDTH(3)) case_3 (.clk_i(clk), .rst_ni(rst_n), .done_o(done_3));
    axi_id_remap_case #(.AXI_ID_WIDTH(8)) case_8 (.clk_i(clk), .rst_ni(rst_n), .done_o(done_8));

    initial begin
        repeat (2) @(posedge clk);
        rst_n = 1'b1;
        wait (done_1 && done_3 && done_8);
        $display("tb_axi_id_remap PASS");
        $finish;
    end

endmodule
