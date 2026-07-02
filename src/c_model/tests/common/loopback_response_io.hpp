#pragma once
// loopback_response_io.hpp — narrow RSP-plane loopback stubs for unit tests.
//
// ResponseChannelSet owns the two B / R deques with configurable capacity
// limits. LoopbackResponsePacketizer pushes into them;
// LoopbackResponseDepacketizer pops from them via the ResponseDepacketizer
// base (pop_b_with_meta / pop_r_with_meta default to the base forwarding
// impl — no override needed).
//
// Lives in tests/common/ because both NMU and NSU unit tests consume it.
// For the integration aggregate that joins both planes, see loopback_channel_set.hpp.
#include "axi/types.hpp"
#include "response_io.hpp"
#include <cstddef>
#include <deque>
#include <optional>

namespace ni::cmodel::testing {

struct ResponseChannelSet {
    std::size_t b_capacity = 32;
    std::size_t r_capacity = 32;
    std::deque<axi::BBeat> b;
    std::deque<axi::RBeat> r;
};

class LoopbackResponsePacketizer : public ResponsePacketizer {
  public:
    explicit LoopbackResponsePacketizer(ResponseChannelSet& ch) : ch_(ch) {}

    bool push_b(const axi::BBeat& b) override {
        if (ch_.b.size() >= ch_.b_capacity) return false;
        ch_.b.push_back(b);
        return true;
    }
    bool push_r(const axi::RBeat& b) override {
        if (ch_.r.size() >= ch_.r_capacity) return false;
        ch_.r.push_back(b);
        return true;
    }

  private:
    ResponseChannelSet& ch_;
};

class LoopbackResponseDepacketizer : public ResponseDepacketizer {
  public:
    explicit LoopbackResponseDepacketizer(ResponseChannelSet& ch) : ch_(ch) {}

    std::optional<axi::BBeat> pop_b() override {
        if (ch_.b.empty()) return std::nullopt;
        auto v = ch_.b.front();
        ch_.b.pop_front();
        return v;
    }
    std::optional<axi::RBeat> pop_r() override {
        if (ch_.r.empty()) return std::nullopt;
        auto v = ch_.r.front();
        ch_.r.pop_front();
        return v;
    }
    // pop_b_with_meta / pop_r_with_meta inherit the base default (meta={0,0}).

  private:
    ResponseChannelSet& ch_;
};

}  // namespace ni::cmodel::testing
