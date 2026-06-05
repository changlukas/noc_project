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
//   docs/noc_cmodel_rtl_plan.md section 3 row 173
#include "ni/port_params.hpp"
#include "noc/noc_req_in.hpp"
#include "noc/noc_rsp_out.hpp"
#include "noc/wormhole_arbiter.hpp"
#include "nsu/axi_master_port.hpp"
#include "nsu/depacketize.hpp"
#include "nsu/meta_buffer.hpp"
#include "nsu/packetize.hpp"
#include "nsu/vc_arbiter.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ni::cmodel::nsu {

struct NsuConfig {
    uint8_t src_id = 0;
    PortParams port_params{};
    std::size_t meta_buffer_per_id_depth = 16;
    std::size_t depkt_aw_q_depth = 16;
    std::size_t depkt_w_q_depth = 16;
    std::size_t depkt_ar_q_depth = 16;
    std::size_t num_vc = 1;
    VcMode vc_mode = VcMode::ReadWriteSplit;
    uint8_t write_rsp_vc = 0;  // B -> write_rsp_vc
    uint8_t read_rsp_vc = 0;   // R -> read_rsp_vc
    std::array<std::vector<uint8_t>, 5> vc_candidates{};
    std::size_t wormhole_per_input_depth = 4;
    std::size_t vc_arbiter_pending_depth = 4;
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
      meta_buffer_(cfg_.meta_buffer_per_id_depth),
      packetize_(wormhole_arbiter_.input(0), wormhole_arbiter_.input(1), meta_buffer_, cfg_.src_id),
      depacketize_(upstream_req_, meta_buffer_, cfg_.depkt_aw_q_depth, cfg_.depkt_w_q_depth,
                   cfg_.depkt_ar_q_depth),
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
// NocReqIn& / NocRspOut&. Null stubs own both interfaces; real DPI wiring
// replaces them at the ShellAdapter tick boundary. Null stubs: pop_flit
// returns nullopt, push_flit returns false (backpressure until DPI wired).
//
// Invariant: NsuStandalone is non-copyable and non-movable (same as Nsu).
// -------------------------------------------------------------------------

namespace detail {

struct NullNocReqIn : noc::NocReqIn {
    std::optional<Flit> pop_flit() override { return std::nullopt; }
};

struct NullNocRspOut : noc::NocRspOut {
    bool push_flit(const Flit&) override { return false; }
    bool credit_avail(uint8_t) const override { return false; }
};

}  // namespace detail

class NsuStandalone {
  public:
    explicit NsuStandalone(NsuConfig cfg)
        : null_req_in_(), null_rsp_out_(), nsu_(std::move(cfg), null_req_in_, null_rsp_out_) {}

    NsuStandalone(const NsuStandalone&) = delete;
    NsuStandalone(NsuStandalone&&) = delete;
    NsuStandalone& operator=(const NsuStandalone&) = delete;
    NsuStandalone& operator=(NsuStandalone&&) = delete;

    AxiMasterPort& axi_master_port() noexcept { return nsu_.axi_master_port(); }
    void tick() { nsu_.tick(); }
    Nsu& nsu() noexcept { return nsu_; }

  private:
    detail::NullNocReqIn null_req_in_;
    detail::NullNocRspOut null_rsp_out_;
    Nsu nsu_;
};

}  // namespace ni::cmodel::nsu
