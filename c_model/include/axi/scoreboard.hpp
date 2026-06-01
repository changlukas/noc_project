// (Scoreboard pattern is independent; see ATTRIBUTION.md)
#pragma once
#include "axi/axi_master.hpp"
#include "axi/types.hpp"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace ni::cmodel::axi {

class Scoreboard {
public:
  // Lane-positioned bus semantics (AXI4):
  //   wr.addr is the ORIGINAL user txn.addr. Each beat covers bpb=1<<size
  //   bytes; the per-beat byte_lane = beat_addr mod DATA_BYTES anchors the
  //   user payload on the bus. The packed user-byte buffer 'data' supplies
  //   bpb bytes per beat starting at offset beat*bpb. WSTRB on the wire is
  //   lane-positioned; the scoreboard checks strb bits at lanes
  //   (byte_lane + j) for j in [0, bpb) and records data[beat*bpb + j] at
  //   memory address beat_addr + j when the corresponding strb bit is set.
  void handle_write_completed(const WriteResult& wr,
                              const std::vector<uint8_t>& data,
                              const std::vector<uint32_t>& strb_per_beat) {
    // Skip memory-error completions (slave never reached memory).
    if (wr.resp == Resp::DECERR || wr.resp == Resp::SLVERR) return;
    // Phase C: a failed exclusive write (AxLOCK=Exclusive that did not earn
    // EXOKAY at the slave) returns OKAY without committing to memory per
    // IHI 0022 §A7.2.3. Skip the expected_ update so the next read still
    // observes the pre-attempt value. A successful exclusive (EXOKAY) and a
    // normal write (OKAY) both fall through to commit.
    if (wr.lock == LockType::Exclusive && wr.resp == Resp::OKAY) return;
    const std::size_t bpb = 1ull << wr.size;
    const std::size_t beat_count = static_cast<std::size_t>(wr.len) + 1u;
    assert(data.size() >= beat_count * bpb &&
           "Scoreboard: data buffer too short for lane-positioned coverage");
    assert(strb_per_beat.size() == beat_count &&
           "Scoreboard: strb_per_beat count mismatch");
    for (std::size_t beat = 0; beat < beat_count; ++beat) {
      // Per-beat address via the shared axi::beat_addr() helper. FIXED keeps
      // wr.addr (last-beat-wins on the same address); INCR advances by bpb;
      // WRAP wraps within the wrap window. The fully-qualified call avoids
      // shadowing with the local 'beat' loop variable.
      const uint64_t beat_addr_v =
          axi::beat_addr(wr.addr, wr.len, wr.size, wr.burst, beat);
      const std::size_t byte_lane =
          static_cast<std::size_t>(beat_addr_v & (DATA_BYTES - 1));
      const uint32_t strb = strb_per_beat[beat];
      // Cap the byte loop at the bus lane room to avoid shifting uint32_t by
      // >=32 (C++ UB). Mirrors the lane_room/copy_bytes pattern used in
      // Memory::perform_read_, AxiMaster W push, and AxiMaster R accumulator.
      const std::size_t lane_room =
          (byte_lane < DATA_BYTES) ? (DATA_BYTES - byte_lane) : 0;
      const std::size_t j_max = std::min(bpb, lane_room);
      for (std::size_t j = 0; j < j_max; ++j) {
        if ((strb >> (byte_lane + j)) & 0x1u) {
          expected_[beat_addr_v + j] = data[beat * bpb + j];
        }
      }
    }
  }
  // Read verification: rr.data is the packed user-byte buffer the master
  // accumulated (bpb per beat, beat_count total). We re-derive per-beat addr
  // from rr.addr/size/burst and compare against expected_.
  void handle_read_observed(const ReadResult& rr) {
    if (rr.resp != Resp::OKAY) return;
    const std::size_t bpb = 1ull << rr.size;
    const std::size_t beat_count = static_cast<std::size_t>(rr.len) + 1u;
    for (std::size_t beat = 0; beat < beat_count; ++beat) {
      const uint64_t beat_addr_v =
          axi::beat_addr(rr.addr, rr.len, rr.size, rr.burst, beat);
      for (std::size_t j = 0; j < bpb; ++j) {
        const std::size_t idx = beat * bpb + j;
        if (idx >= rr.data.size()) break;
        const uint64_t a = beat_addr_v + j;
        auto it = expected_.find(a);
        const uint8_t exp = (it == expected_.end()) ? 0x00 : it->second;
        if (exp != rr.data[idx]) {
          ++mismatches_;
          std::ostringstream oss;
          oss << "[Scoreboard] MISMATCH at addr=0x" << std::hex << a
              << " (scenario line " << std::dec << rr.scenario_line << "): "
              << "expected=0x" << std::hex << +exp
              << " actual=0x" << +rr.data[idx];
          log_.push_back(oss.str());
        }
      }
    }
    ++reads_checked_;
  }
  std::size_t mismatch_count() const { return mismatches_; }
  std::size_t reads_checked()  const { return reads_checked_; }
  const std::vector<std::string>& mismatch_report() const { return log_; }

private:
  std::map<uint64_t, uint8_t> expected_;
  std::size_t mismatches_ = 0;
  std::size_t reads_checked_ = 0;
  std::vector<std::string> log_;
};

}
