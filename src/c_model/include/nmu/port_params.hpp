#pragma once
// NMU-side port-pair parameters. Defaults sourced from
// specgen/generated/cpp/ni_params.h (specgen/source/constants.yaml).
// Fields that NSU does not consume live in nsu/port_params.hpp; shared 5
// AXI-channel depths are duplicated by design (independent NMU/NSU
// evolution, no spec-mandated symmetry).
#include "ni_params.h"

#include <cstddef>

namespace ni::cmodel::nmu {

struct PortParams {
    // 5 AXI-channel slave-port internal FIFO depths.
    std::size_t aw_queue_depth = ni::NMU_QUEUE_DEPTH;
    std::size_t w_queue_depth = ni::NMU_QUEUE_DEPTH;
    std::size_t ar_queue_depth = ni::NMU_QUEUE_DEPTH;
    std::size_t b_queue_depth = ni::NMU_QUEUE_DEPTH;
    std::size_t r_queue_depth = ni::NMU_QUEUE_DEPTH;
    // NMU Depacketize internal demux FIFO depths (NMU consumes B/R).
    std::size_t depkt_b_q_depth = ni::NMU_DEPKT_Q_DEPTH;
    std::size_t depkt_r_q_depth = ni::NMU_DEPKT_Q_DEPTH;
};

}  // namespace ni::cmodel::nmu
