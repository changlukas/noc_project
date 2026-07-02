#pragma once
// NI pipeline path identifiers for stage_occupancy() introspection.
// Shared by NMU and NSU — both classes expose stage_occupancy(NiPath, ...).

#include <cstddef>
#include <cstdint>

namespace ni::cmodel {

enum class NiPath {
    NmuReq,  // NMU request path  (AXI master → NoC)
    NmuRsp,  // NMU response path (NoC → AXI master)
    NsuReq,  // NSU request path  (NoC → AXI slave)
    NsuRsp,  // NSU response path (AXI slave → NoC)
};

}  // namespace ni::cmodel
