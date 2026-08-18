#include "cmodel_dpi.h"

#include <algorithm>

namespace {

svBit req_ready = 0;
unsigned int req_sent = 0;
constexpr unsigned int kRequestCount = 16;

void clear_words(svBitVecVal* value, int words) {
    std::fill(value, value + words, 0U);
}

}  // namespace

extern "C" void cmodel_nmu_set_inputs(
    unsigned long long, svBit, svBitVecVal*, svBitVecVal*, svBitVecVal*, svBitVecVal*,
    svBitVecVal*, svBit, svBitVecVal*, svBitVecVal*, svBitVecVal*, svBitVecVal*, svBit,
    svBitVecVal*, svBitVecVal*, svBit, svBit, svBit, svBitVecVal*, svBitVecVal*,
    svBitVecVal*, svBitVecVal*, svBitVecVal*, svBit, svBitVecVal*, svBitVecVal*,
    svBitVecVal*, svBit, svBit tx_req_ready, svBit, svBitVecVal*, svBit, svBitVecVal*,
    svBitVecVal*) {
    req_ready = tx_req_ready;
}

extern "C" void cmodel_nmu_tick(unsigned long long) {}

extern "C" void cmodel_nmu_get_outputs(
    unsigned long long, svBit* awready, svBit* wready, svBit* arready, svBit* bvalid,
    svBitVecVal* bid, svBitVecVal* bresp, svBit* rvalid, svBitVecVal* rid,
    svBitVecVal* rdata, svBitVecVal* rresp, svBit* rlast, svBit* tx_req_valid,
    svBitVecVal* tx_req_flit, svBit* rx_rsp_ready, svBit* tx_dat_valid,
    svBitVecVal* tx_dat_flit, svBitVecVal* rx_dat_crdvalid) {
    *awready = 0;
    *wready = 0;
    *arready = 0;
    *bvalid = 0;
    clear_words(bid, 1);
    clear_words(bresp, 1);
    *rvalid = 0;
    clear_words(rid, 1);
    clear_words(rdata, 16);
    clear_words(rresp, 1);
    *rlast = 0;
    *rx_rsp_ready = 0;
    *tx_dat_valid = 0;
    clear_words(tx_dat_flit, 20);
    clear_words(rx_dat_crdvalid, 1);

    clear_words(tx_req_flit, 5);
    *tx_req_valid = req_ready && req_sent < kRequestCount;
    if (*tx_req_valid) {
        tx_req_flit[0] = 0x89abcdefU + req_sent;
        tx_req_flit[1] = 0x01234567U;
        tx_req_flit[2] = 0x76543210U;
        tx_req_flit[3] = 0xfedcba98U;
        tx_req_flit[4] = 0x5aU;
        ++req_sent;
    }
}
