#pragma once
// Wormhole VC router for the c_model NoC fabric.
// Spec: docs/superpowers/specs/2026-06-12-router-microarch-design.md
//
// Fixed-vc 3-stage pipeline: stage 1 per-(input port, vc) FIFO (+RC at the
// FIFO head), stage 2 per-(output port, vc) wormhole arbitration + per-output
// VC arbitration + crossbar, stage 3 output FIFO -> link. Credit-based flow
// control; credit reserved at output-FIFO admission (the grant event).
// Lock semantics ported from FlooNoC floo_wormhole_arbiter/floo_vc_arbiter
// with (input port, vc) ownership; decrement point matches BookSim2
// BufferState::SendingFlit.
//
// Convention: +y is NORTH. One Router instance per physical network
// (REQ / RSP are separate objects).
#include "flit.hpp"
#include "ni_flit_constants.h"
#include "ni_params.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace ni::cmodel::noc {

enum class RouterPort : uint8_t { LOCAL = 0, NORTH = 1, EAST = 2, SOUTH = 3, WEST = 4 };
inline constexpr std::size_t ROUTER_PORT_COUNT = 5;

struct RouterConfig {
    uint8_t x = 0;
    uint8_t y = 0;
    uint8_t mesh_x_dim = NI_NOC_MESH_X_DIM;
    uint8_t mesh_y_dim = NI_NOC_MESH_Y_DIM;
    uint8_t num_vc = NI_NOC_NUM_VC;
    std::size_t vc_depth = NI_NOC_ROUTER_VC_DEPTH;
    std::size_t output_fifo_depth = NI_NOC_ROUTER_OUTPUT_FIFO_DEPTH;
};

// Forward half of the router link contract (spec §6). push_flit is always
// accepted — the sender's credit counter guarantees receiver buffer space.
class RouterLink {
  public:
    virtual ~RouterLink() = default;
    virtual void push_flit(const Flit& flit) = 0;
};

// Reverse half: per-VC credit return pulses back to the sender.
class RouterCreditSink {
  public:
    virtual ~RouterCreditSink() = default;
    virtual void receive_credit(uint8_t vc_id) = 0;
};

// XY dimension-order route (spec §4): X first, then Y, equal ejects LOCAL.
// dst_id layout matches nmu::addr_trans (X in low bits).
inline RouterPort route_compute(uint8_t dst_id, const RouterConfig& cfg) {
    const uint8_t dst_x = dst_id & static_cast<uint8_t>((1u << ni::width::X_WIDTH) - 1);
    const uint8_t dst_y = static_cast<uint8_t>(dst_id >> ni::width::X_WIDTH) &
                          static_cast<uint8_t>((1u << ni::width::Y_WIDTH) - 1);
    if (!(dst_x < cfg.mesh_x_dim && dst_y < cfg.mesh_y_dim)) {
        assert(false && "route_compute: dst_id outside mesh range");
        std::abort();
    }
    if (dst_x != cfg.x) return dst_x > cfg.x ? RouterPort::EAST : RouterPort::WEST;
    if (dst_y != cfg.y) return dst_y > cfg.y ? RouterPort::NORTH : RouterPort::SOUTH;
    return RouterPort::LOCAL;
}

class Router {
  public:
    explicit Router(const RouterConfig& cfg) : cfg_(cfg) {
        if (!(cfg_.num_vc >= 1 && cfg_.num_vc <= (1u << ni::header::VC_ID_WIDTH))) {
            assert(false && "Router: num_vc out of range (1 .. 2^VC_ID_WIDTH)");
            std::abort();
        }
        if (cfg_.vc_depth == 0 || cfg_.output_fifo_depth == 0) {
            assert(false && "Router: zero FIFO depth");
            std::abort();
        }
        if (!(cfg_.x < cfg_.mesh_x_dim && cfg_.y < cfg_.mesh_y_dim)) {
            assert(false && "Router: own coordinate outside mesh");
            std::abort();
        }
        for (std::size_t p = 0; p < ROUTER_PORT_COUNT; ++p) {
            input_fifo_[p].resize(cfg_.num_vc);
            credit_[p].assign(cfg_.num_vc, cfg_.vc_depth);
            wormhole_[p].resize(cfg_.num_vc);
            input_adapters_.emplace_back(this, p);
        }
    }
    Router(const Router&) = delete;
    Router(Router&&) = delete;
    Router& operator=(const Router&) = delete;
    Router& operator=(Router&&) = delete;

    RouterLink& input(std::size_t port) {
        assert(port < ROUTER_PORT_COUNT);
        return input_adapters_[port];
    }
    void set_downstream(std::size_t port, RouterLink& link) { downstream_[port] = &link; }
    void set_upstream_credit(std::size_t port, RouterCreditSink& sink) {
        upstream_credit_[port] = &sink;
    }
    // Credit pulse from the downstream node attached to `port`'s output.
    void receive_credit(std::size_t port, uint8_t vc_id) {
        assert(port < ROUTER_PORT_COUNT && vc_id < cfg_.num_vc);
        if (credit_[port][vc_id] >= cfg_.vc_depth) {
            assert(false && "Router: credit counter overflow");
            std::abort();
        }
        ++credit_[port][vc_id];
    }

    void tick();  // Task 5

    // Test introspection
    std::size_t credit(std::size_t out_port, uint8_t vc) const { return credit_[out_port][vc]; }
    std::size_t input_fifo_size(std::size_t port, uint8_t vc) const {
        return input_fifo_[port][vc].size();
    }
    std::size_t output_fifo_size(std::size_t port) const { return output_fifo_[port].size(); }
    uint8_t num_vc() const { return cfg_.num_vc; }
    // Front flit's routed output port for (in_port, vc), or nullopt if empty.
    // Pure read; mirrors stage-2's route check without side effects.
    std::optional<RouterPort> front_route(std::size_t in_port, uint8_t vc) const {
        if (in_port >= ROUTER_PORT_COUNT || vc >= cfg_.num_vc) return std::nullopt;
        const auto& q = input_fifo_[in_port][vc];
        if (q.empty()) return std::nullopt;
        const auto dst = static_cast<uint8_t>(q.front().get_header_field("dst_id"));
        return route_compute(dst, cfg_);
    }

  private:
    struct InputAdapter : RouterLink {
        Router* parent;
        std::size_t port;
        InputAdapter(Router* p, std::size_t idx) : parent(p), port(idx) {}
        void push_flit(const Flit& f) override { parent->accept_flit(port, f); }
    };

    void accept_flit(std::size_t port, const Flit& f);  // Task 5

    struct WormholeState {
        std::optional<std::size_t> locked_input;
        std::size_t rr = 0;
    };

    RouterConfig cfg_;
    // stage-1 input landing register, one flit/port/cycle
    std::array<std::optional<Flit>, ROUTER_PORT_COUNT> landing_{};
    std::array<std::vector<std::deque<Flit>>, ROUTER_PORT_COUNT> input_fifo_{};
    std::array<std::vector<std::size_t>, ROUTER_PORT_COUNT> credit_{};      // [out][vc]
    std::array<std::vector<WormholeState>, ROUTER_PORT_COUNT> wormhole_{};  // [out][vc]
    std::array<std::size_t, ROUTER_PORT_COUNT> vc_rr_{};                    // [out]
    std::array<std::deque<Flit>, ROUTER_PORT_COUNT> output_fifo_{};
    std::array<RouterLink*, ROUTER_PORT_COUNT> downstream_{};
    std::array<RouterCreditSink*, ROUTER_PORT_COUNT> upstream_credit_{};
    // credit return pulses, registered: emitted at the start of next tick
    std::vector<std::pair<std::size_t, uint8_t>> credit_pulse_pending_;
    std::vector<InputAdapter> input_adapters_;
};

inline void Router::accept_flit(std::size_t port, const Flit& f) {
    const auto vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
    if (vc >= cfg_.num_vc) {
        assert(false && "Router::accept_flit: vc_id >= num_vc");
        std::abort();
    }
    if (ni::header::COMMTYPE_ENABLED && f.get_header_field("commtype") != 0) {
        assert(false && "Router::accept_flit: nonzero commtype unsupported");
        std::abort();
    }
    if (ni::header::MULTICAST_ENABLED && f.get_header_field("multicast") != 0) {
        assert(false && "Router::accept_flit: nonzero multicast unsupported");
        std::abort();
    }
    if (landing_[port].has_value()) {
        assert(false && "Router::accept_flit: >1 flit per link per cycle");
        std::abort();
    }
    landing_[port] = f;
}

inline void Router::tick() {
    // Registered credit pulses generated last tick go out first.
    for (const auto& [port, vc] : credit_pulse_pending_) {
        if (upstream_credit_[port]) upstream_credit_[port]->receive_credit(vc);
    }
    credit_pulse_pending_.clear();

    // Stages run in reverse pipeline order so a flit advances one stage per tick.
    // Stage 3: output FIFO -> link (one flit per output port per cycle).
    for (std::size_t out = 0; out < ROUTER_PORT_COUNT; ++out) {
        if (!output_fifo_[out].empty() && downstream_[out]) {
            downstream_[out]->push_flit(output_fifo_[out].front());
            output_fifo_[out].pop_front();
        }
    }

    // Stage 2: per-output grant — wormhole (packet) lock + VC (flit) RR.
    for (std::size_t out = 0; out < ROUTER_PORT_COUNT; ++out) {
        if (output_fifo_[out].size() >= cfg_.output_fifo_depth) continue;
        for (std::size_t k = 0; k < cfg_.num_vc; ++k) {
            const std::size_t vc = (vc_rr_[out] + k) % cfg_.num_vc;
            auto& ws = wormhole_[out][vc];
            std::optional<std::size_t> candidate;
            if (ws.locked_input.has_value()) {
                if (!input_fifo_[*ws.locked_input][vc].empty()) candidate = ws.locked_input;
            } else {
                for (std::size_t j = 0; j < ROUTER_PORT_COUNT; ++j) {
                    const std::size_t in = (ws.rr + j) % ROUTER_PORT_COUNT;
                    const auto& q = input_fifo_[in][vc];
                    if (q.empty()) continue;
                    const auto dst = static_cast<uint8_t>(q.front().get_header_field("dst_id"));
                    if (static_cast<std::size_t>(route_compute(dst, cfg_)) == out) {
                        candidate = in;
                        break;
                    }
                }
            }
            if (!candidate.has_value() || credit_[out][vc] == 0) continue;

            // Grant (spec §5): single atomic event.
            auto& q = input_fifo_[*candidate][vc];
            const Flit flit = q.front();
            q.pop_front();
            assert(credit_[out][vc] > 0 && "Router: credit underflow");
            --credit_[out][vc];
            output_fifo_[out].push_back(flit);
            credit_pulse_pending_.emplace_back(*candidate, static_cast<uint8_t>(vc));
            const uint64_t last = flit.get_header_field("last");
            if (last == 0) {
                ws.locked_input = *candidate;
            } else {
                ws.locked_input.reset();
                ws.rr = (*candidate + 1) % ROUTER_PORT_COUNT;
            }
            vc_rr_[out] = (vc + 1) % cfg_.num_vc;
            break;  // one grant per output port per cycle
        }
    }

    // Stage 1: landing register -> input VC FIFO.
    for (std::size_t port = 0; port < ROUTER_PORT_COUNT; ++port) {
        if (!landing_[port].has_value()) continue;
        const Flit f = *landing_[port];
        landing_[port].reset();
        const auto vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
        assert(input_fifo_[port][vc].size() < cfg_.vc_depth &&
               "Router: input FIFO overflow — upstream credit discipline broken");
        input_fifo_[port][vc].push_back(f);
    }
}

}  // namespace ni::cmodel::noc
