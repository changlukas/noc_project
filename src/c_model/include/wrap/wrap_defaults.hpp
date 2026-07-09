// Default queue depths used by all wrap-layer adapters.
//
// Centralizes the magic numbers that previously appeared as bare literals
// across nmu_wrap.hpp and nsu_wrap.hpp. Names mirror the AdapterConfig /
// NmuConfig / NsuConfig field they populate.
#pragma once
#include <cstddef>

namespace ni::cmodel::wrap {

// AxiSlavePort / AxiMasterPort port_params.*_queue_depth and depkt_*_q_depth.
constexpr std::size_t kAxiQueueDepth = 16;

// ChannelModel req / rsp queue depths (port_params.channel_model_{req,rsp}_depth)
// and the standalone ChannelModel ctor depths.
constexpr std::size_t kChannelModelDepth = 64;

// MetaBuffer shared outstanding pool size, per direction
// (port_params.meta_buffer_max_outstanding).
constexpr std::size_t kMetaBufferMaxOutstanding = 32;

// Distinct AXI IDs presented downstream (port_params.meta_buffer_max_unique_ids).
// 1 matches FlooNoC's ChimneyDefaultCfg. Set to 256 to pass the manager's ID
// through, which the VC throughput round requires.
constexpr std::size_t kMetaBufferMaxUniqueIds = 1;

// Wormhole + VC arbiter staging depth (wormhole_per_input_depth and
// vc_arbiter_pending_depth).
constexpr std::size_t kArbiterFifoDepth = 4;

}  // namespace ni::cmodel::wrap
