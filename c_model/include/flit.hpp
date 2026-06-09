// NI-layer flit container: header + payload bit access dispatched via
// codegen-emitted HEADER_FIELDS[] / *_PAYLOAD_FIELDS[] arrays.
#pragma once
#include "ni_flit_constants.h"
#include <array>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
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

// Generic loop over codegen-emitted HEADER_FIELDS[]. rsvd (enabled=false) is
// absent from the array, so querying it falls through and aborts.
inline FieldPos header_field_pos(std::string_view name) {
    for (const auto& f : ni::HEADER_FIELDS) {
        if (f.name == name) return {f.lsb, f.msb};
    }
    std::fprintf(stderr,
                 "ni::cmodel::detail::header_field_pos: name '%.*s' not found "
                 "in codegen HEADER_FIELDS[] — check ni_packet.json or regen.\n",
                 static_cast<int>(name.size()), name.data());
    assert(false && "header_field_pos: name not found");
    std::abort();
}

// Case-insensitive equality (preserves existing case-normalization contract:
// callers pass "AW" or "aw" interchangeably).
inline bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::toupper(ca) != std::toupper(cb)) return false;
    }
    return true;
}

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
    auto find_in = [&](const auto& arr) -> std::optional<FieldPos> {
        for (const auto& f : arr) {
            if (f.name == field) return FieldPos{f.lsb, f.msb};
        }
        return std::nullopt;
    };
    std::optional<FieldPos> hit;
    if (ieq(channel, "AW"))
        hit = find_in(ni::AW_PAYLOAD_FIELDS);
    else if (ieq(channel, "AR"))
        hit = find_in(ni::AR_PAYLOAD_FIELDS);
    else if (ieq(channel, "W"))
        hit = find_in(ni::W_PAYLOAD_FIELDS);
    else if (ieq(channel, "B"))
        hit = find_in(ni::B_PAYLOAD_FIELDS);
    else if (ieq(channel, "R"))
        hit = find_in(ni::R_PAYLOAD_FIELDS);
    if (hit) return *hit;
    // channel.data() is reused for the array name; case may not match the
    // actual array name (e.g. "aw" prints aw_PAYLOAD_FIELDS[] not AW_...).
    std::fprintf(stderr,
                 "ni::cmodel::detail::payload_field_pos: %.*s.%.*s not found "
                 "in codegen %.*s_PAYLOAD_FIELDS[] — check ni_packet.json or regen.\n",
                 static_cast<int>(channel.size()), channel.data(), static_cast<int>(field.size()),
                 field.data(), static_cast<int>(channel.size()), channel.data());
    assert(false && "payload_field_pos: field not found");
    std::abort();
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
