// DPI signatures for the wire-wrap co-sim. 6 wraps x 3 calls/cycle
// (set_inputs/tick/get_outputs) + lifecycle (init/finalize/check_error).
//
// Error propagation: try/catch in handlers sets g_dpi_error_code; SV side
// polls cmodel_check_error() per wrap per cycle and raises $fatal on
// non-zero.

#ifndef NI_COSIM_CMODEL_DPI_H
#define NI_COSIM_CMODEL_DPI_H

#include "svdpi.h"

#ifdef __cplusplus
extern "C" {
#endif

// Categorized DPI error codes (return value of cmodel_check_error).
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
void cmodel_init(void);
void cmodel_finalize(void);
int cmodel_check_error(const char** msg);

// Perf instrumentation — cmodel_perf_link pushes per-link counters;
// cmodel_perf_sample_tick is called once per clock to snapshot router occupancy.
void cmodel_perf_link(const char* name, long long flit_count, long long stall_cyc);
void cmodel_perf_sample_tick(void);
void cmodel_perf_dump(const char* path);
void cmodel_perf_set_run(const char* scenario, long long total_cyc);

// Watchdog forensics: print every non-idle piece of fabric state (router
// FIFO/credit/wormhole, NMU/NSU stage occupancy and in-flight trackers) to
// stdout. Read-only; the tb watchdog calls it once before $fatal.
void cmodel_dump_fabric_state(void);

// Per-wrap DPI signatures, one block per component.
// Router (per-node) — ONE node's REQ+RSP routers at (x,y). Pins split:
//   NMU/NSU-facing (NI edge, pulse credit) + per-DIRECTION LINK (pulse credit).
// num_vc threads the topology VC count into the wrap config (NOT hardcoded 1).
//
// Per-PORT x per-VC ABI (fixed shape; unused directions stay unwired):
//   - LINK valid/flit/credit are PORT-indexed: SV passes packed arrays sized
//     ROUTER_LINK_PORTS (= router's 5 ports; LOCAL slot unused on the LINK face,
//     N/E/S/W carry the inter-router links). At 2-node only one direction is live.
//   - All credit_return fields are per-VC: marshalled as ONE svBitVecVal word
//     (bit vc = credit pulse on VC vc), valid for num_vc <= 2^VC_ID_WIDTH = 8.
//   - link_*_flit arrays are FLIT_VEC_WORDS-per-port, contiguous (port-major):
//     word index = port * FLIT_VEC_WORDS + w.
//   - link credit arrays are ONE word per port (port-major, bit vc per word).
unsigned long long cmodel_router_create(const char* name, int x_coord, int y_coord, int mesh_x_dim,
                                        int mesh_y_dim, int num_vc);
void cmodel_router_set_inputs(unsigned long long ctx, svBit req_in_valid, svBitVecVal* req_in_flit,
                              svBitVecVal* req_in_credit_return, svBit rsp_in_valid,
                              svBitVecVal* rsp_in_flit, svBitVecVal* rsp_in_credit_return,
                              svBitVecVal* link_req_out_credit, svBitVecVal* link_req_in_valid,
                              svBitVecVal* link_req_in_flit, svBitVecVal* link_rsp_out_credit,
                              svBitVecVal* link_rsp_in_valid, svBitVecVal* link_rsp_in_flit);
void cmodel_router_tick(unsigned long long ctx);
void cmodel_router_get_outputs(unsigned long long ctx, svBit* req_out_valid,
                               svBitVecVal* req_out_flit, svBitVecVal* req_out_credit_return,
                               svBit* rsp_out_valid, svBitVecVal* rsp_out_flit,
                               svBitVecVal* rsp_out_credit_return, svBitVecVal* link_req_out_valid,
                               svBitVecVal* link_req_out_flit, svBitVecVal* link_req_in_credit,
                               svBitVecVal* link_rsp_out_valid, svBitVecVal* link_rsp_out_flit,
                               svBitVecVal* link_rsp_in_credit);

// Nmu — longint-handle ABI (chandle avoided; VCS rejects it as a module
// port); AXI slave side + NoC req/rsp sides.
// Packing conventions (little-endian word order):
//   id fields     : 1 word (8-bit value in low byte)
//   addr fields   : 2 words (64-bit, word[0] = bits[31:0], word[1] = bits[63:32])
//   data fields   : 8 words (256-bit bus = 8 x 32-bit words, little-endian)
//   wstrb         : 1 word (32-bit strobe)
//   flit fields   : FLIT_VEC_WORDS = 13 words (396-bit flit, little-endian)
//   other attribs : 1 word each (low bits used per width)
// num_vc threads the topology VC count into the NmuConfig; make_virtual_networks(num_vc)
// splits it into disjoint write/read VC pools (lower half write, upper half read; num_vc==1
// shares VC0 for both), and each direction round-robins id-agnostically within its pool.
// noc_req_credit_return / noc_rsp_credit_return are per-VC: ONE svBitVecVal word, bit vc =
// credit pulse on VC vc.
// config_path: topology YAML with an `address_map` block (NULL/empty ->
// legacy 16x16 uniform, no-rebase SAM).
// outstanding_depth: shared outstanding pool size per direction (FlooNoC MaxTxns).
// AW and AR pools are independent, each shared across all AXI ids, and the limit
// applies in both ROB modes -- it is the master-side injection budget.
unsigned long long cmodel_nmu_create(const char* name, int src_id, int num_vc,
                                     const char* config_path);
unsigned long long cmodel_nmu_create_ex(const char* name, int src_id, int num_vc, int rob_enabled,
                                        int b_rob_depth, int r_rob_depth, int max_txns_per_id,
                                        int outstanding_depth, const char* config_path);
void cmodel_nmu_set_inputs(unsigned long long ctx, svBit awvalid, svBitVecVal* awid,
                           svBitVecVal* awaddr, svBitVecVal* awlen, svBitVecVal* awsize,
                           svBitVecVal* awburst, svBit awlock, svBitVecVal* awcache,
                           svBitVecVal* awprot, svBitVecVal* awqos, svBit wvalid,
                           svBitVecVal* wdata, svBitVecVal* wstrb, svBit wlast, svBit bready,
                           svBit arvalid, svBitVecVal* arid, svBitVecVal* araddr,
                           svBitVecVal* arlen, svBitVecVal* arsize, svBitVecVal* arburst,
                           svBit arlock, svBitVecVal* arcache, svBitVecVal* arprot,
                           svBitVecVal* arqos, svBit rready, svBit noc_rsp_valid,
                           svBitVecVal* noc_rsp_flit, svBitVecVal* noc_req_credit_return);
void cmodel_nmu_tick(unsigned long long ctx);
void cmodel_nmu_get_outputs(unsigned long long ctx, svBit* awready, svBit* wready, svBit* arready,
                            svBit* bvalid, svBitVecVal* bid, svBitVecVal* bresp, svBit* rvalid,
                            svBitVecVal* rid, svBitVecVal* rdata, svBitVecVal* rresp, svBit* rlast,
                            svBit* noc_req_valid, svBitVecVal* noc_req_flit,
                            svBitVecVal* noc_rsp_credit_return);
// Peak R-RoB slot occupancy (Rob::read_slot_hwm) — sizing telemetry. 0 if the
// handle is invalid or RoB is Disabled.
unsigned int cmodel_nmu_read_slot_hwm(unsigned long long ctx);

// Nsu — NoC consumer (req in) + producer (rsp out) + AXI master side.
// Direction inversion vs. Nmu:
//   cmodel_nsu_set_inputs receives noc_req_flit (consumer) + AXI master ready/B/R.
//   cmodel_nsu_get_outputs produces noc_rsp_flit (producer) + AXI master AW/W/AR.
// Packing conventions (same as cmodel_nmu_*):
//   id fields     : 1 word (8-bit value in low byte)
//   addr fields   : 2 words (64-bit, word[0] = bits[31:0], word[1] = bits[63:32])
//   data fields   : 8 words (256-bit bus = 8 x 32-bit words, little-endian)
//   wstrb         : 1 word (32-bit strobe)
//   flit fields   : FLIT_VEC_WORDS = 13 words (396-bit flit, little-endian)
//   other attribs : 1 word each (low bits used per width)
// num_vc threads the topology VC count into the NsuConfig (write_rsp_vc=0,
// read_rsp_vc=(num_vc>=2)?1:0 — read/write VC split). noc_rsp_credit_return / noc_req_credit_return
// are per-VC: ONE svBitVecVal word, bit vc = credit pulse on VC vc.
// max_unique_ids: 1 collapses every master onto the all-ones downstream AXI id
// (FlooNoC's ChimneyDefaultCfg); 256 passes the master's id through. No other
// value is legal, and the Depacketize constructor asserts it.
// max_outstanding: shared MetaBuffer pool size per direction (FlooNoC MaxTxns).
unsigned long long cmodel_nsu_create(const char* name, int src_id, int num_vc, int max_unique_ids,
                                     int max_outstanding);
void cmodel_nsu_set_inputs(unsigned long long ctx, svBit noc_req_valid, svBitVecVal* noc_req_flit,
                           svBitVecVal* noc_rsp_credit_return, svBit awready, svBit wready,
                           svBit bvalid, svBitVecVal* bid, svBitVecVal* bresp, svBit arready,
                           svBit rvalid, svBitVecVal* rid, svBitVecVal* rdata, svBitVecVal* rresp,
                           svBit rlast);
void cmodel_nsu_tick(unsigned long long ctx);
void cmodel_nsu_get_outputs(unsigned long long ctx, svBit* noc_rsp_valid, svBitVecVal* noc_rsp_flit,
                            svBitVecVal* noc_req_credit_return, svBit* awvalid, svBitVecVal* awid,
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

#endif  // NI_COSIM_CMODEL_DPI_H
