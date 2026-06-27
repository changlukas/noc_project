// ChannelModel wrap IO POD structs — Stage 5b spec §5.1 / §6.2.
// (Deleted in Task 3 together with ChannelModelWrap + the channel DPI.)
#pragma once
#include "wrap/flit_bytes.hpp"  // FlitBytes
#include <array>
#include <cstdint>

namespace ni::cmodel::wrap {

struct ChannelModelInputs {
    bool req_in_valid;
    FlitBytes req_in_flit;
    bool req_in_credit_return;

    bool rsp_in_valid;
    FlitBytes rsp_in_flit;
    bool rsp_in_credit_return;
};

struct ChannelModelOutputs {
    bool req_out_valid;
    FlitBytes req_out_flit;
    bool req_out_credit_return;

    bool rsp_out_valid;
    FlitBytes rsp_out_flit;
    bool rsp_out_credit_return;
};

}  // namespace ni::cmodel::wrap
