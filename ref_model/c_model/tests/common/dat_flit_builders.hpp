#pragma once
// dat_flit_builders.hpp — Data-class flit builders shared by the NMU DAT-face
// unit tests (test_nmu_dat_face.cpp, test_nmu_credit.cpp).
#include "axi/types.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"

#include <cstdint>

namespace ni::cmodel::testing {

// Data-class AW opening a 1-beat wormhole packet (flit_tail=0, per the AW/W
// pairing lock -- WormholeArbiter's own comment: "AW=0, W=wlast").
inline Flit make_data_aw(uint8_t awid, uint8_t dst_id) {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_DataAw);
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("flit_tail", 0);
    f.set_payload_field("AW", "awid", awid);
    f.set_payload_field("AW", "awaddr", 0x100);
    f.set_payload_field("AW", "awlen", 0);
    f.set_payload_field("AW", "awsize", 6);
    f.set_payload_field("AW", "awburst", static_cast<uint64_t>(axi::Burst::INCR));
    return f;
}

inline Flit make_data_w(uint8_t dst_id) {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_DataW);
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("flit_tail", 1);  // wlast closes the wormhole packet
    f.set_payload_field("DATA_W", "wlast", 1);
    f.set_payload_field("DATA_W", "wstrb", 0xFFu);
    return f;
}

inline Flit make_data_r(uint8_t rid, uint8_t src_id, uint8_t dst_id) {
    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_DataR);
    f.set_header_field("src_id", src_id);
    f.set_header_field("dst_id", dst_id);
    f.set_header_field("vc_id", 0);
    f.set_header_field("flit_tail", 1);
    f.set_header_field("ordering_tag", 0);
    f.set_header_field("ordering_req", 0);
    f.set_payload_field("DATA_R", "rid", rid);
    f.set_payload_field("DATA_R", "rresp", static_cast<uint64_t>(axi::Resp::OKAY));
    f.set_payload_field("DATA_R", "ruser", 0);
    f.set_payload_field("DATA_R", "rlast", 1);
    return f;
}

}  // namespace ni::cmodel::testing
