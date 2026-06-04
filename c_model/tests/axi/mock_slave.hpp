#pragma once
#include "axi/types.hpp"
#include <deque>
#include <optional>

namespace ni::cmodel::axi::testing {

class MockSlave {
  public:
    bool push_aw(const AwBeat& b) {
        captured_aw.push_back(b);
        return true;
    }
    bool push_w(const WBeat& b) {
        captured_w.push_back(b);
        return true;
    }
    bool push_ar(const ArBeat& b) {
        captured_ar.push_back(b);
        return true;
    }
    std::optional<BBeat> pop_b() {
        if (queued_b.empty()) return std::nullopt;
        auto r = queued_b.front();
        queued_b.pop_front();
        return r;
    }
    std::optional<RBeat> pop_r() {
        if (queued_r.empty()) return std::nullopt;
        auto r = queued_r.front();
        queued_r.pop_front();
        return r;
    }
    void tick() {}

    std::deque<AwBeat> captured_aw;
    std::deque<WBeat> captured_w;
    std::deque<ArBeat> captured_ar;
    std::deque<BBeat> queued_b;
    std::deque<RBeat> queued_r;
};

}  // namespace ni::cmodel::axi::testing
