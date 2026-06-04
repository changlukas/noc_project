// Algorithms ported from cocotbext-axi (MIT) — see axi/ATTRIBUTION.md
#pragma once
#include "axi/memory_port.hpp"
#include <deque>

namespace ni::cmodel::axi::testing {

class MockMemoryPort : public IMemoryPort {
  public:
    std::size_t write_capacity = 16;
    std::size_t read_capacity = 16;

    bool submit_write(const MemWriteReq& req) override {
        if (captured_writes.size() >= write_capacity) return false;
        captured_writes.push_back(req);
        return true;
    }
    bool submit_read(const MemReadReq& req) override {
        if (captured_reads.size() >= read_capacity) return false;
        captured_reads.push_back(req);
        return true;
    }
    std::optional<MemWriteResp> pop_write_resp() override {
        if (queued_write_resps.empty()) return std::nullopt;
        auto r = queued_write_resps.front();
        queued_write_resps.pop_front();
        return r;
    }
    std::optional<MemReadResp> pop_read_resp() override {
        if (queued_read_resps.empty()) return std::nullopt;
        auto r = queued_read_resps.front();
        queued_read_resps.pop_front();
        return r;
    }

    std::deque<MemWriteReq> captured_writes;
    std::deque<MemReadReq> captured_reads;
    std::deque<MemWriteResp> queued_write_resps;
    std::deque<MemReadResp> queued_read_resps;
};

}  // namespace ni::cmodel::axi::testing
