#pragma once
#include "ni_spec.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <string_view>

namespace ni::cmodel {

class Flit {
public:
  static constexpr int WIDTH_BITS  = ni::FLIT_WIDTH;
  static constexpr int WIDTH_BYTES = (WIDTH_BITS + 7) / 8;

  Flit() = default;
  explicit Flit(const std::array<uint8_t, WIDTH_BYTES>& raw);

  void     set_header_field(std::string_view name, uint64_t value);
  uint64_t get_header_field(std::string_view name) const;

  const std::array<uint8_t, WIDTH_BYTES>& raw() const noexcept { return raw_; }
  bool check_padding_is_zero() const;

private:
  std::array<uint8_t, WIDTH_BYTES> raw_{};
};

namespace detail {

// Look up (lsb, msb) for a header field using codegen's per-field constants.
// Hand-rolled dispatch because codegen has not yet elaborated HeaderField enum
// (sufficiency finding -- see SUFFICIENCY_FINDINGS.md).
struct FieldPos { int lsb, msb; };

inline FieldPos header_field_pos(std::string_view name) {
  if (name == "dst_id")   return {ni::header::DST_ID_LSB,   ni::header::DST_ID_MSB};
  if (name == "src_id")   return {ni::header::SRC_ID_LSB,   ni::header::SRC_ID_MSB};
  if (name == "axi_ch")   return {ni::header::AXI_CH_LSB,   ni::header::AXI_CH_MSB};
  if (name == "last")     return {ni::header::LAST_LSB,     ni::header::LAST_MSB};
  if (name == "rob_idx")  return {ni::header::ROB_IDX_LSB,  ni::header::ROB_IDX_MSB};
  if (name == "rob_req")  return {ni::header::ROB_REQ_LSB,  ni::header::ROB_REQ_MSB};
  // TODO when codegen elaborates HeaderField enum, replace this with table lookup.
  return {-1, -1};  // unknown
}

inline void write_bits(std::array<uint8_t, Flit::WIDTH_BYTES>& raw,
                       int lsb, int msb, uint64_t value) {
  for (int bit = lsb; bit <= msb; ++bit) {
    int byte = bit / 8, off = bit % 8;
    uint64_t v = (value >> (bit - lsb)) & 1u;
    raw[byte] = (raw[byte] & ~(1u << off)) | (v << off);
  }
}

inline uint64_t read_bits(const std::array<uint8_t, Flit::WIDTH_BYTES>& raw,
                          int lsb, int msb) {
  uint64_t v = 0;
  for (int bit = lsb; bit <= msb; ++bit) {
    int byte = bit / 8, off = bit % 8;
    v |= ((raw[byte] >> off) & 1ull) << (bit - lsb);
  }
  return v;
}

} // namespace detail

inline Flit::Flit(const std::array<uint8_t, WIDTH_BYTES>& raw) : raw_(raw) {}

inline void Flit::set_header_field(std::string_view name, uint64_t value) {
  auto p = detail::header_field_pos(name);
  if (p.lsb < 0) return;  // unknown field; silently ignore (sufficiency gap)
  // Silent truncate (RTL-equivalent); debug build asserts oversized value.
  uint64_t mask = (p.msb == p.lsb) ? 1ull : ((1ull << (p.msb - p.lsb + 1)) - 1);
  assert((value & ~mask) == 0 && "value exceeds field width");
  detail::write_bits(raw_, p.lsb, p.msb, value & mask);
}

inline uint64_t Flit::get_header_field(std::string_view name) const {
  auto p = detail::header_field_pos(name);
  if (p.lsb < 0) return 0;
  return detail::read_bits(raw_, p.lsb, p.msb);
}

inline bool Flit::check_padding_is_zero() const {
  for (std::size_t i = 0; i < ni::header::PADDING_FIELDS_COUNT; ++i) {
    int lsb = ni::header::PADDING_FIELDS[i].lsb;
    int msb = ni::header::PADDING_FIELDS[i].msb;
    for (int bit = lsb; bit <= msb; ++bit) {
      int byte = bit / 8, off = bit % 8;
      if ((raw_[byte] >> off) & 1u) {
        return false;
      }
    }
  }
  return true;
}

} // namespace ni::cmodel
