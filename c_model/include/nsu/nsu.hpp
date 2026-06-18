#pragma once
// NSU top-level assembly. Encapsulates Stage 3 NI response-side
// sub-modules into one class with single tick() entrypoint. Mirror of
// nmu::Nmu but asymmetric: NSU has no Rob (no reorder buffer on response
// side) and no addr_trans (uses incoming flit dst_id directly).
//
// Pipeline (req in, AXI out):
//   external NocReqIn ──> Depacketize (snapshots meta to MetaBuffer)
//     ──> AxiMasterPort ──> external AXI slave
//
// Pipeline (rsp from AXI slave, NoC out):
//   external B/R from AXI slave ──> AxiMasterPort ──> Packetize{b,r}
//     (reads meta from MetaBuffer) ──> WormholeArbiter<NocRspOut>(2 in,
//     no pairing) ──> VcArbiter ──> external NocRspOut
//
// Per-cycle tick order (upstream-first):
//   depacketize_.tick(); axi_master_port_.tick();
//   wormhole_arbiter_.tick(); vc_arbiter_.tick();
//
// Lifetime: Nsu deletes move/copy. Member order respects ctor ref deps.
//
// AXI binding: axi_master_port() getter. Testbench wires its
// AxiSlave-side adapters through this getter.
//
// References:
//   docs/superpowers/specs/2026-06-04-nmu-nsu-top-level-design.md
#include "nmu/ni_stage.hpp"
#include "noc/noc_req_in.hpp"
#include "noc/noc_rsp_out.hpp"
#include "noc/wormhole_arbiter.hpp"
#include "nsu/axi_master_port.hpp"
#include "nsu/depacketize.hpp"
#include "nsu/meta_buffer.hpp"
#include "nsu/packetize.hpp"
#include "nsu/vc_arbiter.hpp"
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace ni::cmodel::nsu {

struct NsuConfig {
    uint8_t src_id = 0;
    nsu::PortParams port_params{};
    std::size_t num_vc = 1;
    VcMode vc_mode = VcMode::ReadWriteSplit;
    uint8_t write_rsp_vc = 0;  // B -> write_rsp_vc
    uint8_t read_rsp_vc = 0;   // R -> read_rsp_vc
    std::array<std::vector<uint8_t>, 5> vc_candidates{};
    std::size_t wormhole_per_input_depth = 4;
    std::size_t vc_arbiter_pending_depth = 4;
    std::size_t ni_req_extra_depth = 0;  // extra shift stages on the request path
    std::size_t ni_rsp_extra_depth = 0;  // extra shift stages on the response path
};

class Nsu {
  public:
    Nsu(NsuConfig cfg, noc::NocReqIn& upstream_req, noc::NocRspOut& downstream_rsp);

    Nsu(const Nsu&) = delete;
    Nsu(Nsu&&) = delete;
    Nsu& operator=(const Nsu&) = delete;
    Nsu& operator=(Nsu&&) = delete;

    AxiMasterPort& axi_master_port() noexcept { return axi_master_port_; }

    void tick();

    std::size_t stage_occupancy(NiPath path, std::size_t stage, uint8_t axi_ch) const {
        (void)path;
        (void)stage;
        (void)axi_ch;
        return 0;  // filled per-path in later tasks
    }

  private:
    // Declaration order:
    //   1. cfg_ + external refs.
    //   2. vc_arbiter_ wraps downstream_rsp_.
    //   3. wormhole_arbiter_ wraps vc_arbiter_.
    //   4. meta_buffer_ (no upstream dep).
    //   5. packetize_ takes wormhole_arbiter_.input(0/1) + meta_buffer_.
    //   6. depacketize_ takes upstream_req_ + meta_buffer_.
    //   7. axi_master_port_ takes depacketize_ + packetize_.
    NsuConfig cfg_;
    noc::NocReqIn& upstream_req_;
    noc::NocRspOut& downstream_rsp_;
    VcArbiter vc_arbiter_;
    noc::WormholeArbiter<noc::NocRspOut> wormhole_arbiter_;
    MetaBuffer meta_buffer_;
    Packetize packetize_;
    Depacketize depacketize_;
    AxiMasterPort axi_master_port_;
};

namespace detail {

inline VcArbiter make_vc_arbiter(const NsuConfig& cfg, noc::NocRspOut& downstream) {
    if (cfg.vc_mode == VcMode::ReadWriteSplit) {
        return VcArbiter::read_write_split(downstream, cfg.num_vc, cfg.write_rsp_vc,
                                           cfg.read_rsp_vc, cfg.vc_arbiter_pending_depth);
    }
    auto candidates = cfg.vc_candidates;
    return VcArbiter::multi_candidate(downstream, cfg.num_vc, std::move(candidates),
                                      cfg.vc_arbiter_pending_depth);
}

}  // namespace detail

inline Nsu::Nsu(NsuConfig cfg, noc::NocReqIn& upstream_req, noc::NocRspOut& downstream_rsp)
    : cfg_(std::move(cfg)),
      upstream_req_(upstream_req),
      downstream_rsp_(downstream_rsp),
      vc_arbiter_(detail::make_vc_arbiter(cfg_, downstream_rsp_)),
      wormhole_arbiter_(vc_arbiter_, /*num_inputs=*/2, std::vector<noc::ChannelPairing>{},
                        cfg_.wormhole_per_input_depth),
      meta_buffer_(cfg_.port_params.meta_buffer_per_id_depth),
      packetize_(wormhole_arbiter_.input(0), wormhole_arbiter_.input(1), meta_buffer_, cfg_.src_id),
      depacketize_(upstream_req_, meta_buffer_, cfg_.port_params.depkt_aw_q_depth,
                   cfg_.port_params.depkt_w_q_depth, cfg_.port_params.depkt_ar_q_depth),
      axi_master_port_(depacketize_, packetize_, cfg_.port_params) {}

inline void Nsu::tick() {
    depacketize_.tick();
    axi_master_port_.tick();
    wormhole_arbiter_.tick();
    vc_arbiter_.tick();
}

// -------------------------------------------------------------------------
// Stage 5b: NsuStandalone — hermetic wrapper, no external NoC refs.
//
// ShellAdapters construct NsuStandalone(NsuConfig{...}) without supplying
// NocReqIn& / NocRspOut&. The wrapper owns queue-backed terminal endpoints for
// both interfaces; real DPI wiring drives/drains them at the ShellAdapter tick
// boundary.
//
// NullNocReqIn: ShellAdapter injects flits via inject_req_flit() before
//   calling nsu_.tick(); Nsu's Depacketize stage drains via pop_flit().
//
// NullNocRspOut: push_flit enqueues into an internal deque (capped at
//   kMaxQueueDepth as a drain-forgotten sanity check). With FlooNoC credit
//   enabled (cosim opt-in) push_flit also gates+consumes per-VC credit and
//   credit_avail reflects the counter; with credit OFF (default) it accepts
//   unconditionally. ShellAdapter drains via pop_rsp_flit() each tick.
//
// Invariant: NsuStandalone is non-copyable and non-movable (same as Nsu).
// -------------------------------------------------------------------------

namespace detail {

struct NullNocReqIn : noc::NocReqIn {
    // ShellAdapter accessor: inject one flit per tick from DPI wire.
    void inject_req_flit(const Flit& f) { queue_.push_back(f); }

    // R2 consumer-pulse: size the per-VC pending counter before traffic. Always
    // present (no enable flag — pending only matters when the shell drains it via
    // take_credit, which is cosim-only). Defaults to 1 VC.
    void size_pending(uint8_t num_vc) { pending_.assign(num_vc, 0); }

    // Nsu's Depacketize stage drains via pop_flit() each tick. Depacketize may
    // call pop_flit() MULTIPLE times per tick (it drains in a while-loop), so
    // pending_ MUST accumulate (counter), not latch a single bit.
    std::optional<Flit> pop_flit() override {
        if (queue_.empty()) return std::nullopt;
        Flit f = queue_.front();
        queue_.pop_front();
        const auto vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
        ++pending_[vc];
        return f;
    }

    // ShellAdapter accessor: drain one consumer credit pulse per tick (mirror of
    // noc::LinkCreditOut::take). Returns true when a pulse was emitted.
    bool take_credit(uint8_t vc) {
        if (pending_[vc] == 0) return false;
        --pending_[vc];
        return true;
    }
    std::size_t pending(uint8_t vc) const { return pending_[vc]; }

  private:
    std::deque<Flit> queue_;
    // (count=1, value=0) vector ctor: one VC, pending pulse count 0. NOT a
    // 2-element {1, 0} initializer-list. size_pending() resizes for >1 VC.
    std::vector<std::size_t> pending_{1, 0};
};

struct NullNocRspOut : noc::NocRspOut {
    // Sanity cap: a real ShellAdapter drains every tick, so a queue this deep
    // means the test forgot to drain. Asserts in debug; release builds skip
    // the check (and the queue is still allowed to grow unboundedly).
    static constexpr std::size_t kMaxQueueDepth = 1024;

    // R2 opt-in FlooNoC sender credit (default OFF = today's always-available).
    // When enabled, this models the InjectAdapter credit pattern: a per-VC
    // counter seeded to the downstream (router LOCAL input) depth; push_flit
    // decrements on accept, receive_credit increments on a credit pulse.
    // INVARIANT: credit_[vc] is decremented ONLY in push_flit and incremented
    // ONLY in receive_credit, so credit_[vc] + outstanding == seed holds.
    void enable_credit(uint8_t num_vc, std::size_t seed) {
        credit_enabled_ = true;
        credit_.assign(num_vc, seed);
    }
    void receive_credit(uint8_t vc) { ++credit_[vc]; }

    // Accept a flit into the queue. When credit is disabled this models
    // infinite downstream bandwidth (always accept). When enabled it gates on
    // and consumes one per-VC credit, returning false (backpressure) if none.
    bool push_flit(const Flit& f) override {
        if (credit_enabled_) {
            const auto vc = static_cast<uint8_t>(f.get_header_field("vc_id"));
            if (credit_[vc] == 0) return false;
            --credit_[vc];
        }
        assert(queue_.size() < kMaxQueueDepth &&
               "NullNocRspOut overflow — did the test ShellAdapter forget to drain?");
        queue_.push_back(f);
        return true;
    }
    bool credit_avail(uint8_t vc) const override { return !credit_enabled_ || credit_[vc] > 0; }

    // ShellAdapter accessor: pop one flit per tick for DPI forwarding.
    std::optional<Flit> pop_rsp_flit() {
        if (queue_.empty()) return std::nullopt;
        Flit f = queue_.front();
        queue_.pop_front();
        return f;
    }

  private:
    std::deque<Flit> queue_;
    bool credit_enabled_ = false;
    std::vector<std::size_t> credit_;
};

}  // namespace detail

class NsuStandalone {
  public:
    explicit NsuStandalone(NsuConfig cfg)
        : num_vc_(static_cast<uint8_t>(cfg.num_vc)),
          null_req_in_(),
          null_rsp_out_(),
          nsu_(std::move(cfg), null_req_in_, null_rsp_out_) {
        null_req_in_.size_pending(num_vc_);
    }

    NsuStandalone(const NsuStandalone&) = delete;
    NsuStandalone(NsuStandalone&&) = delete;
    NsuStandalone& operator=(const NsuStandalone&) = delete;
    NsuStandalone& operator=(NsuStandalone&&) = delete;

    AxiMasterPort& axi_master_port() noexcept { return nsu_.axi_master_port(); }
    void tick() { nsu_.tick(); }
    std::size_t stage_occupancy(NiPath path, std::size_t stage, uint8_t axi_ch) const {
        return nsu_.stage_occupancy(path, stage, axi_ch);
    }
    Nsu& nsu() noexcept { return nsu_; }

    // Stage 5b ShellAdapter accessors — inject req side, drain rsp side.
    void inject_req_flit(const Flit& f) { null_req_in_.inject_req_flit(f); }
    std::optional<Flit> pop_rsp_flit() { return null_rsp_out_.pop_rsp_flit(); }
    bool rsp_credit_avail(uint8_t vc = 0) const { return null_rsp_out_.credit_avail(vc); }

    // R2 opt-in FlooNoC credit at the NoC terminal edge (cosim-only; default
    // OFF). Seeds the rsp-out sender counter; the req-in consumer pulse is
    // always active (sized in the ctor) but inert unless req_take_credit drains.
    void enable_noc_credit(std::size_t seed) { null_rsp_out_.enable_credit(num_vc_, seed); }
    void rsp_receive_credit(uint8_t vc = 0) { null_rsp_out_.receive_credit(vc); }
    bool req_take_credit(uint8_t vc = 0) { return null_req_in_.take_credit(vc); }

  private:
    uint8_t num_vc_;
    detail::NullNocReqIn null_req_in_;
    detail::NullNocRspOut null_rsp_out_;
    Nsu nsu_;
};

}  // namespace ni::cmodel::nsu
