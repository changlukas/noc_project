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
    //   wr.addr is the ORIGINAL user txn.addr, which may be mid-window on an
    //   unaligned-start beat 0; per-beat address is derived from the ALIGNED
    //   base (IHI 0022 A3.4.1), matching what the master anchors its lane math
    //   to and what a real slave decodes from AWADDR. Each beat covers
    //   bpb=1<<size bytes; the packed user-byte buffer 'data' supplies bpb
    //   bytes per beat starting at offset beat*bpb. strb_per_beat is
    //   beat-relative (bit j = the beat's local j-th byte, matching
    //   data[beat*bpb + j]); the scoreboard checks bit j directly and records
    //   data[beat*bpb + j] at memory address beat_addr + j when set.
    void handle_write_completed(const WriteResult& wr, const std::vector<uint8_t>& data,
                                const std::vector<uint64_t>& strb_per_beat) {
        // Skip memory-error completions (slave never reached memory).
        if (wr.resp == Resp::DECERR || wr.resp == Resp::SLVERR) return;
        // A failed exclusive write (AxLOCK=Exclusive that did not earn
        // EXOKAY at the slave) returns OKAY without committing to memory per
        // IHI 0022 §A7.2.3. Skip the expected_ update so the next read still
        // observes the pre-attempt value. A successful exclusive (EXOKAY) and a
        // normal write (OKAY) both fall through to commit.
        if (wr.lock == LockType::Exclusive && wr.resp == Resp::OKAY) return;
        const std::size_t bpb = 1ull << wr.size;
        const std::size_t beat_count = static_cast<std::size_t>(wr.len) + 1u;
        assert(data.size() >= beat_count * bpb &&
               "Scoreboard: data buffer too short for lane-positioned coverage");
        assert(strb_per_beat.size() == beat_count && "Scoreboard: strb_per_beat count mismatch");
        // AXI4 unaligned start (A3.4.1): the master's lane math -- and what a real
        // slave decodes from AWADDR -- anchors to this ALIGNED base, not wr.addr.
        const uint64_t aligned_addr = wr.addr & ~((1ull << wr.size) - 1);
        for (std::size_t beat = 0; beat < beat_count; ++beat) {
            // Per-beat address via the shared axi::beat_addr() helper. FIXED keeps
            // aligned_addr (last-beat-wins on the same address); INCR advances by
            // bpb; WRAP wraps within the wrap window. The fully-qualified call
            // avoids shadowing with the local 'beat' loop variable.
            const uint64_t beat_addr_v =
                axi::beat_addr(aligned_addr, wr.len, wr.size, wr.burst, beat);
            const std::size_t byte_lane = static_cast<std::size_t>(beat_addr_v & (DATA_BYTES - 1));
            const uint64_t strb = strb_per_beat[beat];
            // Cap the byte loop at the bus lane room to avoid shifting uint32_t by
            // >=32 (C++ UB). Mirrors the lane_room/copy_bytes pattern used in
            // Memory::perform_read_, AxiMaster W push, and AxiMaster R accumulator.
            const std::size_t lane_room = (byte_lane < DATA_BYTES) ? (DATA_BYTES - byte_lane) : 0;
            const std::size_t j_max = std::min(bpb, lane_room);
            // Mirror axi_master.hpp's W-loop first-beat WSTRB mask (IHI 0022
            // A3.4.1): strb_per_beat is the RAW, un-masked per-beat token (the
            // wire-level prefix mask lives only in the W-loop's WSTRB shift, not
            // in what WriteResult carries), so the scoreboard must re-derive which
            // prefix bytes the slave never actually wrote. Only the operation's
            // very first beat can start mid-window -- a later beat (including a
            // second sub-burst after a 4KB-cross split, which always re-aligns)
            // never is -- so gate on beat index, not a numeric address compare
            // (same WRAP-wraparound pitfall as the write loop if compared
            // numerically instead).
            const uint64_t prefix = (beat == 0) ? (wr.addr - aligned_addr) : 0;
            for (std::size_t j = 0; j < j_max; ++j) {
                if (j < prefix) continue;  // slave never wrote this byte -- not committed
                if ((strb >> j) & 0x1u) {
                    expected_[beat_addr_v + j] = data[beat * bpb + j];
                }
            }
        }
    }
    // Read verification: rr.data is the packed user-byte buffer the master
    // accumulated (bpb per beat, beat_count total). We re-derive per-beat addr
    // from the ALIGNED rr.addr/size/burst -- mirrors handle_write_completed
    // (IHI 0022 A3.4.1: a real slave anchors its lane window to the aligned
    // address it decodes from ARADDR, not the raw rr.addr) -- and compare
    // against expected_, which handle_write_completed populated on the same
    // aligned basis.
    void handle_read_observed(const ReadResult& rr) {
        if (rr.resp != Resp::OKAY) return;
        const std::size_t bpb = 1ull << rr.size;
        const std::size_t beat_count = static_cast<std::size_t>(rr.len) + 1u;
        const uint64_t aligned_addr = rr.addr & ~((1ull << rr.size) - 1);
        for (std::size_t beat = 0; beat < beat_count; ++beat) {
            const uint64_t beat_addr_v =
                axi::beat_addr(aligned_addr, rr.len, rr.size, rr.burst, beat);
            for (std::size_t j = 0; j < bpb; ++j) {
                const std::size_t idx = beat * bpb + j;
                if (idx >= rr.data.size()) break;
                const uint64_t a = beat_addr_v + j;
                auto it = expected_.find(a);
                const uint8_t exp = (it == expected_.end()) ? 0x00 : it->second;
                if (exp != rr.data[idx]) {
                    ++mismatches_;
                    std::ostringstream oss;
                    oss << "[Scoreboard] MISMATCH at addr=0x" << std::hex << a << " (scenario line "
                        << std::dec << rr.scenario_line << "): "
                        << "expected=0x" << std::hex << +exp << " actual=0x" << +rr.data[idx];
                    log_.push_back(oss.str());
                }
            }
        }
        ++reads_checked_;
    }
    std::size_t mismatch_count() const { return mismatches_; }
    std::size_t reads_checked() const { return reads_checked_; }
    const std::vector<std::string>& mismatch_report() const { return log_; }

  private:
    std::map<uint64_t, uint8_t> expected_;
    std::size_t mismatches_ = 0;
    std::size_t reads_checked_ = 0;
    std::vector<std::string> log_;
};

}  // namespace ni::cmodel::axi
