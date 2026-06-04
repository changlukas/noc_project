#pragma once
// Per-channel capture mock for testing modules that emit flits to multiple
// downstream channels (e.g., Packetize after multi-output refactor). Each
// instance captures flits pushed to it into an internal deque for later
// assertion. credit_avail uses the default (returns true).
#include "ni/flit.hpp"
#include "noc/noc_req_out.hpp"
#include "noc/noc_rsp_out.hpp"
#include <cstddef>
#include <deque>
#include <optional>

namespace ni::cmodel::testing {

template <typename Interface>
class PerChannelCapture : public Interface {
  public:
    bool push_flit(const Flit& f) override {
        captured_.push_back(f);
        return true;
    }

    std::optional<Flit> pop() {
        if (captured_.empty()) return std::nullopt;
        Flit f = captured_.front();
        captured_.pop_front();
        return f;
    }
    std::size_t size() const noexcept { return captured_.size(); }
    void clear() noexcept { captured_.clear(); }

  private:
    std::deque<Flit> captured_;
};

using ReqCapture = PerChannelCapture<noc::NocReqOut>;
using RspCapture = PerChannelCapture<noc::NocRspOut>;

}  // namespace ni::cmodel::testing
