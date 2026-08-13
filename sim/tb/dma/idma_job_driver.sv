// idma_job_driver — replays one node's iDMA job file onto the backend's
// request port.
//
// Stimulus is <stim_dir>/node<NODE_ID>/jobs.txt, written by
// sim/tools/gen_dma_jobs.py: eleven whitespace-separated fields per job, in the
// order read below. Read at runtime from a +stim_dir= path, not baked in at
// elaboration, so a run changes stimulus without recompiling.
//
// Lexical shape: the two addresses carry an 0x prefix, the other nine are
// decimal. %x stops at the 'x' of "0x", so the addresses are taken as tokens
// and converted (read_hex below); a %d on one of them would read 0.
//
// The counts leave on jobs_issued_o / jobs_retired_o. Nothing here decides when
// the run is over.

`ifndef IDMA_JOB_DRIVER_SV
`define IDMA_JOB_DRIVER_SV

module idma_job_driver #(
    parameter int unsigned NODE_ID = 0
) (
    input  logic                       clk_i,
    input  logic                       rst_ni,
    output idma_types_pkg::idma_req_t  req_o,
    output logic                       req_valid_o,
    input  logic                       req_ready_i,
    // The endpoint holds rsp_ready high, so every rsp_valid_i cycle retires one
    // job.
    input  logic                       rsp_valid_i,
    output int unsigned                jobs_issued_o,
    output int unsigned                jobs_retired_o
);

    // AxLEN is 8 b, so 255 is the largest burst bound the emitter can state;
    // it means "do not reduce" (see the beo assignment below).
    localparam int unsigned NO_LEN_REDUCTION = 255;

    string stim_dir = "sim/test_patterns/dma";

    int unsigned issued  = 0;   // the file-reading process below owns this
    int unsigned retired = 0;

    always_ff @(posedge clk_i) begin
        if (!rst_ni) retired <= 0;
        else if (rsp_valid_i) retired <= retired + 1;
        // Registered out: Verilator does not reliably propagate a
        // procedurally-assigned output-port variable to the instantiating scope.
        jobs_issued_o  <= issued;
        jobs_retired_o <= retired;
    end

    // One address field: an 0x-prefixed hex token. string.atohex() stops at the
    // 'x', so the prefix comes off first.
    function automatic longint unsigned read_hex(input int fd);
        string tok;
        void'($fscanf(fd, "%s", tok));
        if (tok.substr(0, 1) == "0x") tok = tok.substr(2, tok.len() - 1);
        return tok.atohex();
    endfunction

    initial begin
        int fd;
        int code;
        string path;
        idma_types_pkg::idma_req_t r;
        int unsigned length, src_protocol, dst_protocol, max_src_len, max_dst_len;
        int unsigned aw_decoupled, rw_decoupled, num_errors, axi_id;
        longint unsigned src_addr, dst_addr;

        // Non-blocking throughout: one assignment style per variable.
        req_o       <= '0;
        req_valid_o <= 1'b0;

        void'($value$plusargs("stim_dir=%s", stim_dir));
        path = $sformatf("%s/node%0d/jobs.txt", stim_dir, NODE_ID);
        fd = $fopen(path, "r");
        if (fd == 0) $fatal(1, "[dma_jobs] node%0d: cannot open %s", NODE_ID, path);

        @(posedge rst_ni);

        forever begin
            code = $fscanf(fd, "%d", length);
            if (code != 1) break;   // end of file
            src_addr = read_hex(fd);
            dst_addr = read_hex(fd);
            void'($fscanf(fd, "%d", src_protocol));
            void'($fscanf(fd, "%d", dst_protocol));
            void'($fscanf(fd, "%d", max_src_len));
            void'($fscanf(fd, "%d", max_dst_len));
            void'($fscanf(fd, "%d", aw_decoupled));
            void'($fscanf(fd, "%d", rw_decoupled));
            void'($fscanf(fd, "%d", num_errors));   // the error path is out of scope
            void'($fscanf(fd, "%d", axi_id));

            r = '0;
            r.length   = idma_types_pkg::tf_len_t'(length);
            r.src_addr = idma_types_pkg::addr_t'(src_addr);
            r.dst_addr = idma_types_pkg::addr_t'(dst_addr);
            r.user     = '0;   // AWUSER carries the collective encoding; a DMA emits none
            r.opt.src_protocol = idma_pkg::protocol_e'(src_protocol);
            r.opt.dst_protocol = idma_pkg::protocol_e'(dst_protocol);
            r.opt.axi_id       = idma_types_pkg::id_t'(axi_id);
            r.opt.src = '{burst: axi_pkg::BURST_INCR, cache: '0, lock: 1'b0,
                          prot: '0, qos: '0, region: '0};
            r.opt.dst = r.opt.src;
            r.opt.beo.decouple_aw = aw_decoupled[0];
            r.opt.beo.decouple_rw = rw_decoupled[0];
            // *_max_llen is a LOG length and only acts when the matching
            // reduce_len bit is set (idma_legalizer_page_splitter.sv:37); with
            // it clear the legalizer splits on the 4 KiB page alone, which is
            // what the emitter's 255 asks for.
            r.opt.beo.src_reduce_len = (max_src_len != NO_LEN_REDUCTION);
            r.opt.beo.dst_reduce_len = (max_dst_len != NO_LEN_REDUCTION);
            r.opt.beo.src_max_llen   = 3'(max_src_len);
            r.opt.beo.dst_max_llen   = 3'(max_dst_len);
            // One request per job: every job is the last of its own transfer.
            r.opt.last = 1'b1;

            @(posedge clk_i);
            req_o       <= r;
            req_valid_o <= 1'b1;
            @(posedge clk_i);
            while (!req_ready_i) @(posedge clk_i);
            req_valid_o <= 1'b0;
            issued = issued + 1;
        end
        $fclose(fd);
        $display("[dma_jobs] node%0d: %0d jobs issued from %s", NODE_ID, issued, path);
    end

endmodule

`endif  // IDMA_JOB_DRIVER_SV
