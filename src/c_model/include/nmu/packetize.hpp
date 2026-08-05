#pragma once
// NMU-side request packetizer. Stateless except for a small write-metadata
// FIFO (populated by push_aw, consumed by W beats with wlast=1). Implements
// RequestPacketizer (AW / W / AR only; NMU never emits responses).
//
// Network steering (S3a T6, spec :348 axi_ch -> network map): Narrow-class
// AW/W/AR and Data-class AR all push to the REQ-face outs (aw_out_/w_out_/
// ar_out_); Data-class AW/W push to the DAT-face outs (dat_aw_out_/
// dat_w_out_) instead -- DataAr stays on REQ, the only asymmetry in the map.
// Both AW/W of one Data-class worm land on the SAME network so the caller's
// WormholeArbiter {AW,W} lock (nmu.hpp) spans the whole worm; splitting them
// across networks was the S3a stage design §1 correctness hazard this fixes.
//
// Header fields per push:
//   src_id      — constructor arg (NMU tile coord, fixed per instance)
//   dst_id      — direct-path Packetizer interface: derived from b.addr via
//                 sam_.translate (SamTable). Rob-driven path
//                 (push_*_with_meta) supplies dst_id directly via
//                 AwHeaderMeta, computed from Rob's own SamTable member
//                 (sam_.translate). For W beats, dst inherited from
//                 the AW write-meta FIFO front.
//   vc_id       — placeholder 0; the downstream VcArbiter stamps the real VC
//   axi_ch      — implicit per push_* method
//   flit_tail   — wormhole packet boundary marker (FlooNoC pattern):
//                 AW=0 (start of AW+W wormhole packet);
//                 W=wlast (end on last W beat of burst);
//                 AR=1 (single-flit read request packet).
//   ordering_req,
//   ordering_tag     — 0 in direct-path interface (Disabled mode); Enabled mode
//                 supplies via AwHeaderMeta.
//   commtype,
//   multicast,
//   noc_qos,
//   flit_ecc    — 0-filled placeholder (width 0, ECC unbuilt)
//   rsvd        — 0 by Flit default
#include "axi/types.hpp"
#include "flit.hpp"
#include "nmu/addr_trans.hpp"
#include "router/req_out.hpp"
#include "request_io.hpp"
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <utility>

namespace ni::cmodel::nmu {

// Per-AW/AR metadata for header stamping. Used by both the direct-path Packetizer
// interface (auto-filled from addr_trans + rob_*=0) and the explicit
// push_*_with_meta path (called by Rob with full metadata).
struct AwHeaderMeta {
    uint8_t dst_id;                           // from addr_trans
    uint64_t local_addr;                      // from addr_trans (= awaddr in c_model)
    uint8_t ordering_req;                     // 0 in Disabled mode, 1 in Enabled mode
    uint8_t ordering_tag;                     // 0 in Disabled, allocated in Enabled
    axi::AxiClass cls = axi::AxiClass::Data;  // from addr_trans (SAM space)
};

class NmuPacketizeSink {
  public:
    virtual ~NmuPacketizeSink() = default;
    virtual bool push_aw_with_meta(const axi::AwBeat& b, AwHeaderMeta meta) = 0;
    virtual bool push_w(const axi::WBeat& b) = 0;
    virtual bool push_ar_with_meta(const axi::ArBeat& b, AwHeaderMeta meta) = 0;
};

class Packetize : public RequestPacketizer, public NmuPacketizeSink {
  public:
    // dat_aw_out/dat_w_out: DAT face for Data-class AW/W (S3a T6 steering,
    // spec :348 network map). AR always rides ar_out_ (REQ) regardless of
    // class -- DataAr stays on REQ; only DataAw/DataW move to DAT.
    Packetize(router::NocReqOut& aw_out, router::NocReqOut& w_out, router::NocReqOut& ar_out,
              router::NocReqOut& dat_aw_out, router::NocReqOut& dat_w_out, uint8_t src_id,
              addr_trans::SamTable sam)
        : aw_out_(aw_out),
          w_out_(w_out),
          ar_out_(ar_out),
          dat_aw_out_(dat_aw_out),
          dat_w_out_(dat_w_out),
          src_id_(src_id),
          sam_(std::move(sam)) {}

    // ---- RequestPacketizer interface ----
    bool push_aw(const axi::AwBeat& b) override {
        auto t = sam_.translate(b.addr);
        assert(sam_.burst_footprint_ok(
                   b.addr, addr_trans::burst_last_byte(b.addr, b.len, b.size, b.burst)) &&
               "SAM: AW burst footprint crosses a tile boundary");
        return push_aw_with_meta(b, {t.dst_id, t.local_addr, 0, 0, t.cls});
    }
    // INVARIANT: caller must push_aw before push_w for the same write txn. W
    // FIFO ordering inherits AW issue order; Rob layer enforces this via
    // w_bursts_owed_ (AW-before-W interlock) in Disabled mode.
    bool push_w(const axi::WBeat& b) override;
    bool push_ar(const axi::ArBeat& b) override {
        auto t = sam_.translate(b.addr);
        assert(sam_.burst_footprint_ok(
                   b.addr, addr_trans::burst_last_byte(b.addr, b.len, b.size, b.burst)) &&
               "SAM: AR burst footprint crosses a tile boundary");
        return push_ar_with_meta(b, {t.dst_id, t.local_addr, 0, 0, t.cls});
    }

    // ---- Non-interface methods, called by Rob with full metadata ----
    // Future Enabled mode supplies ordering_tag via this path.
    bool push_aw_with_meta(const axi::AwBeat& b, AwHeaderMeta meta) override;
    bool push_ar_with_meta(const axi::ArBeat& b, AwHeaderMeta meta) override;

  private:
    router::NocReqOut& aw_out_;
    router::NocReqOut& w_out_;
    router::NocReqOut& ar_out_;
    router::NocReqOut& dat_aw_out_;
    router::NocReqOut& dat_w_out_;
    uint8_t src_id_;
    addr_trans::SamTable sam_;

    // W FIFO carries the meta inherited from AW. local_addr/len/size/burst +
    // beat_counter feed the narrow class's lane re-anchor (axi::beat_addr):
    // a W beat's flit carries no address, only its paired AW does.
    struct WMeta {
        uint8_t dst_id;
        uint8_t ordering_req;
        uint8_t ordering_tag;
        axi::AxiClass cls;
        uint64_t local_addr;
        uint8_t len;
        uint8_t size;
        axi::Burst burst;
        uint16_t beat_counter = 0;
    };
    std::deque<WMeta> w_meta_fifo_;
};

// ---- inline impl ----

inline bool Packetize::push_aw_with_meta(const axi::AwBeat& b, AwHeaderMeta meta) {
    // AWUSER[57:8] (collective_op[9:8] + collective_mask[57:10]) is consumed
    // here and never forwarded to the AW payload (spec: docs/noc-target-spec.md
    // AWUSER layout). S4 will translate the mask into the flit's
    // collective_mask/collective_op header fields and fan out; until then a
    // nonzero collective request is a hard failure, not backpressure — `return
    // false` is the retry-until-it-clears idiom (NoC-full, RoB-full) and a
    // collective request never clears on retry, so it would wedge the S1
    // AW/W stage forever indistinguishable from congestion. Fail loud instead,
    // matching addr_trans.hpp / depacketize.hpp / rob.hpp for this class of
    // permanent illegal-input condition.
    if ((b.user >> 8) != 0) {
        assert(false &&
               "nmu::Packetize::push_aw_with_meta: nonzero AWUSER collective_op/collective_mask "
               "unsupported until S4");
        std::abort();  // belt-and-braces for NDEBUG
    }
    // Narrow class rides the 81 b NarrowW payload (64 b data lane): AxSIZE > 3
    // (8 B) does not fit. Same fatal-assert shape as the collective reject
    // above -- a stimulus/SAM-config error, not backpressure.
    if (meta.cls == axi::AxiClass::Narrow && b.size > 3) {
        assert(false &&
               "nmu::Packetize::push_aw_with_meta: narrow class (SAM config space) requires "
               "AWSIZE <= 3 (8 B); larger does not fit the NarrowW payload");
        std::abort();  // belt-and-braces for NDEBUG
    }
    const uint8_t payload_user = static_cast<uint8_t>(b.user & 0xFFu);
    const bool is_data = (meta.cls == axi::AxiClass::Data);

    Flit f;
    f.set_header_field("axi_ch", is_data ? ni::AXI_CH_DataAw : ni::AXI_CH_NarrowAw);
    f.set_header_field("src_id", src_id_);
    f.set_header_field("dst_id", meta.dst_id);
    f.set_header_field("vc_id", 0);
    f.set_header_field("flit_tail", 0);  // AW starts wormhole packet (FlooNoC pattern)
    f.set_header_field("ordering_req", meta.ordering_req);
    f.set_header_field("ordering_tag", meta.ordering_tag);
    f.set_payload_field("AW", "awid", b.id);
    f.set_payload_field("AW", "awaddr", meta.local_addr);  // NOT b.addr (future remap-safe)
    f.set_payload_field("AW", "awlen", b.len);
    f.set_payload_field("AW", "awsize", b.size);
    f.set_payload_field("AW", "awburst", static_cast<uint64_t>(b.burst));
    f.set_payload_field("AW", "awcache", b.cache);
    f.set_payload_field("AW", "awlock", b.lock);
    f.set_payload_field("AW", "awprot", b.prot);
    f.set_payload_field("AW", "awregion", b.region);
    f.set_payload_field("AW", "awqos", b.qos);
    f.set_payload_field("AW", "awuser", payload_user);
    router::NocReqOut& out = is_data ? dat_aw_out_ : aw_out_;
    if (!out.push_flit(f)) return false;
    w_meta_fifo_.push_back({meta.dst_id, meta.ordering_req, meta.ordering_tag, meta.cls,
                            meta.local_addr, b.len, b.size, b.burst, /*beat_counter=*/0});
    return true;
}

// INVARIANT: caller must push_aw before push_w for the same write txn. W
// FIFO ordering inherits AW issue order; Rob layer enforces this via
// w_bursts_owed_ (AW-before-W interlock) in Disabled mode.
inline bool Packetize::push_w(const axi::WBeat& b) {
    // A W beat inherits its AW's dst/rob metadata from the front of w_meta_fifo_.
    // If empty, the W's AW has not yet been admitted to Packetize (its AW is
    // still upstream in the bridge). Backpressure so the W waits WITHOUT blocking
    // AW/AR admission. AXI4 W is non-interleaved (no WID), so the FIFO front is
    // always the correct write for the next W beat in issue order.
    if (w_meta_fifo_.empty()) return false;
    auto& meta = w_meta_fifo_.front();
    const bool is_data = (meta.cls == axi::AxiClass::Data);
    const char* ch = is_data ? "DATA_W" : "NARROW_W";
    Flit f;
    f.set_header_field("axi_ch", is_data ? ni::AXI_CH_DataW : ni::AXI_CH_NarrowW);
    f.set_header_field("src_id", src_id_);
    f.set_header_field("dst_id", meta.dst_id);
    f.set_header_field("vc_id", 0);
    f.set_header_field("flit_tail", b.last ? 1u : 0u);  // W's wlast ends wormhole packet (FlooNoC)
    f.set_header_field("ordering_req", meta.ordering_req);
    f.set_header_field("ordering_tag", meta.ordering_tag);
    f.set_payload_field(ch, "wlast", b.last ? 1u : 0u);
    f.set_payload_field(ch, "wuser", b.user);
    if (is_data) {
        f.set_payload_field(ch, "wstrb", b.strb);
        f.set_payload_bytes(ch, "wdata", b.data.data(), ni::width::NOC_DATA_WIDTH);
    } else {
        // Narrow: extract this beat's addressed 8 B lane from the shared
        // DATA_BYTES-wide WBeat. meta carries the AW basis (the flit's own
        // "AW" payload has no per-beat address); beat_counter positions this
        // beat within the burst (INCR/WRAP addresses move beat to beat).
        const uint64_t addr =
            axi::beat_addr(meta.local_addr, meta.len, meta.size, meta.burst, meta.beat_counter);
        const unsigned lane = axi::narrow_lane(addr);
        f.set_payload_field(ch, "wstrb", (b.strb >> (lane * axi::NARROW_DATA_BYTES)) & 0xFFull);
        f.set_payload_bytes(ch, "wdata", b.data.data() + lane * axi::NARROW_DATA_BYTES,
                            ni::width::NOC_NARROW_DATA_WIDTH);
    }
    router::NocReqOut& out = is_data ? dat_w_out_ : w_out_;
    if (!out.push_flit(f)) return false;
    ++meta.beat_counter;
    if (b.last) w_meta_fifo_.pop_front();
    return true;
}

inline bool Packetize::push_ar_with_meta(const axi::ArBeat& b, AwHeaderMeta meta) {
    // Same narrow-size reject as push_aw_with_meta (see comment there).
    if (meta.cls == axi::AxiClass::Narrow && b.size > 3) {
        assert(false &&
               "nmu::Packetize::push_ar_with_meta: narrow class (SAM config space) requires "
               "ARSIZE <= 3 (8 B); larger does not fit the NarrowR payload");
        std::abort();  // belt-and-braces for NDEBUG
    }
    Flit f;
    f.set_header_field("axi_ch",
                       meta.cls == axi::AxiClass::Data ? ni::AXI_CH_DataAr : ni::AXI_CH_NarrowAr);
    f.set_header_field("src_id", src_id_);
    f.set_header_field("dst_id", meta.dst_id);
    f.set_header_field("vc_id", 0);
    f.set_header_field("flit_tail", 1);
    f.set_header_field("ordering_req", meta.ordering_req);
    f.set_header_field("ordering_tag", meta.ordering_tag);
    f.set_payload_field("AR", "arid", b.id);
    f.set_payload_field("AR", "araddr", meta.local_addr);
    f.set_payload_field("AR", "arlen", b.len);
    f.set_payload_field("AR", "arsize", b.size);
    f.set_payload_field("AR", "arburst", static_cast<uint64_t>(b.burst));
    f.set_payload_field("AR", "arcache", b.cache);
    f.set_payload_field("AR", "arlock", b.lock);
    f.set_payload_field("AR", "arprot", b.prot);
    f.set_payload_field("AR", "arregion", b.region);
    f.set_payload_field("AR", "arqos", b.qos);
    f.set_payload_field("AR", "aruser", b.user);
    if (!ar_out_.push_flit(f)) return false;
    return true;
}

}  // namespace ni::cmodel::nmu
