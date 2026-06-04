#pragma once
// Packetizer: NMU/NSU-shared abstract base class.
//
// Both ports hand outgoing channel beats downstream through a Packetizer.
// The NMU side packetizes REQUEST beats (AW / W / AR) for the forward NoC
// path; the NSU side packetizes RESPONSE beats (B / R) for the return
// path. The same abstract base carries both halves so a single concrete
// implementation can be used on either side (and the loopback test stub
// only needs one class).
//
// Contract: push_* returns true if the beat was accepted, false on
// backpressure. A false return MUST be safely retried — the port re-presents
// the same beat on the next tick without duplicating it. Implementations
// therefore must not partially consume on a false-return path.
//
// A given port only USES its half (request side or response side); the
// other half is exercised by the peer port via the same concrete object.
#include "axi/types.hpp"

namespace ni::cmodel {

class Packetizer {
  public:
    virtual ~Packetizer() = default;

    // Request side (used by NMU AxiSlavePort)
    virtual bool push_aw(const axi::AwBeat& beat) = 0;
    virtual bool push_w(const axi::WBeat& beat) = 0;
    virtual bool push_ar(const axi::ArBeat& beat) = 0;

    // Response side (used by NSU AxiMasterPort)
    virtual bool push_b(const axi::BBeat& beat) = 0;
    virtual bool push_r(const axi::RBeat& beat) = 0;
};

}  // namespace ni::cmodel
