// DatMergeWrap — NI-level merge point for the DAT network's shared LOCAL port
// (S3a T5, controller ruling, translate of floo_nw_chimney.sv's wide-link
// merge: :1327-1356 muxes {WideAw+W (AW/W pre-paired by the SelAw/SelW FSM),
// WideR} onto one floo_wormhole_arbiter; :1433-1440 demuxes the RX side by
// channel enum).
//
// Spec §4.3: a port is one physical rx/tx pair. REQ/RSP each have exactly one
// producer and one consumer at LOCAL (NMU produces/NSU consumes REQ; NSU
// produces/NMU consumes RSP) -- no sharing. DAT does not: DataAw/DataW
// originate at NMU and are consumed at NSU (REQ-shaped), while DataR
// originates at NSU and is consumed at NMU (RSP-shaped) -- both riding the
// SAME physical dat_router_ LOCAL port (stage design §7: "dat_router_ is the
// existing credit Router"). This wrap is the mux (egress: NMU+NSU -> router)
// and demux (ingress: router -> NMU/NSU by axi_ch) that makes that sharing
// well-formed. NMU/NSU keep their own DAT pin pairs (nmu_wrap.sv/nsu_wrap.sv
// unchanged) and are unaware this sits between them and the router; the merge
// is NI/tile-internal, wired in at ni_wrap.sv.
//
// Placement: own DPI context (one-component-one-context, matching Router/
// Nmu/Nsu), not folded into NmuWrap/NsuWrap (both declare a hermetic
// invariant: no refs to other Wraps) or RouterWrap (network-agnostic router
// infrastructure has no business knowing about NMU/NSU channel classes).
//
// Egress mux (NMU input 0 / NSU input 1 -> router): router::WormholeArbiter,
// the existing translated floo_wormhole_arbiter port (ni/wormhole_arbiter.hpp)
// -- NOT a new arbiter. No ChannelPairing: each input is already a
// fully-formed, worm-locked stream (NMU's own dat_wormhole_arbiter_ already
// pairs DataAw with its DataW, exactly the chimney's SelAw/SelW-muxed
// WideAw/W stream; NSU's DataR is single-flit, needs no pairing) -- this is
// structurally identical to nsu.hpp's own RSP-egress wormhole_arbiter_ (2
// inputs, no pairing, each already a complete stream). ONE shared credit pool
// downstream of the arbiter (DatMergeDownstream, term_ below), sized to the
// DAT router's real LOCAL input depth (NOC_ROUTER_VC_DEPTH) -- replaces NMU's
// and NSU's previous independent full-depth pools (the inconsistency the
// blocked report flagged). NMU's/NSU's own per-VC credit toward the arbiter's
// per-input pending stage is unchanged in shape (still ni/wormhole_arbiter.hpp's
// own pending capacity, still seeded via nmu_wrap.hpp/nsu_wrap.hpp's
// enable_dat_noc_credit) -- only the seed value moves from
// NOC_ROUTER_VC_DEPTH to {NMU,NSU}_ARBITER_FIFO_DEPTH (this stage's own
// per-input pending depth, not the router's).
//
// Ingress demux (router -> NMU/NSU): unbuffered, same-cycle pass-through --
// axi_ch selects the destination (DataR -> NMU, DataAw/DataW -> NSU) --
// mirroring the chimney's RX unpack (:1433-1440). NMU's/NSU's own ingress
// queues are themselves unbounded and always accept (see nmu_wrap.hpp/
// nsu_wrap.hpp), so no buffering is needed at the demux itself; the merge
// returns one credit pulse to the router immediately on accept, same as the
// pre-merge direct NI-edge ingress did.
#pragma once
#include "wrap/flit_byte_conv.hpp"  // flit_from_bytes, flit_to_bytes
#include "wrap/router_wrap_io.hpp"  // VcCreditVec
#include "ni/wormhole_arbiter.hpp"
#include "router/req_out.hpp"
#include "ni_flit_constants.h"  // ni::AXI_CH_DataR
#include "ni_params.h"          // NOC_ROUTER_VC_DEPTH, {NMU,NSU}_ARBITER_FIFO_DEPTH
#include <algorithm>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

namespace ni::cmodel::wrap {

namespace detail {

// Router-facing egress terminal: the merge's ONE shared credit pool toward
// the DAT router's real LOCAL input depth. Mirrors nmu_standalone.hpp's
// QueueNocReqOut credit branch (a 3rd copy of a ~15-line pattern -- not worth
// a shared base across nmu/nsu/merge, whose ready-track/pooling needs differ).
struct DatMergeDownstream : router::NocReqOut {
    void enable_credit(uint8_t num_vc, std::size_t seed) { credit_.assign(num_vc, seed); }
    void receive_credit(uint8_t vc) { ++credit_[vc]; }
    bool push_flit(const Flit& f) override {
        const auto vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
        if (credit_[vc] == 0) return false;
        --credit_[vc];
        queue_.push_back(f);
        return true;
    }
    bool credit_avail(uint8_t vc) const override { return credit_[vc] > 0; }
    std::optional<Flit> pop() {
        if (queue_.empty()) return std::nullopt;
        Flit f = queue_.front();
        queue_.pop_front();
        return f;
    }

  private:
    std::deque<Flit> queue_;
    std::vector<std::size_t> credit_;
};

}  // namespace detail

struct DatMergeInputs {
    // From NMU (DataAw/DataW producer): its egress push, and our credit-return
    // pulses it forwards back are NOT here (those are OUR output).
    bool nmu_tx_dat_valid;
    FlitBytes nmu_tx_dat_flit;
    // From NSU (DataR producer): its egress push.
    bool nsu_tx_dat_valid;
    FlitBytes nsu_tx_dat_flit;
    // From the router: credit-return for our egress sends (replenishes term_).
    VcCreditVec tx_dat_crdvalid;
    // From the router: its ejected LOCAL flit (to demux toward NMU/NSU).
    bool rx_dat_valid;
    FlitBytes rx_dat_flit;
};

struct DatMergeOutputs {
    // To NMU: credit-return for its egress sends into wormhole input(0).
    VcCreditVec nmu_tx_dat_crdvalid;
    // To NMU: demuxed DataR ingress.
    bool nmu_rx_dat_valid;
    FlitBytes nmu_rx_dat_flit;
    // To NSU: credit-return for its egress sends into wormhole input(1).
    VcCreditVec nsu_tx_dat_crdvalid;
    // To NSU: demuxed DataAw/DataW ingress.
    bool nsu_rx_dat_valid;
    FlitBytes nsu_rx_dat_flit;
    // To the router: our egress send (drained from the shared pool term_).
    bool tx_dat_valid;
    FlitBytes tx_dat_flit;
    // To the router: credit-return for what we drained from its LOCAL output.
    VcCreditVec rx_dat_crdvalid;
};

class DatMergeWrap {
  public:
    void init(uint8_t dat_num_vc) {
        dat_num_vc_ = dat_num_vc;
        term_.enable_credit(dat_num_vc, static_cast<std::size_t>(::ni::NOC_ROUTER_VC_DEPTH));
        // Per-input pending depth pools credit across all dat_num_vc virtual
        // channels: the WormholeArbiter pending stage has no VC dimension
        // (ponytail: the real per-VC gate is term_, downstream; sizing this
        // stage per-VC too would need a VC-aware arbiter variant nothing else
        // in this codebase has). Sized to the larger of NMU's/NSU's own
        // per-VC arbiter-stage depth (times dat_num_vc) so neither side's
        // unchanged per-VC sender credit can ever outrun this stage's
        // capacity -- see the push_flit asserts in tick().
        const std::size_t per_input_depth =
            static_cast<std::size_t>(dat_num_vc) *
            std::max<std::size_t>(static_cast<std::size_t>(::ni::NMU_ARBITER_FIFO_DEPTH),
                                  static_cast<std::size_t>(::ni::NSU_ARBITER_FIFO_DEPTH));
        wormhole_ = std::make_unique<router::WormholeArbiter<detail::DatMergeDownstream>>(
            term_, /*num_inputs=*/2, /*pairings=*/std::vector<router::ChannelPairing>{},
            per_input_depth);
        in_ = DatMergeInputs{};
        out_ = DatMergeOutputs{};
    }

    void set_inputs(const DatMergeInputs& in) { in_ = in; }

    void tick() {
        if (!wormhole_) return;

        // Step 1: push inbound egress flits. NMU/NSU only send after their
        // OWN per-VC credit (seeded to this stage's per-input depth, see
        // init()) said available, so these pushes cannot fail by
        // construction -- assert rather than silently drop.
        if (in_.nmu_tx_dat_valid) {
            const bool ok = wormhole_->input(0).push_flit(flit_from_bytes(in_.nmu_tx_dat_flit));
            assert(ok &&
                   "DatMergeWrap: NMU DAT egress push rejected -- sender credit desynced from "
                   "the merge's per-input pending depth");
            (void)ok;
        }
        if (in_.nsu_tx_dat_valid) {
            const bool ok = wormhole_->input(1).push_flit(flit_from_bytes(in_.nsu_tx_dat_flit));
            assert(ok &&
                   "DatMergeWrap: NSU DAT egress push rejected -- sender credit desynced from "
                   "the merge's per-input pending depth");
            (void)ok;
        }
        // Router's credit-return replenishes the shared downstream pool.
        for (uint8_t vc = 0; vc < dat_num_vc_; ++vc) {
            if (in_.tx_dat_crdvalid[vc]) term_.receive_credit(vc);
        }

        // Snapshot each input's about-to-drain flit (for per-VC credit-return
        // attribution -- the arbiter itself tracks no VC dimension) and
        // occupancy (to detect which input, if any, actually drained).
        const auto peek0 = wormhole_->peek(0);
        const auto peek1 = wormhole_->peek(1);
        const std::size_t before0 = wormhole_->pending_size(0);
        const std::size_t before1 = wormhole_->pending_size(1);

        // Step 2: advance the arbiter (at most one input drains into term_
        // this tick -- WormholeArbiter::tick() makes a single grant decision).
        wormhole_->tick();

        out_ = DatMergeOutputs{};

        if (peek0 && wormhole_->pending_size(0) < before0) {
            out_.nmu_tx_dat_crdvalid[static_cast<uint8_t>(peek0->get_header_field("vc_id"))] = true;
        }
        if (peek1 && wormhole_->pending_size(1) < before1) {
            out_.nsu_tx_dat_crdvalid[static_cast<uint8_t>(peek1->get_header_field("vc_id"))] = true;
        }

        // Step 3: drain the shared downstream pool toward the router.
        if (auto f = term_.pop()) {
            out_.tx_dat_valid = true;
            out_.tx_dat_flit = flit_to_bytes(*f);
        }

        // Ingress demux (router -> NMU/NSU), unbuffered same-cycle
        // pass-through with immediate credit-return -- see class comment.
        if (in_.rx_dat_valid) {
            const Flit f = flit_from_bytes(in_.rx_dat_flit);
            if (f.get_header_field("axi_ch") == ni::AXI_CH_DataR) {
                out_.nmu_rx_dat_valid = true;
                out_.nmu_rx_dat_flit = in_.rx_dat_flit;
            } else {
                out_.nsu_rx_dat_valid = true;
                out_.nsu_rx_dat_flit = in_.rx_dat_flit;
            }
            out_.rx_dat_crdvalid[static_cast<uint8_t>(f.get_header_field("vc_id"))] = true;
        }
    }

    void get_outputs(DatMergeOutputs& out) const { out = out_; }

    // DAT VC count — read by the DPI handlers to size the per-VC credit loops.
    uint8_t num_vc() const { return dat_num_vc_; }

  private:
    uint8_t dat_num_vc_ = ::ni::NOC_DAT_NUM_VC;
    detail::DatMergeDownstream term_;
    std::unique_ptr<router::WormholeArbiter<detail::DatMergeDownstream>> wormhole_;
    DatMergeInputs in_{};
    DatMergeOutputs out_{};
};

}  // namespace ni::cmodel::wrap
