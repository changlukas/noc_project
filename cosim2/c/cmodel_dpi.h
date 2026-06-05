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
    CMODEL_DPI_OK                     = 0,
    CMODEL_DPI_ERR_GENERIC            = 1,
    CMODEL_DPI_ERR_NOT_INITIALIZED    = 2,
    CMODEL_DPI_ERR_HERMETIC_VIOLATION = 3,
    CMODEL_DPI_ERR_BACKPRESSURE       = 4,
    CMODEL_DPI_ERR_INJECT_BAD_MODE    = 5,
    CMODEL_DPI_ERR_UNKNOWN            = 99
} cmodel_dpi_error_e;

// Lifecycle (5 shell singletons, all-or-nothing init per spec §5.3)
void cmodel_init(const char* scenario_yaml_path);
void cmodel_finalize(void);
int  cmodel_check_error(const char** msg);

// Per-shell DPI signatures appended by Tasks 7-11.
// LoopbackNoc (Task 7) — NoC-only, simplest shell:
void cmodel_loopback_noc_set_inputs(svBit req_in_valid, svBitVecVal* req_in_flit,
                                     svBit req_in_credit_return,
                                     svBit rsp_in_valid, svBitVecVal* rsp_in_flit,
                                     svBit rsp_in_credit_return);
void cmodel_loopback_noc_tick(void);
void cmodel_loopback_noc_get_outputs(svBit* req_out_valid, svBitVecVal* req_out_flit,
                                      svBit* req_out_credit_return,
                                      svBit* rsp_out_valid, svBitVecVal* rsp_out_flit,
                                      svBit* rsp_out_credit_return);

#ifdef __cplusplus
}
#endif

#endif  // COSIM2_CMODEL_DPI_H
