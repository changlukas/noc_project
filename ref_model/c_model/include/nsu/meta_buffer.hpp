#pragma once
#include "axi/types.hpp"
#include "ni_params.h"  // NOC_ID_WIDTH
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace ni::cmodel::nsu {

// Request class, read off the request flit's axi_ch at AW/AR allocate time and
// carried in MetaEntry so the matching B/R response is stamped in the same
// class (NarrowB/DataB, NarrowR/DataR). No SAM involvement on the NSU side --
// the class arrives already resolved in the flit header. Alias of axi::AxiClass
// (nmu::addr_trans::SamEntry's class): one enum, shared by both NI halves.
using AxiClass = axi::AxiClass;

struct MetaEntry {
    uint8_t src_id;       // requesting tile; becomes the response flit dst_id
    uint8_t upstream_id;  // master's original AXI id; restored into bid / rid
    uint8_t ordering_req;
    uint8_t ordering_tag;
    AxiClass cls;  // request's class; the response echoes it
    // AR basis for the read side's narrow lane re-anchor (nsu::Packetize::
    // build_r_flit). Unused by write entries -- B carries no data.
    uint64_t local_addr = 0;
    uint8_t len = 0;
    uint8_t size = 0;
    axi::Burst burst = axi::Burst::INCR;
    // Collective identity of the AW, echoed unmodified onto its B (design
    // §3.1; floo_axi_chimney.sv:557 mcast_mask[AxiB], :621-622). The RSP
    // router's join derives the B's expected-input set from these two fields
    // plus dst_id, so a wrong echo lands the B at a router that is not
    // expecting it. Write entries only -- AR carries no collective surface
    // (ARUSER is 8 b, spec :324), so read entries leave them at UNICAST / 0.
    // Appended, not inserted: Depacketize::pop_ar initializes this struct
    // positionally (depacketize.hpp:340-353) and C++17 has no designated
    // initializers to catch a reordering. Keep new fields at the end.
    uint8_t collective_op = axi::COLLECTIVE_OP_UNICAST;
    uint8_t collective_mask = 0;
    // Which endpoint at src_id issued this. Echoed back as the response's
    // dst_port_id so it reaches the requester and not the tile beside it.
    uint8_t src_port = 0;
};

// Downstream AXI ID presented to the slave, from the master's upstream ID.
// Ported from FlooNoC floo_meta_buffer.sv:89-91 (collapse to '1) and :138-139
// (offset by MaxAtomicTxns, which is 0 here because AtopSupport is off).
//
// '1 is all-ones of the fixed NoC ID width the NSU drives. The endpoint remap
// owns the independently sized external AXI ID and does not reach this port.
//
// The remap is a function of upstream_id ALONE, matching the ported source
// above. Ordering no longer depends on this choice: the response-path fixed
// VC map keys on (dst_id ^ id), so same-id streams from different sources
// land on distinct keys instead of contending.
inline uint8_t remap_downstream_id(uint8_t upstream_id, std::size_t max_unique_ids) {
    constexpr uint8_t collapsed = static_cast<uint8_t>((1u << ni::NOC_ID_WIDTH) - 1u);
    return max_unique_ids == 1 ? collapsed : upstream_id;
}

// Per-downstream-AXI-ID FIFO of {src_id, upstream_id, ordering_req, ordering_tag} entries,
// allocated at AW/AR egress toward the slave and looked up at B/R ingress
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
        read_beat_counter_[rid] = 0;  // next queued entry (if any) starts at beat 0
    }

    // Per-downstream-id running beat index within the front read_[rid] entry's
    // burst. Feeds nsu::Packetize::build_r_flit's narrow lane re-anchor
    // (axi::beat_addr needs the beat index, not just the AR base address).
    // Resets to 0 on commit_read, when the next queued burst becomes front.
    uint16_t read_beat_index(uint8_t rid) const noexcept { return read_beat_counter_[rid]; }
    void advance_read_beat(uint8_t rid) noexcept { ++read_beat_counter_[rid]; }

  private:
    // One bucket per AXI id (NOC_ID_SPACE); occupancy is bounded by the shared
    // counters, not by the bucket count. Static footprint is a modelling
    // artifact, not an RTL cost.
    std::array<std::deque<MetaEntry>, axi::NOC_ID_SPACE> write_;  // per downstream awid
    std::array<std::deque<MetaEntry>, axi::NOC_ID_SPACE> read_;   // per downstream arid
    std::array<uint16_t, axi::NOC_ID_SPACE> read_beat_counter_{};
    std::size_t max_outstanding_;
    std::size_t write_count_ = 0;
    std::size_t read_count_ = 0;
};

}  // namespace ni::cmodel::nsu
