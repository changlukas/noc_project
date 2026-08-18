`resetall
`timescale 1ns / 1ps
`default_nettype none

module tb_common_primitives #(
    parameter int unsigned NOC_FIFO_DEPTH = 4,
    parameter int unsigned AXI_FIFO_DEPTH = 4
);

    logic clk_i = 1'b0;
    logic src_clk_i = 1'b0;
    logic dst_clk_i = 1'b0;
    logic rst_ni = 1'b0;
    logic src_rst_ni = 1'b0;
    logic dst_rst_ni = 1'b0;

    logic       sync_s_valid;
    logic       sync_s_ready;
    logic [7:0] sync_s_data;
    logic       sync_m_valid;
    logic       sync_m_ready;
    logic [7:0] sync_m_data;

    logic       cdc_s_valid;
    logic       cdc_s_ready;
    logic [7:0] cdc_s_data;
    logic       cdc_m_valid;
    logic       cdc_m_ready;
    logic [7:0] cdc_m_data;

    logic       bypass_s_valid;
    logic       bypass_s_ready;
    logic [7:0] bypass_s_data;
    logic       bypass_m_valid;
    logic       bypass_m_ready;
    logic [7:0] bypass_m_data;

    logic       simple_s_valid;
    logic       simple_s_ready;
    logic [7:0] simple_s_data;
    logic       simple_m_valid;
    logic       simple_m_ready;
    logic [7:0] simple_m_data;

    logic       skid_s_valid;
    logic       skid_s_ready;
    logic [7:0] skid_s_data;
    logic       skid_m_valid;
    logic       skid_m_ready;
    logic [7:0] skid_m_data;

    noc_sync_fifo #(
        .T              ( logic [7:0] ),
        .NOC_FIFO_DEPTH ( NOC_FIFO_DEPTH )
    ) i_noc_sync_fifo (
        .clk_i,
        .rst_ni,
        .s_valid_i ( sync_s_valid ),
        .s_ready_o ( sync_s_ready ),
        .s_data_i  ( sync_s_data  ),
        .m_valid_o ( sync_m_valid ),
        .m_ready_i ( sync_m_ready ),
        .m_data_o  ( sync_m_data  )
    );

    axi_async_fifo #(
        .T              ( logic [7:0] ),
        .AXI_FIFO_DEPTH ( AXI_FIFO_DEPTH )
    ) i_axi_async_fifo (
        .src_clk_i,
        .src_rst_ni,
        .src_valid_i ( cdc_s_valid ),
        .src_ready_o ( cdc_s_ready ),
        .src_data_i  ( cdc_s_data  ),
        .dst_clk_i,
        .dst_rst_ni,
        .dst_valid_o ( cdc_m_valid ),
        .dst_ready_i ( cdc_m_ready ),
        .dst_data_o  ( cdc_m_data  )
    );

    noc_reg_slice #(
        .T        ( logic [7:0] ),
        .REG_TYPE ( 0            )
    ) i_noc_reg_slice_bypass (
        .clk_i,
        .rst_ni,
        .s_valid_i ( bypass_s_valid ),
        .s_ready_o ( bypass_s_ready ),
        .s_data_i  ( bypass_s_data  ),
        .m_valid_o ( bypass_m_valid ),
        .m_ready_i ( bypass_m_ready ),
        .m_data_o  ( bypass_m_data  )
    );

    noc_reg_slice #(
        .T        ( logic [7:0] ),
        .REG_TYPE ( 1            )
    ) i_noc_reg_slice_simple (
        .clk_i,
        .rst_ni,
        .s_valid_i ( simple_s_valid ),
        .s_ready_o ( simple_s_ready ),
        .s_data_i  ( simple_s_data  ),
        .m_valid_o ( simple_m_valid ),
        .m_ready_i ( simple_m_ready ),
        .m_data_o  ( simple_m_data  )
    );

    noc_reg_slice #(
        .T        ( logic [7:0] ),
        .REG_TYPE ( 2            )
    ) i_noc_reg_slice_skid (
        .clk_i,
        .rst_ni,
        .s_valid_i ( skid_s_valid ),
        .s_ready_o ( skid_s_ready ),
        .s_data_i  ( skid_s_data  ),
        .m_valid_o ( skid_m_valid ),
        .m_ready_i ( skid_m_ready ),
        .m_data_o  ( skid_m_data  )
    );

    always #5ns clk_i <= ~clk_i;
    always #4ns src_clk_i <= ~src_clk_i;
    always #7ns dst_clk_i <= ~dst_clk_i;

    initial begin
        #100us;
        $fatal(1, "Timed out waiting for common primitive test completion");
    end

    function automatic logic [7:0] sequence_data(
        input logic [7:0] base,
        input int unsigned index
    );
        sequence_data = base + 8'(index);
    endfunction

    task automatic sync_send(input logic [7:0] data);
        begin
            @(negedge clk_i);
            sync_s_data = data;
            sync_s_valid = 1'b1;
            do @(posedge clk_i); while (!sync_s_ready);
            @(negedge clk_i);
            sync_s_valid = 1'b0;
        end
    endtask

    task automatic cdc_send(input logic [7:0] data);
        begin
            @(negedge src_clk_i);
            cdc_s_data = data;
            cdc_s_valid = 1'b1;
            do @(posedge src_clk_i); while (!cdc_s_ready);
            @(negedge src_clk_i);
            cdc_s_valid = 1'b0;
        end
    endtask

    task automatic cdc_receive(input logic [7:0] expected);
        begin
            wait (cdc_m_valid);
            assert (cdc_m_data == expected)
                else $fatal(1, "CDC FIFO expected %0h, got %0h", expected, cdc_m_data);
            @(posedge dst_clk_i);
            #1ps;
        end
    endtask

    task automatic sync_receive(input logic [7:0] expected);
        begin
            wait (sync_m_valid);
            assert (sync_m_data == expected)
                else $fatal(1, "Synchronous FIFO expected %0h, got %0h", expected, sync_m_data);
            @(posedge clk_i);
            #1ps;
        end
    endtask

    initial begin
        sync_s_valid = 1'b0;
        sync_s_data = '0;
        sync_m_ready = 1'b0;
        cdc_s_valid = 1'b0;
        cdc_s_data = '0;
        cdc_m_ready = 1'b0;
        bypass_s_valid = 1'b0;
        bypass_s_data = '0;
        bypass_m_ready = 1'b0;
        simple_s_valid = 1'b0;
        simple_s_data = '0;
        simple_m_ready = 1'b0;
        skid_s_valid = 1'b0;
        skid_s_data = '0;
        skid_m_ready = 1'b0;

        repeat (3) @(posedge clk_i);
        #1ps;
        rst_ni = 1'b1;
        @(posedge src_clk_i);
        #1ps;
        src_rst_ni = 1'b1;
        repeat (2) @(posedge dst_clk_i);
        #1ps;
        dst_rst_ni = 1'b1;

        assert (!sync_m_valid && !cdc_m_valid && !simple_m_valid && !skid_m_valid)
            else $fatal(1, "Primitive output valid was not cleared by reset");

        // The synchronous FIFO is non-fall-through and preserves every entry.
        for (int unsigned index = 0; index < NOC_FIFO_DEPTH; index++) begin
            sync_send(sequence_data(8'h10, index));
        end
        assert (!sync_s_ready) else $fatal(1, "Synchronous FIFO did not become full");
        repeat (3) begin
            assert (sync_m_valid && sync_m_data == 8'h10)
                else $fatal(1, "Synchronous FIFO changed its stalled head transaction");
            @(posedge clk_i);
            #1ps;
        end
        sync_m_ready = 1'b1;
        for (int unsigned index = 0; index < NOC_FIFO_DEPTH; index++) begin
            sync_receive(sequence_data(8'h10, index));
        end
        @(negedge clk_i);
        sync_m_ready = 1'b0;
        assert (!sync_m_valid) else $fatal(1, "Synchronous FIFO did not drain");

        // The Gray-pointer FIFO crosses unrelated clocks without reordering.
        for (int unsigned index = 0; index < AXI_FIFO_DEPTH; index++) begin
            cdc_send(sequence_data(8'h20, index));
        end
        wait (cdc_m_valid);
        repeat (3) begin
            assert (cdc_m_valid && cdc_m_data == 8'h20)
                else $fatal(1, "CDC FIFO changed its stalled head transaction");
            @(posedge dst_clk_i);
            #1ps;
        end
        cdc_m_ready = 1'b1;
        for (int unsigned index = 0; index < AXI_FIFO_DEPTH; index++) begin
            cdc_receive(sequence_data(8'h20, index));
        end
        @(negedge dst_clk_i);
        cdc_m_ready = 1'b0;

        // Bypass has no storage or added latency.
        bypass_m_ready = 1'b1;
        bypass_s_data = 8'h30;
        bypass_s_valid = 1'b1;
        #1ns;
        assert (bypass_s_ready && bypass_m_valid && bypass_m_data == 8'h30)
            else $fatal(1, "Bypass register slice changed the transaction");
        @(posedge clk_i);
        @(negedge clk_i);
        bypass_s_valid = 1'b0;
        bypass_m_ready = 1'b0;

        // A simple register holds its transaction during a downstream stall.
        wait (simple_s_ready);
        simple_s_data = 8'h40;
        simple_s_valid = 1'b1;
        @(posedge clk_i);
        @(negedge clk_i);
        simple_s_valid = 1'b0;
        wait (simple_m_valid);
        repeat (3) begin
            assert (simple_m_data == 8'h40)
                else $fatal(1, "Simple register did not hold stalled data");
            @(posedge clk_i);
        end
        simple_m_ready = 1'b1;
        @(posedge clk_i);
        @(negedge clk_i);
        simple_m_ready = 1'b0;
        assert (!simple_m_valid) else $fatal(1, "Simple register did not retire data");

        // A skid buffer accepts two transactions while its output remains stalled.
        wait (skid_s_ready);
        skid_s_data = 8'h50;
        skid_s_valid = 1'b1;
        @(posedge clk_i);
        @(negedge clk_i);
        skid_s_data = 8'h51;
        @(posedge clk_i);
        @(negedge clk_i);
        skid_s_valid = 1'b0;
        assert (skid_m_valid && skid_m_data == 8'h50)
            else $fatal(1, "Skid buffer did not retain its first transaction");
        skid_m_ready = 1'b1;
        @(posedge clk_i);
        wait (skid_m_valid && skid_m_data == 8'h51);
        @(posedge clk_i);
        @(negedge clk_i);
        skid_m_ready = 1'b0;
        assert (!skid_m_valid) else $fatal(1, "Skid buffer did not drain");

        $display("PASS: common primitive adapters");
        $finish;
    end

endmodule

`resetall
