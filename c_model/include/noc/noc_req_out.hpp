#pragma once
// NocReqOut: abstract sink for REQUEST-side flits leaving an NMU toward the NoC.
//
// The NMU Packetize stage produces request flits (AW / W / AR packetized) and
// hands them to a NocReqOut. Concrete implementations are the real NoC
// adapter (DPI bridge to the SystemVerilog router) and the LoopbackNoc test
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
};

}  // namespace ni::cmodel::noc
