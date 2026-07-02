// PoC queue depths used by all wrap-layer adapters.
//
// Centralizes the magic numbers that previously appeared as bare literals
// across nmu_wrap.hpp, nsu_wrap.hpp, and
// channel_model_wrap.hpp. Names mirror the AdapterConfig /
// NmuConfig / NsuConfig field they populate.
#pragma once
#include <cstddef>

namespace ni::cmodel::wrap {

// AxiSlavePort / AxiMasterPort port_params.*_queue_depth and depkt_*_q_depth.
constexpr std::size_t kPoCAxiQueueDepth = 16;

// ChannelModel req / rsp queue depths (port_params.channel_model_{req,rsp}_depth)
// and the standalone ChannelModel ctor depths.
constexpr std::size_t kPoCChannelModelDepth = 64;

// MetaBuffer per-ID depth (port_params.meta_buffer_per_id_depth and
// NsuConfig::meta_buffer_per_id_depth).
constexpr std::size_t kPoCMetaBufferPerIdDepth = 16;

// Wormhole + VC arbiter staging depth (wormhole_per_input_depth and
// vc_arbiter_pending_depth).
constexpr std::size_t kPoCArbiterFifoDepth = 4;

}  // namespace ni::cmodel::wrap
