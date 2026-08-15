#pragma once
// Ready/valid single-VC router for the c_model NoC fabric — line-translate of
// mainline floo_router.sv for REQ/RSP (RouteAlgo=XYRouting, NumPhysChannels=1,
// LockRouting=1'b1, NoLoopback=1'b1, XYRouteOpt=1'b1 minus its Y->X tie-offs
// (see tie_off), VcImpl degenerates to
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
//
// S4 multicast fork (T3b): a collective head (collective_op != UNICAST) forks
// to the multi-hot branch set of route_mask_fork() (route_mask.hpp). Branch
// accept tracking is the SAME mainline block the DAT Router (router.hpp) got
// in T3 — floo_router.sv:344-394 past_handshakes — but here in its native
// ready/valid form, so the translate is close to verbatim: no credit, no VA,
// no per-branch output VC. A branch "handshakes" by the grant this class
// already performs (downstream ready() or output-FIFO space). The route latch
// generalizes from one port to a port MASK, which is what upstream stores
// anyway (route_sel_q is NumRoutes-wide, floo_route_select.sv:46-57, :211-216).
// Unicast traffic keeps the pre-S4 path: a one-hot branch set pops on its
// single grant, exactly as before.
//
// S4 CollectB join (T4): the reverse-path counterpart, ported from the
// reduction pair mainline hangs off the output arbiter — floo_reduction_sync
// (the per-candidate stream_join_dynamic) and floo_reduction_arbiter (the lzc
// pick, the expected-input mask, and the BRESP survivor scan), instantiated at
// floo_router.sv:396-446 / floo_output_arbiter.sv:84-143. Where a multicast's
// B replicas converge, they merge into ONE B: the input ports the collect
// waits on come from route_mask_join(), the merge forwards a whole input flit,
// and every contributing head is consumed in the same handshake. It carries no
// state — partial arrivals simply wait in their input FIFOs, which is exactly
// the RTL structure.
#include "axi/types.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include "ni_params.h"
#include "router/route_mask.hpp"
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

// floo_router.sv:349 (NoLoopback): a structural tie-off that deletes
// connectivity BEFORE arbitration, not an assert checked after the fact. Named
// by RouterPort, per the port-index convention above.
//
// The XYRouteOpt Y->X arcs (floo_router.sv:350-351) are DELIBERATELY not tied
// off. Upstream may disconnect them because every destination there sits on the
// router's LOCAL port, so no route can ever end on a Y->X movement. Here a
// peripheral hangs off a boundary port, and route_compute ejects to that port at
// the destination coordinate — an x-face ejection of a flit that arrived on
// North/South IS a Y->X movement, and a route resolving to a port the crossbar
// cannot reach is not resolved. The credit Router (router.hpp) carries these
// arcs already and works; keeping the pair consistent removes an asymmetry
// rather than adding a mechanism. The alternative — tying off only the boundary
// ports with no peripheral behind them — would make the crossbar depend on
// topology config this class does not have.
//
// NoLoopback's in==out skip is NOT applied to LOCAL — divergence from
// floo_router.sv's blanket in==out tie-off, per this project's standing
// ruling (IMPLEMENTATION_PLAN.md Stage 3b: "LOCAL->LOCAL is LEGAL by design
// -- the self-transaction path, exercised by passing co-sim; suppress
// self-traffic via the generator's --exclude-self, not the router"). A
// node's own self-targeted traffic (NMU -> its own NSU) is real and
// exercised: route_compute(dst=own coords) legitimately resolves to LOCAL on
// the LOCAL input. Tying that arc off (as floo_router.sv does) strands the
// flit in the LOCAL input FIFO forever — invisible to the credit Router's
// FABRIC-DUMP, which doesn't cover SimpleRouter queues (S3a T6 node0 hang,
// mesh_2x2_vc1).
inline bool tie_off(RouterPort in, RouterPort out) {
    if (in == out && in != RouterPort::LOCAL) return true;  // NoLoopback, floo_router.sv:349
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
            route_lock_[p].assign(cfg_.num_vc, 0);
            fork_done_[p].assign(cfg_.num_vc, 0);
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
    // Route lock state per (input port, vc): the latched branch MASK (0 =
    // unlocked). Mask-valued because a forked worm latches its whole branch
    // set, which is what upstream's route_sel_q holds too
    // (floo_route_select.sv:46-57, :211-216). A unicast worm latches one bit.
    PortMask route_locked(std::size_t in_port, uint8_t vc) const {
        return route_lock_[in_port][vc];
    }
    // Multicast fork state per (input, vc) — mirrors the DAT Router's
    // accessors (router.hpp:181-189, design §1.3 detection b). `expected` is
    // the branch set in force for the parked front flit: the latch once a worm
    // is locked, else recomputed from the front (0 when nothing is parked).
    // `done` holds the branches that already granted that flit. A wedged
    // multicast triages as done != expected frozen across ticks with the
    // missing branches' outputs locked to another worm, instead of a bare
    // timeout. Shares the empty-fork-set fatal assert with the datapath.
    PortMask fork_expected_mask(std::size_t in_port, uint8_t vc) const {
        if (in_port >= ROUTER_PORT_COUNT || vc >= cfg_.num_vc) return 0;
        return head_mask(in_port, vc);
    }
    PortMask fork_done_mask(std::size_t in_port, uint8_t vc) const {
        if (in_port >= ROUTER_PORT_COUNT || vc >= cfg_.num_vc) return 0;
        return fork_done_[in_port][vc];
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

    // Our header has no third collective opcode: on RSP the only collective
    // flits are Bs (Ar/R are always UNICAST, spec §6 :356; AW/W never enter
    // RSP), so `collective_op != UNICAST AND axi_ch in {NarrowB, DataB}` IS
    // mainline's CollectB opcode (design §3.1 encoding note). The join keys on
    // BOTH conditions, never on the opcode alone: upstream's arbiter may
    // reduce by opcode alone (floo_output_arbiter.sv:57-64) because its
    // CollectB is a distinct code — our overload of MULTICAST is not.
    static bool is_b_channel(uint64_t axi_ch) {
        return axi_ch == ni::AXI_CH_NarrowB || axi_ch == ni::AXI_CH_DataB;
    }
    static bool is_collect_b(const Flit& f) {
        return f.get_header_field("collective_op") != ni::COLLECTIVE_OP_UNICAST &&
               is_b_channel(f.get_header_field("axi_ch"));
    }

    // Does a CollectB join fire at `out` this tick, and with which anchor
    // input / expected-input set (floo_reduction_arbiter.sv:69-106,
    // floo_reduction_sync.sv:29-55)? Read-only: the merge itself is
    // grant_join().
    bool join_valid(std::size_t out, std::size_t* sel, PortMask* expected) const;
    void grant_join(std::size_t out, std::size_t sel, PortMask expected);

    // Output-side admission for one grant, and the push that follows it:
    // downstream ready in direct mode (output_fifo_depth == 0), stage-3 FIFO
    // space otherwise. Shared by the unicast grant and the join so the two
    // cannot drift apart.
    bool out_admits(std::size_t out) const {
        if (cfg_.output_fifo_depth == 0) return downstream_[out] && downstream_[out]->ready(0);
        return output_fifo_[out].size() < cfg_.output_fifo_depth;
    }
    void out_push(std::size_t out, const Flit& f) {
        if (cfg_.output_fifo_depth == 0) {
            downstream_[out]->push_flit(f);
        } else {
            output_fifo_[out].push_back(f);
        }
    }

    // route_compute() takes router::RouterConfig; SimpleRouterConfig is its
    // own type (distinct depth/slack fields from the credit router's config,
    // not a hardcoded RTL routing rule), so adapt the coordinate subset each
    // call rather than reuse route_compute's argument type or duplicate its
    // body.
    RouterConfig route_cfg() const {
        RouterConfig rc;
        rc.x = cfg_.x;
        rc.y = cfg_.y;
        rc.mesh_x_dim = cfg_.mesh_x_dim;
        rc.mesh_y_dim = cfg_.mesh_y_dim;
        return rc;
    }
    RouterPort compute_route(uint8_t dst, uint8_t dst_port) const {
        return route_compute(dst, dst_port, route_cfg());
    }

    // Branch set of a flit at an input FIFO head: the one-hot XY route for
    // unicast, the multi-hot fork set for a collective (F1,
    // floo_route_xymask.sv:104-164, reached through the T1 API). An empty
    // collective fork set is fatal — the all-branches-done pop condition below
    // would otherwise be trivially true and silently drop a misrouted
    // multicast (T3 hard rule, router.hpp:280-295).
    PortMask head_expected_mask(const Flit& f) const {
        const auto dst = static_cast<uint8_t>(f.get_header_field("dst_id"));
        const auto dst_port = static_cast<uint8_t>(f.get_header_field("dst_port_id"));
        if (f.get_header_field("collective_op") == ni::COLLECTIVE_OP_UNICAST) {
            return port_bit(compute_route(dst, dst_port));
        }
        // Reserved-code guard (OUR RULE, spec §6 :356 leaves codes 2-3
        // reserved). Every collective classification in this class keys on
        // `!= UNICAST`, which is what makes a one-hot collective still take the
        // collective path (the T3 Critical) — so a reserved code would silently
        // become a fork on REQ or a CollectB on RSP. Rejecting the code itself
        // keeps that keying intact and catches BOTH cases in one place, which
        // narrowing is_collect_b() to `== MULTICAST` would not (it would only
        // hide the B case, turning it into a silently unmerged plain forward).
        if (f.get_header_field("collective_op") != ni::COLLECTIVE_OP_MULTICAST) {
            assert(false &&
                   "SimpleRouter: reserved collective_op code on a flit (only UNICAST "
                   "and MULTICAST are defined)");
            std::abort();
        }
        // Class guard (OUR RULE, design §3.1). One class serves both the REQ
        // and the RSP instance and carries no network field, so the guard is
        // written on axi_ch: the read channels are the only NON-B channels a
        // flit on RSP can carry, and a collective on them is illegal on both
        // networks anyway (ARUSER has no collective surface, spec §6 :324, so
        // reads are unicast everywhere). A collective AW/W is legal REQ fork
        // traffic and is therefore neither rejected nor rejectable here.
        const auto axi_ch = f.get_header_field("axi_ch");
        if (axi_ch == ni::AXI_CH_NarrowR || axi_ch == ni::AXI_CH_DataR ||
            axi_ch == ni::AXI_CH_NarrowAr || axi_ch == ni::AXI_CH_DataAr) {
            assert(false &&
                   "SimpleRouter: non-B collective flit on a read channel — reads are unicast "
                   "everywhere, so the header is mis-stamped");
            std::abort();
        }
        // A CollectB routes UNICAST toward its collector: upstream takes the
        // mask route only for the Multicast opcode (floo_route_select.sv:
        // 184-189), and a CollectB's dst_id names the single collecting node —
        // the wildcard sits on src_id. Its expected-INPUT set is a separate
        // computation, in join_valid().
        if (is_b_channel(axi_ch)) return port_bit(compute_route(dst, dst_port));

        // Boundary-port guard (OUR RULE, design Decision 4). The fork below
        // ignores dst_port_id, so a non-zero one would fork to LOCAL at every
        // member while the CollectB branch above still routes on it.
        assert(f.get_header_field("dst_port_id") == 0 &&
               "collective flit names a boundary port -- a fork replica is copied verbatim, so "
               "one dst_port_id would reach every member (Decision 4: a peripheral is never a "
               "member)");
        const auto src = static_cast<uint8_t>(f.get_header_field("src_id"));
        const auto cmask = static_cast<uint8_t>(f.get_header_field("collective_mask"));
        const PortMask m = route_mask_fork(dst, src, cmask, route_cfg());
        if (m == 0) {
            assert(false &&
                   "SimpleRouter: collective head with empty fork set at this router "
                   "(misrouted multicast — silent drop forbidden)");
            std::abort();
        }
        return m;
    }

    static constexpr bool is_fork_set(PortMask m) { return (m & (m - 1)) != 0; }

    // floo_route_select.sv:200-220 LockRouting: the branch mask latched at
    // (in,vc) takes precedence over a fresh compute. 0 only when unlocked and
    // the FIFO is empty (nothing to route).
    PortMask head_mask(std::size_t in, uint8_t vc) const {
        if (route_lock_[in][vc] != 0) return route_lock_[in][vc];
        const auto& q = input_fifo_[in][vc];
        if (q.empty()) return 0;
        return head_expected_mask(q.front());
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
    std::array<std::vector<PortMask>, ROUTER_PORT_COUNT> route_lock_{};          // [port][vc]
    // Branch outputs that already handshaked the parked front flit
    // (floo_router.sv:338 past_handshakes_q). The ONLY new stored state — the
    // expected set is the latch, or a pure function of the front's header.
    std::array<std::vector<PortMask>, ROUTER_PORT_COUNT> fork_done_{};  // [port][vc]
    std::array<WormholeState, ROUTER_PORT_COUNT> wormhole_{};           // [out]
    std::array<std::deque<Flit>, ROUTER_PORT_COUNT> output_fifo_{};     // stage 3 only
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

// floo_reduction_arbiter.sv:69-106 — one floo_reduction_sync per candidate
// input, all fed the same output's valid set, with an lzc picking the first
// candidate whose join is complete.
//
// lzc MODE, resolved (design §3.2 step 3 [UNVERIFIED]): common_cells 1.39.0
// (FlooNoC Bender.lock rev 9ca8a76) declares `parameter bit MODE = 1'b0` with
// the comment "0 -> trailing zero, 1 -> leading zero", and :100-106
// instantiates it with WIDTH only — so cnt_o is the number of trailing zeros,
// i.e. the index of the LOWEST set bit. Lowest-index-wins holds; the ascending
// scan below is the direct translate.
inline bool SimpleRouter::join_valid(std::size_t out, std::size_t* sel, PortMask* expected) const {
    for (std::size_t in = 0; in < ROUTER_PORT_COUNT; ++in) {
        if (tie_off(static_cast<RouterPort>(in), static_cast<RouterPort>(out))) continue;
        const auto& q = input_fifo_[in][0];
        if (q.empty()) continue;
        const Flit& anchor = q.front();
        if (!is_collect_b(anchor)) continue;
        // masked_valid[out][in] (floo_router.sv:360-362): the arbiter at this
        // output only ever sees the inputs whose head routes HERE.
        if (!port_in_mask(head_mask(in, 0), static_cast<RouterPort>(out))) continue;

        const auto dst = static_cast<uint8_t>(anchor.get_header_field("dst_id"));
        const auto src = static_cast<uint8_t>(anchor.get_header_field("src_id"));
        const auto cmask = static_cast<uint8_t>(anchor.get_header_field("collective_mask"));
        // in_route_mask[i] (floo_reduction_arbiter.sv:72-82, FwdMode=0),
        // through the frozen T1 API.
        const PortMask exp = route_mask_join(dst, src, cmask, route_cfg());
        // stream_join_dynamic fires on `&(inp_valid | ~sel) && |sel`, so an
        // empty sel simply never fires upstream. Here it is FATAL: the
        // all-satisfied-by-vacuity condition would otherwise swallow the B and
        // hang the write, the same class of bug as the fork's empty-branch-set
        // abort.
        if (exp == 0) {
            assert(false &&
                   "SimpleRouter: CollectB with an empty expected-input set at this router "
                   "(misrouted collect — silent drop forbidden)");
            std::abort();
        }
        // OUR RULE: the port the B arrived on must be one of the ports the
        // geometry expects it from. Upstream has the same hole — inp_ready_o[i]
        // is gated on sel_i[i], so an anchor outside its own sel would be
        // forwarded and never consumed, re-firing every cycle. Fatal instead.
        if (!port_in_mask(exp, static_cast<RouterPort>(in))) {
            assert(false &&
                   "SimpleRouter: CollectB arrived on a port outside its own expected-input set "
                   "(echoed collective_mask disagrees with the delivery path)");
            std::abort();
        }

        // floo_reduction_sync.sv:39-45 — a member's valid only counts when its
        // head belongs to the SAME collect (equal dst_id and collective_mask);
        // anything else waits in its own FIFO. Equal dst_id also means every
        // member routes to this same output, so no second route check is owed.
        bool complete = true;
        for (std::size_t j = 0; j < ROUTER_PORT_COUNT; ++j) {
            if (!port_in_mask(exp, static_cast<RouterPort>(j))) continue;
            const auto& qj = input_fifo_[j][0];
            if (qj.empty()) {
                complete = false;
                break;
            }
            const Flit& m = qj.front();
            if (!is_collect_b(m) ||
                m.get_header_field("dst_id") != anchor.get_header_field("dst_id") ||
                m.get_header_field("collective_mask") !=
                    anchor.get_header_field("collective_mask")) {
                complete = false;
                break;
            }
            // OUR RULE on top of :41-43 (ruling 1): the replicas of ONE
            // multicast AW carry the NMU's pre-fanout ordering_tag, the same
            // class, and the same bid. Disagreement means two different writes
            // were about to be merged into one B — a model bug, not
            // backpressure, so it aborts instead of waiting.
            if (m.get_header_field("ordering_tag") != anchor.get_header_field("ordering_tag") ||
                m.get_header_field("axi_ch") != anchor.get_header_field("axi_ch") ||
                m.get_payload_field("B", "bid") != anchor.get_payload_field("B", "bid")) {
                assert(false &&
                       "SimpleRouter: joined CollectB replicas disagree on "
                       "ordering_tag/axi_ch/bid (two different writes merging into one B; "
                       "triage: an R1 violation — concurrent collects with overlapping trees — "
                       "reaches here too, since the same-collect filter is dst_id+mask only)");
                std::abort();
            }
        }
        if (!complete) continue;
        *sel = in;
        *expected = exp;
        return true;
    }
    return false;
}

// The merge itself. popcount(expected) == 1 reaches here like any other size:
// a single-member collect is a plain forward THROUGH the join path, never down
// the unicast path — the unicast candidate scan excludes CollectB outright, so
// routing it there would leave the B forwarded by nobody.
inline void SimpleRouter::grant_join(std::size_t out, std::size_t sel, PortMask expected) {
    // Survivor + BRESP precedence (floo_reduction_arbiter.sv:116-131): start
    // from the lzc-selected flit, then scan the expected inputs in route-index
    // order and let the FIRST SLVERR win and break. DECERR is NOT elevated — a
    // deliberate divergence from AXI worst-response, recorded in
    // docs/router-spec.md. A WHOLE input flit is forwarded and no header is
    // ever rebuilt, so the survivor's src_id stays inside the member set and
    // the echoed collective_mask passes through untouched (any member anchors
    // the same tree).
    Flit survivor = input_fifo_[sel][0].front();
    for (std::size_t j = 0; j < ROUTER_PORT_COUNT; ++j) {
        if (!port_in_mask(expected, static_cast<RouterPort>(j))) continue;
        const Flit& m = input_fifo_[j][0].front();
        if (m.get_payload_field("B", "bresp") == static_cast<uint64_t>(axi::Resp::SLVERR)) {
            survivor = m;
            break;
        }
    }
    out_push(out, survivor);
    // stream_join_dynamic's all-ready (inp_ready_o[i] = oup_valid & oup_ready
    // & sel_i[i]): every contributing head is consumed in the SAME handshake.
    for (std::size_t j = 0; j < ROUTER_PORT_COUNT; ++j) {
        if (port_in_mask(expected, static_cast<RouterPort>(j))) input_fifo_[j][0].pop_front();
    }
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

    // Continuation branch-set check (OUR RULE, src-anchored; the hardened
    // translate of floo_route_select.sv:222-226, which only $warnings on a
    // locked-route mismatch). Keyed on `collective_op != UNICAST`, NEVER on
    // multi-hotness: at a spread-end hop a collective worm's fork set is
    // legally ONE-HOT and legally diverges from the header's dst_id XY route, so a
    // one-hot collective must still be set-checked (this is exactly the T3
    // Critical). Unicast is deliberately exempt — this class latches the route
    // and lets the latch win over any mid-worm dst change
    // (floo_route_select.sv:211-216), it does not recompute-and-assert.
    for (std::size_t in = 0; in < ROUTER_PORT_COUNT; ++in) {
        for (uint8_t vc = 0; vc < cfg_.num_vc; ++vc) {
            if (route_lock_[in][vc] == 0) continue;
            const auto& q = input_fifo_[in][vc];
            if (q.empty()) continue;
            if (q.front().get_header_field("collective_op") == ni::COLLECTIVE_OP_UNICAST) {
                // T3b follow-up (OUR RULE): a MULTI-HOT latch can only have
                // been seeded by a collective head, and in-order link delivery
                // means the front under a held latch belongs to that same
                // worm — so a unicast front here is a continuation that lost
                // its collective_op bit, in every multi-branch case. One-hot
                // latches are deliberately exempt: that subcase is bit-for-bit
                // the legal latch-wins unicast shape
                // (floo_route_select.sv:211-216).
                if (is_fork_set(route_lock_[in][vc])) {
                    assert(false &&
                           "SimpleRouter: unicast front under a multi-hot route latch (fork worm "
                           "continuation lost its collective_op bit)");
                    std::abort();
                }
                continue;
            }
            if (head_expected_mask(q.front()) != route_lock_[in][vc]) {
                assert(false &&
                       "SimpleRouter: fork worm continuation branch set diverges from the "
                       "head's (corrupted W continuation header)");
                std::abort();
            }
        }
    }

    // Stage 2: per-output grant. One wormhole packet per output (single VC:
    // no per-output VC arbitration, floo_vc_arbiter is not translated).
    for (std::size_t out = 0; out < ROUTER_PORT_COUNT; ++out) {
        auto& ws = wormhole_[out];

        // Reduction (CollectB join), evaluated before the grant but NOT before
        // the freeze: upstream's floo_reduction_arbiter and
        // floo_wormhole_arbiter hang off the same output IN PARALLEL, and only
        // the final prio stream_arbiter (index 0 = reduction,
        // floo_output_arbiter.sv:126-139) decides which of the two gets
        // ready_i. So the unicast winner still freezes this tick and simply
        // grants on a later one — delayed, never stolen (design §3.2 step 5).
        //
        // Sequential-loop note: like the unicast grants around it, this reads
        // FIFO state already mutated by earlier outputs in the same tick. Two
        // joins can never share an input (all members of one collect carry the
        // same dst_id, hence the same output), so the only order sensitivity
        // is the pre-S4 one §1.1 rule 5 already blesses — a later output may
        // see a front that an earlier output's grant uncovered.
        std::size_t join_sel = 0;
        PortMask join_expected = 0;
        const bool join = join_valid(out, &join_sel, &join_expected);

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
                // reduce_mask exclusion (floo_output_arbiter.sv:57-67): a
                // CollectB head is never a unicast candidate — it belongs to
                // the reduction arbiter, which merges it with its siblings.
                // Letting it win here would forward one B per member instead
                // of the one merged B the NMU is waiting for.
                if (is_collect_b(input_fifo_[in][0].front())) continue;
                // F2 candidate filter (floo_router.sv:358-362, masked_valid &
                // ~past_handshakes_q): this output must be a branch of the
                // front flit's set that has not yet handshaked it. Unicast
                // fronts never set a done bit, so this is the pre-S4 test.
                if (!port_in_mask(head_mask(in, 0), static_cast<RouterPort>(out))) continue;
                if ((fork_done_[in][0] & port_bit(static_cast<RouterPort>(out))) != 0) continue;
                ws.locked_input = in;
                break;
            }
        }
        // OUR RULE — worm-boundary grant, a DELIBERATE divergence from
        // floo_output_arbiter.sv:126-139, whose prio stream_arbiter arbitrates
        // per BEAT (common_cells stream_arbiter's LockIn holds only within one
        // stalled handshake, not across a worm). Upstream therefore shares this
        // hazard; it is simply outside their tested envelope. On RSP an R burst
        // IS a multi-beat worm, so granting the merged B between beats would
        // put it at the front of a downstream input FIFO under that worm's held
        // one-hot latch — either a FALSE continuation abort on legal traffic,
        // or, where the routes coincide, a grant down the locked unicast path
        // whose flit_tail=1 ends the worm's latch early AND bypasses the B's own
        // join, duplicating it at the collector. Holding to the worm boundary
        // costs latency only: the join is stateless and re-fires every tick.
        //
        // "Mid-worm" = this output's frozen winner has already granted a head
        // (its route latch is held). A merely frozen winner that has not
        // granted yet is NOT mid-worm — nothing is split — and the reduction
        // still takes priority over it.
        const bool mid_worm = ws.locked_input.has_value() && route_lock_[*ws.locked_input][0] != 0;
        if (join && !mid_worm) {
            // The prio arbiter hands ready_i to the reduction branch whenever
            // it is valid, so a valid join suppresses the unicast grant at this
            // output this tick whether or not the output can accept.
            if (out_admits(out)) grant_join(out, join_sel, join_expected);
            continue;
        }
        // A held join falls through so the worm can reach its tail — suppressing
        // the unicast grant here instead would wedge both forever.
        if (!ws.locked_input.has_value()) continue;  // nothing valid for this output yet

        // Idle (no steal) while the frozen winner's FIFO is empty —
        // floo_wormhole_arbiter holds valid_q until last_q, it does not let
        // another requester in even though nothing has been granted yet.
        const std::size_t in = *ws.locked_input;
        if (input_fifo_[in][0].empty()) continue;
        // Same F2 mask, on the frozen winner: this branch already took the
        // parked flit and idles until the slowest branch takes it and the worm
        // advances (§1.1 skew property). Its lock is held throughout.
        if ((fork_done_[in][0] & port_bit(static_cast<RouterPort>(out))) != 0) continue;

        // Downstream/output-fifo readiness gates only the GRANT itself, not
        // who won — the winner was already frozen above.
        if (!out_admits(out)) continue;

        // Grant: single atomic event. Its route is not re-examined —
        // route_lock_ already pins it to `out` once locked (set at the same
        // grant that set ws.locked_input) — so this is the direct translate
        // of floo_route_select's registered bypass, not a recompute-and-compare.
        auto& q = input_fifo_[in][0];
        const Flit flit = q.front();
        const PortMask branches = head_mask(in, 0);
        const bool fork_grant = is_fork_set(branches);
        if (fork_grant) {
            // A fork grant COPIES the parked flit and marks this branch
            // handshaked — it never pops, so every branch granting in this
            // same tick reads the identical q.front() snapshot and no output
            // later in the loop can see the worm's next flit. The pop moves to
            // the all-branches-done pass below (F3).
            fork_done_[in][0] = static_cast<PortMask>(fork_done_[in][0] | (1u << out));
        } else {
            q.pop_front();  // unicast (one-hot set): the pre-S4 immediate pop
        }
        out_push(out, flit);
        const uint64_t flit_tail = flit.get_header_field("flit_tail");
        if (flit_tail == 0) {
            // The latch takes the WHOLE branch set, not just this output —
            // route_sel_q is NumRoutes-wide upstream (floo_route_select.sv:
            // 46-57, :211-216). Idempotent when a second branch grants the
            // same head later in this tick.
            route_lock_[in][0] = branches;
            // ws.locked_input == in already (frozen above or from a prior
            // grant this same worm) — stays locked across the worm.
        } else {
            // Per-branch tail release: THIS output's wormhole lock clears at
            // its own tail grant (F6), while other branches may still owe the
            // parked tail. The route latch therefore clears at the pop, not
            // here, for a fork — a unicast tail grant IS the pop.
            if (!fork_grant) route_lock_[in][0] = 0;
            ws.locked_input.reset();
            ws.rr = (in + 1) % ROUTER_PORT_COUNT;
        }
    }

    // Fork pop pass (F3, floo_router.sv:374-388 cross_ready = &all_handshakes,
    // :378 past_handshakes_d clear, :393-394 FF): a forked flit leaves its
    // input FIFO — and so releases the upstream ready slot, this class's one
    // acknowledgement to the sender — only when EVERY expected branch has
    // handshaked it. Deferring the pop past the output loop keeps one
    // stage-advance per tick. OUR RULE divergence from :383-385: no
    // ignore_routes loopback exclusion — LOCAL is a real branch (S3a ruling).
    for (std::size_t in = 0; in < ROUTER_PORT_COUNT; ++in) {
        for (uint8_t vc = 0; vc < cfg_.num_vc; ++vc) {
            if (fork_done_[in][vc] == 0) continue;
            auto& q = input_fifo_[in][vc];
            if (fork_done_[in][vc] != head_mask(in, vc)) continue;
            const bool tail = q.front().get_header_field("flit_tail") != 0;
            q.pop_front();
            fork_done_[in][vc] = 0;
            if (tail) route_lock_[in][vc] = 0;
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
