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
// Per-cycle tick order: reverse-order staged pipeline (spec §5.3) — later
// stages drain before earlier stages fill, so a beat advances one stage/tick:
//   wormhole_arbiter_.tick(); vc_arbiter_.tick();  // rsp S3 (-> NoC)
//   packetize_.tick();                             // rsp S2
//   axi_master_port_.tick();                       // rsp S1 + req S2
//   depacketize_.tick();                           // req S1
// See the per-stage commentary in Nsu::tick() below.
//
// Lifetime: Nsu deletes move/copy. Member order respects ctor ref deps.
//
// AXI binding: axi_master_port() getter. Testbench wires its
// AxiSlave-side adapters through this getter.
//
// References:
//   docs/superpowers/specs/2026-06-04-nmu-nsu-top-level-design.md
#include "ni_flit_constants.h"
#include "ni/ni_stage.hpp"
#include "router/req_in.hpp"
#include "router/rsp_out.hpp"
#include "ni/wormhole_arbiter.hpp"
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
    Nsu(NsuConfig cfg, router::NocReqIn& upstream_req, router::NocRspOut& downstream_rsp);

    Nsu(const Nsu&) = delete;
    Nsu(Nsu&&) = delete;
    Nsu& operator=(const Nsu&) = delete;
    Nsu& operator=(Nsu&&) = delete;

    AxiMasterPort& axi_master_port() noexcept { return axi_master_port_; }

    void tick();

    std::size_t stage_occupancy(NiPath path, std::size_t stage, uint8_t axi_ch) const {
        if (path == NiPath::NsuReq) {
            // NsuReq: 2 stages
            //   S0 = Depacketize S1 stage registers
            //   S1 = AxiMasterPort per-channel queues (drain side)
            if (stage == 0) return depacketize_.s1_occupancy(axi_ch);
            if (stage == 1) {
                if (axi_ch == ni::AXI_CH_AW) return axi_master_port_.aw_q_size();
                if (axi_ch == ni::AXI_CH_W) return axi_master_port_.w_q_size();
                if (axi_ch == ni::AXI_CH_AR) return axi_master_port_.ar_q_size();
            }
        }
        if (path == NiPath::NsuRsp) {
            // NsuRsp: 3 stages
            //   S0 = Packetize S1 stage registers (accepted B/R beat)
            //   S1 = WormholeArbiter pending queue (S2→S3 boundary)
            //   S2 = VcArbiter pending queue (toward NoC)
            if (stage == 0) {
                if (axi_ch == ni::AXI_CH_B) return packetize_.s1_b_occupancy();
                if (axi_ch == ni::AXI_CH_R) return packetize_.s1_r_occupancy();
            }
            if (stage == 1) {
                // WormholeArbiter inputs: 0=B, 1=R
                if (axi_ch == ni::AXI_CH_B) return wormhole_arbiter_.pending_size(0);
                if (axi_ch == ni::AXI_CH_R) return wormhole_arbiter_.pending_size(1);
            }
            if (stage == 2) {
                std::size_t total = 0;
                for (std::size_t v = 0; v < VcArbiter::NUM_VC_MAX; ++v)
                    total += vc_arbiter_.pending_size(static_cast<uint8_t>(v));
                return total;
            }
        }
        return 0;
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
    router::NocReqIn& upstream_req_;
    router::NocRspOut& downstream_rsp_;
    VcArbiter vc_arbiter_;
    router::WormholeArbiter<router::NocRspOut> wormhole_arbiter_;
    MetaBuffer meta_buffer_;
    Packetize packetize_;
    Depacketize depacketize_;
    AxiMasterPort axi_master_port_;
};

namespace detail {

inline VcArbiter make_vc_arbiter(const NsuConfig& cfg, router::NocRspOut& downstream) {
    if (cfg.vc_mode == VcMode::ReadWriteSplit) {
        return VcArbiter::read_write_split(downstream, cfg.num_vc, cfg.write_rsp_vc,
                                           cfg.read_rsp_vc, cfg.vc_arbiter_pending_depth);
    }
    auto candidates = cfg.vc_candidates;
    return VcArbiter::multi_candidate(downstream, cfg.num_vc, std::move(candidates),
                                      cfg.vc_arbiter_pending_depth);
}

}  // namespace detail

inline Nsu::Nsu(NsuConfig cfg, router::NocReqIn& upstream_req, router::NocRspOut& downstream_rsp)
    : cfg_(std::move(cfg)),
      upstream_req_(upstream_req),
      downstream_rsp_(downstream_rsp),
      vc_arbiter_(detail::make_vc_arbiter(cfg_, downstream_rsp_)),
      wormhole_arbiter_(vc_arbiter_, /*num_inputs=*/2, std::vector<router::ChannelPairing>{},
                        cfg_.wormhole_per_input_depth),
      meta_buffer_(cfg_.port_params.meta_buffer_per_id_depth),
      packetize_(wormhole_arbiter_.input(0), wormhole_arbiter_.input(1), meta_buffer_, cfg_.src_id),
      depacketize_(upstream_req_, meta_buffer_, cfg_.port_params.depkt_aw_q_depth,
                   cfg_.port_params.depkt_w_q_depth, cfg_.port_params.depkt_ar_q_depth),
      axi_master_port_(depacketize_, packetize_, cfg_.port_params) {}

inline void Nsu::tick() {
    // Reverse-order tick for both req and rsp paths (spec §5.3).
    // A beat advances exactly one stage per tick; later stages drain before
    // earlier stages fill, so no double-advance occurs.
    //
    // RSP path (S3 → S2 → S1, reverse order):
    //   S3: wormhole_arbiter_ drains from its pending_ (= S2→S3 register) to
    //       vc_arbiter_, which drains to NoC. Draining before Packetize fills
    //       ensures the slot is free for this tick's S2 output.
    //   S2: packetize_.tick() reads s1_b_/s1_r_ stage registers, builds
    //       Flits, pushes to wormhole input (the S2→S3 register boundary).
    //       Because wormhole_.tick() already ran, the flits pushed here
    //       cannot escape to NoC until the next tick — the arbiter-final-stage
    //       property (spec §5.2: no same-tick Packetize→NoC escape).
    //   S1: axi_master_port_ forward_b/r_to_packetizer_() takes ≤1 beat from
    //       b_q_/r_q_ and calls packetize_.push_b/r() (writes s1_b_/s1_r_).
    //       Packetize_.tick() already drained s1 this tick, so no overwrite.
    //
    // REQ path (S2 → S1, reverse order, unchanged from Task 3):
    //   S2: axi_master_port_ drain_*_from_depacketizer_ consumes from
    //       depacketize_.s1_* stage registers.
    //   S1: depacketize_.tick() decodes a new flit into s1_* registers.
    wormhole_arbiter_.tick();  // RSP S3a: drain S2→S3 boundary to VcArbiter
    vc_arbiter_.tick();        // RSP S3b: drain VcArbiter pending to NoC
    packetize_.tick();         // RSP S2: read S1 regs, push to S2→S3 boundary
    axi_master_port_.tick();   // RSP S1 + REQ S2: bounded B/R accept + req drain
    depacketize_.tick();       // REQ S1: decode flit into S1 stage registers
}

}  // namespace ni::cmodel::nsu
