#pragma once
// loopback_request_io.hpp — narrow REQ-plane loopback stubs for unit tests.
//
// RequestChannelSet owns the three AW / W / AR deques with configurable
// capacity limits. LoopbackRequestPacketizer pushes into them;
// LoopbackRequestDepacketizer pops from them.
//
// Lives in tests/common/ because both NMU and NSU unit tests consume it.
#include "axi/types.hpp"
#include "request_io.hpp"
#include <cstddef>
#include <deque>
#include <optional>

namespace ni::cmodel::testing {

struct RequestChannelSet {
    std::size_t aw_capacity = 32;
    std::size_t w_capacity = 32;
    std::size_t ar_capacity = 32;
    std::deque<axi::AwBeat> aw;
    std::deque<axi::WBeat> w;
    std::deque<axi::ArBeat> ar;
};

class LoopbackRequestPacketizer : public RequestPacketizer {
  public:
    explicit LoopbackRequestPacketizer(RequestChannelSet& ch) : ch_(ch) {}

    bool push_aw(const axi::AwBeat& b) override {
        if (ch_.aw.size() >= ch_.aw_capacity) return false;
        ch_.aw.push_back(b);
        return true;
    }
    bool push_w(const axi::WBeat& b) override {
        if (ch_.w.size() >= ch_.w_capacity) return false;
        ch_.w.push_back(b);
        return true;
    }
    bool push_ar(const axi::ArBeat& b) override {
        if (ch_.ar.size() >= ch_.ar_capacity) return false;
        ch_.ar.push_back(b);
        return true;
    }

  private:
    RequestChannelSet& ch_;
};

class LoopbackRequestDepacketizer : public RequestDepacketizer {
  public:
    explicit LoopbackRequestDepacketizer(RequestChannelSet& ch) : ch_(ch) {}

    std::optional<axi::AwBeat> pop_aw() override {
        if (ch_.aw.empty()) return std::nullopt;
        auto v = ch_.aw.front();
        ch_.aw.pop_front();
        return v;
    }
    std::optional<axi::WBeat> pop_w() override {
        if (ch_.w.empty()) return std::nullopt;
        auto v = ch_.w.front();
        ch_.w.pop_front();
        return v;
    }
    std::optional<axi::ArBeat> pop_ar() override {
        if (ch_.ar.empty()) return std::nullopt;
        auto v = ch_.ar.front();
        ch_.ar.pop_front();
        return v;
    }

  private:
    RequestChannelSet& ch_;
};

}  // namespace ni::cmodel::testing
