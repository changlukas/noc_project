// Copyright (c) 2014-2020 ETH Zurich, University of Bologna
//
// Copyright and related rights are licensed under the Solderpad Hardware
// License, Version 0.51 (the "License"); you may not use this file except in
// compliance with the License.  You may obtain a copy of the License at
// http://solderpad.org/licenses/SHL-0.51. Unless required by applicable law
// or agreed to in writing, software, hardware and materials distributed under
// this License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.
//
// Authors:
// - Andreas Kurth <akurth@iis.ee.ethz.ch>
// - Wolfgang Roenninger <wroennin@iis.ee.ethz.ch>
// - Florian Zaruba <zarubaf@iis.ee.ethz.ch>

`resetall
`timescale 1ns / 1ps
`default_nettype none

// Adapted from pulp-platform/axi v0.39.7 axi_id_remap; tables remain upstream.
module nmu_id_remap #(
    parameter int unsigned AXI_ID_WIDTH,
    parameter int unsigned NOC_ID_WIDTH,
    parameter int unsigned MAX_ACTIVE_IDS,
    parameter int unsigned MAX_OUTSTANDING_PER_ID,
    parameter type slv_req_t  = logic,
    parameter type slv_resp_t = logic,
    parameter type mst_req_t  = logic,
    parameter type mst_resp_t = logic
) (
    input  wire logic     clk_i,
    input  wire logic     rst_n_i,
    input  wire slv_req_t  slv_req_i,
    output wire slv_resp_t slv_resp_o,
    output wire mst_req_t  mst_req_o,
    input  wire mst_resp_t mst_resp_i
);
    localparam int unsigned ID_IDX_W = cf_math_pkg::idx_width(MAX_ACTIVE_IDS);
    typedef logic [ID_IDX_W-1:0] id_idx_t;

    initial begin
        if (AXI_ID_WIDTH < 1 || NOC_ID_WIDTH < ID_IDX_W ||
            MAX_ACTIVE_IDS < 1 || MAX_OUTSTANDING_PER_ID < 1 ||
            $clog2(MAX_ACTIVE_IDS) > AXI_ID_WIDTH)
            $fatal(1, "Invalid NMU remap configuration (%m)");
    end

    assign mst_req_o.aw.addr   = slv_req_i.aw.addr;
    assign mst_req_o.aw.len    = slv_req_i.aw.len;
    assign mst_req_o.aw.size   = slv_req_i.aw.size;
    assign mst_req_o.aw.burst  = slv_req_i.aw.burst;
    assign mst_req_o.aw.lock   = slv_req_i.aw.lock;
    assign mst_req_o.aw.cache  = slv_req_i.aw.cache;
    assign mst_req_o.aw.prot   = slv_req_i.aw.prot;
    assign mst_req_o.aw.qos    = slv_req_i.aw.qos;
    assign mst_req_o.aw.region = slv_req_i.aw.region;
    assign mst_req_o.aw.atop   = slv_req_i.aw.atop;
    assign mst_req_o.aw.user   = slv_req_i.aw.user;

    assign mst_req_o.w        = slv_req_i.w;
    assign mst_req_o.w_valid  = slv_req_i.w_valid;
    assign slv_resp_o.w_ready = mst_resp_i.w_ready;

    assign slv_resp_o.b.resp  = mst_resp_i.b.resp;
    assign slv_resp_o.b.user  = mst_resp_i.b.user;
    assign slv_resp_o.b_valid = mst_resp_i.b_valid;
    assign mst_req_o.b_ready  = slv_req_i.b_ready;

    assign mst_req_o.ar.addr   = slv_req_i.ar.addr;
    assign mst_req_o.ar.len    = slv_req_i.ar.len;
    assign mst_req_o.ar.size   = slv_req_i.ar.size;
    assign mst_req_o.ar.burst  = slv_req_i.ar.burst;
    assign mst_req_o.ar.lock   = slv_req_i.ar.lock;
    assign mst_req_o.ar.cache  = slv_req_i.ar.cache;
    assign mst_req_o.ar.prot   = slv_req_i.ar.prot;
    assign mst_req_o.ar.qos    = slv_req_i.ar.qos;
    assign mst_req_o.ar.region = slv_req_i.ar.region;
    assign mst_req_o.ar.user   = slv_req_i.ar.user;

    assign slv_resp_o.r.data  = mst_resp_i.r.data;
    assign slv_resp_o.r.resp  = mst_resp_i.r.resp;
    assign slv_resp_o.r.last  = mst_resp_i.r.last;
    assign slv_resp_o.r.user  = mst_resp_i.r.user;
    assign slv_resp_o.r_valid = mst_resp_i.r_valid;
    assign mst_req_o.r_ready  = slv_req_i.r_ready;

    wire logic    wr_exists, wr_exists_full, wr_full;
    wire logic    wr_can_alloc, wr_alloc;
    wire id_idx_t wr_free_id, wr_mapped_id, wr_alloc_id;
    logic    aw_hold_reg = 1'b0, aw_hold_next;
    id_idx_t aw_id_reg = '0, aw_id_next;

    assign wr_can_alloc        = (wr_exists && !wr_exists_full) || (!wr_exists && !wr_full);
    assign wr_alloc            = !aw_hold_reg && slv_req_i.aw_valid && wr_can_alloc;
    assign wr_alloc_id         = wr_exists ? wr_mapped_id : wr_free_id;
    assign mst_req_o.aw.id     = NOC_ID_WIDTH'(aw_hold_reg ? aw_id_reg : wr_alloc_id);
    assign mst_req_o.aw_valid  = aw_hold_reg || wr_alloc;
    assign slv_resp_o.aw_ready = mst_req_o.aw_valid && mst_resp_i.aw_ready;

    // Reserve once when offered; the held mapping survives downstream stalls.
    always_comb begin
        aw_hold_next = mst_req_o.aw_valid && !mst_resp_i.aw_ready;
        aw_id_next   = aw_id_reg;
        if (wr_alloc) begin
            aw_id_next = wr_alloc_id;
        end
    end

    always @(posedge clk_i or negedge rst_n_i) begin
        if (~rst_n_i) begin
            aw_hold_reg <= 1'b0;
            aw_id_reg   <= '0;
        end else begin
            aw_hold_reg <= aw_hold_next;
            aw_id_reg   <= aw_id_next;
        end
    end

    axi_id_remap_table #(
        .InpIdWidth      (AXI_ID_WIDTH                   ),
        .MaxUniqInpIds   (MAX_ACTIVE_IDS                 ),
        .MaxTxnsPerId    (MAX_OUTSTANDING_PER_ID         )
    ) i_wr_table (
        .clk_i           (clk_i                          ),
        .rst_ni          (rst_n_i                        ),
        .free_o          (                               ),
        .free_oup_id_o   (wr_free_id                     ),
        .full_o          (wr_full                        ),
        .push_i          (wr_alloc                       ),
        .push_inp_id_i   (slv_req_i.aw.id                ),
        .push_oup_id_i   (wr_alloc_id                    ),
        .exists_inp_id_i (slv_req_i.aw.id                ),
        .exists_o        (wr_exists                      ),
        .exists_oup_id_o (wr_mapped_id                   ),
        .exists_full_o   (wr_exists_full                 ),
        .pop_i           (slv_resp_o.b_valid && slv_req_i.b_ready),
        .pop_oup_id_i    (mst_resp_i.b.id[ID_IDX_W-1:0]  ),
        .pop_inp_id_o    (slv_resp_o.b.id                )
    );

    // synthesis translate_off
    assert property (@(posedge clk_i) disable iff (!rst_n_i)
        mst_req_o.aw_valid && !mst_resp_i.aw_ready |=>
        mst_req_o.aw_valid && $stable(mst_req_o.aw))
        else $fatal(1, "AW changed while stalled");
    assert property (@(posedge clk_i) disable iff (!rst_n_i)
        aw_hold_reg |-> !wr_alloc)
        else $fatal(1, "Repeated AW allocation while held");
    assert property (@(posedge clk_i) disable iff (!rst_n_i)
        slv_req_i.aw_valid && wr_can_alloc && mst_resp_i.aw_ready |-> slv_resp_o.aw_ready)
        else $fatal(1, "Available AW admission was blocked");
    // synthesis translate_on

    wire logic    rd_exists, rd_exists_full, rd_full;
    wire logic    rd_can_alloc, rd_alloc;
    wire id_idx_t rd_free_id, rd_mapped_id, rd_alloc_id;
    logic    ar_hold_reg = 1'b0, ar_hold_next;
    id_idx_t ar_id_reg = '0, ar_id_next;

    assign rd_can_alloc        = (rd_exists && !rd_exists_full) || (!rd_exists && !rd_full);
    assign rd_alloc            = !ar_hold_reg && slv_req_i.ar_valid && rd_can_alloc;
    assign rd_alloc_id         = rd_exists ? rd_mapped_id : rd_free_id;
    assign mst_req_o.ar.id     = NOC_ID_WIDTH'(ar_hold_reg ? ar_id_reg : rd_alloc_id);
    assign mst_req_o.ar_valid  = ar_hold_reg || rd_alloc;
    assign slv_resp_o.ar_ready = mst_req_o.ar_valid && mst_resp_i.ar_ready;

    // Reserve once when offered; the held mapping survives downstream stalls.
    always_comb begin
        ar_hold_next = mst_req_o.ar_valid && !mst_resp_i.ar_ready;
        ar_id_next   = ar_id_reg;
        if (rd_alloc) begin
            ar_id_next = rd_alloc_id;
        end
    end

    always @(posedge clk_i or negedge rst_n_i) begin
        if (~rst_n_i) begin
            ar_hold_reg <= 1'b0;
            ar_id_reg   <= '0;
        end else begin
            ar_hold_reg <= ar_hold_next;
            ar_id_reg   <= ar_id_next;
        end
    end

    axi_id_remap_table #(
        .InpIdWidth      (AXI_ID_WIDTH                   ),
        .MaxUniqInpIds   (MAX_ACTIVE_IDS                 ),
        .MaxTxnsPerId    (MAX_OUTSTANDING_PER_ID         )
    ) i_rd_table (
        .clk_i           (clk_i                          ),
        .rst_ni          (rst_n_i                        ),
        .free_o          (                               ),
        .free_oup_id_o   (rd_free_id                     ),
        .full_o          (rd_full                        ),
        .push_i          (rd_alloc                       ),
        .push_inp_id_i   (slv_req_i.ar.id                ),
        .push_oup_id_i   (rd_alloc_id                    ),
        .exists_inp_id_i (slv_req_i.ar.id                ),
        .exists_o        (rd_exists                      ),
        .exists_oup_id_o (rd_mapped_id                   ),
        .exists_full_o   (rd_exists_full                 ),
        .pop_i           (slv_resp_o.r_valid && slv_req_i.r_ready && slv_resp_o.r.last),
        .pop_oup_id_i    (mst_resp_i.r.id[ID_IDX_W-1:0]  ),
        .pop_inp_id_o    (slv_resp_o.r.id                )
    );

    // synthesis translate_off
    assert property (@(posedge clk_i) disable iff (!rst_n_i)
        mst_req_o.ar_valid && !mst_resp_i.ar_ready |=>
        mst_req_o.ar_valid && $stable(mst_req_o.ar))
        else $fatal(1, "AR changed while stalled");
    assert property (@(posedge clk_i) disable iff (!rst_n_i)
        ar_hold_reg |-> !rd_alloc)
        else $fatal(1, "Repeated AR allocation while held");
    assert property (@(posedge clk_i) disable iff (!rst_n_i)
        slv_req_i.ar_valid && rd_can_alloc && mst_resp_i.ar_ready |-> slv_resp_o.ar_ready)
        else $fatal(1, "Available AR admission was blocked");
    // synthesis translate_on

    // synthesis translate_off
    assert property (@(posedge clk_i) disable iff (!rst_n_i)
        slv_req_i.aw_valid |-> slv_req_i.aw.atop == '0)
        else $fatal(1, "NMU remap does not support ATOP");
    // synthesis translate_on
endmodule
`resetall
