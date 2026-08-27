#pragma once
// loopback_channel_set.hpp — narrow REQ/RSP-plane loopback stubs for unit
// tests, plus the aggregate that joins both planes for integration tests
// wiring NMU and NSU through a single loopback.
//
// RequestChannelSet owns the three AW / W / AR deques with configurable
// capacity limits; ResponseChannelSet owns the two B / R deques the same way.
// The Loopback*Packetizer classes push into them, the Loopback*Depacketizer
// classes pop from them (LoopbackResponseDepacketizer's pop_b_with_meta /
// pop_r_with_meta default to the base forwarding impl — no override needed).
//
// Lives in tests/common/ because both NMU and NSU unit tests consume it.
#include "axi/types.hpp"
#include "request_io.hpp"
#include "response_io.hpp"
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

struct LoopbackChannelSet {
    RequestChannelSet request;
    ResponseChannelSet response;
};

}  // namespace ni::cmodel::testing
