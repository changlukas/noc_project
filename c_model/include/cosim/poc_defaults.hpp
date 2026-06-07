// PoC queue depths used by all cosim shell adapters.
//
// Centralizes the magic numbers that previously appeared as bare literals
// across nmu_shell_adapter.hpp, nsu_shell_adapter.hpp, and
// loopback_noc_shell_adapter.hpp. Names mirror the AdapterConfig /
// NmuConfig / NsuConfig field they populate.
#pragma once
#include <cstddef>

namespace ni::cmodel::cosim {

// AxiSlavePort / AxiMasterPort port_params.*_queue_depth and depkt_*_q_depth.
constexpr std::size_t kPoCAxiQueueDepth = 16;

// LoopbackNoc req / rsp queue depths (port_params.loopback_noc_{req,rsp}_depth)
// and the standalone LoopbackNoc ctor depths.
constexpr std::size_t kPoCLoopbackNocDepth = 64;

// MetaBuffer per-ID depth (port_params.meta_buffer_per_id_depth and
// NsuConfig::meta_buffer_per_id_depth).
constexpr std::size_t kPoCMetaBufferPerIdDepth = 16;

// Wormhole + VC arbiter staging depth (wormhole_per_input_depth and
// vc_arbiter_pending_depth).
constexpr std::size_t kPoCArbiterFifoDepth = 4;

}  // namespace ni::cmodel::cosim
