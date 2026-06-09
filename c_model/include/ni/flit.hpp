// New full-functional Flit container. Replaces c_model/include/flit.hpp.
// Header + payload bit access auto-dispatched via codegen-emitted LSB/MSB constants.
#pragma once
#include "ni_flit_constants.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>

namespace ni::cmodel {

class Flit {
  public:
    static constexpr int WIDTH_BITS = ni::FLIT_WIDTH;
    static constexpr int WIDTH_BYTES = (WIDTH_BITS + 7) / 8;

    Flit() = default;
    explicit Flit(const std::array<uint8_t, WIDTH_BYTES>& raw) : raw_(raw) {}

    // ---- Header field access ----
    void set_header_field(std::string_view name, uint64_t value) noexcept;
    uint64_t get_header_field(std::string_view name) const noexcept;

    // ---- Payload field access ----
    // channel: "AW" | "AR" | "W" | "B" | "R" (case-insensitive accepted)
    void set_payload_field(std::string_view channel, std::string_view field,
                           uint64_t value) noexcept;
    uint64_t get_payload_field(std::string_view channel, std::string_view field) const noexcept;

    // ---- Bulk payload bytes for wide fields (wdata 256-bit, rdata 256-bit) ----
    void set_payload_bytes(std::string_view channel, std::string_view field, const uint8_t* src,
                           std::size_t bit_width) noexcept;
    void get_payload_bytes(std::string_view channel, std::string_view field, uint8_t* dst,
                           std::size_t bit_width) const noexcept;

    const std::array<uint8_t, WIDTH_BYTES>& raw() const noexcept { return raw_; }
    std::array<uint8_t, WIDTH_BYTES>& raw() noexcept { return raw_; }

    bool check_padding_is_zero() const noexcept;

  private:
    std::array<uint8_t, WIDTH_BYTES> raw_{};
};

namespace detail {

struct FieldPos {
    int lsb, msb;
};

// Header field dispatch -- 13 fields, populated from codegen constants
inline FieldPos header_field_pos(std::string_view name) {
    if (name == "noc_qos") return {ni::header::NOC_QOS_LSB, ni::header::NOC_QOS_MSB};
    if (name == "axi_ch") return {ni::header::AXI_CH_LSB, ni::header::AXI_CH_MSB};
    if (name == "src_id") return {ni::header::SRC_ID_LSB, ni::header::SRC_ID_MSB};
    if (name == "dst_id") return {ni::header::DST_ID_LSB, ni::header::DST_ID_MSB};
    if (name == "vc_id") return {ni::header::VC_ID_LSB, ni::header::VC_ID_MSB};
    if (name == "route_par") return {ni::header::ROUTE_PAR_LSB, ni::header::ROUTE_PAR_MSB};
    if (name == "last") return {ni::header::LAST_LSB, ni::header::LAST_MSB};
    if (name == "rob_req") return {ni::header::ROB_REQ_LSB, ni::header::ROB_REQ_MSB};
    if (name == "rob_idx") return {ni::header::ROB_IDX_LSB, ni::header::ROB_IDX_MSB};
    if (name == "commtype") return {ni::header::COMMTYPE_LSB, ni::header::COMMTYPE_MSB};
    if (name == "multicast") return {ni::header::MULTICAST_LSB, ni::header::MULTICAST_MSB};
    if (name == "flit_ecc") return {ni::header::FLIT_ECC_LSB, ni::header::FLIT_ECC_MSB};
    if (name == "rsvd") return {ni::header::RSVD_LSB, ni::header::RSVD_MSB};
    assert(false &&
           "ni::cmodel::detail::header_field_pos: name not found in codegen-generated "
           "header layout — check ni::header::* declarations in "
           "specgen/generated/cpp/ni_flit_constants.h or rerun codegen "
           "(spec field added/renamed without regen?)");
    std::abort();  // release-build safety: prevents UB shifts on {-1,-1} fallthrough
}

// Payload field dispatch -- 5 channels, dispatched into per-channel inner namespace
FieldPos payload_field_pos(std::string_view channel, std::string_view field);

inline void write_bits(std::array<uint8_t, Flit::WIDTH_BYTES>& raw, int lsb, int msb,
                       uint64_t value) {
    for (int bit = lsb; bit <= msb; ++bit) {
        int byte = bit / 8, off = bit % 8;
        uint64_t v = (value >> (bit - lsb)) & 1u;
        raw[byte] = (raw[byte] & ~(1u << off)) | (v << off);
    }
}

inline uint64_t read_bits(const std::array<uint8_t, Flit::WIDTH_BYTES>& raw, int lsb, int msb) {
    uint64_t v = 0;
    for (int bit = lsb; bit <= msb; ++bit) {
        int byte = bit / 8, off = bit % 8;
        v |= ((raw[byte] >> off) & 1ull) << (bit - lsb);
    }
    return v;
}

inline FieldPos payload_field_pos(std::string_view channel, std::string_view field) {
    // AW channel
    if (channel == "AW" || channel == "aw") {
        if (field == "awid") return {ni::payload::aw::AWID_LSB, ni::payload::aw::AWID_MSB};
        if (field == "awaddr") return {ni::payload::aw::AWADDR_LSB, ni::payload::aw::AWADDR_MSB};
        if (field == "awlen") return {ni::payload::aw::AWLEN_LSB, ni::payload::aw::AWLEN_MSB};
        if (field == "awsize") return {ni::payload::aw::AWSIZE_LSB, ni::payload::aw::AWSIZE_MSB};
        if (field == "awburst") return {ni::payload::aw::AWBURST_LSB, ni::payload::aw::AWBURST_MSB};
        if (field == "awcache") return {ni::payload::aw::AWCACHE_LSB, ni::payload::aw::AWCACHE_MSB};
        if (field == "awlock") return {ni::payload::aw::AWLOCK_LSB, ni::payload::aw::AWLOCK_MSB};
        if (field == "awprot") return {ni::payload::aw::AWPROT_LSB, ni::payload::aw::AWPROT_MSB};
        if (field == "awregion")
            return {ni::payload::aw::AWREGION_LSB, ni::payload::aw::AWREGION_MSB};
        if (field == "awqos") return {ni::payload::aw::AWQOS_LSB, ni::payload::aw::AWQOS_MSB};
        if (field == "awuser") return {ni::payload::aw::AWUSER_LSB, ni::payload::aw::AWUSER_MSB};
        if (field == "aw_rsvd") return {ni::payload::aw::AW_RSVD_LSB, ni::payload::aw::AW_RSVD_MSB};
    }
    // AR channel
    if (channel == "AR" || channel == "ar") {
        if (field == "arid") return {ni::payload::ar::ARID_LSB, ni::payload::ar::ARID_MSB};
        if (field == "araddr") return {ni::payload::ar::ARADDR_LSB, ni::payload::ar::ARADDR_MSB};
        if (field == "arlen") return {ni::payload::ar::ARLEN_LSB, ni::payload::ar::ARLEN_MSB};
        if (field == "arsize") return {ni::payload::ar::ARSIZE_LSB, ni::payload::ar::ARSIZE_MSB};
        if (field == "arburst") return {ni::payload::ar::ARBURST_LSB, ni::payload::ar::ARBURST_MSB};
        if (field == "arcache") return {ni::payload::ar::ARCACHE_LSB, ni::payload::ar::ARCACHE_MSB};
        if (field == "arlock") return {ni::payload::ar::ARLOCK_LSB, ni::payload::ar::ARLOCK_MSB};
        if (field == "arprot") return {ni::payload::ar::ARPROT_LSB, ni::payload::ar::ARPROT_MSB};
        if (field == "arregion")
            return {ni::payload::ar::ARREGION_LSB, ni::payload::ar::ARREGION_MSB};
        if (field == "arqos") return {ni::payload::ar::ARQOS_LSB, ni::payload::ar::ARQOS_MSB};
        if (field == "aruser") return {ni::payload::ar::ARUSER_LSB, ni::payload::ar::ARUSER_MSB};
        if (field == "ar_rsvd") return {ni::payload::ar::AR_RSVD_LSB, ni::payload::ar::AR_RSVD_MSB};
    }
    // W channel
    if (channel == "W" || channel == "w") {
        if (field == "wlast") return {ni::payload::w::WLAST_LSB, ni::payload::w::WLAST_MSB};
        if (field == "wuser") return {ni::payload::w::WUSER_LSB, ni::payload::w::WUSER_MSB};
        if (field == "wstrb") return {ni::payload::w::WSTRB_LSB, ni::payload::w::WSTRB_MSB};
        if (field == "wdata") return {ni::payload::w::WDATA_LSB, ni::payload::w::WDATA_MSB};
        if (field == "w_rsvd") return {ni::payload::w::W_RSVD_LSB, ni::payload::w::W_RSVD_MSB};
    }
    // B channel
    if (channel == "B" || channel == "b") {
        if (field == "bid") return {ni::payload::b::BID_LSB, ni::payload::b::BID_MSB};
        if (field == "bresp") return {ni::payload::b::BRESP_LSB, ni::payload::b::BRESP_MSB};
        if (field == "buser") return {ni::payload::b::BUSER_LSB, ni::payload::b::BUSER_MSB};
        if (field == "b_rsvd") return {ni::payload::b::B_RSVD_LSB, ni::payload::b::B_RSVD_MSB};
    }
    // R channel
    if (channel == "R" || channel == "r") {
        if (field == "rlast") return {ni::payload::r::RLAST_LSB, ni::payload::r::RLAST_MSB};
        if (field == "rid") return {ni::payload::r::RID_LSB, ni::payload::r::RID_MSB};
        if (field == "rresp") return {ni::payload::r::RRESP_LSB, ni::payload::r::RRESP_MSB};
        if (field == "ruser") return {ni::payload::r::RUSER_LSB, ni::payload::r::RUSER_MSB};
        if (field == "rdata") return {ni::payload::r::RDATA_LSB, ni::payload::r::RDATA_MSB};
        if (field == "r_rsvd") return {ni::payload::r::R_RSVD_LSB, ni::payload::r::R_RSVD_MSB};
    }
    assert(false &&
           "ni::cmodel::detail::payload_field_pos: (channel, field) pair not found in "
           "codegen-generated payload layout — check channel spelling "
           "(AW|AR|W|B|R, case-insensitive) and field name against "
           "ni::payload::{aw,ar,w,b,r}::* in "
           "specgen/generated/cpp/ni_flit_constants.h, or rerun codegen "
           "(spec field added/renamed without regen?)");
    std::abort();  // release-build safety: prevents UB shifts on {-1,-1} fallthrough
}

}  // namespace detail

inline void Flit::set_header_field(std::string_view name, uint64_t value) noexcept {
    auto p = detail::header_field_pos(name);
    int width = p.msb - p.lsb + 1;
    uint64_t mask = (width >= 64) ? ~uint64_t{0} : ((1ull << width) - 1);
    assert((value & ~mask) == 0 && "value exceeds field width");
    detail::write_bits(raw_, p.lsb, p.msb, value & mask);
}

inline uint64_t Flit::get_header_field(std::string_view name) const noexcept {
    auto p = detail::header_field_pos(name);
    return detail::read_bits(raw_, p.lsb, p.msb);
}

inline void Flit::set_payload_field(std::string_view channel, std::string_view field,
                                    uint64_t value) noexcept {
    auto p = detail::payload_field_pos(channel, field);
    int abs_lsb = ni::HEADER_WIDTH + p.lsb;
    int abs_msb = ni::HEADER_WIDTH + p.msb;
    int width = abs_msb - abs_lsb + 1;
    uint64_t mask = (width >= 64) ? ~uint64_t{0} : ((1ull << width) - 1);
    assert((value & ~mask) == 0 && "value exceeds field width");
    detail::write_bits(raw_, abs_lsb, abs_msb, value & mask);
}

inline uint64_t Flit::get_payload_field(std::string_view channel,
                                        std::string_view field) const noexcept {
    auto p = detail::payload_field_pos(channel, field);
    int abs_lsb = ni::HEADER_WIDTH + p.lsb;
    int abs_msb = ni::HEADER_WIDTH + p.msb;
    return detail::read_bits(raw_, abs_lsb, abs_msb);
}

inline void Flit::set_payload_bytes(std::string_view channel, std::string_view field,
                                    const uint8_t* src, std::size_t bit_width) noexcept {
    auto p = detail::payload_field_pos(channel, field);
    int abs_lsb = ni::HEADER_WIDTH + p.lsb;
    assert(static_cast<int>(bit_width) == p.msb - p.lsb + 1 && "bit_width mismatch");
    for (std::size_t bit = 0; bit < bit_width; ++bit) {
        int src_byte = bit / 8, src_off = bit % 8;
        uint8_t v = (src[src_byte] >> src_off) & 1u;
        int dst_bit = abs_lsb + bit;
        int dst_byte = dst_bit / 8, dst_off = dst_bit % 8;
        raw_[dst_byte] = (raw_[dst_byte] & ~(1u << dst_off)) | (v << dst_off);
    }
}

inline void Flit::get_payload_bytes(std::string_view channel, std::string_view field, uint8_t* dst,
                                    std::size_t bit_width) const noexcept {
    auto p = detail::payload_field_pos(channel, field);
    int abs_lsb = ni::HEADER_WIDTH + p.lsb;
    assert(static_cast<int>(bit_width) == p.msb - p.lsb + 1 && "bit_width mismatch");
    std::memset(dst, 0, (bit_width + 7) / 8);
    for (std::size_t bit = 0; bit < bit_width; ++bit) {
        int src_bit = abs_lsb + bit;
        int src_byte = src_bit / 8, src_off = src_bit % 8;
        uint8_t v = (raw_[src_byte] >> src_off) & 1u;
        int dst_byte = bit / 8, dst_off = bit % 8;
        dst[dst_byte] |= (v << dst_off);
    }
}

inline bool Flit::check_padding_is_zero() const noexcept {
    for (std::size_t i = 0; i < ni::header::PADDING_FIELDS_COUNT; ++i) {
        int lsb = ni::header::PADDING_FIELDS[i].lsb;
        int msb = ni::header::PADDING_FIELDS[i].msb;
        for (int bit = lsb; bit <= msb; ++bit) {
            int byte = bit / 8, off = bit % 8;
            if ((raw_[byte] >> off) & 1u) return false;
        }
    }
    return true;
}

}  // namespace ni::cmodel
