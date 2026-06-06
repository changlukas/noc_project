#pragma once
#include "axi/types.hpp"
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>

namespace ni::cmodel::nsu {

struct MetaEntry {
    uint8_t src_id;
    uint8_t rob_req;
    uint8_t rob_idx;
};

// Per-AXI-ID FIFO of {src_id, rob_req, rob_idx} snapshots captured at AW/AR
// flit ingress. Looked up at B/R flit egress via peek+commit pattern.
//
// AXI4 ordering: per-ID transactions complete in issue order. Each FIFO front
// is the oldest outstanding for that ID. Different IDs are independent.
//
// Atomic-ID tagging is OUT OF SCOPE — a per-ID FIFO suffices for tested fixtures.
class MetaBuffer {
  public:
    explicit MetaBuffer(std::size_t per_id_depth) : per_id_depth_(per_id_depth) {
        assert(per_id_depth > 0 && "MetaBuffer: per_id_depth must be positive");
    }

    // -- Write side (AW snapshot + B consume) --
    void snapshot_write(uint8_t awid, MetaEntry e) {
        if (!(write_[awid].size() < per_id_depth_)) {
            assert(false && "MetaBuffer: per-ID depth exceeded");
            std::abort();  // belt-and-braces for NDEBUG
        }
        write_[awid].push_back(e);
    }
    std::optional<MetaEntry> peek_write(uint8_t bid) const noexcept {
        if (write_[bid].empty()) return std::nullopt;
        return write_[bid].front();
    }
    void commit_write(uint8_t bid) {
        assert(!write_[bid].empty() && "commit_write on empty queue");
        write_[bid].pop_front();
    }

    // -- Read side (AR snapshot + R consume) --
    // Multi-beat R burst: peek every beat, commit only on rlast.
    void snapshot_read(uint8_t arid, MetaEntry e) {
        if (!(read_[arid].size() < per_id_depth_)) {
            assert(false && "MetaBuffer: per-ID depth exceeded");
            std::abort();  // belt-and-braces for NDEBUG
        }
        read_[arid].push_back(e);
    }
    std::optional<MetaEntry> peek_read(uint8_t rid) const noexcept {
        if (read_[rid].empty()) return std::nullopt;
        return read_[rid].front();
    }
    void commit_read(uint8_t rid) {
        assert(!read_[rid].empty() && "commit_read on empty queue");
        read_[rid].pop_front();
    }

  private:
    // ~40KB per MetaBuffer (AXI_ID_SPACE=256 empty deques x ~80B each); assumes sparse-id usage
    std::array<std::deque<MetaEntry>, axi::AXI_ID_SPACE> write_;  // per awid
    std::array<std::deque<MetaEntry>, axi::AXI_ID_SPACE> read_;   // per arid
    std::size_t per_id_depth_;
};

}  // namespace ni::cmodel::nsu
