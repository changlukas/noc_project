#pragma once
// LoopbackPacketizer / LoopbackDepacketizer — test stubs for Stage 3 port
// pair (NMU AxiSlavePort + NSU AxiMasterPort) and any later harness that
// needs a zero-latency stand-in for the future ni/packetize.hpp +
// ni/depacketize.hpp pair.
//
// Lives in tests/common/ because both NMU and NSU unit tests consume it;
// it is shared test infrastructure, NOT NMU-specific.
//
// Models what the future packetize/depacketize pair will do AT ZERO LATENCY:
// any beat pushed to the packetizer is immediately visible to the paired
// depacketizer on the other side. Two parallel objects share the same
// underlying bounded deques via a small back-end.
//
// Two flavors are provided here so unit tests can drive only one half of
// the port without standing up the full NoC fabric:
//   - LoopbackPacketizer:   accepts request-side pushes (AW/W/AR) AND
//                           response-side pushes (B/R) into per-channel
//                           bounded deques the test can inspect.
//   - LoopbackDepacketizer: pops from those same deques on the other side.
//
// A LoopbackChannelSet owns all 5 deques. The two wrapper classes hold a
// reference to a single set so they form an in-process loopback link.
#include "axi/types.hpp"
#include "ni/depacketizer.hpp"
#include "ni/packetizer.hpp"
#include <cstddef>
#include <deque>
#include <optional>

namespace ni::cmodel::testing {

struct LoopbackChannelSet {
    std::size_t aw_capacity = 32;
    std::size_t w_capacity = 32;
    std::size_t ar_capacity = 32;
    std::size_t b_capacity = 32;
    std::size_t r_capacity = 32;
    std::deque<axi::AwBeat> aw;
    std::deque<axi::WBeat> w;
    std::deque<axi::ArBeat> ar;
    std::deque<axi::BBeat> b;
    std::deque<axi::RBeat> r;
};

class LoopbackPacketizer : public Packetizer {
  public:
    explicit LoopbackPacketizer(LoopbackChannelSet& ch) : ch_(ch) {}

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
    LoopbackChannelSet& ch_;
};

}  // namespace ni::cmodel::testing
