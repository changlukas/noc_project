#pragma once
#include "axi/types.hpp"
#include "flit.hpp"
#include "ni_flit_constants.h"
#include "router/null_adapters.hpp"
#include "router/req_in.hpp"
#include "router/route_mask.hpp"
#include "ni/address_map.hpp"
#include "ni/pipeline_stage.hpp"
#include "ni_params.h"
#include "nsu/meta_buffer.hpp"
#include "request_io.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>
#include <stdexcept>
#include <vector>

namespace ni::cmodel::nsu {

// NSU-side request depacketizer. Stateful demux: tick() pulls flits from two
// independent NocReqIn ingresses -- REQ and DAT (S3a T4 ingress + T6
// steering: NMU's Packetize steers Data-class AW/W here; DAT never carries
// Narrow* or DataAr -- see drain_ingress_'s defensive assert) -- reads
// axi_ch, and parks the flit per class, per docs/image/nsu.jpg: narrow AW/W
// in their own S1 stage registers, data AW/W in a bounded queue per DAT VC
// (dat_q_). S3a shared one register per channel across both classes; that
// holds only while arrivals are one ordered stream, and narrow on REQ against
// data on DAT are two. tick() touches no MetaBuffer state and decodes nothing
// but narrow W.
//
// pop_aw() / pop_ar() are the drain stage. Each decodes its flit, remaps the
// master's AXI id to the downstream id, allocates the MetaBuffer entry under
// that key, and hands the beat to AxiMasterPort. Each gates ONLY on its own
// pool: a full write pool stalls pop_aw and leaves pop_ar untouched. Allocating
// here rather than at ingress is what keeps a full pool from head-of-line
// blocking the other channels, mirroring the same independent-channel-draining
// pattern used by the NMU request path.
//
// Pending-flit stash semantics (narrow AW/W and AR only): if a pulled flit's
// S1 register is occupied, the flit is held in `pending_` and re-attempted
// next tick, blocking flits behind it. That is inherent to a serialized NoC
// link, not a modelling defect, and NocReqIn offers no peek to avoid it. Data
// AW/W never stash: their VC queue has depth for every flit the sender's
// credit lets it launch.
//
// W flits have no MetaBuffer side effect — W carries no AXI ID; W ordering is
// handled by a downstream W-meta FIFO.
class Depacketize : public RequestDepacketizer {
  public:
    Depacketize(router::NocReqIn& req_in, MetaBuffer& meta, std::size_t max_unique_ids,
                router::NocReqIn& dat_req_in = router::null_req_in(), uint8_t src_id = 0,
                std::array<address_map::SpaceCoords, 2> space_coords = {}, uint8_t port_id = 0,
                uint8_t dat_num_vc = 1)
        : req_in_(req_in),
          dat_req_in_(dat_req_in),
          meta_(meta),
          max_unique_ids_(max_unique_ids),
          dat_q_(dat_num_vc),
          dat_credit_pending_(dat_num_vc, 0),
          node_(router::detail::split_node_id(src_id)),
          space_coords_(space_coords),
          port_id_(port_id) {
        // Every path that configures an NSU funnels through here (YAML loader, co-sim
        // wrap defaults, direct NsuConfig test fixtures), so this is the config trust
        // boundary: validate with a throw, not an assert, so a misconfigured value fails
        // loud even in a release/NDEBUG build where asserts are compiled out.
        //
        // Only 1 and NOC_ID_SPACE are legal. This mirrors FlooNoC's floo_meta_buffer,
        // whose MaxUniqueIds has exactly two behaviors: collapse every id to one
        // downstream id (MaxUniqueIds==1, floo_meta_buffer.sv:87) or passthrough (else,
        // the unique-id count is set by OutIdWidth, floo_meta_buffer.sv:129). FlooNoC
        // provides no arbitrary-N remap (nothing in the chimney instantiates
        // axi_id_remap), so an intermediate value is intentionally unsupported, not a
        // modelling gap: with InIdWidth==OutIdWidth it would silently degenerate to
        // the identity (full NOC_ID_SPACE) remap.
        if (max_unique_ids != 1 && max_unique_ids != axi::NOC_ID_SPACE) {
            throw std::invalid_argument(
                "max_unique_ids must be 1 (collapse) or NOC_ID_SPACE (passthrough); "
                "FlooNoC provides no arbitrary-N unique-id remap");
        }
    }

    void tick();

    // RequestDepacketizer interface: takes from the narrow S1 stage register
    // or the data VC queue. Called by AxiMasterPort (S2) once per tick
    // (<=1 beat/channel/tick).
    std::optional<axi::AwBeat> pop_aw() override;
    std::optional<axi::WBeat> pop_w() override;
    std::optional<axi::ArBeat> pop_ar() override;

    // stage_occupancy probe. axi_ch uses ni::AXI_CH_* constants. Narrow AW/W
    // and AR are 0/1 register probes. DataAw / DataW count flits of that
    // channel across all VC queues, so they range 0 to
    // dat_num_vc * NOC_NI_DAT_RX_VC_DEPTH, not 0/1.
    std::size_t s1_occupancy(uint8_t axi_ch) const noexcept {
        switch (axi_ch) {
            case ni::AXI_CH_NarrowAw:
                return s1_narrow_aw_.occupancy();
            case ni::AXI_CH_NarrowW:
                return s1_narrow_w_.occupancy();
            case ni::AXI_CH_DataAw:
            case ni::AXI_CH_DataW: {
                std::size_t n = 0;
                for (const auto& q : dat_q_)
                    for (const auto& f : q)
                        if (f.get_header_field("axi_ch") == axi_ch) ++n;
                return n;
            }
            case ni::AXI_CH_NarrowAr:
            case ni::AXI_CH_DataAr:
                return s1_ar_.occupancy();
            default:
                return 0;
        }
    }

    // Wrap accessor: one credit pulse per consumed data flit, at most one per
    // VC per call (mirror of router::LinkCreditOut::take).
    bool take_dat_credit(uint8_t vc) {
        assert(vc < dat_q_.size() && "nsu::Depacketize::take_dat_credit: VC beyond dat_num_vc");
        if (dat_credit_pending_[vc] == 0) return false;
        --dat_credit_pending_[vc];
        return true;
    }

  private:
    router::NocReqIn& req_in_;
    router::NocReqIn& dat_req_in_;
    MetaBuffer& meta_;
    std::size_t max_unique_ids_;
    std::optional<Flit> pending_req_;
    std::optional<Flit> pending_dat_;

    // AW / AR hold the raw flit until pop_aw / pop_ar admit it: the drain stage
    // needs the header's src_id / ordering_req / ordering_tag to allocate the MetaBuffer
    // entry, and decoding is pure. W carries no id and no metadata, so it stays
    // a decoded beat.
    //
    // AW and W are held PER CLASS, per docs/image/nsu.jpg: narrow storage and
    // data storage stay separate up to the AXI Channel Assignment. They were
    // shared through S3a, which assumed one ordered arrival stream -- true
    // within a network, false across two. Narrow rides REQ and data rides DAT,
    // so a shared stage lets the two classes interleave and the W stream stops
    // agreeing with the AW stream on burst boundaries.
    //
    // AR needs no split: DAT carries DataAw/DataW only, so every AR -- narrow
    // and data alike -- arrives on REQ already ordered (drain_ingress_'s
    // is_dat_ingress assert pins that), and AR owns no second channel to stay
    // in step with.
    router::PipelineStage<Flit> s1_narrow_aw_;
    router::PipelineStage<axi::WBeat> s1_narrow_w_;
    router::PipelineStage<Flit> s1_ar_;

    // Data-class ingress, per VC (FlooNoC chimney demuxes VCs before its spill
    // registers, floo_nw_chimney.sv:276-311). One register per data channel is
    // no longer enough: the router holds a wormhole lock per (output, VC), so
    // worms of different VCs arrive interleaved flit by flit and each VC has to
    // reassemble its own burst. Depth = NOC_NI_DAT_RX_VC_DEPTH, the router's
    // LOCAL credit seed: the sender never has more than that many unacknowledged
    // flits per VC, so push cannot overflow. A slot is returned
    // (dat_credit_pending_) when pop_aw / pop_w consume the flit. Both
    // ingresses deposit data-class flits here (DAT in co-sim, REQ in the ctest
    // stubs).
    std::vector<std::deque<Flit>> dat_q_;
    std::vector<std::size_t> dat_credit_pending_;
    uint8_t dat_aw_rr_ = 0;  // VC round-robin for data-class AW admission

    // AXI Channel Assignment (docs/image/nsu.jpg). Every admitted AW enqueues
    // the class and beat count of the burst it owes, and pop_w serves the
    // classes in exactly that order. This is what makes the W stream follow the
    // AW stream: AXI4 W beats carry no id, so their only binding to a write is
    // position. A burst whose beats have not arrived stalls the W stream rather
    // than letting the next class jump the queue.
    struct WBurst {
        AxiClass cls;
        uint32_t beats;
        uint8_t vc;  // data class: the VC queue its W beats arrive on
    };
    std::deque<WBurst> w_order_;
    uint32_t w_beats_done_ = 0;    // beats served for w_order_.front()
    bool aw_prefer_data_ = false;  // round-robin between classes at pop_aw

    // Narrow-class W lane re-anchor (S2 design doc §2 site 2): the AW basis a
    // W burst's beats need is decoded eagerly here, at AW arrival, and staged
    // FIFO-order -- AXI4 W beats stream non-interleaved, one wormhole packet
    // (AW + its W beats) at a time, so the front entry always matches the W
    // beats currently arriving. Pushed only for narrow-class AW (mirrors
    // nmu::Rob's ar_lane_meta_): decode_w's data branch never pops, so an
    // unconditional push would leak one entry per data-class AW and, worse,
    // leave a stale front entry for the next narrow AW to misread.
    // Data class ignores this FIFO entirely (full-width payload, decoded
    // directly, no lane math).
    struct WAddrMeta {
        uint64_t local_addr;
        uint8_t len;
        uint8_t size;
        axi::Burst burst;
        uint16_t beat_counter = 0;
    };
    std::deque<WAddrMeta> w_addr_fifo_;

    // This node's mesh coordinates, and where each space keeps them.
    router::detail::NodeCoord node_;
    std::array<address_map::SpaceCoords, 2> space_coords_;
    // This NI's own endpoint at its coordinate; every request must name it.
    uint8_t port_id_ = 0;

    // The address a request carries names the node the SENDER addressed. For a
    // unicast that is this node and the rewrite is the identity. For a
    // collective replica it is the node the AW's own address resolves to, not
    // this one, because one masked AW reaches N nodes unchanged -- so the tile
    // behind this NSU would decode an address belonging to someone else.
    // Overwriting the coordinate field with this node's own is what makes every
    // replica land at the same offset in its own tile. Unconditional, so there
    // is no case left unhandled: no branch on collective_op, no mask to inspect.
    uint64_t rebase_(uint64_t addr, uint8_t axi_ch) const {
        const bool is_data = axi_ch == ni::AXI_CH_DataAw || axi_ch == ni::AXI_CH_DataAr;
        const auto& c = space_coords_[is_data ? 1u : 0u];
        return address_map::rebase_node_coords(addr, c, node_.x, node_.y);
    }

    axi::AwBeat decode_aw(const Flit& f) const;
    axi::WBeat decode_w(const Flit& f);
    axi::ArBeat decode_ar(const Flit& f) const;
    void drain_ingress_(router::NocReqIn& src, std::optional<Flit>& pending, bool is_dat_ingress);
};

inline axi::AwBeat Depacketize::decode_aw(const Flit& f) const {
    axi::AwBeat b{};
    b.id = static_cast<uint8_t>(f.get_payload_field("AW", "awid"));
    b.addr = rebase_(f.get_payload_field("AW", "awaddr"),
                     static_cast<uint8_t>(f.get_header_field("axi_ch")));
    b.len = static_cast<uint8_t>(f.get_payload_field("AW", "awlen"));
    b.size = static_cast<uint8_t>(f.get_payload_field("AW", "awsize"));
    b.burst = static_cast<axi::Burst>(f.get_payload_field("AW", "awburst"));
    b.cache = static_cast<uint8_t>(f.get_payload_field("AW", "awcache"));
    b.lock = static_cast<uint8_t>(f.get_payload_field("AW", "awlock"));
    b.prot = static_cast<uint8_t>(f.get_payload_field("AW", "awprot"));
    b.region = static_cast<uint8_t>(f.get_payload_field("AW", "awregion"));
    b.user = static_cast<uint8_t>(f.get_payload_field("AW", "awuser"));
    b.qos = static_cast<uint8_t>(f.get_payload_field("AW", "awqos"));
    return b;
}

inline axi::WBeat Depacketize::decode_w(const Flit& f) {
    const bool is_data = f.get_header_field("axi_ch") == ni::AXI_CH_DataW;
    const char* ch = is_data ? "DATA_W" : "NARROW_W";
    axi::WBeat b{};
    b.last = f.get_payload_field(ch, "wlast") != 0;
    b.user = static_cast<uint8_t>(f.get_payload_field(ch, "wuser"));
    if (is_data) {
        b.strb = f.get_payload_field(ch, "wstrb");
        f.get_payload_bytes(ch, "wdata", b.data.data(), ni::width::NOC_DATA_WIDTH);
        return b;
    }
    // Narrow: the flit carries only the addressed 8 B lane. w_addr_fifo_'s
    // front entry is this beat's paired AW (see the struct comment).
    assert(!w_addr_fifo_.empty() &&
           "nsu::Depacketize::decode_w: narrow W flit with no staged AW address basis");
    WAddrMeta& am = w_addr_fifo_.front();
    const uint64_t addr = axi::beat_addr(am.local_addr, am.len, am.size, am.burst, am.beat_counter);
    const unsigned lane = axi::narrow_lane(addr);
    const uint64_t narrow_strb = f.get_payload_field(ch, "wstrb");
    b.strb = narrow_strb << (lane * axi::NARROW_DATA_BYTES);
    f.get_payload_bytes(ch, "wdata", b.data.data() + lane * axi::NARROW_DATA_BYTES,
                        ni::width::NOC_NARROW_DATA_WIDTH);
    ++am.beat_counter;
    if (b.last) w_addr_fifo_.pop_front();
    return b;
}

inline axi::ArBeat Depacketize::decode_ar(const Flit& f) const {
    axi::ArBeat b{};
    b.id = static_cast<uint8_t>(f.get_payload_field("AR", "arid"));
    b.addr = rebase_(f.get_payload_field("AR", "araddr"),
                     static_cast<uint8_t>(f.get_header_field("axi_ch")));
    b.len = static_cast<uint8_t>(f.get_payload_field("AR", "arlen"));
    b.size = static_cast<uint8_t>(f.get_payload_field("AR", "arsize"));
    b.burst = static_cast<axi::Burst>(f.get_payload_field("AR", "arburst"));
    b.cache = static_cast<uint8_t>(f.get_payload_field("AR", "arcache"));
    b.lock = static_cast<uint8_t>(f.get_payload_field("AR", "arlock"));
    b.prot = static_cast<uint8_t>(f.get_payload_field("AR", "arprot"));
    b.region = static_cast<uint8_t>(f.get_payload_field("AR", "arregion"));
    b.user = static_cast<uint8_t>(f.get_payload_field("AR", "aruser"));
    b.qos = static_cast<uint8_t>(f.get_payload_field("AR", "arqos"));
    return b;
}

// drain_ingress_ is the S1 stage for one physical ingress. Narrow AW/W and AR
// park <=1 flit per channel per tick into the S1 stage registers (only narrow
// W is decoded here; AW/AR are decoded at the drain); if a register is already
// occupied (not yet consumed by the S2 AxiMasterPort), the flit is
// backpressured into this ingress's own `pending` stash (head-of-line blocking
// on single-FIFO ingress, same semantics as the original queue-based
// implementation). Data AW/W go straight into their VC queue and are decoded
// at the drain, so they never stash and never rate-limit each other.
// Touches no MetaBuffer state; allocation happens in pop_aw / pop_ar.
// Single-ingress HOL note: unlike the NMU request path, NSU depacketize has NO
// source-side pairing lock on ingress. It demuxes into independent S1 registers
// that drain into bounded AxiMasterPort queues, which drain to the slave.
// The per-ingress HOL is inherent to a single VC (AW/W/AR serialize on one
// channel) but cannot self-cycle: no ingress resource waits on a downstream
// that waits back on it. Given the slave eventually drains, `pending` always
// clears.
inline void Depacketize::drain_ingress_(router::NocReqIn& src, std::optional<Flit>& pending,
                                        bool is_dat_ingress) {
    while (true) {
        Flit f;
        if (pending) {
            f = *pending;
        } else {
            auto opt = src.pop_flit();
            if (!opt) return;
            f = *opt;
        }
        // dst_port_id names which endpoint at this coordinate the request is for.
        // The router delivers by coordinate, so a wrong value lands here silently.
        if (f.get_header_field("dst_port_id") != port_id_) {
            assert(false && "nsu::Depacketize: request dst_port_id names another endpoint");
            std::abort();
        }
        uint64_t ch = f.get_header_field("axi_ch");
        // DAT carries DataAw/DataW only (spec :348; DataAr and every Narrow*
        // channel stay on REQ). A Narrow* AW/W arriving here would corrupt
        // w_addr_fifo_'s FIFO-order contiguity invariant (its front entry
        // must always be the REQ-ingress narrow AW currently being served);
        // fail loud instead of silently mis-pairing narrow lanes.
        assert((!is_dat_ingress || ch == ni::AXI_CH_DataAw || ch == ni::AXI_CH_DataW) &&
               "nsu::Depacketize::drain_ingress_: DAT ingress delivered a channel outside "
               "{DataAw, DataW} -- spec :348 keeps Narrow*/DataAr off DAT");
        switch (ch) {
            case ni::AXI_CH_DataAw:
            case ni::AXI_CH_DataW: {
                const auto vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
                assert(vc < dat_q_.size() &&
                       "nsu::Depacketize: data flit names a VC beyond dat_num_vc");
                assert(dat_q_[vc].size() < static_cast<std::size_t>(::ni::NOC_NI_DAT_RX_VC_DEPTH) &&
                       "nsu::Depacketize: per-VC ingress overflow -- sender credit discipline "
                       "broken");
                dat_q_[vc].push_back(f);
                break;
            }
            case ni::AXI_CH_NarrowAw: {
                if (s1_narrow_aw_.full()) {
                    pending = f;
                    return;
                }
                s1_narrow_aw_.accept(f);
                // Eager decode (in addition to the raw stash above): the W
                // beats that follow need the AW's address basis before pop_aw
                // ever runs (W is decoded here, at arrival; pop_aw may drain
                // later, rate-limited to <=1/tick and gated on meta_.write_full()).
                // Narrow class only -- see w_addr_fifo_'s comment. Narrow class
                // is REQ-exclusive (S3a §1), so this never races the DAT ingress.
                const axi::AwBeat aw = decode_aw(f);
                w_addr_fifo_.push_back({aw.addr, aw.len, aw.size, aw.burst, /*beat_counter=*/0});
                break;
            }
            case ni::AXI_CH_NarrowW: {
                if (s1_narrow_w_.full()) {
                    pending = f;
                    return;
                }
                s1_narrow_w_.accept(decode_w(f));
                break;
            }
            case ni::AXI_CH_NarrowAr:
            case ni::AXI_CH_DataAr:
                if (s1_ar_.full()) {
                    pending = f;
                    return;
                }
                s1_ar_.accept(f);
                break;
            default:
                assert(false &&
                       "nsu::Depacketize::drain_ingress_: NocReqIn delivered flit with axi_ch "
                       "outside {NarrowAw, NarrowW, NarrowAr, DataAw, DataW, DataAr} — NSU request "
                       "path only accepts request channels. Likely cause: NMU packetizer stamped "
                       "wrong axi_ch into a request flit, NoC fabric misrouted a response flit "
                       "into the request ingress, or codegen drift changed ni::AXI_CH_* encoding "
                       "without rebuilding both sides.");
                std::abort();
        }
        pending.reset();
        // The S1 registers accept only one flit per channel per tick.
        // After placing a flit in a register, stop advancing the ingress
        // stream for that channel (subsequent flits for that channel remain
        // for the next tick, preserving the <=1 beat/channel/tick bound).
        // Since the registers are independent, we continue pulling flits for
        // other channels until all of them are full or the ingress is empty.
        //
        // The while(true) loop naturally handles this: after the switch we
        // loop back to pull the next flit. When a channel's register is
        // full, the next flit for that channel goes to `pending`. Because
        // `pending` is a single-slot stash, only one channel can be stalled
        // at a time per ingress (head-of-line blocking on that ingress's
        // single stream). The two ingresses drain independently -- REQ
        // blocked on a full register does not stall DAT, and vice versa.
        // Data AW/W hold no register and never stall the loop: they are
        // bounded by their VC queue's credit, not by a per-tick admission.
    }
}

// tick(): drain both physical ingresses (REQ, DAT) into the narrow S1
// registers and the data VC queues. The two ingresses never contend for the
// same AW or W storage.
inline void Depacketize::tick() {
    drain_ingress_(req_in_, pending_req_, /*is_dat_ingress=*/false);
    drain_ingress_(dat_req_in_, pending_dat_, /*is_dat_ingress=*/true);
    // DAT carries DataAw/DataW only, and both go straight into a VC queue that
    // the sender's credit guarantees has room. Nothing on DAT can stash.
    assert(!pending_dat_ &&
           "nsu::Depacketize::tick: a DAT flit stashed -- DAT carries only "
           "data AW/W, which never backpressure");
}

// pop_aw/pop_w/pop_ar: S2 consumer interface — take from the narrow S1
// register or the data VC queue. Called <=1 time per channel per tick by
// AxiMasterPort::drain_*_from_depkt. Returns nullopt when that storage is
// empty, or when this channel's MetaBuffer pool is full (backpressure: the
// flit stays where it is). A data flit taken here returns its VC's credit.
inline std::optional<axi::AwBeat> Depacketize::pop_aw() {
    // Class select, round-robin so neither class starves the other. Whichever
    // is taken defines the next entry of the W order below.
    const bool narrow_ready = s1_narrow_aw_.full();
    // Data class: round-robin over the VC queues whose head is an AW. A queue
    // headed by a W is mid-burst and owes its beats to an AW already admitted.
    std::optional<uint8_t> data_vc;
    for (std::size_t k = 0; k < dat_q_.size(); ++k) {
        const auto v = static_cast<uint8_t>((dat_aw_rr_ + k) % dat_q_.size());
        if (!dat_q_[v].empty() &&
            dat_q_[v].front().get_header_field("axi_ch") == ni::AXI_CH_DataAw) {
            data_vc = v;
            break;
        }
    }
    const bool data_ready = data_vc.has_value();
    if (!narrow_ready && !data_ready) return std::nullopt;
    const bool take_data = data_ready && (!narrow_ready || aw_prefer_data_);
    if (narrow_ready && data_ready) aw_prefer_data_ = !aw_prefer_data_;
    if (meta_.write_full()) return std::nullopt;
    Flit f;
    if (take_data) {
        f = dat_q_[*data_vc].front();
        dat_q_[*data_vc].pop_front();
        ++dat_credit_pending_[*data_vc];
        dat_aw_rr_ = static_cast<uint8_t>((*data_vc + 1) % dat_q_.size());
    } else {
        f = s1_narrow_aw_.take();
    }
    axi::AwBeat b = decode_aw(f);
    w_order_.push_back({take_data ? AxiClass::Data : AxiClass::Narrow,
                        static_cast<uint32_t>(b.len) + 1u, take_data ? *data_vc : uint8_t{0}});
    const uint8_t downstream_id = remap_downstream_id(b.id, max_unique_ids_);
    const AxiClass cls =
        (f.get_header_field("axi_ch") == ni::AXI_CH_DataAw) ? AxiClass::Data : AxiClass::Narrow;
    MetaEntry e{
        static_cast<uint8_t>(f.get_header_field("src_id")),
        b.id,
        static_cast<uint8_t>(f.get_header_field("ordering_req")),
        static_cast<uint8_t>(f.get_header_field("ordering_tag")),
        cls,
    };
    // Collective identity travels with the AW and comes back on its B (design
    // §3.1). Buffered here rather than re-derived: the B has no address to
    // translate from, and the RSP join matches on the exact echoed pair.
    e.collective_op = static_cast<uint8_t>(f.get_header_field("collective_op"));
    e.collective_mask = static_cast<uint8_t>(f.get_header_field("collective_mask"));
    e.src_port = static_cast<uint8_t>(f.get_header_field("src_port_id"));
    meta_.allocate_write(downstream_id, e);
    b.id = downstream_id;
    return b;
}
inline std::optional<axi::WBeat> Depacketize::pop_w() {
    // Serve strictly in AW order. An admitted burst whose next beat has not
    // arrived stalls the W stream; letting the other class through here is
    // exactly the reordering that breaks the AW/W binding.
    if (w_order_.empty()) return std::nullopt;
    const WBurst& front = w_order_.front();
    axi::WBeat b;
    if (front.cls == AxiClass::Data) {
        auto& q = dat_q_[front.vc];
        if (q.empty()) return std::nullopt;
        assert(q.front().get_header_field("axi_ch") == ni::AXI_CH_DataW &&
               "nsu::Depacketize::pop_w: W stream on this VC interrupted by a non-W flit -- "
               "fabric broke per-VC wormhole contiguity");
        b = decode_w(q.front());
        q.pop_front();
        ++dat_credit_pending_[front.vc];
    } else {
        if (!s1_narrow_w_.full()) return std::nullopt;
        b = s1_narrow_w_.take();
    }
    if (++w_beats_done_ >= w_order_.front().beats) {
        assert(b.last &&
               "nsu::Depacketize::pop_w: burst ended without WLAST -- the W stream and "
               "the AW that named it disagree on beat count");
        w_order_.pop_front();
        w_beats_done_ = 0;
    }
    return b;
}
inline std::optional<axi::ArBeat> Depacketize::pop_ar() {
    if (!s1_ar_.full()) return std::nullopt;
    if (meta_.read_full()) return std::nullopt;
    const Flit f = s1_ar_.take();
    axi::ArBeat b = decode_ar(f);
    const uint8_t downstream_id = remap_downstream_id(b.id, max_unique_ids_);
    const AxiClass cls =
        (f.get_header_field("axi_ch") == ni::AXI_CH_DataAr) ? AxiClass::Data : AxiClass::Narrow;
    // Positional init: MetaEntry is append-only for that reason. C++17 has no
    // designated initializers to pin field names, so inserting a field ahead of
    // the AR basis below would silently shift b.addr/len/size/burst. Members
    // past burst are set by name, not position: they carry defaults an AR is
    // meant to keep (collective_op = UNICAST), and restating those to reach a
    // later member is how a default drifts out of step with the struct.
    MetaEntry e{
        static_cast<uint8_t>(f.get_header_field("src_id")),
        b.id,
        static_cast<uint8_t>(f.get_header_field("ordering_req")),
        static_cast<uint8_t>(f.get_header_field("ordering_tag")),
        cls,
        b.addr,
        b.len,
        b.size,
        b.burst,
    };
    e.src_port = static_cast<uint8_t>(f.get_header_field("src_port_id"));
    meta_.allocate_read(downstream_id, e);
    b.id = downstream_id;
    return b;
}

}  // namespace ni::cmodel::nsu
