// RouterChannelShellAdapter — ShellAdapter wrapping the production RouterChannel.
//
// Owns one RouterChannel(num_vc=1) and drives BOTH nodes per tick. Per node the
// 3-step pattern is identical to ChannelModelShellAdapter, indexed by node:
//   req_in (NMU inject) -> nmu_req_out(n).push_flit
//   rsp_in (NSU inject) -> nsu_rsp_out(n).push_flit
//   req_out (to NSU)    <- nsu_req_in(n).pop_flit
//   rsp_out (to NMU)    <- nmu_rsp_in(n).pop_flit
//   *_out_credit_return  = inject-side credit_avail(0)
// The SV-provided *_in_credit_return inputs are accepted but unused: RouterChannel
// manages credit internally (eject pop returns router credit; inject mirror gates
// the NMU via *_out_credit_return). num_vc=1: the SV NoC wrappers fatal otherwise.
//
// Depth: RouterChannel's default vc_depth is 4 (NI_NOC_ROUTER_VC_DEPTH); the SV
// credit feedback is registered one cycle behind (beta-tick), so a too-shallow
// inject mirror risks a stale-credit overshoot. Pin vc_depth/out_fifo to the
// PoC ChannelModel depth (kPoCChannelModelDepth=64) for margin, and throw if a
// push is ever rejected so a silent flit-drop becomes a loud DPI failure (the
// NMU is credit-gated via *_out_credit_return, so a reject means a real bug).
#pragma once
#include "cosim/flit_byte_conv.hpp"  // flit_from_bytes, flit_to_bytes
#include "cosim/poc_defaults.hpp"    // kPoCChannelModelDepth
#include "cosim/router_channel_shell_io.hpp"
#include "noc/router_channel.hpp"
#include <memory>
#include <stdexcept>

namespace ni::cmodel::cosim {

class RouterChannelShellAdapter {
  public:
    void init() {
        channel_ = std::make_unique<noc::RouterChannel>(
            /*num_vc=*/1, /*vc_depth=*/kPoCChannelModelDepth,
            /*out_fifo_depth=*/kPoCChannelModelDepth);
        in_ = RouterChannelInputs{};
        out_ = RouterChannelOutputs{};
    }

    void set_inputs(const RouterChannelInputs& in) { in_ = in; }

    void tick() {
        for (std::size_t n = 0; n < kRouterChannelNodes; ++n) {
            if (in_.node[n].req_in_valid &&
                !channel_->nmu_req_out(n).push_flit(flit_from_bytes(in_.node[n].req_in_flit))) {
                throw std::runtime_error(
                    "RouterChannelShellAdapter: req push rejected (credit "
                    "discipline violated at node " +
                    std::to_string(n) + ")");
            }
            if (in_.node[n].rsp_in_valid &&
                !channel_->nsu_rsp_out(n).push_flit(flit_from_bytes(in_.node[n].rsp_in_flit))) {
                throw std::runtime_error(
                    "RouterChannelShellAdapter: rsp push rejected (credit "
                    "discipline violated at node " +
                    std::to_string(n) + ")");
            }
        }

        channel_->tick();

        out_ = RouterChannelOutputs{};
        for (std::size_t n = 0; n < kRouterChannelNodes; ++n) {
            if (auto f = channel_->nsu_req_in(n).pop_flit()) {
                out_.node[n].req_out_valid = true;
                out_.node[n].req_out_flit = flit_to_bytes(*f);
            }
            if (auto f = channel_->nmu_rsp_in(n).pop_flit()) {
                out_.node[n].rsp_out_valid = true;
                out_.node[n].rsp_out_flit = flit_to_bytes(*f);
            }
            out_.node[n].req_out_credit_return = channel_->nmu_req_out(n).credit_avail(0);
            out_.node[n].rsp_out_credit_return = channel_->nsu_rsp_out(n).credit_avail(0);
        }
    }

    void get_outputs(RouterChannelOutputs& out) const { out = out_; }

  private:
    std::unique_ptr<noc::RouterChannel> channel_;
    RouterChannelInputs in_{};
    RouterChannelOutputs out_{};
};

}  // namespace ni::cmodel::cosim
