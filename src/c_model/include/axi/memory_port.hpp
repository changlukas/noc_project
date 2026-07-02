// Algorithms ported from cocotbext-axi (MIT) — see axi/ATTRIBUTION.md
#pragma once
#include "axi/types.hpp"
#include <optional>

namespace ni::cmodel::axi {

struct MemWriteReq {
    uint64_t addr;
    std::array<uint8_t, DATA_BYTES> data;
    uint32_t strb;
    uint8_t id;
    bool last;
    uint64_t tag;
};

struct MemWriteResp {
    uint8_t id;
    Resp resp;
    uint64_t tag;
};

struct MemReadReq {
    uint64_t addr;
    uint8_t size;
    uint8_t id;
    bool last;
    uint64_t tag;
};

struct MemReadResp {
    uint8_t id;
    std::array<uint8_t, DATA_BYTES> data;
    Resp resp;
    bool last;
    uint64_t tag;
};

class IMemoryPort {
  public:
    virtual ~IMemoryPort() = default;
    virtual bool submit_write(const MemWriteReq&) = 0;
    virtual bool submit_read(const MemReadReq&) = 0;
    virtual std::optional<MemWriteResp> pop_write_resp() = 0;
    virtual std::optional<MemReadResp> pop_read_resp() = 0;
};

}  // namespace ni::cmodel::axi
