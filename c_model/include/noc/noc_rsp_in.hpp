#pragma once
// NocRspIn: abstract source of RESPONSE-side flits arriving at an NMU from the NoC.
//
// The NMU Depacketize stage pulls response flits (B / R) from a NocRspIn
// and reassembles them into AXI beats. Concrete implementations are the
// real NoC adapter (DPI bridge from the SystemVerilog router) and the
// ChannelModel test fixture. The matching RTL pin set is the response-in
// half of the merged `NocIntfMosiPins` bundle (ni_signals.json NOC_INTF_MOSI
// — noc_rsp_valid_i / noc_rsp_flit_i / noc_rsp_credit_o); the req-out half
// of that same pin bundle is driven by NocReqOut.
//
// Contract: pop_flit returns nullopt when no flit is currently available.
// Callers must NOT busy-wait — they call once per tick and process whatever
// comes back. A returned flit is consumed; the source advances by one slot.
#include "flit.hpp"

#include <optional>

namespace ni::cmodel::noc {

class NocRspIn {
  public:
    virtual ~NocRspIn() = default;

    // Pop one response flit from upstream. Returns nullopt when empty.
    virtual std::optional<Flit> pop_flit() = 0;
};

}  // namespace ni::cmodel::noc
