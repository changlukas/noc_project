#ifndef NI_CMODEL_STAGED_BEATS_HPP
#define NI_CMODEL_STAGED_BEATS_HPP
#include "axi/types.hpp"
#include <cstdint>
namespace ni::cmodel {
// RoB-admitted request beats + route metadata: the S1->S2 stage payload.
// AW/AR admitted by Rob carry route+rob meta computed in S1. Field set
// mirrors nmu::AwHeaderMeta (packetize.hpp): dst_id, local_addr, rob_req,
// rob_idx are all uint8_t to match the actual struct definition.
struct AdmittedAw {
    axi::AwBeat beat;
    uint8_t dst_id;
    uint64_t local_addr;
    uint8_t rob_req;
    uint8_t rob_idx;
};
struct AdmittedAr {
    axi::ArBeat beat;
    uint8_t dst_id;
    uint64_t local_addr;
    uint8_t rob_req;
    uint8_t rob_idx;
};
// W carries AW-inherited meta (matches packetize.hpp w_meta_fifo_ contract).
struct AdmittedW {
    axi::WBeat beat;
};
}  // namespace ni::cmodel
#endif
