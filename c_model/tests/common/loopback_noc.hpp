// LoopbackNoc — test-only NoC bridge. Implements all 4 NoC abstracts via
// inner adapter classes (avoids C++ same-name virtual ambiguity from inheriting
// 4 abstract bases on the outer class).
//
// Bounded deque per direction. Accepts multiple flits per tick — does NOT
// model 1-flit/cycle physical NoC pacing. That's vc_arb's responsibility.
// Latency/throughput numbers from tests using LoopbackNoc are non-physical.
//
// Optional configurable latency variant: set_req_delay(cycles) / set_rsp_delay(cycles)
// makes push_flit enqueue into a delay pipe; tick() ages entries; visible queue
// serves pop_flit only when cycles_left==0.
#pragma once
#include "noc/noc_req_out.hpp"
#include "noc/noc_req_in.hpp"
#include "noc/noc_rsp_out.hpp"
#include "noc/noc_rsp_in.hpp"
#include <cstddef>
#include <deque>
#include <optional>
#include <utility>

namespace ni::cmodel::testing {

class LoopbackNoc {
public:
  LoopbackNoc(std::size_t req_depth, std::size_t rsp_depth)
      : req_depth_(req_depth), rsp_depth_(rsp_depth),
        req_out_adapter_{this}, req_in_adapter_{this},
        rsp_out_adapter_{this}, rsp_in_adapter_{this} {}

  noc::NocReqOut& req_out() noexcept { return req_out_adapter_; }
  noc::NocReqIn&  req_in()  noexcept { return req_in_adapter_;  }
  noc::NocRspOut& rsp_out() noexcept { return rsp_out_adapter_; }
  noc::NocRspIn&  rsp_in()  noexcept { return rsp_in_adapter_;  }

  void set_req_delay(unsigned cycles) noexcept { req_delay_ = cycles; }
  void set_rsp_delay(unsigned cycles) noexcept { rsp_delay_ = cycles; }

  // Age delayed entries; promote to visible queue when cycles_left==0.
  void tick() {
    auto age = [](std::deque<std::pair<Flit, unsigned>>& pipe,
                  std::deque<Flit>& visible, std::size_t cap) {
      for (auto& e : pipe) if (e.second > 0) --e.second;
      while (!pipe.empty() && pipe.front().second == 0 && visible.size() < cap) {
        visible.push_back(pipe.front().first);
        pipe.pop_front();
      }
    };
    age(req_pipe_, req_q_, req_depth_);
    age(rsp_pipe_, rsp_q_, rsp_depth_);
  }

  // Test introspection
  std::size_t req_q_size()    const noexcept { return req_q_.size(); }
  std::size_t rsp_q_size()    const noexcept { return rsp_q_.size(); }
  std::size_t req_pipe_size() const noexcept { return req_pipe_.size(); }
  std::size_t rsp_pipe_size() const noexcept { return rsp_pipe_.size(); }

private:
  // Inner adapters: each implements ONE abstract and forwards to outer
  struct ReqOutAdapter : noc::NocReqOut {
    LoopbackNoc* p;
    explicit ReqOutAdapter(LoopbackNoc* parent) : p(parent) {}
    bool push_flit(const Flit& f) override {
      if (p->req_delay_ > 0) {
        if (p->req_pipe_.size() + p->req_q_.size() >= p->req_depth_) return false;
        p->req_pipe_.emplace_back(f, p->req_delay_);
      } else {
        if (p->req_q_.size() >= p->req_depth_) return false;
        p->req_q_.push_back(f);
      }
      return true;
    }
  };
  struct ReqInAdapter : noc::NocReqIn {
    LoopbackNoc* p;
    explicit ReqInAdapter(LoopbackNoc* parent) : p(parent) {}
    std::optional<Flit> pop_flit() override {
      if (p->req_q_.empty()) return std::nullopt;
      Flit f = p->req_q_.front();
      p->req_q_.pop_front();
      return f;
    }
  };
  struct RspOutAdapter : noc::NocRspOut {
    LoopbackNoc* p;
    explicit RspOutAdapter(LoopbackNoc* parent) : p(parent) {}
    bool push_flit(const Flit& f) override {
      if (p->rsp_delay_ > 0) {
        if (p->rsp_pipe_.size() + p->rsp_q_.size() >= p->rsp_depth_) return false;
        p->rsp_pipe_.emplace_back(f, p->rsp_delay_);
      } else {
        if (p->rsp_q_.size() >= p->rsp_depth_) return false;
        p->rsp_q_.push_back(f);
      }
      return true;
    }
  };
  struct RspInAdapter : noc::NocRspIn {
    LoopbackNoc* p;
    explicit RspInAdapter(LoopbackNoc* parent) : p(parent) {}
    std::optional<Flit> pop_flit() override {
      if (p->rsp_q_.empty()) return std::nullopt;
      Flit f = p->rsp_q_.front();
      p->rsp_q_.pop_front();
      return f;
    }
  };

  std::size_t req_depth_, rsp_depth_;
  unsigned req_delay_ = 0, rsp_delay_ = 0;
  std::deque<Flit> req_q_, rsp_q_;
  std::deque<std::pair<Flit, unsigned>> req_pipe_, rsp_pipe_;
  ReqOutAdapter req_out_adapter_;
  ReqInAdapter  req_in_adapter_;
  RspOutAdapter rsp_out_adapter_;
  RspInAdapter  rsp_in_adapter_;
};

}  // namespace ni::cmodel::testing
