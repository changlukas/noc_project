#pragma once
// NSU top-level assembly. Encapsulates the NI response-side
// sub-modules into one class with single tick() entrypoint. Mirror of
// nmu::Nmu but asymmetric: NSU has no Rob (no reorder buffer on response
// side) and no addr_trans (uses incoming flit dst_id directly).
//
// Pipeline (req in, AXI out; REQ network -- Depacketize's REQ ingress -- and
// DAT network -- Depacketize's second, DAT, ingress, S3a T4; NMU's Packetize
// steers DataAw/DataW here per T6. Both ingresses demux into the SAME
// s1_aw_/s1_w_/s1_ar_ registers -- see nsu::Depacketize's class comment):
//   external NocReqIn (REQ + DAT) ──> Depacketize (allocates meta in MetaBuffer)
//     ──> AxiMasterPort ──> external AXI slave
//
// Pipeline (rsp from AXI slave, NoC out; RSP network -- B/NarrowR):
//   external B/R from AXI slave ──> AxiMasterPort ──> Packetize{b,r}
//     (reads meta from MetaBuffer) ──> WormholeArbiter<NocRspOut>(2 in,
//     no pairing) ──> VcAllocator ──> external NocRspOut
//
// DAT egress face (S3a T4 arbiter + T6 steering -- DataR; single input, so
// no wormhole arbiter in front -- a 1-input wormhole arbiter is dead code,
// per stage design §5.2. Packetize steers Data-class R here; ctest may still
// push directly via dat_vc_allocator().push_flit(...) to exercise it in isolation):
//   Packetize{r} (Data class) ──> VcAllocator(dat_num_vc) ──> external NocRspOut (DPI bridge)
//
// Per-cycle tick order: reverse-order staged pipeline — later
// stages drain before earlier stages fill, so a beat advances one stage/tick:
//   wormhole_arbiter_.tick(); vc_allocator_.tick();  // rsp S3 (-> NoC)
//   dat_vc_allocator_.tick();                        // DAT egress drain (independent network)
//   packetize_.tick();                             // rsp S2
//   axi_master_port_.tick();                       // rsp S1 + req S2
//   depacketize_.tick();                           // req S1 (drains BOTH REQ and DAT ingresses)
// See the per-stage commentary in Nsu::tick() below. REQ/RSP/DAT are
// independent networks draining into disjoint sinks, so relative tick order
// across networks introduces no coupling (S3a stage design §5.4).
//
// Lifetime: Nsu deletes move/copy. Member order respects ctor ref deps.
//
// AXI binding: axi_master_port() getter. Testbench wires its
// AxiSlave-side adapters through this getter.
#include "ni_flit_constants.h"
#include "ni/ni_stage.hpp"
#include "router/req_in.hpp"
#include "router/rsp_out.hpp"
#include "ni/wormhole_arbiter.hpp"
#include "nsu/axi_master_port.hpp"
#include "nsu/depacketize.hpp"
#include "nsu/meta_buffer.hpp"
#include "nsu/packetize.hpp"
#include "nsu/vc_allocator.hpp"
#include "ni_params.h"
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
    // DAT face VC count (S3a T4; R only -- no B rides DAT, per network map §1).
    std::size_t dat_num_vc = ni::NOC_DAT_NUM_VC;
    std::size_t wormhole_per_input_depth = ni::NSU_ARBITER_FIFO_DEPTH;
    std::size_t vc_allocator_pending_depth = ni::NSU_ARBITER_FIFO_DEPTH;
};

class Nsu {
  public:
    // upstream_dat_req / downstream_dat_rsp: the DAT face (S3a T4). Every
    // assembly site wires these explicitly -- no default -- since Nsu is the
    // top-level product-facing class; see router/null_adapters.hpp for the
    // sentinel callers that don't yet exercise DAT traffic pass.
    Nsu(NsuConfig cfg, router::NocReqIn& upstream_req, router::NocRspOut& downstream_rsp,
        router::NocReqIn& upstream_dat_req, router::NocRspOut& downstream_dat_rsp);

    Nsu(const Nsu&) = delete;
    Nsu(Nsu&&) = delete;
    Nsu& operator=(const Nsu&) = delete;
    Nsu& operator=(Nsu&&) = delete;

    AxiMasterPort& axi_master_port() noexcept { return axi_master_port_; }

    // DAT egress face (S3a T4 + T6 steering). Non-const: Packetize feeds this
    // (Data-class R); ctest may still push R flits directly via
    // dat_vc_allocator().push_flit(...) to exercise the arbiter in isolation.
    VcAllocator& dat_vc_allocator() noexcept { return dat_vc_allocator_; }

    void tick();

    std::size_t stage_occupancy(NiPath path, std::size_t stage, uint8_t axi_ch) const {
        if (path == NiPath::NsuReq) {
            // NsuReq: 2 stages
            //   S0 = Depacketize S1 stage registers
            //   S1 = AxiMasterPort per-channel queues (drain side)
            if (stage == 0) return depacketize_.s1_occupancy(axi_ch);
            if (stage == 1) {
                if (axi_ch == ni::AXI_CH_NarrowAw || axi_ch == ni::AXI_CH_DataAw)
                    return axi_master_port_.aw_q_size();
                if (axi_ch == ni::AXI_CH_NarrowW || axi_ch == ni::AXI_CH_DataW)
                    return axi_master_port_.w_q_size();
                if (axi_ch == ni::AXI_CH_NarrowAr || axi_ch == ni::AXI_CH_DataAr)
                    return axi_master_port_.ar_q_size();
            }
        }
        if (path == NiPath::NsuRsp) {
            // NsuRsp: 3 stages
            //   S0 = Packetize S1 stage registers (accepted B/R beat)
            //   S1 = WormholeArbiter pending queue (S2→S3 boundary)
            //   S2 = VcAllocator pending queue (toward NoC)
            const bool is_b = (axi_ch == ni::AXI_CH_NarrowB || axi_ch == ni::AXI_CH_DataB);
            const bool is_r = (axi_ch == ni::AXI_CH_NarrowR || axi_ch == ni::AXI_CH_DataR);
            if (stage == 0) {
                if (is_b) return packetize_.s1_b_occupancy();
                if (is_r) return packetize_.s1_r_occupancy();
            }
            if (stage == 1) {
                // WormholeArbiter inputs: 0=B, 1=R
                if (is_b) return wormhole_arbiter_.pending_size(0);
                if (is_r) return wormhole_arbiter_.pending_size(1);
            }
            if (stage == 2) {
                std::size_t total = 0;
                for (std::size_t v = 0; v < VcAllocator::NUM_VC_MAX; ++v)
                    total += vc_allocator_.pending_size(static_cast<uint8_t>(v));
                return total;
            }
        }
        return 0;
    }

  private:
    // Declaration order:
    //   1. cfg_ + external refs.
    //   2. vc_allocator_ wraps downstream_rsp_.
    //   3. wormhole_arbiter_ wraps vc_allocator_.
    //   4. dat_vc_allocator_ wraps downstream_dat_rsp_ (independent DAT egress
    //      face, S3a T4; no wormhole arbiter -- single input, per §5.2).
    //   5. meta_buffer_ (no upstream dep).
    //   6. packetize_ takes wormhole_arbiter_.input(0/1) (Narrow R + B, RSP),
    //      dat_vc_allocator_ (Data R, DAT; T6 steering), + meta_buffer_.
    //   7. depacketize_ takes upstream_req_ + upstream_dat_req_ + meta_buffer_.
    //   8. axi_master_port_ takes depacketize_ + packetize_.
    NsuConfig cfg_;
    router::NocReqIn& upstream_req_;
    router::NocRspOut& downstream_rsp_;
    router::NocReqIn& upstream_dat_req_;
    router::NocRspOut& downstream_dat_rsp_;
    VcAllocator vc_allocator_;
    router::WormholeArbiter<router::NocRspOut> wormhole_arbiter_;
    VcAllocator dat_vc_allocator_;
    MetaBuffer meta_buffer_;
    Packetize packetize_;
    Depacketize depacketize_;
    AxiMasterPort axi_master_port_;
};

inline Nsu::Nsu(NsuConfig cfg, router::NocReqIn& upstream_req, router::NocRspOut& downstream_rsp,
                router::NocReqIn& upstream_dat_req, router::NocRspOut& downstream_dat_rsp)
    : cfg_(std::move(cfg)),
      upstream_req_(upstream_req),
      downstream_rsp_(downstream_rsp),
      upstream_dat_req_(upstream_dat_req),
      downstream_dat_rsp_(downstream_dat_rsp),
      vc_allocator_(downstream_rsp_, cfg_.num_vc, cfg_.vc_allocator_pending_depth),
      wormhole_arbiter_(vc_allocator_, /*num_inputs=*/2, std::vector<router::ChannelPairing>{},
                        cfg_.wormhole_per_input_depth),
      dat_vc_allocator_(downstream_dat_rsp_, cfg_.dat_num_vc, cfg_.vc_allocator_pending_depth),
      meta_buffer_(cfg_.port_params.meta_buffer_max_outstanding),
      packetize_(wormhole_arbiter_.input(0), wormhole_arbiter_.input(1), dat_vc_allocator_,
                 meta_buffer_, cfg_.src_id),
      depacketize_(upstream_req_, meta_buffer_, cfg_.port_params.meta_buffer_max_unique_ids,
                   upstream_dat_req_),
      axi_master_port_(depacketize_, packetize_, cfg_.port_params) {}

inline void Nsu::tick() {
    // Reverse-order tick for both req and rsp paths.
    // A beat advances exactly one stage per tick; later stages drain before
    // earlier stages fill, so no double-advance occurs.
    //
    // RSP path (S3 → S2 → S1, reverse order):
    //   S3: wormhole_arbiter_ drains from its pending_ (= S2→S3 register) to
    //       vc_allocator_, which drains to NoC. Draining before Packetize fills
    //       ensures the slot is free for this tick's S2 output.
    //   S2: packetize_.tick() reads s1_b_/s1_r_ stage registers, builds
    //       Flits, pushes to wormhole input (the S2→S3 register boundary).
    //       Because wormhole_.tick() already ran, the flits pushed here
    //       cannot escape to NoC until the next tick — the arbiter-final-stage
    //       property (no same-tick Packetize→NoC escape).
    //   S1: axi_master_port_ forward_b/r_to_packetizer_() takes ≤1 beat from
    //       b_q_/r_q_ and calls packetize_.push_b/r() (writes s1_b_/s1_r_).
    //       Packetize_.tick() already drained s1 this tick, so no overwrite.
    //
    // REQ path (S2 → S1, reverse order):
    //   S2: axi_master_port_ drain_*_from_depacketizer_ consumes from
    //       depacketize_.s1_* stage registers.
    //   S1: depacketize_.tick() decodes a new flit into s1_* registers.
    wormhole_arbiter_.tick();  // RSP S3a: drain S2→S3 boundary to VcAllocator
    vc_allocator_.tick();      // RSP S3b: drain VcAllocator pending to NoC
    // DAT egress (S3a T4 + T6 steering): independent network, own drain;
    // Packetize feeds this with Data-class R below (S2), same reverse-order
    // arbiter-final-stage property as the RSP wormhole/VC pair above.
    dat_vc_allocator_.tick();
    packetize_.tick();        // RSP S2: read S1 regs, push to S2→S3 boundary
    axi_master_port_.tick();  // RSP S1 + REQ S2: bounded B/R accept + req drain
    depacketize_.tick();      // REQ S1: decode flit into S1 stage registers
}

}  // namespace ni::cmodel::nsu
