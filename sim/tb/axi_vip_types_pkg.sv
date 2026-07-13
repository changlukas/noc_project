// Per-config pulp AXI struct typedefs + flat(ni_signals_pkg) -> pulp-struct
// mapping. Monitor taps derive BOTH stream sides from the flat structs so the
// (out-of-scope) user fields compare as constant 0 on both ends.
`include "axi/typedef.svh"

`ifndef AXI_VIP_TYPES_PKG_SV
`define AXI_VIP_TYPES_PKG_SV

package axi_vip_types_pkg;

    localparam int unsigned VIP_AW = ni_params_pkg::AXI_ADDR_WIDTH_DFLT;
    localparam int unsigned VIP_DW = ni_params_pkg::AXI_DATA_WIDTH_DFLT;
    localparam int unsigned VIP_IW = ni_params_pkg::AXI_ID_WIDTH_DFLT;
    localparam int unsigned VIP_UW = 1;  // pulp minimum; flat struct has no user

    typedef logic [VIP_AW-1:0]   vip_addr_t;
    typedef logic [VIP_DW-1:0]   vip_data_t;
    typedef logic [VIP_DW/8-1:0] vip_strb_t;
    typedef logic [VIP_IW-1:0]   vip_id_t;
    typedef logic [VIP_UW-1:0]   vip_user_t;

    `AXI_TYPEDEF_ALL(vip, vip_addr_t, vip_id_t, vip_data_t, vip_strb_t, vip_user_t)

    // Address-map rule type for addr_decode (common_cells): field names
    // idx/start_addr/end_addr are the addr_decode contract; end_addr is
    // exclusive.
    typedef struct packed {
        int unsigned idx;
        vip_addr_t   start_addr;
        vip_addr_t   end_addr;
    } vip_rule_t;

    function automatic vip_req_t vip_req_from_flat(input ni_signals_pkg::axi_req_t f);
        vip_req_t r;
        r = '0;
        r.aw.id     = f.awid;     r.aw.addr  = f.awaddr;  r.aw.len   = f.awlen;
        r.aw.size   = f.awsize;   r.aw.burst = f.awburst; r.aw.lock  = f.awlock;
        r.aw.cache  = f.awcache;  r.aw.prot  = f.awprot;  r.aw.qos   = f.awqos;
        r.aw.region = f.awregion; r.aw_valid = f.awvalid;
        r.w.data    = f.wdata;    r.w.strb   = f.wstrb;   r.w.last   = f.wlast;
        r.w_valid   = f.wvalid;   r.b_ready  = f.bready;
        r.ar.id     = f.arid;     r.ar.addr  = f.araddr;  r.ar.len   = f.arlen;
        r.ar.size   = f.arsize;   r.ar.burst = f.arburst; r.ar.lock  = f.arlock;
        r.ar.cache  = f.arcache;  r.ar.prot  = f.arprot;  r.ar.qos   = f.arqos;
        r.ar.region = f.arregion; r.ar_valid = f.arvalid; r.r_ready  = f.rready;
        return r;
    endfunction

    function automatic vip_resp_t vip_rsp_from_flat(input ni_signals_pkg::axi_rsp_t f);
        vip_resp_t r;
        r = '0;
        r.aw_ready = f.awready; r.w_ready = f.wready;
        r.b.id     = f.bid;     r.b.resp  = f.bresp;  r.b_valid = f.bvalid;
        r.ar_ready = f.arready;
        r.r.id     = f.rid;     r.r.data  = f.rdata;  r.r.resp  = f.rresp;
        r.r.last   = f.rlast;   r.r_valid = f.rvalid;
        return r;
    endfunction

endpackage

`endif  // AXI_VIP_TYPES_PKG_SV
