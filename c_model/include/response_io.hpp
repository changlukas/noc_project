#pragma once
// response_io.hpp — narrow RSP-plane producer/consumer interfaces.
//
// The response network carries B / R beats from NSU back toward NMU.
// Two roles are defined:
//   ResponsePacketizer   — produces response beats (NSU side)
//   ResponseDepacketizer — consumes response beats (NMU side)
//
// ResponseMeta carries ROB-related header fields extracted from a response
// flit. rob_req=0 → Disabled mode (rob_idx has no meaning);
// rob_req=1 → Enabled mode (rob_idx identifies the ROB slot).
//
// ResponseDepacketizer provides default pop_b_with_meta / pop_r_with_meta
// implementations that delegate to the pure-virtual pop_b / pop_r and inject
// ResponseMeta{0, 0}, so Disabled-mode callers and non-ROB-aware concrete
// classes keep working without overriding those methods.
#include "axi/types.hpp"
#include <cstdint>
#include <optional>
#include <utility>

namespace ni::cmodel {

struct ResponseMeta {
    uint8_t rob_idx;
    uint8_t rob_req;
};

class ResponsePacketizer {
  public:
    virtual ~ResponsePacketizer() = default;
    virtual bool push_b(const axi::BBeat& beat) = 0;
    virtual bool push_r(const axi::RBeat& beat) = 0;
};

class ResponseDepacketizer {
  public:
    virtual ~ResponseDepacketizer() = default;

    virtual std::optional<axi::BBeat> pop_b() = 0;
    virtual std::optional<axi::RBeat> pop_r() = 0;

    // Default implementations forward to pop_b/pop_r and return meta={0,0}.
    virtual std::optional<std::pair<axi::BBeat, ResponseMeta>> pop_b_with_meta() {
        auto b = pop_b();
        if (!b) return std::nullopt;
        return std::make_pair(*b, ResponseMeta{0, 0});
    }
    virtual std::optional<std::pair<axi::RBeat, ResponseMeta>> pop_r_with_meta() {
        auto r = pop_r();
        if (!r) return std::nullopt;
        return std::make_pair(*r, ResponseMeta{0, 0});
    }
};

}  // namespace ni::cmodel
