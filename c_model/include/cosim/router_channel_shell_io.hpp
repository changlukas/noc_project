// RouterChannel shell IO — bidirectional 2-node NoC pin bundle.
//
// Each node's NoC interface is identical in shape to the single-NMU/single-NSU
// ChannelModel bundle, so RouterChannelInputs/Outputs are 2x the ChannelModel
// structs indexed by node (0,0)=node[0], (1,0)=node[1]. Per node the field
// meaning is: req_in = local NMU injects a request; rsp_in = local NSU injects a
// response; req_out = request ejected toward the local NSU; rsp_out = response
// ejected toward the local NMU; *_credit_return as in ChannelModel.
#pragma once
#include "cosim/channel_model_shell_io.hpp"
#include <array>
#include <cstddef>

namespace ni::cmodel::cosim {

inline constexpr std::size_t kRouterChannelNodes = 2;

struct RouterChannelInputs {
    std::array<ChannelModelInputs, kRouterChannelNodes> node{};
};

struct RouterChannelOutputs {
    std::array<ChannelModelOutputs, kRouterChannelNodes> node{};
};

}  // namespace ni::cmodel::cosim
