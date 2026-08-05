// DPI signatures for the wire-wrap co-sim. 6 wraps x 3 calls/cycle
// (set_inputs/tick/get_outputs) + lifecycle (init/finalize/check_error).
//
// Three physical networks (S3a T5, spec §4.3): REQ / RSP use ready/valid
// (scalar per port, single-VC per S1 Q2); DAT uses credit (per-VC pulse
// vector, unchanged mechanism). Pin naming, node's own view: tx_* = this
// node's transmit side, rx_* = receive side (mechanical `*_out_*`/`*_in_*`
// rename).
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
//
// Router (per-node) — ONE node's REQ + RSP + DAT routers at (x,y). Every
// network's pins are ONE uniform per-PORT array (router_wrap_io.hpp
// ROUTER_LINK_PORTS = 5: LOCAL + N/E/S/W; LOCAL carries this node's own
// NMU/NSU traffic, N/E/S/W the inter-router links — unused directions stay
// unwired, driving 0). dat_num_vc threads the topology VC count into the DAT
// router (REQ/RSP are fixed single-VC, S1 Q2).
//
// Per-PORT marshalling (fixed shape):
//   - REQ/RSP valid/ready are per-port packed bit-vectors [LINK_PORTS-1:0],
//     ONE svBitVecVal word (scalar per port, single-VC, spec TXREQREADY).
//   - REQ/RSP flit arrays are <NET>_VEC_WORDS-per-port, contiguous
//     (port-major): word index = port * <NET>_VEC_WORDS + w. REQ
//     <NET>_VEC_WORDS = 5 (137 b); RSP = 4 (127 b) — see dpi_marshal.hpp
//     ReqFlitMarshal/RspFlitMarshal.
//   - DAT valid/flit mirror the pre-S3a LINK-only shape (DAT_VEC_WORDS = 20,
//     629 b) now applied uniformly to LOCAL too; DAT credit
//     (tx_dat_crdvalid/rx_dat_crdvalid) is per-VC: ONE svBitVecVal word per
//     port (bit vc = credit pulse on VC vc), valid for dat_num_vc <=
//     2^VC_ID_WIDTH = 8.
unsigned long long cmodel_router_create(const char* name, int x_coord, int y_coord, int mesh_x_dim,
                                        int mesh_y_dim, int dat_num_vc);
void cmodel_router_set_inputs(unsigned long long ctx, svBitVecVal* rx_req_valid,
                              svBitVecVal* rx_req_flit, svBitVecVal* tx_req_ready,
                              svBitVecVal* rx_rsp_valid, svBitVecVal* rx_rsp_flit,
                              svBitVecVal* tx_rsp_ready, svBitVecVal* rx_dat_valid,
                              svBitVecVal* rx_dat_flit, svBitVecVal* tx_dat_crdvalid);
void cmodel_router_tick(unsigned long long ctx);
void cmodel_router_get_outputs(unsigned long long ctx, svBitVecVal* tx_req_valid,
                               svBitVecVal* tx_req_flit, svBitVecVal* rx_req_ready,
                               svBitVecVal* tx_rsp_valid, svBitVecVal* tx_rsp_flit,
                               svBitVecVal* rx_rsp_ready, svBitVecVal* tx_dat_valid,
                               svBitVecVal* tx_dat_flit, svBitVecVal* rx_dat_crdvalid);

// DatMerge — NI-level DAT LOCAL-port merge point (S3a T5, controller ruling,
// translate of floo_nw_chimney.sv's wide-link merge). One instance per node,
// created in ni_wrap.sv, sitting between nmu_wrap/nsu_wrap's DAT pins and
// router_wrap's DAT LOCAL port. See wrap/dat_merge_wrap.hpp for the full
// rationale. dat_num_vc mirrors cmodel_router_create's / cmodel_nmu_create's
// same-named parameter (the DAT face's VC count; REQ/RSP have no analog
// here, DatMerge is DAT-only).
// Egress (NMU DataAw/W + NSU DataR -> router LOCAL rx): nmu_tx_dat_*/
// nsu_tx_dat_* are the two producer pushes; tx_dat_crdvalid is the router's
// credit-return for our sends (per-VC, one word, bit=vc); tx_dat_valid/flit
// (outputs) is our merged send toward the router.
// Ingress (router LOCAL tx -> NMU DataR / NSU DataAw+W): rx_dat_valid/flit is
// the router's ejected flit; nmu_rx_dat_*/nsu_rx_dat_* (outputs) is the
// axi_ch-demuxed delivery; rx_dat_crdvalid (output) is our credit-return to
// the router; nmu_tx_dat_crdvalid/nsu_tx_dat_crdvalid (outputs) are our
// credit-return to each producer for what we drained from their pending
// stage.
unsigned long long cmodel_dat_merge_create(const char* name, int dat_num_vc);
void cmodel_dat_merge_set_inputs(unsigned long long ctx, svBit nmu_tx_dat_valid,
                                 svBitVecVal* nmu_tx_dat_flit, svBit nsu_tx_dat_valid,
                                 svBitVecVal* nsu_tx_dat_flit, svBitVecVal* tx_dat_crdvalid,
                                 svBit rx_dat_valid, svBitVecVal* rx_dat_flit);
void cmodel_dat_merge_tick(unsigned long long ctx);
void cmodel_dat_merge_get_outputs(unsigned long long ctx, svBitVecVal* nmu_tx_dat_crdvalid,
                                  svBit* nmu_rx_dat_valid, svBitVecVal* nmu_rx_dat_flit,
                                  svBitVecVal* nsu_tx_dat_crdvalid, svBit* nsu_rx_dat_valid,
                                  svBitVecVal* nsu_rx_dat_flit, svBit* tx_dat_valid,
                                  svBitVecVal* tx_dat_flit, svBitVecVal* rx_dat_crdvalid);

// Nmu — longint-handle ABI (chandle avoided; VCS rejects it as a module
// port); AXI slave side + three NoC faces (REQ egress ready/valid, RSP
// ingress ready/valid, DAT ingress+egress credit).
// Packing conventions (little-endian word order; word counts derived from
// ni::FLIT_WIDTH / axi::DATA_WIDTH in src/dpi/dpi_marshal.hpp):
//   id fields     : 1 word (8-bit value in low byte)
//   addr fields   : 2 words (64-bit, word[0] = bits[31:0], word[1] = bits[63:32])
//   data fields   : DATA_VEC_WORDS = 16 words (512-bit bus, little-endian)
//   wstrb         : WSTRB_VEC_WORDS = 2 words (64-bit strobe)
//   flit fields   : DAT_VEC_WORDS = 20 words (629-bit flit, little-endian,
//                   tail word explicitly masked to the last DAT_FLIT_WIDTH bits)
//   other attribs : 1 word each (low bits used per width)
// dat_num_vc threads the topology VC count into the NmuConfig DAT face
// (REQ/RSP are fixed single-VC, S1 Q2 — no VC/vnet split on them anymore).
// tx_dat_crdvalid / rx_dat_crdvalid are per-VC: ONE svBitVecVal word, bit vc
// = credit pulse on VC vc.
// config_path: topology YAML with an `address_map` block (NULL/empty ->
// legacy 16x16 uniform, no-rebase SAM).
// outstanding_depth: shared outstanding pool size per direction (FlooNoC MaxTxns).
// AW and AR pools are independent, each shared across all AXI ids, and the limit
// applies in both ROB modes -- it is the master-side injection budget.
unsigned long long cmodel_nmu_create(const char* name, int src_id, int dat_num_vc,
                                     const char* config_path);
unsigned long long cmodel_nmu_create_ex(const char* name, int src_id, int dat_num_vc,
                                        int rob_enabled, int b_rob_depth, int r_rob_depth,
                                        int max_txns_per_id, int outstanding_depth,
                                        const char* config_path);
void cmodel_nmu_set_inputs(unsigned long long ctx, svBit awvalid, svBitVecVal* awid,
                           svBitVecVal* awaddr, svBitVecVal* awlen, svBitVecVal* awsize,
                           svBitVecVal* awburst, svBit awlock, svBitVecVal* awcache,
                           svBitVecVal* awprot, svBitVecVal* awqos, svBit wvalid,
                           svBitVecVal* wdata, svBitVecVal* wstrb, svBit wlast, svBit bready,
                           svBit arvalid, svBitVecVal* arid, svBitVecVal* araddr,
                           svBitVecVal* arlen, svBitVecVal* arsize, svBitVecVal* arburst,
                           svBit arlock, svBitVecVal* arcache, svBitVecVal* arprot,
                           svBitVecVal* arqos, svBit rready, svBit tx_req_ready, svBit rx_rsp_valid,
                           svBitVecVal* rx_rsp_flit, svBit rx_dat_valid, svBitVecVal* rx_dat_flit,
                           svBitVecVal* tx_dat_crdvalid);
void cmodel_nmu_tick(unsigned long long ctx);
void cmodel_nmu_get_outputs(unsigned long long ctx, svBit* awready, svBit* wready, svBit* arready,
                            svBit* bvalid, svBitVecVal* bid, svBitVecVal* bresp, svBit* rvalid,
                            svBitVecVal* rid, svBitVecVal* rdata, svBitVecVal* rresp, svBit* rlast,
                            svBit* tx_req_valid, svBitVecVal* tx_req_flit, svBit* rx_rsp_ready,
                            svBit* tx_dat_valid, svBitVecVal* tx_dat_flit,
                            svBitVecVal* rx_dat_crdvalid);
// Peak R-RoB slot occupancy (Rob::read_slot_hwm) — sizing telemetry. 0 if the
// handle is invalid or RoB is Disabled.
unsigned int cmodel_nmu_read_slot_hwm(unsigned long long ctx);

// Nsu — three NoC faces (REQ ingress ready/valid, RSP egress ready/valid,
// DAT ingress+egress credit) + AXI master side.
// Direction inversion vs. Nmu:
//   cmodel_nsu_set_inputs receives rx_req_* (ingress) + AXI master ready/B/R.
//   cmodel_nsu_get_outputs produces tx_rsp_* (egress) + AXI master AW/W/AR.
// Packing conventions (same as cmodel_nmu_*):
//   id fields     : 1 word (8-bit value in low byte)
//   addr fields   : 2 words (64-bit, word[0] = bits[31:0], word[1] = bits[63:32])
//   data fields   : DATA_VEC_WORDS = 16 words (512-bit bus = 16 x 32-bit words, little-endian)
//   wstrb         : WSTRB_VEC_WORDS = 2 words (64-bit strobe)
//   flit fields   : DAT_VEC_WORDS = 20 words (629-bit flit, little-endian,
//                   tail word explicitly masked to the last DAT_FLIT_WIDTH bits)
//   other attribs : 1 word each (low bits used per width)
// dat_num_vc threads the topology VC count into the NsuConfig DAT face
// (REQ/RSP are fixed single-VC, S1 Q2). tx_dat_crdvalid / rx_dat_crdvalid
// are per-VC: ONE svBitVecVal word, bit vc = credit pulse on VC vc.
// max_unique_ids: 1 collapses every master onto the all-ones downstream AXI id
// (FlooNoC's ChimneyDefaultCfg); 256 passes the master's id through. No other
// value is legal, and the Depacketize constructor asserts it.
// max_outstanding: shared MetaBuffer pool size per direction (FlooNoC MaxTxns).
unsigned long long cmodel_nsu_create(const char* name, int src_id, int dat_num_vc,
                                     int max_unique_ids, int max_outstanding);
void cmodel_nsu_set_inputs(unsigned long long ctx, svBit rx_req_valid, svBitVecVal* rx_req_flit,
                           svBit tx_rsp_ready, svBit rx_dat_valid, svBitVecVal* rx_dat_flit,
                           svBitVecVal* tx_dat_crdvalid, svBit awready, svBit wready, svBit bvalid,
                           svBitVecVal* bid, svBitVecVal* bresp, svBit arready, svBit rvalid,
                           svBitVecVal* rid, svBitVecVal* rdata, svBitVecVal* rresp, svBit rlast);
void cmodel_nsu_tick(unsigned long long ctx);
void cmodel_nsu_get_outputs(unsigned long long ctx, svBit* rx_req_ready, svBit* tx_rsp_valid,
                            svBitVecVal* tx_rsp_flit, svBit* tx_dat_valid, svBitVecVal* tx_dat_flit,
                            svBitVecVal* rx_dat_crdvalid, svBit* awvalid, svBitVecVal* awid,
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
