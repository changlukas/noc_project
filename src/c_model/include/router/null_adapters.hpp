#pragma once
// Null-object adapters for NocReqOut / NocRspIn / NocReqIn / NocRspOut.
//
// S3a T4 gave Nmu/Nsu a second (DAT) NoC face alongside REQ/RSP (per-network
// arbiter pairs + a second ingress). Lower-level components that construct
// one of these interfaces directly (nmu::Depacketize, nsu::Depacketize) and
// only care about one network default the other ingress/egress to these:
// pop always empty, push always accepts-and-discards. The accept-and-discard
// choice on push matches NocReqOut/NocRspOut's own credit_avail() default of
// `true` -- a VcAllocator/WormholeArbiter sitting in front of one of these
// never sees a credit_avail=true / push_flit=false contradiction, which
// would trip their "downstream must not lie about credit" asserts.
//
// Top-level Nmu/Nsu construction does NOT default to these -- every NI face
// is an explicit caller choice there (see nmu.hpp / nsu.hpp). Existing
// integration tests that predate the DAT face pass these explicitly to keep
// compiling unchanged.
#include "flit.hpp"
#include "router/req_in.hpp"
#include "router/req_out.hpp"
#include "router/rsp_in.hpp"
#include "router/rsp_out.hpp"
#include <optional>

namespace ni::cmodel::router {

class NullNocReqOut : public NocReqOut {
  public:
    bool push_flit(const Flit&) override { return true; }
};

class NullNocRspIn : public NocRspIn {
  public:
    std::optional<Flit> pop_flit() override { return std::nullopt; }
};

class NullNocReqIn : public NocReqIn {
  public:
    std::optional<Flit> pop_flit() override { return std::nullopt; }
};

class NullNocRspOut : public NocRspOut {
  public:
    bool push_flit(const Flit&) override { return true; }
};

// Process-wide singletons: all four are stateless, so sharing one instance
// across every default-bound caller is safe.
inline NocReqOut& null_req_out() {
    static NullNocReqOut inst;
    return inst;
}
inline NocRspIn& null_rsp_in() {
    static NullNocRspIn inst;
    return inst;
}
inline NocReqIn& null_req_in() {
    static NullNocReqIn inst;
    return inst;
}
inline NocRspOut& null_rsp_out() {
    static NullNocRspOut inst;
    return inst;
}

}  // namespace ni::cmodel::router
