#pragma once
// Wormhole VC router for the c_model NoC fabric.
//
// 3-stage pipeline: stage 1 per-(input port, vc) FIFO (+RC at the
// FIFO head), stage 2 per-output wormhole arbitration (one wormhole packet per
// output until last flit) + VC allocation (VA) + crossbar, stage 3
// output FIFO -> link. Credit-based flow
// control; credit reserved at output-FIFO admission (the grant event).
// Lock semantics ported from FlooNoC floo_wormhole_arbiter/floo_vc_arbiter:
// per-output ownership locked to one (input port, vc) until packet last flit;
// decrement point matches BookSim2
// BufferState::SendingFlit.
// VA stage ported from the deprecated FlooNoC vc_router_util suite
// (hw/deprecated/floo_vc_assignment.sv, floo_vc_selection.sv,
// floo_vc_router_switch.sv, floo_vc_router.sv): after arbitration picks a
// candidate head flit, the output-side VC is assigned (preferred-VC map +
// FVADA fallback), credit is gated and consumed on the ASSIGNED output VC,
// and the assigned VC is restamped into the departing header. The upstream
// credit pulse keeps the INPUT-side VC (the FIFO slot freed). A flit with
// header fixed_vc=1 bypasses assignment entirely: out_vc = vc_id (NI pin,
// spec extension — not in the deprecated RTL).
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

namespace ni::cmodel::router {

enum class RouterPort : uint8_t { LOCAL = 0, NORTH = 1, EAST = 2, SOUTH = 3, WEST = 4 };
inline constexpr std::size_t ROUTER_PORT_COUNT = 5;

struct RouterConfig {
    uint8_t x = 0;
    uint8_t y = 0;
    uint8_t mesh_x_dim = NOC_MESH_X_DIM;
    uint8_t mesh_y_dim = NOC_MESH_Y_DIM;
    uint8_t num_vc = NOC_DAT_NUM_VC;
    std::size_t vc_depth = NOC_ROUTER_VC_DEPTH;
    std::size_t output_fifo_depth = NOC_ROUTER_OUTPUT_FIFO_DEPTH;
};

// Forward half of the router link contract. push_flit is always
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

// XY dimension-order route: X first, then Y, equal ejects LOCAL.
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

// Preferred output VC as a function of (this-hop output, next-hop route).
// Verbatim port of the XY-optimized hand map, floo_vc_assignment.sv:84-93
// (gen_xy_routing_optimized; FlooNoC Eject == our LOCAL; the index is the
// position of the next-hop direction in the ordered set of directions
// reachable through that link under XY — comment :85 "N: N,Ej, E: N,E,S,Ej,
// S: S,Ej, W: N,S,W,Ej"). The whole expression wraps % num_vc.
inline uint8_t preferred_vc(RouterPort out, RouterPort next_hop, uint8_t num_vc) {
    uint32_t pref;
    const bool out_ns = out == RouterPort::NORTH || out == RouterPort::SOUTH;
    if (out == RouterPort::LOCAL) {
        pref = 0;  // :86 OutputId >= Eject -> 0
    } else if (next_hop == RouterPort::LOCAL) {
        pref = out_ns ? 1 : 3;  // :88 (N/S eject) / :89 (E/W eject)
    } else if (out_ns) {
        pref = 0;  // :90 straight N/S
    } else if (next_hop == RouterPort::NORTH) {
        pref = 0;  // :91
    } else if (next_hop == RouterPort::SOUTH) {
        pref = out == RouterPort::EAST ? 2 : 1;  // :92
    } else {
        pref = out == RouterPort::EAST ? 1 : 2;  // :93 (E->E / W->W)
    }
    return static_cast<uint8_t>(pref % num_vc);
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

    void tick();

    // Test introspection
    std::size_t credit(std::size_t out_port, uint8_t vc) const { return credit_[out_port][vc]; }
    std::size_t input_fifo_size(std::size_t port, uint8_t vc) const {
        return input_fifo_[port][vc].size();
    }
    std::size_t output_fifo_size(std::size_t port) const { return output_fifo_[port].size(); }
    uint8_t num_vc() const { return cfg_.num_vc; }
    // Configured per-VC input FIFO capacity.
    std::size_t vc_depth() const { return cfg_.vc_depth; }
    // Configured per-output FIFO capacity.
    std::size_t output_fifo_depth() const { return cfg_.output_fifo_depth; }
    // Wormhole lock state per output port (nullopt = unlocked). Read-only
    // introspection for the co-sim fabric state dump.
    std::optional<std::size_t> wormhole_locked_input(std::size_t out_port) const {
        return wormhole_[out_port].locked_input;
    }
    std::optional<uint8_t> wormhole_locked_input_vc(std::size_t out_port) const {
        return wormhole_[out_port].locked_input_vc;
    }
    std::optional<uint8_t> wormhole_locked_output_vc(std::size_t out_port) const {
        return wormhole_[out_port].locked_output_vc;
    }
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

    void accept_flit(std::size_t port, const Flit& f);

    // Next-hop XY route seen from the neighbor behind `out` (D3: computed on
    // the fly from dst_id; bit-identical to the RTL's stored hdr.lookahead for
    // deterministic XY, floo_vc_assignment.sv:55-65). Never called for LOCAL.
    RouterPort next_hop_route(std::size_t out, uint8_t dst) const {
        RouterConfig n = cfg_;
        switch (static_cast<RouterPort>(out)) {
            case RouterPort::NORTH:
                ++n.y;
                break;
            case RouterPort::EAST:
                ++n.x;
                break;
            case RouterPort::SOUTH:
                --n.y;
                break;
            case RouterPort::WEST:
                --n.x;
                break;
            case RouterPort::LOCAL:
                assert(false && "Router: next_hop_route on LOCAL output");
                std::abort();
        }
        return route_compute(dst, n);
    }

    uint8_t preferred_out_vc(std::size_t out, uint8_t dst) const {
        const auto o = static_cast<RouterPort>(out);
        if (o == RouterPort::LOCAL) return 0;  // floo_vc_assignment.sv:86
        return preferred_vc(o, next_hop_route(out, dst), cfg_.num_vc);
    }

    // VA: floo_vc_assignment + floo_vc_selection translate, run per candidate
    // between arbitration and the grant event (floo_vc_router.sv:277-302,
    // SingleStage wiring :413-421). Returns the assigned output-side VC, or
    // nullopt when no eligible VC has credit — the candidate is not grantable
    // this tick (vc_valid_o gating, floo_vc_assignment.sv:96-116).
    std::optional<uint8_t> vc_assignment(std::size_t out, const Flit& f) const {
        // fixed_vc=1 bypass (D8): the NI-pinned VC is kept verbatim, credit
        // still gated on it, never overflowed to another VC.
        if (f.get_header_field("fixed_vc") != 0) {
            const auto vcid = static_cast<uint8_t>(f.get_header_field("vc_id"));
            if (credit_[out][vcid] > 0) return vcid;
            return std::nullopt;
        }
        const auto dst = static_cast<uint8_t>(f.get_header_field("dst_id"));
        const uint8_t pref = preferred_out_vc(out, dst);
        // FVADA: preferred VC not-full -> take it (floo_vc_selection.sv:32-34).
        if (credit_[out][pref] > 0) return pref;
        // Wormhole head (flit_tail=0): preferred VC only, no overflow, so the
        // whole worm rides one downstream VC (wh_vc_en gating with
        // FixedWormholeVC=0: floo_vc_router.sv:295, floo_vc_assignment.sv:110-112).
        if (f.get_header_field("flit_tail") == 0) return std::nullopt;
        // FVADA overflow: another non-full VC. The RTL scan loop overwrites
        // upward, so the HIGHEST-index non-full VC wins — faithful to the
        // overwrite order, do not "fix" (floo_vc_selection.sv:37-45).
        std::optional<uint8_t> sel;
        for (uint8_t v = 0; v < cfg_.num_vc; ++v) {
            if (v != pref && credit_[out][v] > 0) sel = v;
        }
        return sel;
    }

    struct WormholeState {
        std::optional<std::size_t> locked_input;
        // Input-side VC of the in-flight worm: which input FIFO keeps draining.
        std::optional<uint8_t> locked_input_vc;
        // Output-side (VA-assigned) VC the worm rides downstream: credit is
        // consumed and headers are stamped with this VC. Before the VA stage
        // the two coincided; post-VA they split.
        std::optional<uint8_t> locked_output_vc;
        std::size_t rr = 0;  // input round-robin (unlocked scan)
    };

    RouterConfig cfg_;
    // stage-1 input register, one flit/port/cycle
    std::array<std::optional<Flit>, ROUTER_PORT_COUNT> input_reg_{};
    std::array<std::vector<std::deque<Flit>>, ROUTER_PORT_COUNT> input_fifo_{};
    std::array<std::vector<std::size_t>, ROUTER_PORT_COUNT> credit_{};  // [out][vc]
    std::array<WormholeState, ROUTER_PORT_COUNT> wormhole_{};           // per-output (across VCs)
    std::array<std::size_t, ROUTER_PORT_COUNT> vc_rr_{};                // [out]
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
    if (input_reg_[port].has_value()) {
        assert(false && "Router::accept_flit: >1 flit per link per cycle");
        std::abort();
    }
    input_reg_[port] = f;
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

    // Stage 2: per-output grant. One wormhole packet per output across VCs.
    // Arbitration picks the candidate on the INPUT-side VC; VA then assigns
    // the OUTPUT-side VC (credit consume + header stamp). The upstream credit
    // pulse keeps the input-side VC (the FIFO slot freed).
    for (std::size_t out = 0; out < ROUTER_PORT_COUNT; ++out) {
        if (output_fifo_[out].size() >= cfg_.output_fifo_depth) continue;
        auto& ws = wormhole_[out];
        std::optional<std::size_t> candidate;
        uint8_t in_vc = 0;
        uint8_t out_vc = 0;
        if (ws.locked_input.has_value()) {
            // Locked: serve only the in-flight (input, input vc) until its last
            // flit; every continuation rides the head's assigned output VC
            // (mech 5, floo_vc_router.sv:295) and gates on THAT VC's credit.
            in_vc = *ws.locked_input_vc;
            auto& lq = input_fifo_[*ws.locked_input][in_vc];
            if (!lq.empty() && credit_[out][*ws.locked_output_vc] > 0) {
                const auto dst = static_cast<uint8_t>(lq.front().get_header_field("dst_id"));
                if (static_cast<std::size_t>(route_compute(dst, cfg_)) != out) {
                    assert(false &&
                           "Router: locked wormhole continuation routes to a different output "
                           "(malformed packet: flit_tail=0 head not closed by flit_tail=1 on this "
                           "(input,vc))");
                    std::abort();
                }
                // A non-pinned worm's locked output VC is always the head's
                // preferred VC; a pinned (fixed_vc=1) worm's NI-chosen VC
                // legitimately differs, so the check is conditioned.
                if (lq.front().get_header_field("fixed_vc") == 0 &&
                    *ws.locked_output_vc != preferred_out_vc(out, dst)) {
                    assert(false &&
                           "Router: locked wormhole output VC diverges from the recomputed "
                           "preferred VC (fixed_vc=0)");
                    std::abort();
                }
                candidate = ws.locked_input;
                out_vc = *ws.locked_output_vc;
            }
        } else {
            // Unlocked: VC round-robin, then input round-robin, over head flits
            // routed here. Credit is unknowable before VA (it depends on the
            // flit's fixed_vc bit and next-hop route), so there is no credit
            // pre-filter; VA gates credit per candidate, and a candidate whose
            // VA fails is skipped and the scan continues (D7, work-conserving).
            for (std::size_t kv = 0; kv < cfg_.num_vc && !candidate.has_value(); ++kv) {
                const auto vc = static_cast<uint8_t>((vc_rr_[out] + kv) % cfg_.num_vc);
                for (std::size_t j = 0; j < ROUTER_PORT_COUNT; ++j) {
                    const std::size_t in = (ws.rr + j) % ROUTER_PORT_COUNT;
                    const auto& q = input_fifo_[in][vc];
                    if (q.empty()) continue;
                    const auto dst = static_cast<uint8_t>(q.front().get_header_field("dst_id"));
                    if (static_cast<std::size_t>(route_compute(dst, cfg_)) != out) continue;
                    const auto assigned = vc_assignment(out, q.front());
                    if (!assigned.has_value()) continue;  // VA failure: no grant (mech 7)
                    candidate = in;
                    in_vc = vc;
                    out_vc = *assigned;
                    break;
                }
            }
        }
        if (!candidate.has_value()) continue;

        // Grant: single atomic event. Credit is consumed on the ASSIGNED
        // output VC; the upstream pulse returns the INPUT-side VC.
        auto& q = input_fifo_[*candidate][in_vc];
        Flit flit = q.front();
        q.pop_front();
        assert(credit_[out][out_vc] > 0 && "Router: credit underflow");
        --credit_[out][out_vc];
        // Stamp the assigned VC into the departing header (mech 6,
        // floo_vc_router_switch.sv:61,88 hdr.vc_id = vc_assignment_id_i).
        flit.set_header_field("vc_id", out_vc);
        output_fifo_[out].push_back(flit);
        credit_pulse_pending_.emplace_back(*candidate, in_vc);
        const uint64_t flit_tail = flit.get_header_field("flit_tail");
        if (flit_tail == 0) {
            ws.locked_input = *candidate;
            ws.locked_input_vc = in_vc;
            ws.locked_output_vc = out_vc;
        } else {
            ws.locked_input.reset();
            ws.locked_input_vc.reset();
            ws.locked_output_vc.reset();
            ws.rr = (*candidate + 1) % ROUTER_PORT_COUNT;
            vc_rr_[out] = static_cast<std::size_t>((in_vc + 1) % cfg_.num_vc);
        }
    }

    // Stage 1: input register -> input VC FIFO.
    for (std::size_t port = 0; port < ROUTER_PORT_COUNT; ++port) {
        if (!input_reg_[port].has_value()) continue;
        const Flit f = *input_reg_[port];
        input_reg_[port].reset();
        const auto vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
        assert(input_fifo_[port][vc].size() < cfg_.vc_depth &&
               "Router: input FIFO overflow — upstream credit discipline broken");
        input_fifo_[port][vc].push_back(f);
    }
}

}  // namespace ni::cmodel::router
