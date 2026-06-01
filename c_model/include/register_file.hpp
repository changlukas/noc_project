#pragma once
#include "ni_spec.hpp"
#include <cstdint>
#include <unordered_map>

namespace ni::cmodel {

enum class AbiResult { Ok, DecErr, SlvErr };

struct AbiResponse {
  AbiResult status;
  uint32_t  data;     // 0 if status != Ok
};

class RegisterFile {
public:
  RegisterFile();

  AbiResponse read32(uint32_t offset);
  AbiResponse write32(uint32_t offset, uint32_t value, uint8_t wstrb = 0b1111);

  uint32_t read_field(uint32_t offset, uint32_t mask) const;
  void     write_field(uint32_t offset, uint32_t mask, uint32_t value);

  void reset();
  bool last_write_triggered_irq() const   { return last_irq_; }
  bool last_write_cleared_rw1c_field() const { return last_rw1c_clear_; }

private:
  std::unordered_map<uint32_t, uint32_t> storage_;
  bool last_irq_         = false;
  bool last_rw1c_clear_  = false;

  bool is_mapped_(uint32_t offset) const;
  bool is_wo_(uint32_t offset) const;
  bool is_rw1c_(uint32_t offset) const;
};

} // namespace ni::cmodel
