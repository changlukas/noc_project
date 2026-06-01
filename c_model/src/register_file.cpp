#include "register_file.hpp"
#include "ni_spec.hpp"
#include <unordered_set>

namespace ni::cmodel {

namespace {
  const std::unordered_set<uint32_t>& known_offsets() {
    static const std::unordered_set<uint32_t> s{
        ni::regs::ALL_OFFSETS,
        ni::regs::ALL_OFFSETS + ni::regs::ALL_OFFSETS_COUNT};
    return s;
  }

  // Offset -> AccessMode dispatch. Mirrors the per-register *_ACCESS
  // constexpr emitted by codegen; a hand-written switch keeps the lookup
  // header-only and avoids generating yet another table.
  static ni::regs::AccessMode access_mode_of(uint32_t offset) {
    using AM = ni::regs::AccessMode;
    switch (offset) {
      case ni::regs::PKT_PROBE_EN_OFFSET:             return ni::regs::PKT_PROBE_EN_ACCESS;
      case ni::regs::PKT_PROBE_MODE_OFFSET:           return ni::regs::PKT_PROBE_MODE_ACCESS;
      case ni::regs::PKT_WINDOW_SIZE_OFFSET:          return ni::regs::PKT_WINDOW_SIZE_ACCESS;
      case ni::regs::PKT_BYTE_COUNT_OFFSET:           return ni::regs::PKT_BYTE_COUNT_ACCESS;
      case ni::regs::PKT_BANDWIDTH_OFFSET:            return ni::regs::PKT_BANDWIDTH_ACCESS;
      case ni::regs::TXN_PROBE_EN_OFFSET:             return ni::regs::TXN_PROBE_EN_ACCESS;
      case ni::regs::TXN_THRESHOLD_0_OFFSET:          return ni::regs::TXN_THRESHOLD_0_ACCESS;
      case ni::regs::TXN_THRESHOLD_1_OFFSET:          return ni::regs::TXN_THRESHOLD_1_ACCESS;
      case ni::regs::TXN_THRESHOLD_2_OFFSET:          return ni::regs::TXN_THRESHOLD_2_ACCESS;
      case ni::regs::TXN_THRESHOLD_3_OFFSET:          return ni::regs::TXN_THRESHOLD_3_ACCESS;
      case ni::regs::TXN_BIN_0_COUNT_OFFSET:          return ni::regs::TXN_BIN_0_COUNT_ACCESS;
      case ni::regs::TXN_BIN_1_COUNT_OFFSET:          return ni::regs::TXN_BIN_1_COUNT_ACCESS;
      case ni::regs::TXN_BIN_2_COUNT_OFFSET:          return ni::regs::TXN_BIN_2_COUNT_ACCESS;
      case ni::regs::TXN_BIN_3_COUNT_OFFSET:          return ni::regs::TXN_BIN_3_COUNT_ACCESS;
      case ni::regs::TXN_BIN_4_COUNT_OFFSET:          return ni::regs::TXN_BIN_4_COUNT_ACCESS;
      case ni::regs::TXN_MIN_LATENCY_OFFSET:          return ni::regs::TXN_MIN_LATENCY_ACCESS;
      case ni::regs::TXN_MAX_LATENCY_OFFSET:          return ni::regs::TXN_MAX_LATENCY_ACCESS;
      case ni::regs::TXN_TOTAL_COUNT_OFFSET:          return ni::regs::TXN_TOTAL_COUNT_ACCESS;
      case ni::regs::ERR_STATUS_OFFSET:               return ni::regs::ERR_STATUS_ACCESS;
      case ni::regs::ECC_UNCORR_ERR_CNT_OFFSET:       return ni::regs::ECC_UNCORR_ERR_CNT_ACCESS;
      case ni::regs::LAST_ERR_INFO_OFFSET:            return ni::regs::LAST_ERR_INFO_ACCESS;
      case ni::regs::IRQ_ENABLE_OFFSET:               return ni::regs::IRQ_ENABLE_ACCESS;
      case ni::regs::ECC_CORR_ERR_CNT_OFFSET:         return ni::regs::ECC_CORR_ERR_CNT_ACCESS;
      case ni::regs::ROUTE_PAR_ERR_CNT_OFFSET:        return ni::regs::ROUTE_PAR_ERR_CNT_ACCESS;
      case ni::regs::AXI_PARITY_ERR_CNT_OFFSET:       return ni::regs::AXI_PARITY_ERR_CNT_ACCESS;
      case ni::regs::PENDING_R_COUNT_OFFSET:          return ni::regs::PENDING_R_COUNT_ACCESS;
      case ni::regs::PENDING_W_COUNT_OFFSET:          return ni::regs::PENDING_W_COUNT_ACCESS;
      case ni::regs::QUIESCE_CTRL_OFFSET:             return ni::regs::QUIESCE_CTRL_ACCESS;
      case ni::regs::QUIESCE_STATUS_OFFSET:           return ni::regs::QUIESCE_STATUS_ACCESS;
      case ni::regs::EXCLUSIVE_MONITOR_CTRL_OFFSET:   return ni::regs::EXCLUSIVE_MONITOR_CTRL_ACCESS;
      case ni::regs::EXCLUSIVE_MONITOR_STATUS_OFFSET: return ni::regs::EXCLUSIVE_MONITOR_STATUS_ACCESS;
      default: return AM::RW;
    }
  }
}

RegisterFile::RegisterFile() {
  reset();
}

void RegisterFile::reset() {
  storage_.clear();
  // Reset values from codegen <REG>_RESET constants — hand-list of
  // (offset, RESET_const) pairs. Each entry references a codegen symbol;
  // no spec value is hardcoded.
  storage_[ni::regs::PKT_PROBE_EN_OFFSET]            = ni::regs::PKT_PROBE_EN_RESET;
  storage_[ni::regs::PKT_PROBE_MODE_OFFSET]          = ni::regs::PKT_PROBE_MODE_RESET;
  storage_[ni::regs::PKT_WINDOW_SIZE_OFFSET]         = ni::regs::PKT_WINDOW_SIZE_RESET;
  storage_[ni::regs::PKT_BYTE_COUNT_OFFSET]          = ni::regs::PKT_BYTE_COUNT_RESET;
  storage_[ni::regs::PKT_BANDWIDTH_OFFSET]           = ni::regs::PKT_BANDWIDTH_RESET;
  storage_[ni::regs::TXN_PROBE_EN_OFFSET]            = ni::regs::TXN_PROBE_EN_RESET;
  storage_[ni::regs::TXN_THRESHOLD_0_OFFSET]         = ni::regs::TXN_THRESHOLD_0_RESET;
  storage_[ni::regs::TXN_THRESHOLD_1_OFFSET]         = ni::regs::TXN_THRESHOLD_1_RESET;
  storage_[ni::regs::TXN_THRESHOLD_2_OFFSET]         = ni::regs::TXN_THRESHOLD_2_RESET;
  storage_[ni::regs::TXN_THRESHOLD_3_OFFSET]         = ni::regs::TXN_THRESHOLD_3_RESET;
  storage_[ni::regs::TXN_BIN_0_COUNT_OFFSET]         = ni::regs::TXN_BIN_0_COUNT_RESET;
  storage_[ni::regs::TXN_BIN_1_COUNT_OFFSET]         = ni::regs::TXN_BIN_1_COUNT_RESET;
  storage_[ni::regs::TXN_BIN_2_COUNT_OFFSET]         = ni::regs::TXN_BIN_2_COUNT_RESET;
  storage_[ni::regs::TXN_BIN_3_COUNT_OFFSET]         = ni::regs::TXN_BIN_3_COUNT_RESET;
  storage_[ni::regs::TXN_BIN_4_COUNT_OFFSET]         = ni::regs::TXN_BIN_4_COUNT_RESET;
  storage_[ni::regs::TXN_MIN_LATENCY_OFFSET]         = ni::regs::TXN_MIN_LATENCY_RESET;
  storage_[ni::regs::TXN_MAX_LATENCY_OFFSET]         = ni::regs::TXN_MAX_LATENCY_RESET;
  storage_[ni::regs::TXN_TOTAL_COUNT_OFFSET]         = ni::regs::TXN_TOTAL_COUNT_RESET;
  storage_[ni::regs::ERR_STATUS_OFFSET]              = ni::regs::ERR_STATUS_RESET;
  storage_[ni::regs::ECC_UNCORR_ERR_CNT_OFFSET]      = ni::regs::ECC_UNCORR_ERR_CNT_RESET;
  storage_[ni::regs::LAST_ERR_INFO_OFFSET]           = ni::regs::LAST_ERR_INFO_RESET;
  storage_[ni::regs::IRQ_ENABLE_OFFSET]              = ni::regs::IRQ_ENABLE_RESET;
  storage_[ni::regs::ECC_CORR_ERR_CNT_OFFSET]        = ni::regs::ECC_CORR_ERR_CNT_RESET;
  storage_[ni::regs::ROUTE_PAR_ERR_CNT_OFFSET]       = ni::regs::ROUTE_PAR_ERR_CNT_RESET;
  storage_[ni::regs::AXI_PARITY_ERR_CNT_OFFSET]      = ni::regs::AXI_PARITY_ERR_CNT_RESET;
  storage_[ni::regs::PENDING_R_COUNT_OFFSET]         = ni::regs::PENDING_R_COUNT_RESET;
  storage_[ni::regs::PENDING_W_COUNT_OFFSET]         = ni::regs::PENDING_W_COUNT_RESET;
  storage_[ni::regs::QUIESCE_CTRL_OFFSET]            = ni::regs::QUIESCE_CTRL_RESET;
  storage_[ni::regs::QUIESCE_STATUS_OFFSET]          = ni::regs::QUIESCE_STATUS_RESET;
  storage_[ni::regs::EXCLUSIVE_MONITOR_CTRL_OFFSET]  = ni::regs::EXCLUSIVE_MONITOR_CTRL_RESET;
  storage_[ni::regs::EXCLUSIVE_MONITOR_STATUS_OFFSET]= ni::regs::EXCLUSIVE_MONITOR_STATUS_RESET;
  last_irq_         = false;
  last_rw1c_clear_  = false;
}

bool RegisterFile::is_mapped_(uint32_t offset) const {
  return known_offsets().count(offset) != 0;
}
bool RegisterFile::is_wo_(uint32_t offset) const {
  return access_mode_of(offset) == ni::regs::AccessMode::WO;
}
bool RegisterFile::is_rw1c_(uint32_t offset) const {
  return access_mode_of(offset) == ni::regs::AccessMode::RW1C;
}

AbiResponse RegisterFile::read32(uint32_t offset) {
  // Check misalignment first: spec distinguishes misaligned vs unmapped,
  // and mapped offsets are all word-aligned, so a misaligned offset can
  // never be a mapped one — checking unmapped first would mis-classify
  // misaligned accesses as unmapped (DecErr) instead of misaligned (SlvErr).
  if (offset % 4 != 0) {
    if constexpr (ni::regs::csr_policy::MISALIGNED_IS_SLVERR) {
      return {AbiResult::SlvErr, 0};
    } else {
      return {AbiResult::DecErr, 0};  // misaligned = lower-aligned/decerr fallback
    }
  }
  if (!is_mapped_(offset)) {
    // policy: unmapped_read = decerr
    if constexpr (ni::regs::csr_policy::UNMAPPED_READ_IS_DECERR) {
      return {AbiResult::DecErr, 0};
    } else {
      return {AbiResult::Ok, 0};  // unmapped_read = zero (fallback)
    }
  }
  // WO registers always read as 0 regardless of underlying storage.
  if (access_mode_of(offset) == ni::regs::AccessMode::WO) {
    return {AbiResult::Ok, 0};
  }
  return {AbiResult::Ok, storage_[offset]};
}

AbiResponse RegisterFile::write32(uint32_t offset, uint32_t value, uint8_t wstrb) {
  // Same ordering as read32: misalignment is checked before mapping.
  if (offset % 4 != 0) {
    if constexpr (ni::regs::csr_policy::MISALIGNED_IS_SLVERR) {
      return {AbiResult::SlvErr, 0};
    }
    return {AbiResult::DecErr, 0};
  }
  if (!is_mapped_(offset)) {
    if constexpr (ni::regs::csr_policy::UNMAPPED_READ_IS_DECERR) {
      // write follows same policy as read for unmapped (no separate spec field)
      return {AbiResult::DecErr, 0};
    }
    return {AbiResult::Ok, 0};
  }
  if (wstrb != 0b1111) {
    if constexpr (ni::regs::csr_policy::SUB_WORD_WRITE_IS_SLVERR) {
      return {AbiResult::SlvErr, 0};
    }
    // sub_word_write = ignored: silently drop, return Ok
    return {AbiResult::Ok, 0};
  }
  using AM = ni::regs::AccessMode;
  switch (access_mode_of(offset)) {
    case AM::RO:
      // AXI4-Lite convention: writes to RO silently ignored, response Ok.
      last_irq_         = false;
      last_rw1c_clear_  = false;
      return {AbiResult::Ok, 0};
    case AM::RW1C: {
      uint32_t before  = storage_[offset];
      uint32_t after   = before & ~value;
      storage_[offset] = after;
      last_irq_        = false;
      last_rw1c_clear_ = (before != after);
      return {AbiResult::Ok, 0};
    }
    case AM::WO:
      storage_[offset]  = value;
      last_irq_         = false;
      last_rw1c_clear_  = false;
      return {AbiResult::Ok, 0};
    case AM::RW:
    default:
      storage_[offset]  = value;
      last_irq_         = false;
      last_rw1c_clear_  = false;
      return {AbiResult::Ok, 0};
  }
}

uint32_t RegisterFile::read_field(uint32_t offset, uint32_t mask) const {
  auto it = storage_.find(offset);
  uint32_t val = (it == storage_.end()) ? 0 : it->second;
  int shift = 0;
  while (shift < 32 && !((mask >> shift) & 1)) ++shift;
  return (val & mask) >> shift;
}

void RegisterFile::write_field(uint32_t offset, uint32_t mask, uint32_t value) {
  uint32_t v = storage_[offset];
  int shift = 0;
  while (shift < 32 && !((mask >> shift) & 1)) ++shift;
  v = (v & ~mask) | ((value << shift) & mask);
  storage_[offset] = v;
}

} // namespace ni::cmodel
