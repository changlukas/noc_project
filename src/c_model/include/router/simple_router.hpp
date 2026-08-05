#pragma once
// Ready/valid single-VC router for the c_model NoC fabric — line-translate of
// mainline floo_router.sv for REQ/RSP (RouteAlgo=XYRouting, NumPhysChannels=1,
// LockRouting=1'b1, NoLoopback=1'b1, XYRouteOpt=1'b1, VcImpl degenerates to
// passthrough at NumVirtChannels==NumPhysChannels==1). The credit Router
// (router.hpp) stays DAT's translate, untouched.
//
// Two-stage pipeline (floo_router.sv:129-144 InFifoDepth, :448-470
// OutFifoDepth): stage 1 per-(input port, vc) FIFO; stage 2 per-output
// wormhole-locked arbitration + crossbar, feeding either directly downstream
// (output_fifo_depth == 0, 1-stage total) or into an optional stage-3 output
// FIFO (output_fifo_depth > 0, 2-stage total), per spec §3.
//
// Two independent lock constructs are carried over from FlooNoC, not merged:
//   - route lock, per (input port, vc): floo_route_select.sv:200-220
//     (LockRouting) — the OUTPUT a worm's head flit resolved to is latched
//     and reused for every following body/tail flit, never recomputed. Not
//     "recompute + assert on mismatch" (that is Router's shortcut, avoided
//     here per binding review note).
//   - wormhole/arbiter lock, per output: floo_router.sv:425 instantiates
//     floo_output_arbiter, which instantiates floo_wormhole_arbiter
//     (floo_output_arbiter.sv:69-81); LockIn=1'b1 is hardcoded inside
//     (floo_wormhole_arbiter.sv:40), not an instantiation parameter — this is
//     what keeps one input's worm contiguous at a shared output while other
//     inputs wait. The WINNER itself is decided by a snapshot/freeze
//     (floo_wormhole_arbiter.sv:61-77, valid_d/valid_q/last_q): the
//     contending-input set is captured the instant any input asserts valid
//     while the output is idle, independent of downstream ready (ready_o is
//     derived from that frozen selection, not the other way around). An
//     input that turns valid after the freeze is excluded from this
//     arbitration round even if the frozen winner has not yet been granted —
//     backpressure must not let a later-arriving input become a candidate,
//     let alone win.
//
// Ready/valid flow control (floo_router.sv:473-475: "At the end point, we
// cannot make valid dependent on ready ... there must be cuts at the input of
// the endpoint"): ready is an almost-full early ready computed off current
// occupancy, ready = (size + SLACK <= depth). SLACK's shipped default is
// PROVISIONAL — see SimpleRouterConfig::ready_slack.
//
// Single VC: REQ/RSP are ratified 1-VC networks (S1). floo_vc_arbiter is not
// translated — degenerates to a passthrough only at
// NumVirtChannels==NumPhysChannels==1 (floo_vc_arbiter.sv:33-35); num_vc>1
// would need a real VC arbiter, which is self-designed-arbitration territory
// and out of this class's scope, so construction asserts num_vc==1.
//
// Port indices: reuses router::RouterPort (LOCAL=0,N,E,S,W) and
// router::route_compute() verbatim — not FlooNoC's own numbering. Mainline
// constructs that name a port (the tie-off conditions below) translate BY
// NAME, matching floo_router.sv:349-357 read as `in == South && out == East`.
#include "flit.hpp"
#include "ni_flit_constants.h"
#include "ni_params.h"
#include "router/router.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>
#include <vector>

namespace ni::cmodel::router {

struct SimpleRouterConfig {
    uint8_t x = 0;
    uint8_t y = 0;
    uint8_t mesh_x_dim = NOC_MESH_X_DIM;
    uint8_t mesh_y_dim = NOC_MESH_Y_DIM;
    // NumVirtChannels (floo_router.sv:23). Genuine parameter per the S1
    // ruling's "spec numbers are defaults, never hardcoded," but this class
    // has no VC arbiter translated — see file header. Must be 1.
    uint8_t num_vc = 1;
    // InFifoDepth, floo_router.sv:129-144.
    std::size_t input_fifo_depth = NOC_ROUTER_VC_DEPTH;
    // OutFifoDepth, floo_router.sv:448-470. 0 is legal (gen_no_out_fifo):
    // stage 2 drives the downstream link directly, no stage 3.
    std::size_t output_fifo_depth = 0;
    // Almost-full slack (floo_router.sv:473-475, Q1 in
    // .superpowers/sdd/IMPLEMENTATION_PLAN/s3a-stage-design.md §4): ready
    // deasserts `ready_slack` flits before the FIFO is physically full, to
    // cover the multi-cycle round trip of a registered ready wire between two
    // nodes (the design's own register-chain accounting: A out reg(T) -> B
    // set_inputs(T+1) -> B tick -> B out reg(T+1) -> A set_inputs(T+2) -> A
    // decides — at least two registrations, so a deasserted ready reaches the
    // sender 2+ cycles late).
    //
    // PROVISIONAL DEFAULT: 2 is the structural floor proved by that chain,
    // not a guess. The credit Router's analogous constant (NOC_ROUTER_VC_DEPTH
    // = 8) was fixed by MEASURING the credit loop in co-sim (returned 5
    // cycles); the same measurement is owed here — drive a known ready-loop
    // scenario through two wrapped SimpleRouter nodes in co-sim and count
    // cycles from a downstream ready deassertion to the upstream node
    // observing it — and is out of reach without co-sim (this task's tier is
    // Windows ctest only). Calibration item: T5 (wrap) or T7 (tail),
    // whichever lands the two-node co-sim harness first.
    //
    // Must be >= 1 (construction asserts this): RTL's own FIFO ready is a
    // slack=1 baseline, so 0 — zero round-trip margin — is unconditionally
    // wrong, not a legitimate degenerate calibration point.
    std::size_t ready_slack = 2;
};

// Ready/valid half of a SimpleRouter link, receiver side: the sender queries
// ready(vc) before push_flit, mirroring valid_i/ready_o/data_i on one
// direction of the spec §4.3 wire pair. SimpleRouter itself implements this
// per input port (see input()) and holds pointers to it per output
// (set_downstream()) — the same interface serves both directions since both
// ends of a link are, from the link's perspective, "a receiver with a ready
// signal."
class SimpleRouterLink {
  public:
    virtual ~SimpleRouterLink() = default;
    virtual bool ready(uint8_t vc) const = 0;
    virtual void push_flit(const Flit& flit) = 0;
};

// floo_router.sv:349-357 (NoLoopback && XYRouteOpt): structural tie-offs that
// delete connectivity BEFORE arbitration, not an assert checked after the
// fact. Named by RouterPort, per the port-index convention above.
//
// Behaviourally these arcs are unreachable anyway: route_compute is XY-DOR,
// so it never targets the input's own port (no loopback) and never turns
// South/North input traffic onto East/West (X is already resolved before a
// flit starts moving on Y). The skip is required translate fidelity
// regardless — floo_router.sv physically disconnects these arcs, it does not
// rely on route_compute's invariant.
inline bool tie_off(RouterPort in, RouterPort out) {
    if (in == out) return true;  // NoLoopback, floo_router.sv:349
    if ((in == RouterPort::SOUTH || in == RouterPort::NORTH) &&
        (out == RouterPort::EAST || out == RouterPort::WEST)) {
        return true;  // XYRouteOpt Y->X turn, floo_router.sv:350-351
    }
    return false;
}

class SimpleRouter {
  public:
    explicit SimpleRouter(const SimpleRouterConfig& cfg) : cfg_(cfg) {
        if (cfg_.num_vc != 1) {
            assert(false && "SimpleRouter: num_vc must be 1 (no VC arbiter translated)");
            std::abort();
        }
        if (cfg_.ready_slack < 1) {
            assert(false &&
                   "SimpleRouter: ready_slack must be >= 1 (0 is unconditionally wrong, "
                   "not a calibration point — RTL's own FIFO ready baseline is slack=1)");
            std::abort();
        }
        if (cfg_.input_fifo_depth < cfg_.ready_slack + 1) {
            assert(false && "SimpleRouter: input_fifo_depth must be >= ready_slack + 1");
            std::abort();
        }
        if (!(cfg_.x < cfg_.mesh_x_dim && cfg_.y < cfg_.mesh_y_dim)) {
            assert(false && "SimpleRouter: own coordinate outside mesh");
            std::abort();
        }
        for (std::size_t p = 0; p < ROUTER_PORT_COUNT; ++p) {
            input_fifo_[p].resize(cfg_.num_vc);
            route_lock_[p].resize(cfg_.num_vc);
            input_adapters_.emplace_back(this, p);
        }
    }
    SimpleRouter(const SimpleRouter&) = delete;
    SimpleRouter(SimpleRouter&&) = delete;
    SimpleRouter& operator=(const SimpleRouter&) = delete;
    SimpleRouter& operator=(SimpleRouter&&) = delete;

    SimpleRouterLink& input(std::size_t port) {
        assert(port < ROUTER_PORT_COUNT);
        return input_adapters_[port];
    }
    void set_downstream(std::size_t port, SimpleRouterLink& link) {
        assert(port < ROUTER_PORT_COUNT);
        downstream_[port] = &link;
    }

    void tick();

    // Test introspection
    bool ready(std::size_t port, uint8_t vc) const {
        assert(port < ROUTER_PORT_COUNT && vc < cfg_.num_vc);
        return input_fifo_[port][vc].size() + cfg_.ready_slack <= cfg_.input_fifo_depth;
    }
    std::size_t input_fifo_size(std::size_t port, uint8_t vc) const {
        return input_fifo_[port][vc].size();
    }
    std::size_t output_fifo_size(std::size_t port) const { return output_fifo_[port].size(); }
    // Wormhole lock state per output port (nullopt = unlocked).
    std::optional<std::size_t> wormhole_locked_input(std::size_t out_port) const {
        return wormhole_[out_port].locked_input;
    }
    // Route lock state per (input port, vc) (nullopt = unlocked).
    std::optional<RouterPort> route_locked(std::size_t in_port, uint8_t vc) const {
        return route_lock_[in_port][vc];
    }

  private:
    struct InputAdapter : SimpleRouterLink {
        SimpleRouter* parent;
        std::size_t port;
        InputAdapter(SimpleRouter* p, std::size_t idx) : parent(p), port(idx) {}
        bool ready(uint8_t vc) const override { return parent->ready(port, vc); }
        void push_flit(const Flit& f) override { parent->accept_flit(port, f); }
    };

    void accept_flit(std::size_t port, const Flit& f);

    // route_compute() takes router::RouterConfig; SimpleRouterConfig is its
    // own type (distinct depth/slack fields from the credit router's config,
    // not a hardcoded RTL routing rule), so adapt the coordinate subset each
    // call rather than reuse route_compute's argument type or duplicate its
    // body.
    RouterPort compute_route(uint8_t dst) const {
        RouterConfig rc;
        rc.x = cfg_.x;
        rc.y = cfg_.y;
        rc.mesh_x_dim = cfg_.mesh_x_dim;
        rc.mesh_y_dim = cfg_.mesh_y_dim;
        return route_compute(dst, rc);
    }

    // floo_route_select.sv:200-220 LockRouting: the route latched at (in,vc)
    // takes precedence over a fresh compute. Returns nullopt only when
    // unlocked and the FIFO is empty (nothing to route).
    std::optional<RouterPort> head_route(std::size_t in, uint8_t vc) const {
        if (route_lock_[in][vc].has_value()) return route_lock_[in][vc];
        const auto& q = input_fifo_[in][vc];
        if (q.empty()) return std::nullopt;
        const auto dst = static_cast<uint8_t>(q.front().get_header_field("dst_id"));
        return compute_route(dst);
    }

    struct WormholeState {
        // Frozen winner for this output (single VC: no locked_vc). Set at
        // the freeze (floo_wormhole_arbiter.sv:61-77), before any grant may
        // have happened; stays set across the whole worm once granting
        // starts; cleared only when the worm's tail flit is granted.
        std::optional<std::size_t> locked_input;
        std::size_t rr = 0;  // input round-robin (unlocked scan)
    };

    SimpleRouterConfig cfg_;
    // stage-1 input register, one flit/port/cycle
    std::array<std::optional<Flit>, ROUTER_PORT_COUNT> input_reg_{};
    std::array<std::vector<std::deque<Flit>>, ROUTER_PORT_COUNT> input_fifo_{};  // [port][vc]
    std::array<std::vector<std::optional<RouterPort>>, ROUTER_PORT_COUNT>
        route_lock_{};                                               // [port][vc]
    std::array<WormholeState, ROUTER_PORT_COUNT> wormhole_{};        // [out]
    std::array<std::deque<Flit>, ROUTER_PORT_COUNT> output_fifo_{};  // stage 3 only
    std::array<SimpleRouterLink*, ROUTER_PORT_COUNT> downstream_{};
    std::vector<InputAdapter> input_adapters_;
};

inline void SimpleRouter::accept_flit(std::size_t port, const Flit& f) {
    const auto vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
    if (vc >= cfg_.num_vc) {
        assert(false && "SimpleRouter::accept_flit: vc_id >= num_vc");
        std::abort();
    }
    if (input_reg_[port].has_value()) {
        assert(false && "SimpleRouter::accept_flit: >1 flit per link per cycle");
        std::abort();
    }
    input_reg_[port] = f;
}

inline void SimpleRouter::tick() {
    // Stages run in reverse pipeline order so a flit advances one stage per
    // tick, matching router::Router's convention.

    // Stage 3 (output_fifo_depth > 0 only): output FIFO -> link, gated on
    // downstream ready (floo_router.sv:448-470 gen_out_fifo; ready/valid, so
    // unlike the credit Router this drain is NOT unconditional).
    for (std::size_t out = 0; out < ROUTER_PORT_COUNT; ++out) {
        if (cfg_.output_fifo_depth == 0) continue;
        if (output_fifo_[out].empty() || !downstream_[out]) continue;
        if (!downstream_[out]->ready(0)) continue;
        downstream_[out]->push_flit(output_fifo_[out].front());
        output_fifo_[out].pop_front();
    }

    // Stage 2: per-output grant. One wormhole packet per output (single VC:
    // no per-output VC arbitration, floo_vc_arbiter is not translated).
    for (std::size_t out = 0; out < ROUTER_PORT_COUNT; ++out) {
        auto& ws = wormhole_[out];

        // Winner snapshot/freeze (floo_wormhole_arbiter.sv:61-77
        // valid_d/valid_q/last_q): determined the instant any input becomes
        // a valid, non-tied-off candidate, INDEPENDENT of downstream
        // readiness — ready_o is derived from the frozen winner, not the
        // other way around (:61-65 gate ready_o on valid_selected_idx, which
        // is already fixed by then). Runs every tick regardless of
        // downstream ready so a later-arriving input can never join or steal
        // an arbitration round that is already in progress.
        if (!ws.locked_input.has_value()) {
            for (std::size_t j = 0; j < ROUTER_PORT_COUNT; ++j) {
                const std::size_t in = (ws.rr + j) % ROUTER_PORT_COUNT;
                if (tie_off(static_cast<RouterPort>(in), static_cast<RouterPort>(out))) continue;
                if (input_fifo_[in][0].empty()) continue;
                const auto route = head_route(in, 0);
                if (route.has_value() && static_cast<std::size_t>(*route) == out) {
                    ws.locked_input = in;
                    break;
                }
            }
        }
        if (!ws.locked_input.has_value()) continue;  // nothing valid for this output yet

        // Idle (no steal) while the frozen winner's FIFO is empty —
        // floo_wormhole_arbiter holds valid_q until last_q, it does not let
        // another requester in even though nothing has been granted yet.
        const std::size_t in = *ws.locked_input;
        if (input_fifo_[in][0].empty()) continue;

        // Downstream/output-fifo readiness gates only the GRANT itself, not
        // who won — the winner was already frozen above.
        const bool direct = (cfg_.output_fifo_depth == 0);
        if (direct) {
            if (!downstream_[out] || !downstream_[out]->ready(0)) continue;
        } else {
            if (output_fifo_[out].size() >= cfg_.output_fifo_depth) continue;
        }

        // Grant: single atomic event. Its route is not re-examined —
        // route_lock_ already pins it to `out` once locked (set at the same
        // grant that set ws.locked_input) — so this is the direct translate
        // of floo_route_select's registered bypass, not a recompute-and-compare.
        auto& q = input_fifo_[in][0];
        const Flit flit = q.front();
        q.pop_front();
        if (direct) {
            downstream_[out]->push_flit(flit);
        } else {
            output_fifo_[out].push_back(flit);
        }
        const uint64_t flit_tail = flit.get_header_field("flit_tail");
        if (flit_tail == 0) {
            route_lock_[in][0] = static_cast<RouterPort>(out);
            // ws.locked_input == in already (frozen above or from a prior
            // grant this same worm) — stays locked across the worm.
        } else {
            route_lock_[in][0].reset();
            ws.locked_input.reset();
            ws.rr = (in + 1) % ROUTER_PORT_COUNT;
        }
    }

    // Stage 1: input register -> input FIFO. Ready/valid discipline: a caller
    // that pushes without having observed ready() overflows the FIFO past
    // input_fifo_depth, which aborts loudly rather than silently corrupting
    // state (same guard as router::Router's stage 1).
    for (std::size_t port = 0; port < ROUTER_PORT_COUNT; ++port) {
        if (!input_reg_[port].has_value()) continue;
        const Flit f = *input_reg_[port];
        input_reg_[port].reset();
        const auto vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
        assert(input_fifo_[port][vc].size() < cfg_.input_fifo_depth &&
               "SimpleRouter: input FIFO overflow — ready/valid discipline broken");
        input_fifo_[port][vc].push_back(f);
    }
}

}  // namespace ni::cmodel::router
