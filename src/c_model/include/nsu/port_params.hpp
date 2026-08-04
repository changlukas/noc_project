#pragma once
// NSU-side port-pair parameters. Defaults sourced from
// specgen/generated/cpp/ni_params.h (specgen/source/constants.yaml).
// Fields that NMU does not consume live in nmu/port_params.hpp; shared 5
// AXI-channel depths are duplicated by design (independent NMU/NSU
// evolution, no spec-mandated symmetry).
#include "ni_params.h"

#include <cstddef>

namespace ni::cmodel::nsu {

struct PortParams {
    // 5 AXI-channel master-port internal FIFO depths.
    std::size_t aw_queue_depth = ni::NSU_QUEUE_DEPTH;
    std::size_t w_queue_depth = ni::NSU_QUEUE_DEPTH;
    std::size_t ar_queue_depth = ni::NSU_QUEUE_DEPTH;
    std::size_t b_queue_depth = ni::NSU_QUEUE_DEPTH;
    std::size_t r_queue_depth = ni::NSU_QUEUE_DEPTH;
    // NSU MetaBuffer shared outstanding pool size, per direction (write / read).
    std::size_t meta_buffer_max_outstanding = ni::NSU_META_BUFFER_MAX_OUTSTANDING;
    // Count of distinct AXI IDs the NSU presents downstream. 1 collapses every
    // request onto the all-ones ID; AXI_ID_SPACE passes the master's ID through.
    std::size_t meta_buffer_max_unique_ids = ni::NSU_META_BUFFER_MAX_UNIQUE_IDS;
};

}  // namespace ni::cmodel::nsu
