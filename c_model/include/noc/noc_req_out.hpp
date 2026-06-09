#pragma once
// NocReqOut: abstract sink for REQUEST-side flits leaving an NMU toward the NoC.
//
// The NMU Packetize stage produces request flits (AW / W / AR packetized) and
// hands them to a NocReqOut. Concrete implementations are the real NoC
// adapter (DPI bridge to the SystemVerilog router) and the ChannelModel test
// fixture. Naming mirrors the ni_signals.json pin struct `NocReqOutPins` so
// header, RTL pin set, and abstract C++ base share one identifier.
//
// Contract: push_flit returns true if the flit was accepted, false on
// downstream backpressure. A false return MUST be safely retried — the
// caller re-presents the same flit on the next tick without duplicating it;
// implementations therefore must not partially consume on a false-return
// path.
#include "ni/flit.hpp"

namespace ni::cmodel::noc {

class NocReqOut {
  public:
    virtual ~NocReqOut() = default;

    // Push one request flit downstream. Returns false on backpressure.
    virtual bool push_flit(const Flit& flit) = 0;

    // Per-VC credit availability query (gem5 Garnet OutVcState /
    // BookSim BufferState::IsFullFor pattern). Caller SHOULD check
    // credit_avail(vc) before push_flit(flit_with_vc_id=vc) to model the
    // sender-side per-VC credit mirror. Default impl returns true: mocks
    // that don't track per-VC capacity remain valid (e.g., legacy
    // single-VC fixtures); overrides should return false when the
    // sender-side per-VC outstanding count has reached the configured
    // depth.
    virtual bool credit_avail(uint8_t /*vc_id*/) const { return true; }
};

}  // namespace ni::cmodel::noc
