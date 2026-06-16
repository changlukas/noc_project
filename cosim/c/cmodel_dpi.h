// DPI signatures for Stage 5b wire-wrap co-sim. 5 shells x 3 calls/cycle
// (set_inputs/tick/get_outputs) + lifecycle (init/finalize/check_error).
//
// Error propagation: try/catch in handlers sets g_dpi_error_code; SV side
// polls cmodel_check_error() per shell per cycle and raises $fatal on
// non-zero. See spec §5.2.

#ifndef COSIM2_CMODEL_DPI_H
#define COSIM2_CMODEL_DPI_H

#include "svdpi.h"

#ifdef __cplusplus
extern "C" {
#endif

// Categorized DPI error codes (return value of cmodel_check_error).
// Per Stage 5b spec §5.2.
typedef enum {
    CMODEL_DPI_OK = 0,
    CMODEL_DPI_ERR_GENERIC = 1,
    CMODEL_DPI_ERR_NOT_INITIALIZED = 2,
    CMODEL_DPI_ERR_HERMETIC_VIOLATION = 3,
    CMODEL_DPI_ERR_BACKPRESSURE = 4,
    CMODEL_DPI_ERR_INJECT_BAD_MODE = 5,
    CMODEL_DPI_ERR_REINIT_FORBIDDEN = 6,
    CMODEL_DPI_ERR_UNKNOWN = 99
} cmodel_dpi_error_e;

// Lifecycle — session state machine (init/finalize) + per-instance *_create.
void cmodel_init(const char* scenario_yaml_path);
void cmodel_finalize(void);
int cmodel_check_error(const char** msg);
// Scenario completion + scoreboard query (polled by tb_top.sv exit logic).
int cmodel_done(void);
int cmodel_scoreboard_clean(void);
void cmodel_dump_scoreboard(void);

// Per-shell DPI signatures appended by Tasks 5-11.
// ChannelModel (Task 5) — NoC-only, simplest shell; first chandle migration:
void* cmodel_channel_model_create(const char* name);
void cmodel_channel_model_set_inputs(void* ctx, svBit req_in_valid, svBitVecVal* req_in_flit,
                                     svBit req_in_credit_return, svBit rsp_in_valid,
                                     svBitVecVal* rsp_in_flit, svBit rsp_in_credit_return);
void cmodel_channel_model_tick(void* ctx);
void cmodel_channel_model_get_outputs(void* ctx, svBit* req_out_valid, svBitVecVal* req_out_flit,
                                      svBit* req_out_credit_return, svBit* rsp_out_valid,
                                      svBitVecVal* rsp_out_flit, svBit* rsp_out_credit_return);

// RouterChannel (Task 3) — 2-node bidirectional NoC fabric; pins suffixed _n0/_n1.
void* cmodel_router_channel_create(const char* name);
void cmodel_router_channel_set_inputs(void* ctx, svBit req_in_valid_n0, svBitVecVal* req_in_flit_n0,
                                      svBit req_in_credit_return_n0, svBit rsp_in_valid_n0,
                                      svBitVecVal* rsp_in_flit_n0, svBit rsp_in_credit_return_n0,
                                      svBit req_in_valid_n1, svBitVecVal* req_in_flit_n1,
                                      svBit req_in_credit_return_n1, svBit rsp_in_valid_n1,
                                      svBitVecVal* rsp_in_flit_n1, svBit rsp_in_credit_return_n1);
void cmodel_router_channel_tick(void* ctx);
void cmodel_router_channel_get_outputs(
    void* ctx, svBit* req_out_valid_n0, svBitVecVal* req_out_flit_n0,
    svBit* req_out_credit_return_n0, svBit* rsp_out_valid_n0, svBitVecVal* rsp_out_flit_n0,
    svBit* rsp_out_credit_return_n0, svBit* req_out_valid_n1, svBitVecVal* req_out_flit_n1,
    svBit* req_out_credit_return_n1, svBit* rsp_out_valid_n1, svBitVecVal* rsp_out_flit_n1,
    svBit* rsp_out_credit_return_n1);

// AxiMaster (Task 6) — chandle ABI; per-instance dump path + scoreboard wiring.
// Packing: svBitVecVal* for multi-bit fields (little-endian word order).
//   id fields     : 1 word (8-bit value in low byte)
//   addr fields   : 2 words (64-bit, word[0] = bits[31:0], word[1] = bits[63:32])
//   data fields   : 8 words (256-bit bus = 8 x 32-bit words, little-endian)
//   wstrb         : 1 word (32-bit strobe)
//   other attribs : 1 word each (low bits used per width)
void* cmodel_master_create(const char* name);
void cmodel_master_set_inputs(void* ctx, svBit awready, svBit wready, svBit arready, svBit bvalid,
                              svBitVecVal* bid, svBitVecVal* bresp, svBit rvalid, svBitVecVal* rid,
                              svBitVecVal* rdata, svBitVecVal* rresp, svBit rlast);
void cmodel_master_tick(void* ctx);
void cmodel_master_get_outputs(void* ctx, svBit* awvalid, svBitVecVal* awid, svBitVecVal* awaddr,
                               svBitVecVal* awlen, svBitVecVal* awsize, svBitVecVal* awburst,
                               svBit* awlock, svBitVecVal* awcache, svBitVecVal* awprot,
                               svBitVecVal* awqos, svBit* wvalid, svBitVecVal* wdata,
                               svBitVecVal* wstrb, svBit* wlast, svBit* bready, svBit* arvalid,
                               svBitVecVal* arid, svBitVecVal* araddr, svBitVecVal* arlen,
                               svBitVecVal* arsize, svBitVecVal* arburst, svBit* arlock,
                               svBitVecVal* arcache, svBitVecVal* arprot, svBitVecVal* arqos,
                               svBit* rready);

// AxiSlave (Task 7) — chandle ABI; accepts AW/W/AR from master wire; drives
// awready/wready/arready handshake + B/R response channels back to master.
// Packing: same conventions as cmodel_master_* (little-endian word order).
//   id fields     : 1 word (8-bit value in low byte)
//   addr fields   : 2 words (64-bit, word[0] = bits[31:0], word[1] = bits[63:32])
//   data fields   : 8 words (256-bit bus = 8 x 32-bit words, little-endian)
//   wstrb         : 1 word (32-bit strobe)
//   other attribs : 1 word each (low bits used per width)
void* cmodel_slave_create(const char* name);
void cmodel_slave_set_inputs(void* ctx, svBit awvalid, svBitVecVal* awid, svBitVecVal* awaddr,
                             svBitVecVal* awlen, svBitVecVal* awsize, svBitVecVal* awburst,
                             svBit awlock, svBitVecVal* awcache, svBitVecVal* awprot,
                             svBitVecVal* awqos, svBit wvalid, svBitVecVal* wdata,
                             svBitVecVal* wstrb, svBit wlast, svBit arvalid, svBitVecVal* arid,
                             svBitVecVal* araddr, svBitVecVal* arlen, svBitVecVal* arsize,
                             svBitVecVal* arburst, svBit arlock, svBitVecVal* arcache,
                             svBitVecVal* arprot, svBitVecVal* arqos, svBit bready, svBit rready);
void cmodel_slave_tick(void* ctx);
void cmodel_slave_get_outputs(void* ctx, svBit* awready, svBit* wready, svBit* arready,
                              svBit* bvalid, svBitVecVal* bid, svBitVecVal* bresp, svBit* rvalid,
                              svBitVecVal* rid, svBitVecVal* rdata, svBitVecVal* rresp,
                              svBit* rlast);

// Nmu (Task 8) — chandle ABI; AXI slave side + NoC req/rsp sides.
// Packing conventions (same as cmodel_slave_*):
//   id fields     : 1 word (8-bit value in low byte)
//   addr fields   : 2 words (64-bit, word[0] = bits[31:0], word[1] = bits[63:32])
//   data fields   : 8 words (256-bit bus = 8 x 32-bit words, little-endian)
//   wstrb         : 1 word (32-bit strobe)
//   flit fields   : FLIT_VEC_WORDS = 13 words (408-bit flit, little-endian)
//   other attribs : 1 word each (low bits used per width)
void* cmodel_nmu_create(const char* name);
void cmodel_nmu_set_inputs(void* ctx, svBit awvalid, svBitVecVal* awid, svBitVecVal* awaddr,
                           svBitVecVal* awlen, svBitVecVal* awsize, svBitVecVal* awburst,
                           svBit awlock, svBitVecVal* awcache, svBitVecVal* awprot,
                           svBitVecVal* awqos, svBit wvalid, svBitVecVal* wdata, svBitVecVal* wstrb,
                           svBit wlast, svBit bready, svBit arvalid, svBitVecVal* arid,
                           svBitVecVal* araddr, svBitVecVal* arlen, svBitVecVal* arsize,
                           svBitVecVal* arburst, svBit arlock, svBitVecVal* arcache,
                           svBitVecVal* arprot, svBitVecVal* arqos, svBit rready,
                           svBit noc_rsp_valid, svBitVecVal* noc_rsp_flit,
                           svBit noc_req_credit_return);
void cmodel_nmu_tick(void* ctx);
void cmodel_nmu_get_outputs(void* ctx, svBit* awready, svBit* wready, svBit* arready, svBit* bvalid,
                            svBitVecVal* bid, svBitVecVal* bresp, svBit* rvalid, svBitVecVal* rid,
                            svBitVecVal* rdata, svBitVecVal* rresp, svBit* rlast,
                            svBit* noc_req_valid, svBitVecVal* noc_req_flit,
                            svBit* noc_rsp_credit_return);

// Nsu (Task 9) — NoC consumer (req in) + producer (rsp out) + AXI master side.
// Direction inversion vs. Nmu:
//   cmodel_nsu_set_inputs receives noc_req_flit (consumer) + AXI master ready/B/R.
//   cmodel_nsu_get_outputs produces noc_rsp_flit (producer) + AXI master AW/W/AR.
// Packing conventions (same as cmodel_nmu_*):
//   id fields     : 1 word (8-bit value in low byte)
//   addr fields   : 2 words (64-bit, word[0] = bits[31:0], word[1] = bits[63:32])
//   data fields   : 8 words (256-bit bus = 8 x 32-bit words, little-endian)
//   wstrb         : 1 word (32-bit strobe)
//   flit fields   : FLIT_VEC_WORDS = 13 words (408-bit flit, little-endian)
//   other attribs : 1 word each (low bits used per width)
void* cmodel_nsu_create(const char* name);
void cmodel_nsu_set_inputs(void* ctx, svBit noc_req_valid, svBitVecVal* noc_req_flit,
                           svBit noc_rsp_credit_return, svBit awready, svBit wready, svBit bvalid,
                           svBitVecVal* bid, svBitVecVal* bresp, svBit arready, svBit rvalid,
                           svBitVecVal* rid, svBitVecVal* rdata, svBitVecVal* rresp, svBit rlast);
void cmodel_nsu_tick(void* ctx);
void cmodel_nsu_get_outputs(void* ctx, svBit* noc_rsp_valid, svBitVecVal* noc_rsp_flit,
                            svBit* noc_req_credit_return, svBit* awvalid, svBitVecVal* awid,
                            svBitVecVal* awaddr, svBitVecVal* awlen, svBitVecVal* awsize,
                            svBitVecVal* awburst, svBit* awlock, svBitVecVal* awcache,
                            svBitVecVal* awprot, svBitVecVal* awqos, svBit* wvalid,
                            svBitVecVal* wdata, svBitVecVal* wstrb, svBit* wlast, svBit* bready,
                            svBit* arvalid, svBitVecVal* arid, svBitVecVal* araddr,
                            svBitVecVal* arlen, svBitVecVal* arsize, svBitVecVal* arburst,
                            svBit* arlock, svBitVecVal* arcache, svBitVecVal* arprot,
                            svBitVecVal* arqos, svBit* rready);

#ifdef __cplusplus
}
#endif

#endif  // COSIM2_CMODEL_DPI_H
