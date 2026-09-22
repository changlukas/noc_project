`timescale 1ns / 1ps
module tb_floo_vc_arbiter;
parameter int N=2;
parameter int DEPTH=2;
logic clk=0, rst_n=0;
always #5 clk=~clk;
logic [N-1:0] valid_i='0, ready_o, ready_i='1, valid_o, credit='0;
logic [N-1:0][31:0] data_i;
logic [0:0][31:0] data_o;
int count[N], total=0, available[N], both=0, stalled=0;
floo_vc_arbiter #(.NumVirtChannels(N),.NumPhysChannels(1),.NumCredits(DEPTH),
.flit_t(logic [31:0]),.VcImpl(floo_pkg::VcCredit)) dut
(.clk_i(clk),.rst_ni(rst_n),.valid_i,.ready_o,.data_i,.ready_i,.valid_o,.data_o,.credit_i(credit));
always @(posedge clk) begin
 if(!rst_n) begin
  for(int v=0;v<N;v++) begin available[v]=DEPTH; count[v]=0; end
 end else begin
  if(!$onehot0(valid_o)) $fatal(1,"not onehot");
  if(ready_i=='1) begin
   bit eligible;
   eligible=0;
   for(int v=0;v<N;v++) eligible |= valid_i[v] && available[v]>0;
   if(eligible && valid_o=='0) $fatal(1,"avoidable credit arbitration bubble");
  end
  for(int v=0;v<N;v++) begin
   if(valid_o[v] && !ready_i[v]) stalled++;
   if(valid_o[v] && ready_i[v]) begin
    if(!valid_i[v] || available[v]<=0) $fatal(1,"uncredited transfer");
    if(data_o[0] != 32'(v)) $fatal(1,"payload wrong");
    if(credit[v]) both++;
    count[v]++; total++;
    available[v]--;
   end
   if(credit[v]) available[v]++;
   if(available[v]<0 || available[v]>DEPTH) $fatal(1,"credit conservation");
  end
 end
end
initial begin
 for(int v=0;v<N;v++) begin data_i[v]=32'(v); count[v]=0; end
 repeat(3) @(negedge clk); rst_n=1;valid_i='1;
 repeat(DEPTH*N+3) @(negedge clk);
 for(int v=0;v<N;v++) if(count[v]!=DEPTH) $fatal(1,"exhaustion N=%0d vc=%0d count=%0d",N,v,count[v]);
 if(valid_o!='0) $fatal(1,"sent without credit");
 credit='1;@(negedge clk);credit='0;
 repeat(N+3) @(negedge clk);
 for(int v=0;v<N;v++) if(count[v]!=DEPTH+1) $fatal(1,"recovery starvation");
 credit='1;@(negedge clk);credit='0;
 for(int cycle=0;cycle<256;cycle++) begin
  for(int v=0;v<N;v++) begin
   valid_i[v]=((cycle+v)%5)!=0;
   ready_i[v]=((cycle+2*v)%7)!=0;
   credit[v]=(available[v]<DEPTH) && ((cycle+v)%3)!=0;
  end
  @(negedge clk);
 end
 credit='0;valid_i='0;ready_i='0;rst_n=0;
 repeat(2) @(negedge clk);
 rst_n=1;ready_i='1;valid_i='1;
 repeat(DEPTH*N+3) @(negedge clk);
 for(int v=0;v<N;v++) if(count[v]!=DEPTH) $fatal(1,"reset reseed failed");
 if(both==0 || stalled==0) $fatal(1,"vacuous give/take or stall");
 $display("PASS credit arbiter N=%0d depth=%0d total=%0d simultaneous=%0d stalls=%0d",N,DEPTH,total,both,stalled);$finish;
end
initial begin #100000; $fatal(1,"timeout");end
endmodule
