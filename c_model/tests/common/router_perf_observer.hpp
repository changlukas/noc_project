#pragma once
#include "common/perf_common.hpp"
#include "common/perf_stats.hpp"
#include "noc/router.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ni::cmodel::testing {

struct ObservedRouter {
    std::string label;
    const noc::Router* router;
};

struct RouterPerfConfig {
    StatsConfig occupancy_stats{};  // thresholds when NOC_PERF=1, else empty
};

// Polls a set of Routers once per cycle (after their tick). Credit-stall =
// a (in_port, vc) front flit routed to output p with credit(p,vc)==0.
class RouterPerfObserver {
  public:
    RouterPerfObserver(const uint64_t& now, PhaseController& phase,
                       std::vector<ObservedRouter> routers, RouterPerfConfig cfg)
        : now_(now), phase_(phase), routers_(std::move(routers)), cfg_(std::move(cfg)) {
        per_router_.reserve(routers_.size());
        for (const auto& r : routers_) per_router_.push_back(PerRouter{cfg_.occupancy_stats});
    }

    void sample() {
        (void)now_;
        if (phase_.phase() != Phase::Measurement) return;
        for (std::size_t ri = 0; ri < routers_.size(); ++ri) {
            const noc::Router& r = *routers_[ri].router;
            PerRouter& pr = per_router_[ri];
            const uint8_t nvc = r.num_vc();
            for (std::size_t in = 0; in < noc::ROUTER_PORT_COUNT; ++in) {
                for (uint8_t vc = 0; vc < nvc; ++vc) {
                    auto out = r.front_route(in, vc);
                    if (out && r.credit(static_cast<std::size_t>(*out), vc) == 0) {
                        ++credit_stall_cycles_;
                        ++pr.stall;
                    }
                    pr.in_fifo.add(r.input_fifo_size(in, vc));
                }
            }
            for (std::size_t p = 0; p < noc::ROUTER_PORT_COUNT; ++p) {
                pr.out_fifo.add(r.output_fifo_size(p));
                ++pr.out_cycles[p];
                if (r.output_fifo_size(p) > 0) ++pr.out_nonempty[p];
            }
        }
    }

    std::size_t credit_stall_cycles() const { return credit_stall_cycles_; }
    std::size_t router_count() const { return routers_.size(); }
    const std::string& label(std::size_t i) const { return routers_[i].label; }
    double out_nonempty_ratio(std::size_t i, std::size_t port) const {
        const auto& pr = per_router_[i];
        return pr.out_cycles[port] ? static_cast<double>(pr.out_nonempty[port]) /
                                         static_cast<double>(pr.out_cycles[port])
                                   : 0.0;
    }
    const Stats& in_fifo(std::size_t i) const { return per_router_[i].in_fifo; }
    const Stats& out_fifo(std::size_t i) const { return per_router_[i].out_fifo; }
    std::size_t stall(std::size_t i) const { return per_router_[i].stall; }

  private:
    struct PerRouter {
        explicit PerRouter(StatsConfig c) : in_fifo(c), out_fifo(c) {}
        Stats in_fifo;
        Stats out_fifo;
        std::size_t stall = 0;
        std::size_t out_cycles[noc::ROUTER_PORT_COUNT] = {};
        std::size_t out_nonempty[noc::ROUTER_PORT_COUNT] = {};
    };
    const uint64_t& now_;
    PhaseController& phase_;
    std::vector<ObservedRouter> routers_;
    RouterPerfConfig cfg_;
    std::vector<PerRouter> per_router_;
    std::size_t credit_stall_cycles_ = 0;
};

}  // namespace ni::cmodel::testing
