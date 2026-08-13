// Type binding for pulp iDMA (sim/dv/idma-0.6.5) at this project's widths.
// idma_req_t/idma_rsp_t are macro-built, not package types upstream, so the
// backend cannot be instantiated without a file like this one. Promoted from
// the elaboration probe .superpowers/sdd/ai-dataflow/idma_lint_top.sv.
`include "axi/typedef.svh"
`include "idma/typedef.svh"

`ifndef IDMA_TYPES_PKG_SV
`define IDMA_TYPES_PKG_SV

package idma_types_pkg;

    localparam int unsigned DATA_WIDTH   = ni_params_pkg::AXI_DATA_WIDTH_DFLT;
    localparam int unsigned ADDR_WIDTH   = ni_params_pkg::AXI_ADDR_WIDTH_DFLT;
    // The tile crossbar's slave port, which is what the DMA drives.
    localparam int unsigned AXI_ID_WIDTH = ni_params_pkg::AXI_INITIATOR_ID_WIDTH_DFLT;
    localparam int unsigned USER_WIDTH   = ni_params_pkg::AXI_AWUSER_WIDTH_DFLT;
    // Transfer length in bytes. 20 covers a whole 0x100000 tile window; the
    // legal range is 12 to ADDR_WIDTH.
    localparam int unsigned TF_LEN_WIDTH = 20;

    typedef logic [ADDR_WIDTH-1:0]   addr_t;
    typedef logic [DATA_WIDTH-1:0]   data_t;
    typedef logic [DATA_WIDTH/8-1:0] strb_t;
    typedef logic [USER_WIDTH-1:0]   user_t;
    typedef logic [AXI_ID_WIDTH-1:0] id_t;
    typedef logic [TF_LEN_WIDTH-1:0] tf_len_t;

    `AXI_TYPEDEF_ALL(axi, addr_t, id_t, data_t, strb_t, user_t)

    `IDMA_TYPEDEF_OPTIONS_T(options_t, id_t)
    `IDMA_TYPEDEF_REQ_T(idma_req_t, tf_len_t, addr_t, options_t, user_t)
    `IDMA_TYPEDEF_ERR_PAYLOAD_T(err_payload_t, addr_t)
    `IDMA_TYPEDEF_RSP_T(idma_rsp_t, err_payload_t)

    // The backend's meta channels. rw_axi carries one protocol per direction,
    // so each wrapper holds a single AXI address channel.
    typedef struct packed { axi_ar_chan_t ar_chan; } axi_read_meta_channel_t;
    typedef struct packed { axi_read_meta_channel_t axi; } read_meta_channel_t;
    typedef struct packed { axi_aw_chan_t aw_chan; } axi_write_meta_channel_t;
    typedef struct packed { axi_write_meta_channel_t axi; } write_meta_channel_t;

endpackage

`endif  // IDMA_TYPES_PKG_SV
