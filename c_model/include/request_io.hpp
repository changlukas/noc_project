#pragma once
// request_io.hpp — narrow REQ-plane producer/consumer interfaces.
//
// The request network carries AW / W / AR beats from NMU toward NSU.
// Two roles are defined:
//   RequestPacketizer   — produces request beats (NMU side)
//   RequestDepacketizer — consumes request beats (NSU side)
//
// Each interface covers only its own plane; no wrong-side stubs needed.
#include "axi/types.hpp"
#include <optional>

namespace ni::cmodel {

class RequestPacketizer {
  public:
    virtual ~RequestPacketizer() = default;
    virtual bool push_aw(const axi::AwBeat& beat) = 0;
    virtual bool push_w(const axi::WBeat& beat) = 0;
    virtual bool push_ar(const axi::ArBeat& beat) = 0;
};

class RequestDepacketizer {
  public:
    virtual ~RequestDepacketizer() = default;
    virtual std::optional<axi::AwBeat> pop_aw() = 0;
    virtual std::optional<axi::WBeat> pop_w() = 0;
    virtual std::optional<axi::ArBeat> pop_ar() = 0;
};

}  // namespace ni::cmodel
