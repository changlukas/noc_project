"""Runtime-loaded case oracle for the fixed-XY research batch."""
from pathlib import Path
import json
import dependent_dma_plan as dma


def emit(plan, directory):
    directory=Path(directory)
    directory.mkdir(parents=True,exist_ok=True)
    original=set(plan.initial)
    (directory/"check_regions.txt").write_text("".join(
        f"{r.node} {r.address:x} {r.length} {r.shard} {r.offset} {int(r in original)}\n"
        for r in plan.final))
    lines=[]
    for t in plan.transfers:
        waits=dma.requirements(t)
        lines.append(f"{t.issuer} {t.source.address:x} {t.destination.address:x} {t.source.length} {t.user:x} "
                     f"{t.phase or 'transfer'} {len(waits)}"+
                     "".join(f" {n} {c}" for n,c in waits)+"\n")
    (directory/"check_jobs.txt").write_text("".join(lines))
    (directory/"check_meta.txt").write_text(
        f"{len(plan.transfers)} {sum(r.length for r in plan.final)} {dma.OPERATION_LABELS[plan.operation]}\n")
    (directory/"check_header.txt").write_text(json.dumps(dma.result_header(plan))[:-1]+"\n")


def checker_sv(node_count, memory_targets):
    lines=[f"    localparam int MEM_TARGET [{node_count}] = '{{{', '.join(map(str,memory_targets))}}};"]
    def mem(node):
        return f"g_endpoint[{node}].u_endpoint.g_tile_mem[MEM_TARGET[{node}]].i_mem.i_sim_mem.mem"
    lines += ["    function automatic logic [7:0] shard_byte(input int shard, input int offset);",
              "        return 8'(shard + 1) ^ 8'(offset) ^ 8'(offset >> 8) ^ 8'(offset >> 16);",
              "    endfunction",
              "    function automatic logic [7:0] read_shard(input int node, input logic [ADDR_WIDTH-1:0] address);",
              "        case (node)"]
    lines += [f"            {n}: return {mem(n)}[address];" for n in range(node_count)]
    lines += ['            default: $fatal(1, "invalid shard endpoint");',"        endcase","        return 'x;","    endfunction",
              "    task automatic write_shard(input int node, input logic [ADDR_WIDTH-1:0] address, input logic [7:0] value);",
              "        case (node)"]
    lines += [f"            {n}: {mem(n)}[address] = value;" for n in range(node_count)]
    lines += ['            default: $fatal(1, "invalid shard endpoint");',"        endcase","    endtask"]
    template=r'''    typedef struct {
        int node;
        logic [ADDR_WIDTH-1:0] address;
        int length, shard, offset, original;
    } xy_region_t;
    typedef struct {
        logic [ADDR_WIDTH-1:0] source, destination;
        int length;
        logic [63:0] user_value;
        string phase;
        int waits[NUM_ENDPOINTS];
    } xy_job_t;
    xy_region_t xy_regions[$];
    xy_job_t xy_jobs[NUM_ENDPOINTS][$];
    int EXPECTED_JOBS[NUM_ENDPOINTS];
    int xy_checked[NUM_ENDPOINTS], xy_retired[NUM_ENDPOINTS];
    int xy_expected_total;
    longint unsigned xy_expected_bytes;
    string xy_label, xy_header;
    bit xy_loaded = 0;
    logic [NUM_ENDPOINTS-1:0] operation_job_valid, operation_response_valid;
    bit operation_started_reg = 0, operation_done_reg = 0;
    longint unsigned operation_start_reg = 0, operation_end_reg = 0;
    int unsigned operation_retired_reg = 0;
    initial begin
        int fd, code, node, dependency_count, peer, count;
        xy_region_t region;
        xy_job_t job;
        string directory;
        for (int n=0;n<NUM_ENDPOINTS;n++) EXPECTED_JOBS[n]=0;
        if (!$value$plusargs("stim_dir=%s", directory)) $fatal(1, "missing stimulus directory");
        fd=$fopen({directory,"/check_meta.txt"},"r");
        if (!fd || $fscanf(fd,"%d %d %s",xy_expected_total,xy_expected_bytes,xy_label)!=3)
            $fatal(1,"malformed check metadata");
        $fclose(fd);
        fd=$fopen({directory,"/check_header.txt"},"r");
        if (!fd || $fgets(xy_header,fd)==0) $fatal(1,"missing check header");
        xy_header=xy_header.substr(0,xy_header.len()-2);
        $fclose(fd);
        fd=$fopen({directory,"/check_regions.txt"},"r");
        if (!fd) $fatal(1,"missing region oracle");
        while (!$feof(fd)) begin
            code=$fscanf(fd,"%d %h %d %d %d %d",region.node,region.address,
                         region.length,region.shard,region.offset,region.original);
            if (code!=6) begin
                if (!$feof(fd)) $fatal(1,"malformed region oracle");
                break;
            end
            if (region.node<0 || region.node>=NUM_ENDPOINTS || region.length<=0)
                $fatal(1,"invalid region oracle");
            xy_regions.push_back(region);
            for (int k=0;k<region.length;k++)
                write_shard(region.node,region.address+ADDR_WIDTH'(k),
                    region.original ? shard_byte(region.shard,k+region.offset) : ~shard_byte(region.shard,k+region.offset));
        end
        $fclose(fd);
        fd=$fopen({directory,"/check_jobs.txt"},"r");
        if (!fd) $fatal(1,"missing job oracle");
        while (!$feof(fd)) begin
            for (int n=0;n<NUM_ENDPOINTS;n++) job.waits[n]=0;
            code=$fscanf(fd,"%d %h %h %d %h %s %d",node,job.source,job.destination,
                         job.length,job.user_value,job.phase,dependency_count);
            if (code!=7) begin
                if (!$feof(fd)) $fatal(1,"malformed job oracle");
                break;
            end
            if (node<0 || node>=NUM_ENDPOINTS || job.length<=0 ||
                dependency_count<0 || dependency_count>NUM_ENDPOINTS) $fatal(1,"invalid job oracle");
            repeat (dependency_count) begin
                if ($fscanf(fd,"%d %d",peer,count)!=2 || peer<0 || peer>=NUM_ENDPOINTS || count<=0)
                    $fatal(1,"malformed dependency oracle");
                job.waits[peer]=count;
            end
            // Static indices avoid incorrect dynamic queue push_back code generation.
            case (node)
__XY_PUSH_CASES__
                default: $fatal(1,"invalid job node");
            endcase
            EXPECTED_JOBS[node]++;
        end
        $fclose(fd);
        count=0;
        for (int n=0;n<NUM_ENDPOINTS;n++) count+=EXPECTED_JOBS[n];
        if (count!=xy_expected_total) $fatal(1,"job oracle count mismatch");
        xy_loaded=1;
    end
    always_ff @(posedge clk_i) begin
        if (!rst_ni) begin
            operation_started_reg<=0;
            operation_done_reg<=0;
            operation_start_reg<=0;
            operation_end_reg<=0;
            operation_retired_reg<=0;
        end else if (xy_loaded) begin
            if (!operation_started_reg && |operation_job_valid) begin
                operation_started_reg<=1;
                operation_start_reg<=live_cyc;
            end
            operation_retired_reg<=operation_retired_reg+$countones(operation_response_valid);
            if (!operation_done_reg && operation_retired_reg+$countones(operation_response_valid)==xy_expected_total) begin
                operation_done_reg<=1;
                operation_end_reg<=live_cyc;
            end
        end
    end
'''
    template=template.replace("__XY_PUSH_CASES__", "\n".join(
        f"                {n}: xy_jobs[{n}].push_back(job);" for n in range(node_count)))
    lines.extend(template.splitlines())
    for n in range(node_count):
        endpoint=f"g_endpoint[{n}].u_endpoint"
        lines += [f"    assign operation_job_valid[{n}]={endpoint}.dma_job_req_valid;",
                  f"    assign operation_response_valid[{n}]={endpoint}.dma_job_rsp_valid;",
                  "    always @(posedge clk_i) begin",
                  f"        if (!rst_ni) begin xy_checked[{n}]=0; xy_retired[{n}]=0; end",
                  "        else if (xy_loaded) begin",
                  f"            if ({endpoint}.dma_job_req_valid && {endpoint}.dma_job_req_ready) begin",
                  f'                if (xy_checked[{n}]>=EXPECTED_JOBS[{n}]) $fatal(1,"unexpected job");',
                  f"                for (int p=0;p<NUM_ENDPOINTS;p++)",
                  f"                    if (jobs_retired[p]<xy_jobs[{n}][xy_checked[{n}]].waits[p])",
                  '                        $fatal(1,"dependency violation");',
                  f"                if ({endpoint}.dma_job_req.src_addr!=xy_jobs[{n}][xy_checked[{n}]].source ||",
                  f"                    {endpoint}.dma_job_req.dst_addr!=xy_jobs[{n}][xy_checked[{n}]].destination ||",
                  f"                    {endpoint}.dma_job_req.length!=xy_jobs[{n}][xy_checked[{n}]].length ||",
                  f"                    {endpoint}.dma_job_req.user!=xy_jobs[{n}][xy_checked[{n}]].user_value)",
                  f'                    $fatal(1,"request mismatch node{n} job%0d actual=%h/%h/%0d/%h expected=%h/%h/%0d/%h",xy_checked[{n}],{endpoint}.dma_job_req.src_addr,{endpoint}.dma_job_req.dst_addr,{endpoint}.dma_job_req.length,{endpoint}.dma_job_req.user,xy_jobs[{n}][xy_checked[{n}]].source,xy_jobs[{n}][xy_checked[{n}]].destination,xy_jobs[{n}][xy_checked[{n}]].length,xy_jobs[{n}][xy_checked[{n}]].user_value);',
                  f'                $display("[dma_phase] node={n} job=%0d phase=%s event=issue cycle=%0d",xy_checked[{n}],xy_jobs[{n}][xy_checked[{n}]].phase,live_cyc);',
                  f"                xy_checked[{n}]++;",
                  "            end",
                  f"            if ({endpoint}.dma_job_rsp_valid) begin",
                  f'                if (xy_retired[{n}]>=EXPECTED_JOBS[{n}]) $fatal(1,"unexpected response");',
                  f'                $display("[dma_phase] node={n} job=%0d phase=%s event=retire cycle=%0d",xy_retired[{n}],xy_jobs[{n}][xy_retired[{n}]].phase,live_cyc);',
                  f"                xy_retired[{n}]++;",
                  "            end","        end","    end"]
    lines.extend(r'''    initial begin
        bit complete;
        longint unsigned checked_bytes;
        int operation_fd;
        string operation_path;
        checked_bytes=0;
        do begin
            @(posedge clk_i);
            complete=rst_ni && xy_loaded && operation_done_reg;
            for (int node=0;node<NUM_ENDPOINTS;node++) begin
                if (jobs_issued[node]>EXPECTED_JOBS[node]) $fatal(1,"extra DMA job");
                complete &= jobs_done[node] && jobs_retired[node]==EXPECTED_JOBS[node];
            end
        end while (!complete);
        for (int node=0;node<NUM_ENDPOINTS;node++)
            if (xy_checked[node]!=EXPECTED_JOBS[node]) $fatal(1,"unchecked jobs");
        foreach (xy_regions[i]) begin
            for (int k=0;k<xy_regions[i].length;k++) begin
                if (read_shard(xy_regions[i].node,xy_regions[i].address+ADDR_WIDTH'(k)) !==
                    shard_byte(xy_regions[i].shard,k+xy_regions[i].offset)) $fatal(1,"payload mismatch");
                checked_bytes++;
            end
        end
        if (checked_bytes!=xy_expected_bytes || !operation_started_reg || operation_end_reg<operation_start_reg)
            $fatal(1,"invalid operation accounting");
        if ($value$plusargs("operation_out=%s",operation_path)) begin
            operation_fd=$fopen(operation_path,"w");
            if (!operation_fd) $fatal(1,"cannot open operation output");
            $fdisplay(operation_fd,"%s, \"start_cycle\": %0d, \"end_cycle\": %0d, \"duration_cycles\": %0d, \"checked_bytes\": %0d, \"status\": \"PASS\"}",
                      xy_header,operation_start_reg,operation_end_reg,operation_end_reg-operation_start_reg+1,checked_bytes);
            $fclose(operation_fd);
        end
        $display("PASS: %s retired %0d transfers, checked %0d bytes",xy_label,xy_expected_total,checked_bytes);
        $finish(0);
    end
'''.splitlines())
    return lines


def validate_snapshot(plan, directory):
    import tempfile
    with tempfile.TemporaryDirectory() as temporary:
        emit(plan, temporary)
        for expected in Path(temporary).iterdir():
            actual=Path(directory)/"oracle"/expected.name
            if not actual.exists() or actual.read_bytes()!=expected.read_bytes():
                raise ValueError(f"runtime oracle provenance mismatch: {expected.name}")


if __name__=="__main__":
    import argparse
    import shutil
    parser=argparse.ArgumentParser()
    parser.add_argument("--snapshot",nargs=2,metavar=("STIMULUS","RESULT"),required=True)
    args=parser.parse_args()
    source,destination=map(Path,args.snapshot)
    destination=destination/"oracle"
    destination.mkdir(parents=True,exist_ok=True)
    for name in ("check_regions.txt","check_jobs.txt","check_meta.txt","check_header.txt","dependent_plan.json"):
        shutil.copy2(source/name,destination/name)
