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

// NMU RoB pool depths, per direction. Both <= 1 << ROB_IDX_WIDTH = 256, the
// addressable range of the rob_idx header field. A B entry holds {id, resp};
// an R entry holds one beat of rdata, so the two are sized independently.
constexpr std::size_t kRobBDepth = 32;
constexpr std::size_t kRobRDepth = 32;

// Per-AXI-ID order-list depth (FlooNoC MaxRoTxnsPerId, floo_rob.sv:12). [TBD] --
// 32 is FlooNoC's default over 8 ids; ours spans 256. The depth sweep decides.
constexpr std::size_t kRobMaxTxnsPerId = 32;

// Wormhole + VC arbiter staging depth (wormhole_per_input_depth and
// vc_arbiter_pending_depth).
constexpr std::size_t kArbiterFifoDepth = 4;

}  // namespace ni::cmodel::wrap
