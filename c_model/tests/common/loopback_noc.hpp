// LoopbackNoc -- testbench-only NoC bridge with multi-NSU + per-NSU latency.
//
// Single-NSU ctor (LoopbackNoc(req_depth, rsp_depth)) is the backward-compat
// path: all 256 dst_id default-route to NSU_0; legacy aliases
// (req_in/req_out/rsp_in/rsp_out) point at NSU_0 endpoints; legacy
// set_req_delay/set_rsp_delay apply globally as before.
//
// Multi-NSU ctor (LoopbackNoc(num_nsu, req_per_nsu, rsp_total)) requires
// explicit set_dst_route(dst_id, nsu_idx) -- unmapped dst pushes assert.
//
// Per-NSU response latency (set_nsu_latency / set_nsu_latency_range) replaces
// global rsp_delay_ for the configured NSU (not additive; see spec sec 7.3).
//
// Bounded deque per direction. Accepts multiple flits per tick -- does NOT
// model 1-flit/cycle physical NoC pacing. That's vc_arb's responsibility.
// Latency/throughput numbers from tests using LoopbackNoc are non-physical.
#pragma once
#include "ni/flit.hpp"
#include "ni_flit_constants.h"
#include "noc/noc_req_in.hpp"
#include "noc/noc_req_out.hpp"
#include "noc/noc_rsp_in.hpp"
#include "noc/noc_rsp_out.hpp"
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <random>
#include <utility>
#include <vector>

namespace ni::cmodel::testing {

class LoopbackNoc {
public:
    // Backward-compat single-NSU ctor. Defaults all dst_id to NSU_0.
    LoopbackNoc(std::size_t req_depth, std::size_t rsp_depth)
        : LoopbackNoc(/*num_nsu=*/1, req_depth, rsp_depth) {
        // Override default-unmapped: route all dst to NSU_0 for legacy fixtures.
        for (std::size_t d = 0; d < DST_ID_SPACE; ++d) {
            dst_to_nsu_[d] = 0;
        }
    }

    // Multi-NSU ctor. Caller must call set_dst_route() for each dst_id used.
    LoopbackNoc(std::size_t num_nsu,
                std::size_t req_q_depth_per_nsu,
                std::size_t rsp_q_depth_total)
        : num_nsu_(num_nsu),
          req_q_depth_per_nsu_(req_q_depth_per_nsu),
          rsp_q_depth_total_(rsp_q_depth_total),
          nsu_req_q_(num_nsu),
          nsu_rsp_delay_q_(num_nsu),
          nsu_latency_(num_nsu),
          nmu_req_out_adapter_{this},
          nmu_rsp_in_adapter_{this} {
        dst_to_nsu_.fill(-1);
        nsu_req_in_adapters_.reserve(num_nsu);
        nsu_rsp_out_adapters_.reserve(num_nsu);
        for (std::size_t i = 0; i < num_nsu; ++i) {
            nsu_req_in_adapters_.emplace_back(this, i);
            nsu_rsp_out_adapters_.emplace_back(this, i);
        }
    }

    // NMU-side (single)
    noc::NocReqOut& nmu_req_out() noexcept { return nmu_req_out_adapter_; }
    noc::NocRspIn&  nmu_rsp_in()  noexcept { return nmu_rsp_in_adapter_;  }

    // NSU-side (per-NSU). 0-indexed; bounds-asserted.
    noc::NocReqIn&  nsu_req_in(std::size_t nsu_idx) noexcept {
        assert(nsu_idx < num_nsu_);
        return nsu_req_in_adapters_[nsu_idx];
    }
    noc::NocRspOut& nsu_rsp_out(std::size_t nsu_idx) noexcept {
        assert(nsu_idx < num_nsu_);
        return nsu_rsp_out_adapters_[nsu_idx];
    }

    // Legacy aliases -- single-NSU compatibility (point at NSU_0)
    noc::NocReqOut& req_out() noexcept { return nmu_req_out(); }
    noc::NocReqIn&  req_in()  noexcept { return nsu_req_in(0); }
    noc::NocRspOut& rsp_out() noexcept { return nsu_rsp_out(0); }
    noc::NocRspIn&  rsp_in()  noexcept { return nmu_rsp_in(); }

    // Routing: dst_id -> nsu_idx
    void set_dst_route(uint8_t dst_id, std::size_t nsu_idx) noexcept {
        assert(nsu_idx < num_nsu_);
        dst_to_nsu_[dst_id] = static_cast<int8_t>(nsu_idx);
    }

    // Per-NSU response latency (static)
    void set_nsu_latency(std::size_t nsu_idx, std::size_t cycles) noexcept {
        assert(nsu_idx < num_nsu_);
        nsu_latency_[nsu_idx] = NsuLatencyConfig{
            /*is_random=*/false, cycles, 0, 0};
    }
    // Per-NSU response latency (random uniform in [min, max] inclusive)
    void set_nsu_latency_range(std::size_t nsu_idx,
                               std::size_t min,
                               std::size_t max) noexcept {
        assert(nsu_idx < num_nsu_);
        assert(min <= max);
        nsu_latency_[nsu_idx] = NsuLatencyConfig{
            /*is_random=*/true, 0, min, max};
    }
    void set_random_seed(uint64_t seed) noexcept { rng_.seed(seed); }

    // Per-VC credit depth configuration and introspection.
    void set_per_vc_depth(std::size_t depth) noexcept {
        assert(depth > 0);
        per_vc_depth_ = depth;
    }
    std::size_t per_vc_depth() const noexcept { return per_vc_depth_; }
    std::size_t nmu_req_per_vc_in_flight(uint8_t vc_id) const noexcept {
        return nmu_req_per_vc_in_flight_[vc_id];
    }
    std::size_t nsu_rsp_per_vc_in_flight(uint8_t vc_id) const noexcept {
        return nsu_rsp_per_vc_in_flight_[vc_id];
    }

    // Legacy global delay (preserved for single-NSU fixtures only;
    // multi-NSU mode uses set_nsu_latency instead -- these assert if mixed).
    void set_req_delay(unsigned cycles) noexcept {
        assert(num_nsu_ == 1 &&
               "set_req_delay only supported in single-NSU mode; "
               "use set_nsu_latency in multi-NSU mode");
        req_delay_ = cycles;
    }
    void set_rsp_delay(unsigned cycles) noexcept {
        assert(num_nsu_ == 1 &&
               "set_rsp_delay only supported in single-NSU mode; "
               "use set_nsu_latency in multi-NSU mode");
        rsp_delay_ = cycles;
    }

    void tick() {
        // Per-NSU response delay aging
        for (std::size_t i = 0; i < num_nsu_; ++i) {
            for (auto& e : nsu_rsp_delay_q_[i]) {
                if (e.cycles_remaining > 0) --e.cycles_remaining;
            }
            while (!nsu_rsp_delay_q_[i].empty()
                   && nsu_rsp_delay_q_[i].front().cycles_remaining == 0
                   && rsp_q_.size() < rsp_q_depth_total_) {
                rsp_q_.push_back(nsu_rsp_delay_q_[i].front().flit);
                nsu_rsp_delay_q_[i].pop_front();
                --total_delayed_rsp_count_;
            }
        }
        // Legacy req/rsp pipe aging
        auto age = [](std::deque<std::pair<Flit, unsigned>>& pipe,
                      std::deque<Flit>& visible, std::size_t cap) {
            for (auto& e : pipe) {
                if (e.second > 0) --e.second;
            }
            while (!pipe.empty() && pipe.front().second == 0
                   && visible.size() < cap) {
                visible.push_back(pipe.front().first);
                pipe.pop_front();
            }
        };
        age(req_pipe_, req_q_, req_q_depth_per_nsu_);
        age(rsp_pipe_, rsp_q_, rsp_q_depth_total_);
    }

    // Test introspection
    std::size_t nsu_req_q_size(std::size_t i) const noexcept {
        return nsu_req_q_[i].size();
    }
    std::size_t rsp_q_size() const noexcept { return rsp_q_.size(); }

    // Legacy single-NSU introspection (preserved for backward compat).
    // req_q_size() returns the visible NSU_0 request-queue size, plus any
    // in-flight req_q_ from the legacy global-delay path.
    std::size_t req_q_size() const noexcept {
        return nsu_req_q_[0].size() + req_q_.size();
    }
    std::size_t req_pipe_size() const noexcept { return req_pipe_.size(); }
    std::size_t rsp_pipe_size() const noexcept { return rsp_pipe_.size(); }

private:
    static constexpr std::size_t DST_ID_SPACE   = 1u << ni::header::DST_ID_WIDTH;
    static constexpr std::size_t NUM_VC_MAX        = 1u << ni::header::VC_ID_WIDTH;  // 8
    // Default per-VC depth is effectively unlimited so that existing fixtures
    // that push more than any small sentinel to one VC are unaffected.
    // Tests that want to exercise credit exhaustion call set_per_vc_depth().
    static constexpr std::size_t kDefaultPerVcDepth =
        std::numeric_limits<std::size_t>::max();

    struct NsuLatencyConfig {
        bool        is_random = false;
        std::size_t value     = 0;
        std::size_t min       = 0;
        std::size_t max       = 0;
    };

    struct DelayedFlit { Flit flit; std::size_t cycles_remaining; };

    struct NmuReqOutAdapter : noc::NocReqOut {
        LoopbackNoc* p;
        explicit NmuReqOutAdapter(LoopbackNoc* parent) : p(parent) {}
        bool push_flit(const Flit& f) override {
            uint8_t vc  = static_cast<uint8_t>(f.get_header_field("vc_id"));
            uint8_t dst = static_cast<uint8_t>(f.get_header_field("dst_id"));
            int8_t  nsu = p->dst_to_nsu_[dst];
            assert(nsu >= 0 && "LoopbackNoc: unmapped dst_id");
            if (p->nmu_req_per_vc_in_flight_[vc] >= p->per_vc_depth_) return false;
            // Legacy global req delay path (single-NSU mode only; req_delay_
            // is gated by set_req_delay's num_nsu_==1 assert).
            if (p->req_delay_ > 0) {
                if (p->req_pipe_.size() + p->req_q_.size()
                        >= p->req_q_depth_per_nsu_) {
                    return false;
                }
                p->req_pipe_.emplace_back(f, p->req_delay_);
                ++p->nmu_req_per_vc_in_flight_[vc];
                return true;
            }
            if (p->nsu_req_q_[nsu].size() >= p->req_q_depth_per_nsu_) {
                return false;
            }
            p->nsu_req_q_[nsu].push_back(f);
            ++p->nmu_req_per_vc_in_flight_[vc];
            return true;
        }
        bool credit_avail(uint8_t vc_id) const override {
            return p->nmu_req_per_vc_in_flight_[vc_id] < p->per_vc_depth_;
        }
    };
    struct NsuReqInAdapter : noc::NocReqIn {
        LoopbackNoc* p;
        std::size_t  i;
        NsuReqInAdapter(LoopbackNoc* parent, std::size_t idx)
            : p(parent), i(idx) {}
        std::optional<Flit> pop_flit() override {
            // Legacy global req delay path drains via req_q_ (single-NSU
            // only -- NSU_0 is the sole NSU when req_delay_ is non-zero).
            if (i == 0 && !p->req_q_.empty()) {
                Flit f = p->req_q_.front();
                p->req_q_.pop_front();
                uint8_t vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
                assert(p->nmu_req_per_vc_in_flight_[vc] > 0);
                --p->nmu_req_per_vc_in_flight_[vc];
                return f;
            }
            if (p->nsu_req_q_[i].empty()) return std::nullopt;
            Flit f = p->nsu_req_q_[i].front();
            p->nsu_req_q_[i].pop_front();
            uint8_t vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
            assert(p->nmu_req_per_vc_in_flight_[vc] > 0);
            --p->nmu_req_per_vc_in_flight_[vc];
            return f;
        }
    };
    struct NsuRspOutAdapter : noc::NocRspOut {
        LoopbackNoc* p;
        std::size_t  i;
        NsuRspOutAdapter(LoopbackNoc* parent, std::size_t idx)
            : p(parent), i(idx) {}
        bool push_flit(const Flit& f) override {
            uint8_t vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
            if (p->nsu_rsp_per_vc_in_flight_[vc] >= p->per_vc_depth_) return false;
            const auto& cfg = p->nsu_latency_[i];
            std::size_t latency;
            if (cfg.is_random) {
                std::uniform_int_distribution<std::size_t> dist(cfg.min, cfg.max);
                latency = dist(p->rng_);
            } else {
                latency = cfg.value;
            }
            if (latency == 0) {
                // Fast path: bypass per-NSU delay queue.
                if (p->rsp_delay_ > 0) {
                    // Legacy global rsp delay (single-NSU mode only).
                    if (p->rsp_pipe_.size() + p->rsp_q_.size()
                            >= p->rsp_q_depth_total_) {
                        return false;
                    }
                    p->rsp_pipe_.emplace_back(f, p->rsp_delay_);
                } else {
                    if (p->rsp_q_.size() >= p->rsp_q_depth_total_) return false;
                    p->rsp_q_.push_back(f);
                }
                ++p->nsu_rsp_per_vc_in_flight_[vc];
                return true;
            }
            // Per-NSU delay path. Aggregate capacity:
            //   total_delayed_rsp_count_ + rsp_pipe_.size() + rsp_q_.size()
            //       <= rsp_q_depth_total_
            if (p->total_delayed_rsp_count_ + p->rsp_pipe_.size()
                    + p->rsp_q_.size() >= p->rsp_q_depth_total_) {
                return false;
            }
            p->nsu_rsp_delay_q_[i].push_back({f, latency});
            ++p->total_delayed_rsp_count_;
            ++p->nsu_rsp_per_vc_in_flight_[vc];
            return true;
        }
        bool credit_avail(uint8_t vc_id) const override {
            return p->nsu_rsp_per_vc_in_flight_[vc_id] < p->per_vc_depth_;
        }
    };
    struct NmuRspInAdapter : noc::NocRspIn {
        LoopbackNoc* p;
        explicit NmuRspInAdapter(LoopbackNoc* parent) : p(parent) {}
        std::optional<Flit> pop_flit() override {
            if (p->rsp_q_.empty()) return std::nullopt;
            Flit f = p->rsp_q_.front();
            p->rsp_q_.pop_front();
            uint8_t vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
            assert(p->nsu_rsp_per_vc_in_flight_[vc] > 0);
            --p->nsu_rsp_per_vc_in_flight_[vc];
            return f;
        }
    };

    std::size_t      num_nsu_;
    std::size_t      req_q_depth_per_nsu_;
    std::size_t      rsp_q_depth_total_;
    std::size_t      per_vc_depth_ = kDefaultPerVcDepth;
    std::array<std::size_t, NUM_VC_MAX> nmu_req_per_vc_in_flight_{};
    std::array<std::size_t, NUM_VC_MAX> nsu_rsp_per_vc_in_flight_{};
    std::array<int8_t, DST_ID_SPACE>        dst_to_nsu_{};
    std::vector<std::deque<Flit>>           nsu_req_q_;
    std::deque<Flit>                        rsp_q_;
    std::vector<std::deque<DelayedFlit>>    nsu_rsp_delay_q_;
    std::size_t                             total_delayed_rsp_count_ = 0;
    std::vector<NsuLatencyConfig>           nsu_latency_;
    unsigned                                req_delay_ = 0, rsp_delay_ = 0;
    std::deque<std::pair<Flit, unsigned>>   req_pipe_, rsp_pipe_;
    std::deque<Flit>                        req_q_;   // legacy global req delay output
    std::mt19937_64                         rng_;
    NmuReqOutAdapter                        nmu_req_out_adapter_;
    NmuRspInAdapter                         nmu_rsp_in_adapter_;
    std::vector<NsuReqInAdapter>            nsu_req_in_adapters_;
    std::vector<NsuRspOutAdapter>           nsu_rsp_out_adapters_;
};

}  // namespace ni::cmodel::testing
