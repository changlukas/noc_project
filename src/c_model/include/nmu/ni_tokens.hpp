#ifndef NI_CMODEL_NI_TOKENS_HPP
#define NI_CMODEL_NI_TOKENS_HPP
#include "axi/types.hpp"
#include <cstdint>
namespace ni::cmodel {
// NMU req S1->S2: AW/AR admitted by Rob with route+rob meta computed in S1.
// Field set mirrors nmu::AwHeaderMeta (packetize.hpp): dst_id, local_addr,
// rob_req, rob_idx are all uint8_t to match the actual struct definition.
// NOTE: brief sketch used uint16_t for rob_idx; actual AwHeaderMeta uses
// uint8_t — following the source-of-truth in packetize.hpp.
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
