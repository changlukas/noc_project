#pragma once
#include "axi/types.hpp"
#include "ni/flit.hpp"
#include "ni/packetizer.hpp"
#include "noc/noc_rsp_out.hpp"
#include "nsu/meta_buffer.hpp"
#include <cassert>
#include <cstdint>
#include <cstdlib>

namespace ni::cmodel::nsu {

// NSU-side response packetizer. Looks up dst_id/rob_* from MetaBuffer via
// peek+commit so a failed NocRspOut.push_flit() never desynchronizes.
// Implements 5-method Packetizer interface; push_aw/push_w/push_ar assert
// false (NSU never emits requests).
class Packetize : public Packetizer {
  public:
    Packetize(noc::NocRspOut& b_out, noc::NocRspOut& r_out, MetaBuffer& meta, uint8_t src_id)
        : b_out_(b_out), r_out_(r_out), meta_(meta), src_id_(src_id) {}

    // ---- Packetizer interface (response methods are real) ----
    bool push_b(const axi::BBeat& b) override;
    bool push_r(const axi::RBeat& b) override;

    // ---- Request methods assert false ----
    bool push_aw(const axi::AwBeat&) override {
        assert(false &&
               "nsu::Packetize::push_aw: NSU packetizer handles response side only "
               "(B/R into NocRspOut) — AW belongs on the request side "
               "(NMU Packetize). Likely cause: AxiMasterPort wiring routed a "
               "request beat into the NSU packetizer, or test fixture invoked the "
               "wrong Packetizer instance.");
        std::abort();
        return false;
    }
    bool push_w(const axi::WBeat&) override {
        assert(false &&
               "nsu::Packetize::push_w: NSU packetizer handles response side only "
               "(B/R into NocRspOut) — W belongs on the request side "
               "(NMU Packetize). Likely cause: AxiMasterPort wiring routed a "
               "request beat into the NSU packetizer, or test fixture invoked the "
               "wrong Packetizer instance.");
        std::abort();
        return false;
    }
    bool push_ar(const axi::ArBeat&) override {
        assert(false &&
               "nsu::Packetize::push_ar: NSU packetizer handles response side only "
               "(B/R into NocRspOut) — AR belongs on the request side "
               "(NMU Packetize). Likely cause: AxiMasterPort wiring routed a "
               "request beat into the NSU packetizer, or test fixture invoked the "
               "wrong Packetizer instance.");
        std::abort();
        return false;
    }

  private:
    noc::NocRspOut& b_out_;
    noc::NocRspOut& r_out_;
    MetaBuffer& meta_;
    uint8_t src_id_;
};

inline bool Packetize::push_b(const axi::BBeat& b) {
    auto meta_opt = meta_.peek_write(b.id);
    if (!meta_opt.has_value()) {
        assert(false && "B with no matching outstanding AW (MetaBuffer empty for id)");
        std::abort();  // belt-and-braces for NDEBUG
    }
    const auto& m = *meta_opt;

    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_B);
    f.set_header_field("src_id", src_id_);
    f.set_header_field("dst_id", m.src_id);
    f.set_header_field("vc_id", 0);
    f.set_header_field("last", 1);
    f.set_header_field("rob_req", m.rob_req);
    f.set_header_field("rob_idx", m.rob_idx);
    f.set_payload_field("B", "bid", b.id);
    f.set_payload_field("B", "bresp", static_cast<uint64_t>(b.resp));
    f.set_payload_field("B", "buser", b.user);
    if (!b_out_.push_flit(f)) return false;
    meta_.commit_write(b.id);
    return true;
}

inline bool Packetize::push_r(const axi::RBeat& b) {
    auto meta_opt = meta_.peek_read(b.id);
    if (!meta_opt.has_value()) {
        assert(false && "R with no matching outstanding AR (MetaBuffer empty for id)");
        std::abort();  // belt-and-braces for NDEBUG
    }
    const auto& m = *meta_opt;

    Flit f;
    f.set_header_field("axi_ch", ni::AXI_CH_R);
    f.set_header_field("src_id", src_id_);
    f.set_header_field("dst_id", m.src_id);
    f.set_header_field("vc_id", 0);
    f.set_header_field("last", 1);
    f.set_header_field("rob_req", m.rob_req);
    f.set_header_field("rob_idx", m.rob_idx);
    f.set_payload_field("R", "rid", b.id);
    f.set_payload_field("R", "rresp", static_cast<uint64_t>(b.resp));
    f.set_payload_field("R", "ruser", b.user);
    f.set_payload_field("R", "rlast", b.last ? 1u : 0u);
    f.set_payload_bytes("R", "rdata", b.data.data(), axi::NOC_DATA_WIDTH_BITS);
    if (!r_out_.push_flit(f)) return false;
    if (b.last) meta_.commit_read(b.id);
    return true;
}

}  // namespace ni::cmodel::nsu
