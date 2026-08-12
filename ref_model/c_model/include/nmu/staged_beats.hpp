#ifndef NI_CMODEL_STAGED_BEATS_HPP
#define NI_CMODEL_STAGED_BEATS_HPP
#include "axi/types.hpp"
#include <cstdint>
namespace ni::cmodel {
// RoB-admitted request beats + route metadata: the S1->S2 stage payload.
// AW/AR admitted by Rob carry route+rob meta computed in S1. Field set
// mirrors nmu::AwHeaderMeta (packetize.hpp): dst_id, local_addr, ordering_req,
// ordering_tag are all uint8_t to match the actual struct definition.
struct AdmittedAw {
    axi::AwBeat beat;
    uint8_t dst_id;
    uint64_t local_addr;
    uint8_t ordering_req;
    uint8_t ordering_tag;
    axi::AxiClass cls = axi::AxiClass::Data;
    // Translated at admission and carried through the stage: Packetize asserts
    // the meta still agrees with the beat's AWUSER. AR has no counterpart --
    // ARUSER carries no collective field.
    uint8_t collective_op = axi::COLLECTIVE_OP_UNICAST;
    uint8_t collective_mask = 0;
};
struct AdmittedAr {
    axi::ArBeat beat;
    uint8_t dst_id;
    uint64_t local_addr;
    uint8_t ordering_req;
    uint8_t ordering_tag;
    axi::AxiClass cls = axi::AxiClass::Data;
};
// W carries AW-inherited meta (matches packetize.hpp w_meta_fifo_ contract).
struct AdmittedW {
    axi::WBeat beat;
};
}  // namespace ni::cmodel
#endif
