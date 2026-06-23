#pragma once
// NocReqIn: abstract source of REQUEST-side flits arriving at an NSU from the NoC.
//
// The NSU Depacketize stage pulls request flits (AW / W / AR) from a
// NocReqIn and reassembles them into AXI beats. Concrete implementations
// are the real NoC adapter (DPI bridge from the SystemVerilog router) and
// the ChannelModel test fixture. The matching RTL pin set is the
// request-in half of the merged `NocIntfMisoPins` bundle (ni_signals.json
// NOC_INTF_MISO — noc_req_valid_i / noc_req_flit_i / noc_req_credit_o);
// the rsp-out half of that same pin bundle is driven by NocRspOut.
//
// Contract: pop_flit returns nullopt when no flit is currently available.
// Callers must NOT busy-wait — they call once per tick and process whatever
// comes back. A returned flit is consumed; the source advances by one slot.
#include "flit.hpp"

#include <optional>

namespace ni::cmodel::router {

class NocReqIn {
  public:
    virtual ~NocReqIn() = default;

    // Pop one request flit from upstream. Returns nullopt when empty.
    virtual std::optional<Flit> pop_flit() = 0;
};

}  // namespace ni::cmodel::router
