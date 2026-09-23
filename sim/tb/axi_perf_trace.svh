// Passive NMU AXI trace shared by the DMA and file-master testbenches.
for (genvar i = 0; i < $size(master_axi_req); i++) begin : g_axi_trace
    integer trace_fd = 0;
    integer dependent_trace = 0;
    string trace_path;
    initial begin
        void'($value$plusargs("dependent_jobs=%d", dependent_trace));
        if (dependent_trace || $test$plusargs("packet_trace")) begin
            if (!$value$plusargs("perf_out=%s", trace_path)) trace_path = "perf.json";
            trace_fd = $fopen($sformatf("%s.axi%0d.csv", trace_path, i), "w");
            if (!trace_fd) $fatal(1, "Cannot open AXI trace (instance %m)");
            $fdisplay(trace_fd, "cycle,event,id,address,len,size,last,ready,in_window,user");
        end
    end
    always @(posedge clk_i) begin
        if (trace_fd) begin
            if (~rst_n_i) begin
                $fdisplay(trace_fd, "%0d,RESET,0,0,0,0,0,0,0,0", live_cyc);
            end else begin
                if (master_axi_req[i].awvalid)
                    $fdisplay(trace_fd, "%0d,AW,%0d,%0h,%0d,%0d,0,%0d,%0d,%0h", live_cyc,
                        master_axi_req[i].awid, master_axi_req[i].awaddr,
                        master_axi_req[i].awlen, master_axi_req[i].awsize,
                        master_axi_rsp[i].awready, perf_measure_en, master_awuser[i]);
                if (master_axi_req[i].arvalid)
                    $fdisplay(trace_fd, "%0d,AR,%0d,%0h,%0d,%0d,0,%0d,%0d,0", live_cyc,
                        master_axi_req[i].arid, master_axi_req[i].araddr,
                        master_axi_req[i].arlen, master_axi_req[i].arsize,
                        master_axi_rsp[i].arready, perf_measure_en);
                if (master_axi_req[i].wvalid)
                    $fdisplay(trace_fd, "%0d,W,0,0,0,0,%0d,%0d,%0d,0", live_cyc,
                        master_axi_req[i].wlast, master_axi_rsp[i].wready, perf_measure_en);
                if (master_axi_rsp[i].rvalid)
                    $fdisplay(trace_fd, "%0d,R,%0d,0,0,0,%0d,%0d,%0d,0", live_cyc,
                        master_axi_rsp[i].rid, master_axi_rsp[i].rlast,
                        master_axi_req[i].rready, perf_measure_en);
                if (master_axi_rsp[i].bvalid)
                    $fdisplay(trace_fd, "%0d,B,%0d,0,0,0,0,%0d,%0d,0", live_cyc,
                        master_axi_rsp[i].bid, master_axi_req[i].bready, perf_measure_en);
            end
        end
    end
    final begin
        if (trace_fd) begin
            $fdisplay(trace_fd, "%0d,END,0,0,0,0,0,0,0,0", live_cyc);
            $fclose(trace_fd);
        end
    end
end
