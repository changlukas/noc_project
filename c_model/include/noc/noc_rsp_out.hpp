#pragma once
// NocRspOut: abstract sink for RESPONSE-side flits leaving an NSU toward the NoC.
//
// The NSU Packetize stage produces response flits (B / R packetized) and
// hands them to a NocRspOut. Concrete implementations are the real NoC
// adapter (DPI bridge to the SystemVerilog router) and the ChannelModel test
// fixture. The matching RTL pin set is the response-out half of the merged
// `NocIntfMisoPins` bundle (ni_signals.json NOC_INTF_MISO — noc_rsp_valid_o
// / noc_rsp_flit_o / noc_rsp_credit_i); the req-in half of that same pin
// bundle is consumed by NocReqIn.
//
// Contract: push_flit returns true if the flit was accepted, false on
// downstream backpressure. A false return MUST be safely retried — the
// caller re-presents the same flit on the next tick without duplicating it;
// implementations therefore must not partially consume on a false-return
// path.
#include "ni/flit.hpp"

namespace ni::cmodel::noc {

class NocRspOut {
  public:
    virtual ~NocRspOut() = default;

    // Push one response flit downstream. Returns false on backpressure.
    virtual bool push_flit(const Flit& flit) = 0;

    // Per-VC credit availability query — see noc_req_out.hpp for
    // semantics. Default impl returns true so legacy mocks compile.
    virtual bool credit_avail(uint8_t /*vc_id*/) const { return true; }
};

}  // namespace ni::cmodel::noc
