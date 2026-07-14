#pragma once
#include "axi/types.hpp"
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace ni::cmodel::nsu {

struct MetaEntry {
    uint8_t src_id;       // requesting tile; becomes the response flit dst_id
    uint8_t upstream_id;  // manager's original AXI id; restored into bid / rid
    uint8_t rob_req;
    uint8_t rob_idx;
};

// Downstream AXI ID presented to the subordinate, from the manager's upstream ID.
// Ported from FlooNoC floo_meta_buffer.sv:89-91 (collapse to '1) and :138-139
// (offset by MaxAtomicTxns, which is 0 here because AtopSupport is off).
//
// The remap is a function of upstream_id ALONE, matching the ported source
// above. Ordering no longer depends on this choice: the response-path fixed
// VC map keys on (dst_id ^ id), so same-id streams from different sources
// land on distinct keys instead of contending.
inline uint8_t remap_downstream_id(uint8_t upstream_id, std::size_t max_unique_ids) {
    return max_unique_ids == 1 ? static_cast<uint8_t>(axi::AXI_ID_SPACE - 1) : upstream_id;
}

// Per-downstream-AXI-ID FIFO of {src_id, upstream_id, rob_req, rob_idx} entries,
// allocated at AW/AR egress toward the subordinate and looked up at B/R ingress
// via a peek+commit pattern.
//
// Capacity is a SHARED pool of max_outstanding entries per direction, not a
// per-ID depth. This mirrors FlooNoC's MaxTxns (floo_meta_buffer.sv:94,112,
// 148,173), whose doc calls it "the number of both incoming and outgoing
// transactions that can be handled by the network interface". A full pool
// reports through write_full() / read_full(); the caller backpressures.
//
// AXI4 ordering: per-ID transactions complete in issue order, so each bucket's
// front is the oldest outstanding for that downstream ID. Different IDs are
// independent.
//
// Atomic-ID tagging is OUT OF SCOPE (AtopSupport = 0).
class MetaBuffer {
  public:
    explicit MetaBuffer(std::size_t max_outstanding) : max_outstanding_(max_outstanding) {
        assert(max_outstanding > 0 && "MetaBuffer: max_outstanding must be positive");
    }

    // -- Write side (AW allocate + B consume) --
    bool write_full() const noexcept { return write_count_ >= max_outstanding_; }
    void allocate_write(uint8_t downstream_id, MetaEntry e) {
        assert(!write_full() && "MetaBuffer: allocate_write on a full pool; check write_full()");
        write_[downstream_id].push_back(e);
        ++write_count_;
    }
    std::optional<MetaEntry> peek_write(uint8_t bid) const noexcept {
        if (write_[bid].empty()) return std::nullopt;
        return write_[bid].front();
    }
    void commit_write(uint8_t bid) {
        assert(!write_[bid].empty() && "commit_write on empty queue");
        write_[bid].pop_front();
        --write_count_;
    }

    // -- Read side (AR allocate + R consume) --
    // Multi-beat R burst: peek every beat, commit only on rlast.
    bool read_full() const noexcept { return read_count_ >= max_outstanding_; }
    void allocate_read(uint8_t downstream_id, MetaEntry e) {
        assert(!read_full() && "MetaBuffer: allocate_read on a full pool; check read_full()");
        read_[downstream_id].push_back(e);
        ++read_count_;
    }
    std::optional<MetaEntry> peek_read(uint8_t rid) const noexcept {
        if (read_[rid].empty()) return std::nullopt;
        return read_[rid].front();
    }
    void commit_read(uint8_t rid) {
        assert(!read_[rid].empty() && "commit_read on empty queue");
        read_[rid].pop_front();
        --read_count_;
    }

  private:
    // 256 buckets sized by AXI_ID_SPACE; occupancy is bounded by the shared
    // counters, not by the bucket count. Static footprint is a modelling
    // artifact, not an RTL cost.
    std::array<std::deque<MetaEntry>, axi::AXI_ID_SPACE> write_;  // per downstream awid
    std::array<std::deque<MetaEntry>, axi::AXI_ID_SPACE> read_;   // per downstream arid
    std::size_t max_outstanding_;
    std::size_t write_count_ = 0;
    std::size_t read_count_ = 0;
};

}  // namespace ni::cmodel::nsu
