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

    // beo.*_max_llen is a 3-bit LOG length, so the file states 0..7; 8 is
    // outside the field and encodes "no reduction", which is the value
    // idma_legalizer_page_splitter.sv:37 uses internally when reduce_len is
    // clear. Anything above 8 is a malformed file, not a large burst.
    localparam int unsigned MAX_LOG_LEN_NONE = 8;

    string stim_dir = "sim/test_patterns/dma";
    string jobs_path;

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

    // One decimal field. $fscanf leaves its target UNMODIFIED on a failed match,
    // so an unchecked read assembles a record out of the previous job's
    // leftovers; every field is checked and a bad one stops the run.
    function automatic int unsigned read_dec(input int fd, input string field);
        int unsigned v;
        if ($fscanf(fd, "%d", v) != 1)
            $fatal(1, "[dma_jobs] node%0d: %s: short or malformed job file %s",
                   NODE_ID, field, jobs_path);
        return v;
    endfunction

    // One address field: an 0x-prefixed hex token, accumulated here rather than
    // through string.atohex() -- IEEE 1800-2017 declares atohex() as returning
    // `integer`, 32 bits signed, and an address is ADDR_WIDTH (48).
    function automatic longint unsigned read_hex(input int fd, input string field);
        string tok;
        longint unsigned v = 0;
        byte c;
        int unsigned d;
        if ($fscanf(fd, "%s", tok) != 1)
            $fatal(1, "[dma_jobs] node%0d: %s: short or malformed job file %s",
                   NODE_ID, field, jobs_path);
        if (tok.len() < 3 || tok.substr(0, 1) != "0x")
            $fatal(1, "[dma_jobs] node%0d: %s: expected an 0x-prefixed address, got '%s'",
                   NODE_ID, field, tok);
        // Before accumulating, not after: past 16 digits the top of the token
        // shifts out of v and the range check below would pass on a value that
        // was never in range.
        if (tok.len() > 18)
            $fatal(1, "[dma_jobs] node%0d: %s: address '%s' is wider than 64 b",
                   NODE_ID, field, tok);
        for (int i = 2; i < tok.len(); i++) begin
            c = tok.getc(i);
            if (c >= "0" && c <= "9")      d = c - "0";
            else if (c >= "a" && c <= "f") d = c - "a" + 10;
            else if (c >= "A" && c <= "F") d = c - "A" + 10;
            else $fatal(1, "[dma_jobs] node%0d: %s: '%s' is not a hex address",
                        NODE_ID, field, tok);
            v = (v << 4) | longint'(d);
        end
        if (v >= (64'd1 << idma_types_pkg::ADDR_WIDTH))
            $fatal(1, "[dma_jobs] node%0d: %s: address %s exceeds ADDR_WIDTH (%0d b)",
                   NODE_ID, field, tok, idma_types_pkg::ADDR_WIDTH);
        return v;
    endfunction

    // The file states a log length; the field is three bits wide plus its own
    // reduce bit, so an out-of-range value is rejected rather than truncated --
    // 3'(8) and 3'(16) are both 0, which would silently ask for 1-beat bursts.
    function automatic logic [2:0] log_len(input int unsigned v, input string field);
        if (v > MAX_LOG_LEN_NONE)
            $fatal(1, "[dma_jobs] node%0d: %s=%0d is out of range (0..%0d)",
                   NODE_ID, field, v, MAX_LOG_LEN_NONE);
        return 3'(v);
    endfunction

    initial begin
        int fd;
        int code;
        idma_types_pkg::idma_req_t r;
        int unsigned length, src_protocol, dst_protocol, max_src_len, max_dst_len;
        int unsigned aw_decoupled, rw_decoupled, num_errors, axi_id;
        longint unsigned src_addr, dst_addr;

        // Non-blocking throughout: one assignment style per variable.
        req_o       <= '0;
        req_valid_o <= 1'b0;

        void'($value$plusargs("stim_dir=%s", stim_dir));
        jobs_path = $sformatf("%s/node%0d/jobs.txt", stim_dir, NODE_ID);
        fd = $fopen(jobs_path, "r");
        if (fd == 0) $fatal(1, "[dma_jobs] node%0d: cannot open %s", NODE_ID, jobs_path);

        @(posedge rst_ni);

        forever begin
            // Only the first field of a record may be ABSENT: that is the end of
            // the file, and $fscanf answers EOF (negative) for it. A field that
            // is present but unreadable answers 0, which is a corrupt file and
            // not a shorter run. Every field after this one is checked inside
            // the readers.
            code = $fscanf(fd, "%d", length);
            if (code < 0) break;
            if (code != 1)
                $fatal(1, "[dma_jobs] node%0d: length: malformed job file %s",
                       NODE_ID, jobs_path);
            src_addr     = read_hex(fd, "src_addr");
            dst_addr     = read_hex(fd, "dst_addr");
            src_protocol = read_dec(fd, "src_protocol");
            dst_protocol = read_dec(fd, "dst_protocol");
            max_src_len  = read_dec(fd, "max_src_len");
            max_dst_len  = read_dec(fd, "max_dst_len");
            aw_decoupled = read_dec(fd, "aw_decoupled");
            rw_decoupled = read_dec(fd, "rw_decoupled");
            num_errors   = read_dec(fd, "num_errors");   // the error path is out of scope
            axi_id       = read_dec(fd, "axi_id");

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
            // *_max_llen only acts when the matching reduce_len bit is set
            // (idma_legalizer_page_splitter.sv:37); with it clear the legalizer
            // splits on the 4 KiB page alone.
            r.opt.beo.src_reduce_len = (max_src_len != MAX_LOG_LEN_NONE);
            r.opt.beo.dst_reduce_len = (max_dst_len != MAX_LOG_LEN_NONE);
            r.opt.beo.src_max_llen   = log_len(max_src_len, "max_src_len");
            r.opt.beo.dst_max_llen   = log_len(max_dst_len, "max_dst_len");
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
        $display("[dma_jobs] node%0d: %0d jobs issued from %s", NODE_ID, issued, jobs_path);
    end

endmodule

`endif  // IDMA_JOB_DRIVER_SV
