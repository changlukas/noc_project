#pragma once
// LoopbackDepacketizer — paired with LoopbackPacketizer (see
// loopback_packetizer.hpp in this same directory). Pops directly from the
// shared LoopbackChannelSet, providing the "NoC delivered a beat" half of
// the loopback stub.
//
// Lives in tests/common/ alongside loopback_packetizer.hpp because both
// NMU and NSU unit tests consume it; not NMU-specific.
#include "axi/types.hpp"
#include "ni/depacketizer.hpp"
#include "common/loopback_packetizer.hpp"
#include <optional>

namespace ni::cmodel::testing {

class LoopbackDepacketizer : public Depacketizer {
  public:
    explicit LoopbackDepacketizer(LoopbackChannelSet& ch) : ch_(ch) {}

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
    LoopbackChannelSet& ch_;
};

}  // namespace ni::cmodel::testing
