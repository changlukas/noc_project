// DPI export signatures. Auto-imported by cosim/sv/nmu_cmodel_proxy.sv and
// nsu_cmodel_proxy.sv via `import "DPI-C" function ...` declarations.
//
// Address width: AXI_ADDR_WIDTH = 64 bits.
//   Verilator maps bit [63:0] → svBitVecVal[2] (two 32-bit words, little-endian
//   word order: [0] = bits[31:0], [1] = bits[63:32]).
//
// Data width: NOC_DATA_WIDTH / 8 = 32 bytes = 256 bits.
//   Verilator maps bit [255:0] → svBitVecVal[8] (eight 32-bit words,
//   same little-endian word order).
//
// WSTRB width: 32 bits → single svBitVecVal.
#ifndef COSIM_AXI_DPI_H
#define COSIM_AXI_DPI_H

#include "svdpi.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void cmodel_init(const char* scenario_yaml_path);
void cmodel_finalize(void);
void cmodel_tick(void);
int cmodel_done(void);
int cmodel_scoreboard_clean(void);

// ---------------------------------------------------------------------------
// NMU per-channel pin getters
//
// SV side passes output pointers; C fills them.
// addr:  svBitVecVal[2]  — [0]=bits[31:0], [1]=bits[63:32]
// data:  svBitVecVal[8]  — [0]=bytes[3:0], ..., [7]=bytes[31:28]
// strb:  svBitVecVal     — 32-bit WSTRB, fits in one word
// Multi-bit narrow fields (id/len/size/burst/cache/prot/qos/resp) fit in one
// svBitVecVal each. Single-bit fields (valid/ready/last/lock) use svBit.
// ---------------------------------------------------------------------------

void cmodel_nmu_get_aw(svBit* valid, svBit* ready, svBitVecVal* id, svBitVecVal* addr,
                       svBitVecVal* len, svBitVecVal* size, svBitVecVal* burst, svBit* lock,
                       svBitVecVal* cache, svBitVecVal* prot, svBitVecVal* qos);

void cmodel_nmu_get_w(svBit* valid, svBit* ready, svBitVecVal* data, svBitVecVal* strb,
                      svBit* last);

void cmodel_nmu_get_ar(svBit* valid, svBit* ready, svBitVecVal* id, svBitVecVal* addr,
                       svBitVecVal* len, svBitVecVal* size, svBitVecVal* burst, svBit* lock,
                       svBitVecVal* cache, svBitVecVal* prot, svBitVecVal* qos);

void cmodel_nmu_get_b(svBit* valid, svBit* ready, svBitVecVal* id, svBitVecVal* resp);

void cmodel_nmu_get_r(svBit* valid, svBit* ready, svBitVecVal* id, svBitVecVal* data,
                      svBitVecVal* resp, svBit* last);

// ---------------------------------------------------------------------------
// NSU per-channel pin getters (same signature shape as NMU)
// ---------------------------------------------------------------------------

void cmodel_nsu_get_aw(svBit* valid, svBit* ready, svBitVecVal* id, svBitVecVal* addr,
                       svBitVecVal* len, svBitVecVal* size, svBitVecVal* burst, svBit* lock,
                       svBitVecVal* cache, svBitVecVal* prot, svBitVecVal* qos);

void cmodel_nsu_get_w(svBit* valid, svBit* ready, svBitVecVal* data, svBitVecVal* strb,
                      svBit* last);

void cmodel_nsu_get_ar(svBit* valid, svBit* ready, svBitVecVal* id, svBitVecVal* addr,
                       svBitVecVal* len, svBitVecVal* size, svBitVecVal* burst, svBit* lock,
                       svBitVecVal* cache, svBitVecVal* prot, svBitVecVal* qos);

void cmodel_nsu_get_b(svBit* valid, svBit* ready, svBitVecVal* id, svBitVecVal* resp);

void cmodel_nsu_get_r(svBit* valid, svBit* ready, svBitVecVal* id, svBitVecVal* data,
                      svBitVecVal* resp, svBit* last);

#ifdef __cplusplus
}
#endif

#endif  // COSIM_AXI_DPI_H
